# Baby Redis

Baby Redis is a small Redis-like server written in C as a learning project. It
listens on `127.0.0.1:6379`, parses commands using the Redis Serialization
Protocol (RESP), and stores key/value pairs in memory.

The main implementation in `gorilla.c` supports `PING`, `ECHO`, `SET`, and
`GET`. The simpler `nc_server.c` is an earlier TCP-server prototype.

## Run it

```sh
gcc -Wall -Wextra -o baby-redis gorilla.c
./baby-redis
```

In another terminal, connect with the official `redis-cli` (after having installed it):

```sh
redis-cli PING
redis-cli SET greeting hello
redis-cli GET greeting
```

Data is kept only in memory and is lost when the server stops.
Concurrent clients are not supported.
