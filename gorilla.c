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
} RedisCommand;

typedef struct {
    char *key;
    char *value;
} DatabaseEntry;

typedef struct {
    DatabaseEntry *entries[MAX_ENTRIES];
    int size;
} Database;


int parse_simple_string(const char *input, char output[]); // Unused but keep it for now
int parse_bulk_string(const char *input, char output[]);
RedisCommand *parse_redis_command(const char *input);
void handle_command(int client_fd, RedisCommand *command);
void print_string_bytes(const char *s); // Just for debugging
char *serialize_bulk_string(const char *str, size_t size);
char *serialize_simple_string(const char *str, size_t size);
char *serialize_error(const char *str, size_t size);
void db_init(Database *db);
int db_find(const Database *db, const char *key);
void db_set(Database *db, char *key, char *value);
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
RedisCommand *parse_redis_command(const char *input) {
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
    RedisCommand *redis_command = malloc(sizeof(RedisCommand));
    if (redis_command == NULL) return NULL;
    redis_command->argc = argc;
    int bulk_parsed_bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (*command_ptr == '\0') {
            free(redis_command);
            return NULL;
        }
        // find a '$'
        if (*command_ptr != '$') {
            free(redis_command);
            return NULL;
        }
        // call parse_bulk_string() & store the result in argv[i]
        bulk_parsed_bytes = parse_bulk_string(command_ptr, redis_command->argv[i]);
        if (bulk_parsed_bytes == 0) {
            free(redis_command);
            return NULL;
        }
        // move ptr forward by the number of characters + \r\n twice + the $byte itself
        command_ptr += bulk_parsed_bytes;
    }

    return redis_command;
}

/* Serialize string into a RESP bulk string from values. Returns a pointer to a character buffer. */
char *serialize_bulk_string(const char *str, size_t size) {
    // Calculate memory needed for header
    int header_len = snprintf(NULL, 0, "$%zu\r\n", size);
    // Allocate memory for the entire RESP string (final \r\n and \0 included)
    char *result = malloc(header_len + size + 2 + 1);
    if (result == NULL) return NULL;
    // Actually write the header
    snprintf(result, header_len + 1, "$%zu\r\n", size);
    // Copy the actual data
    memcpy(result + header_len, str, size);
    // Add final \r\n\0
    memcpy(result + header_len + size, "\r\n", 2);
    result[header_len + size + 2] = '\0';

    // Return the pointer to the dynamically allocated memory for the command
    return result;
}

/* Serialize string into a RESP simple string from values. Returns a pointer to a character buffer. */
char *serialize_simple_string(const char *str, size_t size) {
    // '+' + data + "\r\n" + '\0'
    char *result = malloc(1 + size + 2 + 1);
    if (result == NULL) return NULL;

    result[0] = '+';
    memcpy(result + 1, str, size);
    memcpy(result + 1 + size, "\r\n", 2);
    result[1 + size + 2] = '\0';

    return result;
}

/* Serialize string into a RESP error. Returns a pointer to a character buffer. */
char *serialize_error(const char *str, size_t size) {
    // '-' + error message + "\r\n\" + '0'
    char *result = malloc(1 + size + 2 + 1);
    if (result == NULL) return NULL;

    result[0] = '-';
    memcpy(result + 1, str, size);
    memcpy(result + 1 + size, "\r\n", 2);
    result[1 + size + 2] = '\0';

    return result;
}

/* Receive an already parsed command. Sends to the server the relevant RESP bulk string. */
void handle_command(int client_fd, RedisCommand *command) {
    if (command->argc == 0) return; 
    char *response = NULL;
    
    // Parse first argv
    if (strcmp(command->argv[0], "PING") == 0 && command->argc == 1) { // No optional argv allowed yet 
        response = serialize_simple_string("PONG", 4);
    } else if (strcmp(command->argv[0], "ECHO") == 0 && command->argc == 2) { // Redis accepts just 1 argv for the ECHO command
        // Send bulk string
        response = serialize_bulk_string(command->argv[1], strlen(command->argv[1]));
    } else {
        const char *error_msg = "ERR unknown command or wrong arguments";
        response = serialize_error(error_msg, strlen(error_msg));
    }

    if (response == NULL) {
        perror("malloc");
        return;
    }

    ssize_t bytes_sent = send(client_fd, response, strlen(response), 0);

    if (bytes_sent == -1) {
        perror("send");
    }

    free(response);
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

/* Add (or update) the provided key-value pair in the database. */
void db_set(Database *db, char *key, char *value) {
    int index = db_find(db, key);
    // Key not found, add it
    if (index == -1) {
        db->entries[db->size] = malloc(sizeof(DatabaseEntry));
        db->entries[db->size]->key = key;
        db->entries[db->size]->value = value;
        db->size++;
    } else { // Key was already present, update value only
        db->entries[index]->value = value;
    }
}

/* Search the database for a given key. Returns pointer to the value if key is found. */
char *db_get(Database *db, char *key) {
    int index = db_find(db, key);
    if (index == -1) return NULL;
    return db->entries[index]->value;
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

        ssize_t bytes_received = recv(
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
        if (cmd != NULL) {
            handle_command(client_fd, cmd);
            free(cmd);
        }

        // close the CLIENT socket
        close(client_fd);
    }

    close(server_fd);

    return 0;
}
