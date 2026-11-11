#include "packet-format.h"
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

void send_chunk(int sockfd, FILE *file, struct sockaddr_in *srv_addr, uint32_t seq) {
  data_pkt_t data_pkt;
  fseek(file, seq * MAX_CHUNK_SIZE, SEEK_SET);
  size_t data_len = fread(data_pkt.data, 1, MAX_CHUNK_SIZE, file);
  data_pkt.seq_num = htonl(seq);
  ssize_t sent_len =
      sendto(sockfd, &data_pkt, offsetof(data_pkt_t, data) + data_len, 0,
             (struct sockaddr *)srv_addr, sizeof(*srv_addr));

  if (sent_len != offsetof(data_pkt_t, data) + data_len) {
    exit(EXIT_FAILURE);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 5) {
    exit(EXIT_FAILURE);
  }

  char *file_name = argv[1];
  char *host = argv[2];
  int port = atoi(argv[3]);

  int window_size = atoi(argv[4]);
  if (!(port > 0 && (window_size >= 1 && window_size <= MAX_WINDOW_SIZE))) {
    exit(EXIT_FAILURE);
  }

  FILE *file = fopen(file_name, "r");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server host address.
  struct hostent *he;
  if (!(he = gethostbyname(host))) {
    perror("gethostbyname");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = *((struct in_addr *)he->h_addr),
  };

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  fseek(file, 0, SEEK_END);
  long fsize = ftell(file);
  uint32_t total_chunks = (fsize / MAX_CHUNK_SIZE) + 1;

  uint32_t base = 0;
  uint32_t next_seq = 0;
  uint32_t selective_acks = 0;
  int dup_acks = 0;
  int timeouts = 0;

  while (base < total_chunks) {
    while (next_seq < total_chunks && next_seq < base + window_size) {
      send_chunk(sockfd, file, &srv_addr, next_seq);
      next_seq++;
    }

    struct sockaddr_in src_addr;
    ack_pkt_t ack_pkt;
    ssize_t len =
        recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0,
                 (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});

    if (len < 0) {
      timeouts++;
      if (timeouts >= MAX_RETRIES) {
        close(sockfd);
        fclose(file);
        exit(EXIT_FAILURE);
      }
      dup_acks = 0;
      for (uint32_t i = base; i < next_seq; i++) {
        if (i == base || !(selective_acks & (1U << (i - base - 1)))) {
          send_chunk(sockfd, file, &srv_addr, i);
        }
      }
    } else {
      if (src_addr.sin_addr.s_addr != srv_addr.sin_addr.s_addr ||
          src_addr.sin_port != srv_addr.sin_port) {
        continue;
      }
      timeouts = 0;
      uint32_t ack_seq = ntohl(ack_pkt.seq_num);
      uint32_t ack_mask = ntohl(ack_pkt.selective_acks);
      if (ack_seq > base) {
        uint32_t advance = ack_seq - base;
        if (advance >= 32) {
          selective_acks = 0;
        } else {
          selective_acks >>= advance;
        }
        selective_acks |= ack_mask;
        base = ack_seq;
        if (next_seq < base) {
          next_seq = base;
        }
        dup_acks = 0;
      } else {
        if (ack_seq == base) {
          selective_acks |= ack_mask;
        }
        dup_acks++;
        if (dup_acks == FAST_RETRANSMIT_DUPACKS) {
          send_chunk(sockfd, file, &srv_addr, base);
          dup_acks = 0;
        }
      }
    }
  }

  // Clean up and exit.
  close(sockfd);
  fclose(file);

  return EXIT_SUCCESS;
}
