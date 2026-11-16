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
  // usa-se o strtol para garantir que o port está entre 1 e 65535 e não tem letras misturadas
  char *end;
  errno = 0;
  long port_val = strtol(argv[2], &end, 10);
  if (errno != 0 || *argv[2] == '\0' || *end != '\0' || port_val < 1 || port_val > 65535) {
    exit(EXIT_FAILURE);
  }
  int port = (int)port_val;

  // uma janela <= 0 não consegue receber pacotes, e o campo selective_acks tem 32 bits, logo o pacote nunca permite janelas maiores que 32
  errno = 0;
  long win_val = strtol(argv[3], &end, 10);
  if (errno != 0 || *argv[3] == '\0' || *end != '\0' || win_val < 1 || win_val > MAX_WINDOW_SIZE) {
    perror("window");
    exit(EXIT_FAILURE);
  }
  int window_size = (int)win_val;



  FILE *file = fopen(file_name, "w");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server socket.
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }
  // o recvfrom() bloqueia para sempre, então colocamos 4s de timeout para ainda apanhar retransmissões caso falhe o último ACK
  struct timeval tv;
  tv.tv_sec = 4;
  tv.tv_usec = 0;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

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
  while (true) {
    struct sockaddr_in src_addr;
    data_pkt_t data_pkt;
    len =
        recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
                 (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});

    // se passarem os 4s e ainda não tivermos recebido tudo até ao último chunk, apagamos o ficheiro que ficou a meio
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

    // guarda o endereço do primeiro remetente e ignora pacotes que venham de outros sítios
    if (!has_sender) {sender_addr = src_addr; has_sender = true;}
    else {if ((src_addr.sin_addr.s_addr != sender_addr.sin_addr.s_addr) || (src_addr.sin_port != sender_addr.sin_port))
          {continue;}}

    // ignora pacotes corrompidos que nem sequer têm os 4 bytes do seq_num para o data_len não dar underflow
    if (len < (ssize_t)offsetof(data_pkt_t, data)) {
      continue;
    }

    uint32_t seq = ntohl(data_pkt.seq_num);
    size_t data_len = len - offsetof(data_pkt_t, data);
    // só escreve no ficheiro se o chunk estiver dentro da janela e ainda não tiver sido recebido antes
    if (seq >= base && seq < base + window_size && (last_chunk == -1 || seq <= (uint32_t)last_chunk)) {
      if (seq == base || !(selective_acks & (1U << (seq - base - 1)))) {
        fseek(file, seq * MAX_CHUNK_SIZE, SEEK_SET);
        fwrite(data_pkt.data, 1, data_len, file);
        fflush(file);
        if (data_len < MAX_CHUNK_SIZE){
          last_chunk = seq;
        }
        if (seq == base){
          // como chegou o chunk da base, avança a janela e aproveita para avançar também pelos seguintes que já cá estavam
          base ++;
          while (selective_acks & 1){
            base++;
            selective_acks >>= 1;
          }
        } else if (seq > base){
          // se chegou fora de ordem, mete o bit correspondente a 1 no selective_acks
          selective_acks |= (1U << (seq - base -1));
        }
      }
    }

    // envia sempre o ACK atualizado, mesmo quando o chunk é repetido, porque o ACK anterior pode ter-se perdido
    ack_pkt_t ack = { .seq_num = htonl(base), .selective_acks = htonl(selective_acks)};
    sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr));
  }

  // Clean up and exit.
  close(sockfd);
  fclose(file);

  return EXIT_SUCCESS;
}
