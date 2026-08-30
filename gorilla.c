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


int parse_simple_string(const char *input, char output[]);
int parse_bulk_string(const char *input, char output[]);
Command *parse_command(const char *input);
void handle_command(Database *db, Command *command, char response[]);
void print_string_bytes(const char *s); // Just for debugging
void serialize_bulk_string(const char *str, size_t str_len, char output[]);
void serialize_simple_string(const char *str, size_t str_len, char output[]);
void serialize_error(const char *str, size_t str_len, char output[]);
void db_init(Database *db);
int db_find(const Database *db, const char *key); // Helper method
int db_set(Database *db, char *key, char *value);
char *db_get(Database *db, char *key);

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
    if (bytes >= MAX_ARG_SIZE) return 0; // prevent buffer overflow
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
Command *parse_command(const char *input) {
    if (*input != '*') return 0;
    // Parse the number of expected argv & check it is followed by \r\n
    char *argc_end_ptr;
    unsigned int argc = strtoul(input + 1, &argc_end_ptr, 10);
    if (argc_end_ptr[0] != '\r' || argc_end_ptr[1] != '\n') return 0;
    if (argc > MAX_ARGS) return 0;
    char *command_ptr = argc_end_ptr;
    // Move past the array header
    command_ptr += 2;
    // Populate structure
    Command *command = malloc(sizeof *command);
    if (command == NULL) return NULL;
    command->argc = argc;
    int bulk_parsed_bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (*command_ptr == '\0') {
            free(command);
            return NULL;
        }
        // find a '$'
        if (*command_ptr != '$') {
            free(command);
            return NULL;
        }
        // call parse_bulk_string() & store the result in argv[i]
        bulk_parsed_bytes = parse_bulk_string(command_ptr, command->argv[i]);
        if (bulk_parsed_bytes == 0) {
            free(command);
            return NULL;
        }
        // move ptr forward by the number of characters + \r\n twice + the $byte itself
        command_ptr += bulk_parsed_bytes;
    }

    return command;
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
        error_msg = "1";
        if (value == NULL) {
            serialize_error(error_msg, strlen(error_msg), response);
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
        db->entries[index]->value = value;
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

        // recv, reads bytes sent by the client
        char input_buffer[BUFFER_SIZE] = {0};
        char output_buffer[BUFFER_SIZE] = {0};

        ssize_t bytes_received = recv(
            client_fd,
            input_buffer,
            sizeof(input_buffer) - 1,
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

        input_buffer[bytes_received] = '\0';
        
        printf("Received %d bytes:\n", bytes_received);
        print_string_bytes(input_buffer);

        // send
        Command *command = parse_command(input_buffer);

        if (command != NULL) {
            handle_command(db, command, output_buffer);
            free(command);
        }

        ssize_t bytes_sent = send(client_fd, output_buffer, strlen(output_buffer), 0);

        if (bytes_sent == -1) {
            perror("send");
        }

        // close the CLIENT socket
        close(client_fd);
    }

    close(server_fd);

    return 0;
}
