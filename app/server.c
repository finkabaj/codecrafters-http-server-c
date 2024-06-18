#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 1024
char *response_ok = "HTTP/1.1 200 OK\r\n\r\n";
char *response_not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
char *response_text_plain = "HTTP/1.1 200 OK\r\nContent-Type: "
                            "text/plain\r\nContent-Length: %d\r\n\r\n%s";

struct HandleConnectionArgs {
  int client_fd;
  int server_fd;
};

void *handle_connection(void *arg);

int main() {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  // You can use print statements as follows for debugging, they'll be visible
  // when running tests.
  printf("Logs from your program will appear here!\n");

  int server_fd, client_addr_len;

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    printf("Socket creation failed: %s...\n", strerror(errno));
    return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    printf("SO_REUSEADDR failed: %s \n", strerror(errno));
    return 1;
  }

  struct sockaddr_in serv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(4221),
      .sin_addr = {htonl(INADDR_ANY)},
  };

  if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
    printf("Bind failed: %s \n", strerror(errno));
    return 1;
  }

  int connection_backlog = 10;
  if (listen(server_fd, connection_backlog) != 0) {
    printf("Listen failed: %s \n", strerror(errno));
    return 1;
  }

  printf("Waiting for a client to connect...\n");

  while (1) {
    struct sockaddr_in client_addr;
    client_addr_len = sizeof(client_addr);
    int client_fd =
        accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);

    if (client_fd < 0) {
      printf("Accept failed: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    printf("Client connected\n");

    struct HandleConnectionArgs *args =
        malloc(sizeof(struct HandleConnectionArgs));
    args->client_fd = client_fd;
    args->server_fd = server_fd;

    pthread_t th;
    pthread_create(&th, NULL, handle_connection, (void *)args);
  }

  close(server_fd);

  return 0;
}

void *handle_connection(void *arg) {
  struct HandleConnectionArgs *args = (struct HandleConnectionArgs *)arg;

  int client_fd = args->client_fd;
  int server_fd = args->server_fd;

  char request_buffer[BUF_SIZE];

  if (read(client_fd, request_buffer, BUF_SIZE) < 0) {
    printf("Read failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  int buf_len = strlen(request_buffer);
  char orig_buffer[buf_len];
  memcpy(orig_buffer, request_buffer, buf_len);

  char *path = strtok(request_buffer, " ");
  path = strtok(NULL, " ");

  char *endpoint = strtok(path, "/");
  char *reply = NULL;

  if (endpoint == NULL) {
    reply = response_ok;
  } else if (strcmp(endpoint, "user-agent") == 0) {
    char *pre = "User-Agent";
    char *user_agent = NULL;
    for (char *head = strtok(orig_buffer, "\r\n"); head != NULL;
         head = strtok(NULL, "\r\n")) {
      if (strncmp(pre, head, strlen(pre)) == 0) {
        user_agent = head + strlen(pre) + 2;
        break;
      }
    }
    if (user_agent != NULL) {
      size_t reply_size = strlen(response_text_plain) + strlen(user_agent) + 1;
      reply = (char *)malloc(reply_size);
      if (reply == NULL) {
        printf("Memory allocation failed: %s\n", strerror(errno));
        close(client_fd);
        close(server_fd);
        exit(EXIT_FAILURE);
      }
      sprintf(reply, response_text_plain, strlen(user_agent), user_agent);
    } else {
      reply = response_not_found;
    }
  } else if (strcmp(endpoint, "echo") == 0 &&
             (endpoint = strtok(NULL, "/")) != NULL) {
    size_t reply_size = strlen(response_text_plain) + strlen(endpoint) + 1;
    reply = (char *)malloc(reply_size);
    if (reply == NULL) {
      printf("Memory allocation failed: %s\n", strerror(errno));
      close(client_fd);
      close(server_fd);
      exit(EXIT_FAILURE);
    }
    sprintf(reply, response_text_plain, strlen(endpoint), endpoint);
  } else {
    reply = response_not_found;
  }

  int bytes_sent = send(client_fd, reply, strlen(reply), 0);

  if (reply != response_ok && reply != response_not_found) {
    free(reply);
  }

  free(arg);
  close(client_fd);
}
