// A minimal TCP server that handles PING and ECHO commands using Redis-style responses.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 6379
#define BACKLOG 10
#define BUFFER_SIZE 1024

typedef enum {
    CMD_INVALID,
    CMD_PING,
    CMD_ECHO
} Command;

typedef struct {
    Command type;
    char *argument;
} ParsedCommand;


ParsedCommand parse_command(char *input);

/* Parse argument from request and returns the parsed command. */
ParsedCommand parse_command(char *input) {
    ParsedCommand result = {CMD_INVALID, NULL};
    if (input == NULL) return result;
    char *space = strchr(input, ' ');

    if (strncmp(input, "PING", 4) == 0) {
        result.type = CMD_PING;
    } else if (strncmp(input, "ECHO", 4) == 0 && input[4] == ' ') {
        result.type = CMD_ECHO;
        result.argument = space + 1;
    }

    return result;
}

int main(void) {
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
        char buffer[BUFFER_SIZE];

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
        printf("%s\n", buffer);
        
        
        // send
        const char *response = "";
        ParsedCommand cmd = parse_command(buffer);
        int bytes_sent = 0;

        switch (cmd.type) {
            case CMD_PING:
                response = "+PONG\r\n";
                bytes_sent = send(client_fd, response, strlen(response), 0);
                break;

            case CMD_ECHO:
                if (cmd.argument != NULL) {
                    bytes_sent = send(client_fd, cmd.argument, strlen(cmd.argument), 0);
                    bytes_sent = send(client_fd, "\r\n", 2, 0);
                }
                break;

            default:
                response = "-ERR unknown command\r\n";
                bytes_sent = send(client_fd, response, strlen(response), 0);
                break;
        }

        if (bytes_sent == -1) {
            perror("send");
        }

        // close the CLIENT socket
        close(client_fd);
    }

    close(server_fd);

    return 0;
}
