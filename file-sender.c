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
  // usa o fseek para saltar logo para a posição certa do ficheiro sem ter de o carregar todo para a memória
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
  // verifica se foram passados os 4 argumentos pedidos para não dar erro ao aceder ao argv
  if (argc != 5) {
    exit(EXIT_FAILURE);
  }

  char *file_name = argv[1];
  char *host = argv[2];
  // valida com strtol se o port 1 a 65535 e a janela 1 a 32 são números válidos
  char *end;
  errno = 0;
  long port_val = strtol(argv[3], &end, 10);
  if (errno != 0 || *argv[3] == '\0' || *end != '\0' || port_val < 1 || port_val > 65535) {
    exit(EXIT_FAILURE);
  }
  int port = (int)port_val;

  errno = 0;
  long win_val = strtol(argv[4], &end, 10);
  if (errno != 0 || *argv[4] == '\0' || *end != '\0' || win_val < 1 || win_val > MAX_WINDOW_SIZE) {
    exit(EXIT_FAILURE);
  }
  int window_size = (int)win_val;

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
  // coloca um timeout de 1 segundo no socket para não ficar preso à espera caso um pacote ou ACK se perca
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    perror("fseek");
    exit(EXIT_FAILURE);
  }
  long fsize = ftell(file);
  if (fsize < 0) {
    perror("ftell");
    exit(EXIT_FAILURE);
  }
  // soma-se 1 porque mesmo quando o tamanho é múltiplo de 1000 é preciso enviar um último chunk de 0 bytes a indicar o fim
  uint32_t total_chunks = (fsize / MAX_CHUNK_SIZE) + 1;

  uint32_t base = 0; //base da janela de envio
  uint32_t next_seq = 0; //próximo chunk a enviar
  uint32_t selective_acks = 0; //bits dos chunks já confirmados fora de ordem
  int dup_acks = 0; //contador de ACKs repetidos para o fast retransmit
  int timeouts = 0; //contador de timeouts seguidos

  while (base < total_chunks) {
    // vai enviando os próximos chunks enquanto houver espaço na janela
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
      // se der 3 timeouts seguidos sem receber nenhum ACK do recetor, desiste
      timeouts++;
      if (timeouts >= MAX_RETRIES) {
        close(sockfd);
        fclose(file);
        exit(EXIT_FAILURE);
      }
      dup_acks = 0;
      // volta a enviar os chunks da janela que ainda não foram confirmados no selective_acks
      for (uint32_t i = base; i < next_seq; i++) {
        if (i == base || !(selective_acks & (1U << (i - base - 1)))) {
          send_chunk(sockfd, file, &srv_addr, i);
        }
      }
    } else {
      // ignora se o pacote vier de outro endereço ou não tiver o tamanho certo de um ACK
      if (src_addr.sin_addr.s_addr != srv_addr.sin_addr.s_addr ||
          src_addr.sin_port != srv_addr.sin_port) {
        continue;
      }
      if (len != sizeof(ack_pkt_t)) {
        continue;
      }
      timeouts = 0;
      uint32_t ack_seq = ntohl(ack_pkt.seq_num);
      uint32_t ack_mask = ntohl(ack_pkt.selective_acks);
      if (ack_seq > base) {
        // como a base avançou, puxa os bits do selective_acks para a direita e junta os novos que vieram no ACK
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
      } else if (ack_seq == base) {
        // se a base não avançou é um ACK duplicado, então ao 3º reenvia logo o chunk da base sem esperar pelo timeout
        selective_acks |= ack_mask;
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
