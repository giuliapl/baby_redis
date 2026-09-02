# Baby Redis

Baby Redis is a deliberately small, single-threaded Redis-compatible server
written to explore RESP parsing, TCP stream framing, memory ownership, and
in-memory storage in C. It listens on `127.0.0.1:6379`, parses commands using
the Redis Serialization Protocol (RESP), and stores key/value pairs in memory.

The main implementation in `server.c` supports `PING`, `ECHO`, `SET`, and
`GET`. The simpler `nc_server.c` is an earlier TCP-server prototype.

## Run it

```sh
gcc -Wall -Wextra -o baby-redis server.c
./baby-redis
```

In another terminal, connect with the official `redis-cli` (after having installed it):

```sh
redis-cli PING
redis-cli SET greeting hello
redis-cli GET greeting
```

## Limitations

Baby Redis is an educational implementation rather than a production-ready
server. It handles one client at a time, uses fixed-size command and database
limits, supports text-only keys and values, and does not persist data. All data
is lost when the server stops.
