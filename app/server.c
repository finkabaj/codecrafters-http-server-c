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
char *response_octec_stream =
    "HTTP/1.1 200 OK\r\nContent-Type: "
    "application/octet-stream\r\nContent-Length: %d\r\n\r\n%s";
char *response_created = "HTTP/1.1 201 Created\r\n\r\n";

struct HandleConnectionArgs {
  int client_fd;
  int server_fd;
  char *dir;
};

char *find_substring_between(const char *str, const char *start_str,
                             const char *end_str);
void *handle_connection(void *arg);
char *handle_echo(char *endpoint, int client_fd, int server_fd);
char *handle_user_agent(char orig_buffer[BUF_SIZE], int client_fd,
                        int server_fd);
char *handle_get_files(char *filename, char *dir, int client_fd, int server_fd);
char *handle_post_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                        int client_fd, int server_fd);

int main(int argc, char **argv) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  char *dir = "tmp";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--directory") == 0) {
      if (++i == argc) {
        printf("use: --directory file");
        exit(EXIT_FAILURE);
      }

      dir = argv[i];
    } else {
      printf("invalid flag");
      exit(EXIT_FAILURE);
    }
  }

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
    args->dir = dir;

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
  char *dir = args->dir;

  char request_buffer[BUF_SIZE];

  if (read(client_fd, request_buffer, BUF_SIZE) < 0) {
    printf("Read failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  int buf_len = strlen(request_buffer);
  char orig_buffer[BUF_SIZE];
  memcpy(orig_buffer, request_buffer, buf_len);

  char *path = strtok(request_buffer, " ");
  char *method = path;
  path = strtok(NULL, " ");

  char *endpoint = strtok(path, "/");
  char *reply = NULL;

  if (endpoint == NULL && strcmp(method, "GET") == 0) {
    reply = response_ok;
  } else if (endpoint != NULL && strcmp(endpoint, "user-agent") == 0 &&
             strcmp(method, "GET") == 0) {
    reply = handle_user_agent(orig_buffer, client_fd, server_fd);
  } else if (endpoint != NULL && strcmp(endpoint, "echo") == 0 &&
             strcmp(method, "GET") == 0 &&
             (endpoint = strtok(NULL, "/")) != NULL) {
    reply = handle_echo(endpoint, client_fd, server_fd);
  } else if (endpoint != NULL && strcmp(endpoint, "files") == 0 &&
             strcmp(method, "GET") == 0 &&
             (endpoint = strtok(NULL, "/")) != NULL) {
    reply = handle_get_files(endpoint, dir, client_fd, server_fd);
  } else if (endpoint != NULL && strcmp(endpoint, "files") == 0 &&
             strcmp(method, "POST") == 0 &&
             (endpoint = strtok(NULL, "/")) != NULL) {
    reply = handle_post_files(endpoint, dir, orig_buffer, client_fd, server_fd);
  } else {
    reply = response_not_found;
  }

  int bytes_sent = send(client_fd, reply, strlen(reply), 0);

  if (reply != response_ok && reply != response_not_found &&
      reply != response_created) {
    free(reply);
  }

  free(arg);
  close(client_fd);
  return 0;
}

char *handle_echo(char *endpoint, int client_fd, int server_fd) {
  char *reply = NULL;
  size_t reply_size = strlen(response_text_plain) + strlen(endpoint) + 1;
  reply = (char *)malloc(reply_size);
  if (reply == NULL) {
    printf("Memory allocation failed: %s\n", strerror(errno));
    close(client_fd);
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  sprintf(reply, response_text_plain, strlen(endpoint), endpoint);

  return reply;
}

char *handle_user_agent(char orig_buffer[], int client_fd, int server_fd) {
  char *reply = NULL;
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

  return reply;
}

char *handle_get_files(char *filename, char *dir, int client_fd,
                       int server_fd) {
  char *reply = NULL;
  size_t len;

  // TODO: validate path

  char path[strlen(filename) + strlen(dir) + 2];
  sprintf(path, "%s/%s", dir, filename);

  FILE *fp;
  if ((fp = fopen(path, "r")) == NULL) {
    printf("file %s not found\n", filename);
    return response_not_found;
  }

  char *f_buff;

  fseek(fp, 0, SEEK_END);
  len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if ((f_buff = (char *)malloc(len)) == NULL) {
    printf("Memory allocation failed: %s\n", strerror(errno));
    fclose(fp);
    return response_not_found;
  }
  fread(f_buff, 1, len, fp);

  fclose(fp);

  size_t reply_size = strlen(response_octec_stream) + len + 1;

  if ((reply = (char *)malloc(reply_size)) == NULL) {
    printf("Memory allocation failed: %s\n", strerror(errno));
    free(f_buff);
    return response_not_found;
  }
  sprintf(reply, response_octec_stream, len, f_buff);
  free(f_buff);

  return reply;
}

char *handle_post_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                        int client_fd, int server_fd) {
  char *content_length_str = NULL;
  if ((content_length_str = find_substring_between(
           orig_buffer, "Content-Length: ", "\r\n")) == NULL) {
    printf("handle_post_files: Content-Length header not found\n");
    return response_not_found;
  }

  int content_length = atoi(content_length_str);
  free(content_length_str);

  if (content_length <= 0 || content_length > BUF_SIZE) {
    printf("handle_post_files: invalid content length\n");
    return response_not_found;
  }

  char *content_start = strstr(orig_buffer, "\r\n\r\n");
  if (!content_start) {
    printf("handle_post_files: couldn't find start of content\n");
    return response_not_found;
  }
  content_start += 4;

  char *path = NULL;
  if ((path = (char *)malloc((strlen(filename) + strlen(dir) + 2) *
                             sizeof(char))) == NULL) {
    printf("handle_post_files: allocation failed\n");
    return response_not_found;
  }
  sprintf(path, "%s/%s", dir, filename);

  FILE *fp = fopen(path, "wx");

  if (!fp) {
    if (errno == EEXIST) {
      printf("handle_post_files: file %s already exists\n", filename);
    } else {
      printf("handle_post_files: error creating %s file: %s\n", filename,
             strerror(errno));
    }
    free(path);
    return response_not_found;
  }

  size_t written = fwrite(content_start, 1, content_length, fp);
  fclose(fp);
  free(path);

  if (written != content_length) {
    printf("handle_post_files: error while writing to file %s\n", filename);
    return response_not_found;
  }

  return response_created;
}

char *find_substring_between(const char *str, const char *start_str,
                             const char *end_str) {
  char *start = strstr(str, start_str);
  if (start == NULL) {
    return NULL;
  }

  start += strlen(start_str);

  char *end = strstr(start, end_str);
  if (end == NULL) {
    return NULL;
  }

  size_t len = end - start;

  char *result = (char *)malloc(len + 1);
  if (result == NULL) {
    return NULL;
  }

  strncpy(result, start, len);
  result[len] = '\0';

  return result;
}
