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
#define ACCEPT_ENCODING "gzip"

enum CONTENT_TYPE {
  octet_stream,
  text_plain,
};

char *response_ok = "HTTP/1.1 200 OK\r\n\r\n";
char *response_not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
char *response_created = "HTTP/1.1 201 Created\r\n\r\n";

struct HandleConnectionArgs {
  int client_fd;
  int server_fd;
  char *dir;
};

int find_substring_between(char **str, const char *orig_str,
                           const char *start_str, const char *end_str);
void *handle_connection(void *arg);
char *handle_echo(char *endpoint, char orig_buffer[BUF_SIZE], int client_fd,
                  int server_fd);
char *handle_user_agent(char orig_buffer[BUF_SIZE], int client_fd,
                        int server_fd);
char *handle_get_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                       int client_fd, int server_fd);
char *handle_post_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                        int client_fd, int server_fd);
int get_encoding(char **str, char orig_buffer[BUF_SIZE]);
char *get_response_with_body(enum CONTENT_TYPE type, int content_length,
                             char *body, char *encoding);

// TODO: change sprintf to strncat

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

  ssize_t bytes_read = read(client_fd, request_buffer, BUF_SIZE - 1);
  if (bytes_read < 0) {
    printf("Read failed: %s\n", strerror(errno));
    goto cleanup;
  }
  request_buffer[bytes_read] = '\0';

  char *reply = NULL;
  char method[16] = {0};
  char path[BUF_SIZE] = {0};
  char *endpoint = NULL;

  if (sscanf(request_buffer, "%15s %1023s", method, path) != 2) {
    printf("Failed parse request line\n");
    reply = response_not_found;
    goto end;
  }

  endpoint = path + 1;
  char *slash = strchr(endpoint, '/');
  if (slash) {
    slash++;
  }

  if (strlen(endpoint) == 0 && strcmp(method, "GET") == 0) {
    reply = response_ok;
  } else if (strncmp(endpoint, "user-agent", 10) == 0 &&
             strcmp(method, "GET") == 0) {
    reply = handle_user_agent(request_buffer, client_fd, server_fd);
  } else if (strncmp(endpoint, "echo", 4) == 0 && strcmp(method, "GET") == 0 &&
             slash) {
    reply = handle_echo(slash, request_buffer, client_fd, server_fd);
  } else if (strncmp(endpoint, "files", 5) == 0 && strcmp(method, "GET") == 0 &&
             slash) {
    reply = handle_get_files(slash, dir, request_buffer, client_fd, server_fd);
  } else if (strncmp(endpoint, "files", 5) == 0 &&
             strcmp(method, "POST") == 0 && slash) {
    reply = handle_post_files(slash, dir, request_buffer, client_fd, server_fd);
  } else {
    reply = response_not_found;
  }

end:
  send(client_fd, reply, strlen(reply), 0);

  if (reply != response_ok && reply != response_not_found &&
      reply != response_created) {
    free(reply);
  }

cleanup:
  free(arg);
  close(client_fd);
  return NULL;
}

char *handle_echo(char *slash, char orig_buffer[BUF_SIZE], int client_fd,
                  int server_fd) {
  char *reply = NULL;
  char *accept_encoding = NULL;
  get_encoding(&accept_encoding, orig_buffer);
  reply =
      get_response_with_body(text_plain, strlen(slash), slash, accept_encoding);
  if (reply == NULL) {
    printf("Memory allocation failed: %s\n", strerror(errno));
    close(client_fd);
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  return reply;
}

char *handle_user_agent(char orig_buffer[BUF_SIZE], int client_fd,
                        int server_fd) {
  char *reply = NULL;
  char *pre = "User-Agent: ";
  char *user_agent = NULL;
  char *accept_encoding = NULL;
  get_encoding(&accept_encoding, orig_buffer);
  int user_agent_len =
      find_substring_between(&user_agent, orig_buffer, pre, "\r\n");
  if (user_agent_len > 0) {
    reply = get_response_with_body(text_plain, user_agent_len, user_agent,
                                   accept_encoding);
    if (reply == NULL) {
      printf("Memory allocation failed: %s\n", strerror(errno));
      free(user_agent);
      close(client_fd);
      close(server_fd);
      exit(EXIT_FAILURE);
    }
    free(user_agent);
  } else {
    reply = response_not_found;
  }

  return reply;
}

char *handle_get_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                       int client_fd, int server_fd) {
  char *reply = NULL;
  size_t len;

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
  char *encoding = NULL;
  get_encoding(&encoding, orig_buffer);

  reply = get_response_with_body(octet_stream, len, f_buff, encoding);

  if (reply == NULL) {
    printf("Memory allocation failed: %s\n", strerror(errno));
    free(f_buff);
    return response_not_found;
  }
  free(f_buff);

  return reply;
}

char *handle_post_files(char *filename, char *dir, char orig_buffer[BUF_SIZE],
                        int client_fd, int server_fd) {
  char *content_length_str = NULL;
  if (find_substring_between(&content_length_str, orig_buffer,
                             "Content-Length: ", "\r\n") == -1) {
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

int find_substring_between(char **str, const char *orig_str,
                           const char *start_str, const char *end_str) {
  char *start = strstr(orig_str, start_str);
  if (start == NULL) {
    return -1;
  }

  start += strlen(start_str);

  char *end = strstr(start, end_str);
  if (end == NULL) {
    return -1;
  }

  size_t len = end - start;

  *str = (char *)malloc(len + 1);
  if (*str == NULL) {
    return -1;
  }

  strncpy(*str, start, len);
  (*str)[len] = '\0';

  return len;
}

int get_encoding(char **str, char orig_buffer[BUF_SIZE]) {
  int len =
      find_substring_between(str, orig_buffer, "Accept-Encoding: ", "\r\n");

  if (len == -1) {
    return -1;
  }

  *str = strtok(*str, ", ");

  while (*str != NULL) {
    if (strcmp(*str, ACCEPT_ENCODING) == 0) {
      return strlen(*str);
    }
    *str = strtok(NULL, ", ");
  }

  *str = NULL;

  return -1;
}

char *get_response_with_body(enum CONTENT_TYPE type, int content_length,
                             char *body, char *encoding) {
  char *response_template = "HTTP/1.1 200 OK\r\n"
                            "%s"
                            "%s"
                            "Content-Length: %d\r\n"
                            "\r\n"
                            "%s";
  char encoding_header[64] = "";
  char *content_type_header = type == octet_stream
                                  ? "Content-Type: application/octet-stream\r\n"
                                  : "Content-Type: text/plain\r\n";

  if (encoding) {
    snprintf(encoding_header, sizeof(encoding_header),
             "Content-Encoding: %s\r\n", encoding);
  }

  size_t response_size = strlen(response_template) +
                         strlen(content_type_header) + strlen(encoding_header) +
                         content_length;
  char *reply = malloc(response_size);
  if (!reply) {
    printf("get_response_text_plain: allocation failed\n");
    return NULL;
  }

  snprintf(reply, response_size, response_template, content_type_header,
           encoding_header, content_length, body);

  return reply;
}
