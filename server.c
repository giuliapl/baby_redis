#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 6379
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define MAX_ARGS 4
#define MAX_ARG_SIZE 100
#define MAX_ENTRIES 100

typedef struct {
    int argc;
    char argv[MAX_ARGS][MAX_ARG_SIZE];
} Command;

typedef struct {
    char *key;
    char *value;
} DatabaseEntry;

typedef struct {
    DatabaseEntry *entries[MAX_ENTRIES];
    int size;
} Database;

typedef enum {
    PARSE_INVALID,
    PARSE_INCOMPLETE,
    PARSE_COMPLETE
} ParseStatus;

int parse_simple_string(const char *input, char output[]);
ParseStatus parse_bulk_string(const char *input, size_t input_len, char output[], size_t *consumed);
ParseStatus parse_command(const char *input, size_t input_len, Command **result, size_t *consumed);
void handle_command(Database *db, Command *command, char response[]);
void print_string_bytes(const char *s); // Just for debugging
void serialize_bulk_string(const char *str, size_t str_len, char output[]);
void serialize_null_bulk_string(char output[]);
void serialize_simple_string(const char *str, size_t str_len, char output[]);
void serialize_error(const char *str, size_t str_len, char output[]);
void db_init(Database *db);
int db_find(const Database *db, const char *key); // Helper method
int db_set(Database *db, char *key, char *value);
char *db_get(Database *db, char *key);
int send_all(int fd, const char *buffer, size_t length);

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
ParseStatus parse_bulk_string(const char *input, size_t input_len, char output[], size_t *consumed) {
    // Check first character is $
    if (input_len == 0) return PARSE_INCOMPLETE;
    if (input[0] != '$') return PARSE_INVALID;

    size_t position = 1;
    size_t bytes = 0;
    if (position == input_len) return PARSE_INCOMPLETE;
    if (input[position] < '0' || input[position] > '9') return PARSE_INVALID;

    while (position < input_len && input[position] >= '0' && input[position] <= '9') {
        size_t digit = (size_t)(input[position] - '0');
        if (bytes > (MAX_ARG_SIZE - 1 - digit) / 10) return PARSE_INVALID;
        bytes = bytes * 10 + digit;
        position++;
    }

    if (position == input_len) return PARSE_INCOMPLETE;
    if (input[position] != '\r') return PARSE_INVALID;
    if (position + 1 >= input_len) return PARSE_INCOMPLETE;
    if (input[position + 1] != '\n') return PARSE_INVALID;
    position += 2;

    // Start of the actual bulk string
    const char *string_start = input + position;
    // Parse string, copy into output & check closing \r\n are present
    if (bytes >= MAX_ARG_SIZE) return PARSE_INVALID;
    if (input_len - position < bytes + 2) return PARSE_INCOMPLETE;
    memcpy(output, string_start, bytes);
    output[bytes] = '\0';
    // Closing \r\n must come immediately after the payload
    const char *end = string_start + bytes;
    if (end[0] != '\r' || end[1] != '\n') return PARSE_INVALID;

    // Return total number of input characters consumed
    *consumed = position + bytes + 2;
    return PARSE_COMPLETE;
}

/* Parse one RESP command and report how many input bytes belong to it. */
ParseStatus parse_command(const char *input, size_t input_len, Command **result, size_t *consumed) {
    *result = NULL;
    *consumed = 0;
    if (input_len == 0) return PARSE_INCOMPLETE;
    if (input[0] != '*') return PARSE_INVALID;

    size_t position = 1;
    unsigned int argc = 0;
    if (position == input_len) return PARSE_INCOMPLETE;
    if (input[position] < '0' || input[position] > '9') return PARSE_INVALID;

    while (position < input_len && input[position] >= '0' && input[position] <= '9') {
        argc = argc * 10 + (unsigned int)(input[position] - '0');
        if (argc > MAX_ARGS) return PARSE_INVALID;
        position++;
    }

    if (position == input_len) return PARSE_INCOMPLETE;
    if (input[position] != '\r') return PARSE_INVALID;
    if (position + 1 >= input_len) return PARSE_INCOMPLETE;
    if (input[position + 1] != '\n') return PARSE_INVALID;
    position += 2;

    // Populate structure
    Command *command = malloc(sizeof *command);
    if (command == NULL) return PARSE_INVALID;
    command->argc = argc;
    for (unsigned int i = 0; i < argc; i++) {
        size_t argument_bytes = 0;
        ParseStatus status = parse_bulk_string(
            input + position,
            input_len - position,
            command->argv[i],
            &argument_bytes
        );
        if (status != PARSE_COMPLETE) {
            free(command);
            return status;
        }
        position += argument_bytes;
    }

    *result = command;
    *consumed = position;
    return PARSE_COMPLETE;
}

/* send() may write only part of a response, so keep going until all bytes are sent. */
int send_all(int fd, const char *buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(fd, buffer + sent, length - sent, 0);
        if (result == -1 && errno == EINTR) continue;
        if (result <= 0) return 0;
        sent += (size_t)result;
    }
    return 1;
}

/* Serialize string into a RESP bulk string from values. Writes into provided output. */
void serialize_bulk_string(const char *str, size_t str_len, char output[]) {
    // Calculate memory needed for header
    int header_len = snprintf(NULL, 0, "$%zu\r\n", str_len);
    // Actually write the header
    snprintf(output, header_len + 1, "$%zu\r\n", str_len);
    // Copy the actual data
    memcpy(output + header_len, str, str_len);
    // Add final \r\n\0
    memcpy(output + header_len + str_len, "\r\n", 2);
    output[header_len + str_len + 2] = '\0';
}

/* Serialize a missing value as a RESP null bulk string. */
void serialize_null_bulk_string(char output[]) {
    memcpy(output, "$-1\r\n", 6);
}

/* Serialize string into a RESP simple string from values. Writes into provided output. */
void serialize_simple_string(const char *str, size_t str_len, char output[]) {
    // '+' + data + "\r\n" + '\0'
    output[0] = '+';
    memcpy(output + 1, str, str_len);
    memcpy(output + 1 + str_len, "\r\n", 2);
    output[1 + str_len + 2] = '\0';
}

/* Serialize string into a RESP error. Writes into provided output. */
void serialize_error(const char *str, size_t str_len, char output[]) {
    // '-' + error message + "\r\n\" + '0'
    output[0] = '-';
    memcpy(output + 1, str, str_len);
    memcpy(output + 1 + str_len, "\r\n", 2);
    output[1 + str_len + 2] = '\0';
}

/* Receive an initialized database, an already parsed command and an output buffer.
Writes into the provided output the relevant RESP response output. */
void handle_command(Database *db, Command *command, char response[]) {
    char *error_msg = "ERR no arguments provided";
    if (command->argc == 0) {
        serialize_error(error_msg, strlen(error_msg), response);
        return;
    }
    // Execute command
    if (strcmp(command->argv[0], "PING") == 0 && command->argc == 1) { // No optional argv allowed yet 
        serialize_simple_string("PONG", 4, response);
    } else if (strcmp(command->argv[0], "ECHO") == 0 && command->argc == 2) { // Redis accepts just 1 argv for the ECHO command
        serialize_bulk_string(command->argv[1], strlen(command->argv[1]), response);
    } else if (strcmp(command->argv[0], "SET") == 0 && command->argc == 3) {
        int is_set = db_set(db, command->argv[1], command->argv[2]);
        error_msg = "ERR failed to set key into db";
        if (!is_set) {
            serialize_error(error_msg, strlen(error_msg), response);
            return;
        }
        serialize_simple_string("OK", 2, response);
    } else if (strcmp(command->argv[0], "GET") == 0 && command->argc == 2) {
        char *value = db_get(db, command->argv[1]);
        if (value == NULL) {
            serialize_null_bulk_string(response);
            return;
        }
        serialize_bulk_string(value, strlen(value), response);
    } else {
        error_msg = "ERR unknown command or wrong arguments";
        serialize_error(error_msg, strlen(error_msg), response);
    }
}

/* Initialize the database by setting its size to 0 and all entries to NULL. Does not allocate memory. */
void db_init(Database *db) {
    db->size = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        db->entries[i] = NULL;
    }
}

/* Search through db->entries for a given key. Returns the key's index if found, -1 otherwise. */
int db_find(const Database *db, const char *key) {
    for (int i = 0; i < db->size; i++) {
        if (strcmp(db->entries[i]->key, key) == 0) return i;
    }
    return -1;
}

/* Add (or update) the provided key-value pair in the database. Returns 1 if valid, 0 otherwise. */
int db_set(Database *db, char *key, char *value) {
    int index = db_find(db, key);

    if (index == -1) { // Key not found, add it
        if (db->size >= MAX_ENTRIES) return 0;
        DatabaseEntry *entry = malloc(sizeof *entry);
        if (entry == NULL) return 0;

        entry->key = malloc(strlen(key) + 1);
        if (entry->key == NULL) {
            free(entry);
            return 0;
        }

        entry->value = malloc(strlen(value) + 1);
        if (entry->value == NULL) {
            free(entry->key);
            free(entry);
            return 0;
        }

        strcpy(entry->key, key); // could have used strdup()
        strcpy(entry->value, value);

        db->entries[db->size] = entry;
        db->size++;
    } else { // Key was already present, update value only
        size_t required = strlen(value) + 1;
        char *new_value = realloc(db->entries[index]->value, required);
        if (new_value == NULL) return 0;  // db->entries[index]->value remains valid
        db->entries[index]->value = new_value;
        memcpy(db->entries[index]->value, value, required);
        // Allocate-copy-free alternative:
        // char *new_value = malloc(strlen(value) + 1);
        // if (new_value == NULL) return 0;
        // strcpy(new_value, value);          // Database-owned copy
        // free(db->entries[index]->value);   // Release old allocation
        // db->entries[index]->value = new_value;
    }

    return 1;
}

/* Search the database for a given key. Returns pointer to the value if key is found. */
char *db_get(Database *db, char *key) {
    int index = db_find(db, key);
    if (index == -1) return NULL;
    return db->entries[index]->value;
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

    Database *db = malloc(sizeof *db);
    if (db == NULL) return -1;
    db_init(db);

    // Main server loop
    while (1) {

        // accept, waits until a client connects
        client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        printf("Client connected\n");

        // TCP is a byte stream: accumulate bytes until complete commands exist.
        char input_buffer[BUFFER_SIZE] = {0};
        char output_buffer[BUFFER_SIZE] = {0};
        size_t buffered = 0;
        int close_client = 0;

        while (!close_client) {
            ssize_t bytes_received = recv(
                client_fd,
                input_buffer + buffered,
                sizeof(input_buffer) - buffered,
                0
            );

            if (bytes_received == -1 && errno == EINTR) continue;
            if (bytes_received == -1) {
                perror("recv");
                break;
            }
            if (bytes_received == 0) {
                printf("Client disconnected\n");
                break;
            }

            buffered += (size_t)bytes_received;
            size_t processed = 0;

            while (processed < buffered) {
                Command *command = NULL;
                size_t consumed = 0;
                ParseStatus status = parse_command(
                    input_buffer + processed,
                    buffered - processed,
                    &command,
                    &consumed
                );

                if (status == PARSE_INCOMPLETE) break;
                if (status == PARSE_INVALID) {
                    const char *error = "-ERR invalid RESP command\r\n";
                    send_all(client_fd, error, strlen(error));
                    close_client = 1;
                    break;
                }

                handle_command(db, command, output_buffer);
                free(command);
                if (!send_all(client_fd, output_buffer, strlen(output_buffer))) {
                    close_client = 1;
                    break;
                }
                processed += consumed;
            }

            if (processed > 0) {
                memmove(input_buffer, input_buffer + processed, buffered - processed);
                buffered -= processed;
            }

            if (buffered == sizeof(input_buffer)) {
                const char *error = "-ERR command too large\r\n";
                send_all(client_fd, error, strlen(error));
                close_client = 1;
            }
        }

        // close the CLIENT socket
        close(client_fd);
    }

    close(server_fd);

    return 0;
}
