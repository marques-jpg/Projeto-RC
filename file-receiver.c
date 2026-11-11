#include "packet-format.h"
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

int main(int argc, char *argv[]) {
  // verifica se a quantidade de argumentos é diferente de 4 para não dar Segmetation Fault
  if (argc != 4) {
    perror("argc");
    exit(EXIT_FAILURE);
  }

  
  char *file_name = argv[1];
  int port = atoi(argv[2]);


  // uma janela <= 0 não consegue receber pacotes, e o campo selective_acks tem 32 bits, logo o pacote nunca permite janelas maiores que 32
  int window_size = atoi(argv[3]);
  if ((port <= 0) || (window_size < 1) || (window_size > MAX_WINDOW_SIZE)) {
    perror("window");
    exit(EXIT_FAILURE);
  }



  FILE *file = fopen(file_name, "w");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server socket.
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  // o recvfrom() por defeito bloqueia para sempre se não chegarem pacotes, então criamos um timeout de 4 segundos. Assim, caso um pacote se perca na rede, o remetente vai retransmitir o último chunk ao fim de 1s e, como o recetor fica 4s à espera antes de fechar, ainda fica ligado para responder ao novo ACK
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }
  struct timeval tv;
  tv.tv_sec = 4;
  tv.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Allow address reuse so we can rebind to the same port,
  // after restarting the server.
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) <
      0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
      .sin_port = htons(port),
  };
  if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
    perror("bind");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Receiving on port: %d\n", port);

  uint32_t base = 0; //base da janela de receção
  uint32_t selective_acks = 0; //bits dos chunks fora de ordem
  int64_t last_chunk = -1; //guarda o seq_num do último chunk quando chegar um pacote com menos de 1000 bytes de dados
  bool has_sender = false;
  struct sockaddr_in sender_addr; //fixar o primeiro remetente

  ssize_t len;
  while (true) { // Iterate over segments, until last the segment is detected.
    // Receive segment.
    struct sockaddr_in src_addr;
    data_pkt_t data_pkt;
    len =
        recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
                 (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
    // printf("Received segment %d, size %ld.\n", ntohl(data_pkt.seq_num), len);

    // Write data to file.
    // fwrite(data_pkt.data, 1, len - offsetof(data_pkt_t, data), file);
  
    if (len < 0) { 
      if (!(last_chunk != -1 && base > last_chunk)) {
        fclose(file); 
	      close(sockfd);
        remove(file_name); 
	      exit(EXIT_FAILURE);
      } 
      break;
    } 
    
    printf("Received segment %d, size %ld.\n", ntohl(data_pkt.seq_num), len);


    if (!has_sender) {sender_addr = src_addr; has_sender = true;}
    else {if ((src_addr.sin_addr.s_addr != sender_addr.sin_addr.s_addr) || (src_addr.sin_port != sender_addr.sin_port))
          {continue;}}

    uint32_t seq = ntohl(data_pkt.seq_num);
    size_t data_len = len - offsetof(data_pkt_t, data);
    if (seq >= base && seq < base + window_size) {
      fseek(file, seq * MAX_CHUNK_SIZE, SEEK_SET);
      fwrite(data_pkt.data, 1, data_len, file);
      fflush(file);
      if (data_len < MAX_CHUNK_SIZE){
        last_chunk = seq;
      }
      if (seq == base){
        base ++;
        while (selective_acks & 1){
          base++;
          selective_acks >>= 1;
        }
      } else if (seq > base){
        selective_acks |= (1U << (seq - base -1));
      }
    }

    ack_pkt_t ack = { .seq_num = htonl(base), .selective_acks = htonl(selective_acks)};
    sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr));
  }




  // do { // Iterate over segments, until last the segment is detected.
  //   // Receive segment.
  //   struct sockaddr_in src_addr;
  //   data_pkt_t data_pkt;
  //   len =
  //       recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
  //                (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
  //   printf("Received segment %d, size %ld.\n", ntohl(data_pkt.seq_num), len);

  //   // Write data to file.
  //   fwrite(data_pkt.data, 1, len - offsetof(data_pkt_t, data), file);
  // } while (len == sizeof(data_pkt_t));

  // Clean up and exit.
  close(sockfd);
  fclose(file);

  return EXIT_SUCCESS;
}
