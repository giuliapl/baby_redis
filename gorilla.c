#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 6379
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define MAX_ARGS 4
#define MAX_ARG_SIZE 100

typedef struct {
    int argc;
    char argv[MAX_ARGS][MAX_ARG_SIZE];
} RedisCommand;

int parse_simple_string(const char *input, char output[]);
int parse_bulk_string(const char *input, char output[]);
RedisCommand *parse_redis_command(const char *input);
void handle_command(int client_fd, RedisCommand *command);
void print_string_bytes(const char *s);

/* Used for debugging: prints string bytes. */
void print_string_bytes(const char *s) {
    for (int i = 0; s[i] != '\0'; i++) {
        unsigned char c = s[i];
        if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else printf("%c", c);
    }
    printf("\\0\n");
}

/* Parse a RESP simple string: +string\r\n. Returns 1 if valid, 0 otherwise. */
int parse_simple_string(const char *input, char output[]) {
    if (*input != '+') return 0;
    int i = 0;
    input++;
    while (*input != '\r') {
        if (*input == '\0') return 0; // if no \r is ever encountered, we exit with error
        output[i++] = *input++;
    }
    input++;
    if (*input != '\n') return 0;
    output[i] = '\0';

    return 1;
}

/* Parse a RESP bulk string: $bytes\r\nstring\r\n. Returns the number of bytes effectively consumed. */
int parse_bulk_string(const char *input, char output[]) {
    // Check first character is $
    if (*input != '$') return 0;
    // Parse the number of expected characters & check it is followed by \r\n
    char *length_end;
    unsigned long length = strtoul(input + 1, &length_end, 10);
    
    if (length_end[0] != '\r' || length_end[1] != '\n') return 0;
    size_t bytes = (size_t)length;
    // Start of the actual bulk string
    const char *string_start = length_end + 2;
    // Parse string, copy into output & check closing \r\n are present
    for (size_t i = 0; i < bytes; i++) {
        if (string_start[i] == '\0') return 0; // return error if no \r is ever encountered
        output[i] = string_start[i];
    }
    output[bytes] = '\0';
    // Closing \r\n must come immediately after the payload
    const char *end = string_start + bytes;
    if (end[0] != '\r' || end[1] != '\n') return 0;

    // Return total number of input characters consumed
    return (int)((end + 2) - input);
}

/* Parse a RESP command: *argc\r\nbulk_str_argvs. Returns the pointer to the newly created command. */
RedisCommand *parse_redis_command(const char *input) {
    if (*input != '*') return 0;
    // Parse the number of expected argv & check it is followed by \r\n
    char *argc_end_ptr;
    unsigned int argc = strtoul(input + 1, &argc_end_ptr, 10);
    if (argc_end_ptr[0] != '\r' || argc_end_ptr[1] != '\n') return 0;
    char *command_ptr = argc_end_ptr;
    // Move past the array header
    command_ptr += 2;
    // Populate structure
    RedisCommand *redis_command = malloc(sizeof(RedisCommand));
    redis_command->argc = argc;
    int bulk_parsed_bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (*command_ptr == '\0') return 0;
        // find a '$'
        if (*command_ptr == '$') {
            // call parse_bulk_string() & store the result in argv[i]
            bulk_parsed_bytes = parse_bulk_string(command_ptr, redis_command->argv[i]);
        }
        // move ptr forward by the number of characters + \r\n twice + the $byte itself
        command_ptr += bulk_parsed_bytes;
    }
    printf("output: %s", redis_command->argv[0]);
    return redis_command;
}

/* Receive an already parsed command. Sends to the server the relevant RESP bulk string. */
void handle_command(int client_fd, RedisCommand *command) {
    if (command->argc == 0) return; 
    ssize_t bytes_sent = 0;
    const char *response = "";
    
    // Parse first argv
    if (strcmp(command->argv[0], "PING") == 0) { // No optional argv allowed yet 
        response = "+PONG\r\n";
        bytes_sent = send(client_fd, response, strlen(response), 0);
    } else if (strcmp(command->argv[0], "ECHO") == 0) {
        // Send bulk string. Redis accepts just 1 argv for the ECHO command
        char header[64];
        int header_len = snprintf(header, sizeof(header), "$%zu\r\n", strlen(command->argv[1])); // builds a RESP bulk string from values
        response = command->argv[1];
        bytes_sent += send(client_fd, header, header_len, 0);
        bytes_sent += send(client_fd, response, strlen(response), 0);
        bytes_sent += send(client_fd, "\r\n", 2, 0);
    } else {
        response = "-ERR unknown command\r\n";
        bytes_sent = send(client_fd, response, strlen(response), 0);
    }

    if (bytes_sent == -1) {
        perror("send");
    }
}

int main(void) {
    // char *test = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n";
    // RedisCommand *res = parse_redis_command(test);

    // File descriptors
    int server_fd;
    int client_fd;

    // sockaddr_in represents an IPv4 socket address. 127.0.0.1:6379
    struct sockaddr_in address;

    // Create a socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        exit(1);
    }


    // Describe the address we want to bind to
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);


    // bind
    if (bind(
            server_fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) == -1) {

        perror("bind");
        close(server_fd);
        exit(1);
    }


    // listen
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Redis server listening on 127.0.0.1:%d\n", PORT);


    // Main server loop
    while (1) {

        // accept, waits until a client connects
        client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        printf("Client connected\n");

        // recv, reads bytes sent by the client
        char buffer[BUFFER_SIZE] = {0};

        int bytes_received = recv(
            client_fd,
            buffer,
            sizeof(buffer) - 1,
            0
        );

        if (bytes_received == -1) {
            perror("recv");
            close(client_fd);
            continue;
        }

        if (bytes_received == 0) {
            printf("Client disconnected\n");
            close(client_fd);
            continue;
        }

        buffer[bytes_received] = '\0';
        
        printf("Received %d bytes:\n", bytes_received);
        print_string_bytes(buffer);

        // send
        RedisCommand *cmd = parse_redis_command(buffer);
        handle_command(client_fd, cmd);

        // close the CLIENT socket
        close(client_fd);
    }

    close(server_fd);

    return 0;
}