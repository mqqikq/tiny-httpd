# tiny-httpd

[![CI](https://github.com/mqqikq/tiny-httpd/actions/workflows/ci.yml/badge.svg)](https://github.com/mqqikq/tiny-httpd/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A static-file HTTP/1.1 server written from scratch in C. No libevent, no
libuv, no third-party HTTP library — just POSIX sockets, hand-rolled request
parsing, and an `epoll` event loop driving non-blocking I/O.

> **Status: Week 2.** Non-blocking sockets, a single-threaded `epoll` event
> loop, HTTP keep-alive, `sendfile()` zero-copy file streaming, idle-connection
> timeouts, `SO_REUSEPORT` multi-worker processes, and an access log. Week 1's
> blocking baseline is preserved in git history; week 3 (`/metrics`, Docker,
> `wrk` benchmarks) is next — see the [roadmap](#roadmap).

## Why this exists

This is a portfolio project demonstrating the layer most web frameworks hide:
how an HTTP server actually parses bytes off a socket, decides what file to
serve, and writes a correct response back — including the parts that are
easy to get wrong (partial reads/writes, percent-encoding, symlink escapes,
`SIGPIPE`, signal-safe shutdown).

## Features

- Real HTTP/1.1 request-line and header parsing (no `sscanf("%s %s %s")` shortcuts)
- `GET` and `HEAD`, correct status codes: `200`, `400`, `403`, `404`, `405`
- MIME type detection by extension (20+ types)
- Directory requests resolve to `index.html`
- **Security-hardened static file serving**:
  - percent-decoding happens *before* traversal checks (not after)
  - `..` segments are rejected even when percent-encoded (`%2e%2e`)
  - the resolved path is canonicalized with `realpath()` and verified to
    stay inside the document root — so a symlink planted inside the doc
    root can't be used to read files outside it either
  - embedded NUL bytes (`%00`) are rejected
- Correct `Content-Length` / `Content-Type` / `Date` / `Server` headers
- **Non-blocking I/O on a single-threaded `epoll` event loop** — one slow or
  half-sent request never stalls any other connection (see the test that
  proves it: `test_slow_client_does_not_block_other_connections`)
- **HTTP keep-alive**, spec-correct by default: HTTP/1.1 connections stay
  open unless the client sends `Connection: close`; HTTP/1.0 connections
  close unless the client opts in with `Connection: keep-alive`
- **`sendfile()` zero-copy** file bodies — the kernel copies file bytes
  straight to the socket, never through a userspace buffer
- **Idle-connection timeouts** (`-t`, default 30s) so a keep-alive client
  that goes silent doesn't hold a file descriptor forever
- **`SO_REUSEPORT` multi-worker processes** (`-w N`) — N independent
  processes each bind the same port; the kernel load-balances connections
  across them, with a supervisor process forwarding graceful shutdown
- **Access log** (combined-log-ish, to stdout) for every completed request:
  client IP, request line, status, response size
- Graceful shutdown on `SIGINT`/`SIGTERM` — single-worker or multi-worker,
  no orphaned sockets or zombie processes
- Clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`,
  AddressSanitizer, UndefinedBehaviorSanitizer, and valgrind (memory **and**
  file-descriptor leak checks across plain, keep-alive, and idle-timeout
  connections — see [CI](.github/workflows/ci.yml))

## Quick start

```bash
# Linux or WSL -- epoll, accept4, and SO_REUSEPORT are Linux-only
make
./build/httpd -p 8080 -r www
curl http://localhost:8080/
```

```
Usage: ./build/httpd [-p port] [-r doc_root] [-t idle_timeout] [-w workers]
  -p, --port      TCP port to listen on (default 8080)
  -r, --root      directory to serve files from (default www)
  -t, --timeout   idle keep-alive timeout in seconds (default 30)
  -w, --workers   worker processes sharing the port via SO_REUSEPORT (default 1)
  -v, --verbose   log connection state transitions to stderr
  -h, --help      show this help
```

## Testing

```bash
make unit-test   # parser + path-resolution unit tests (C, no framework)
make test        # unit tests + end-to-end integration tests (Python, stdlib only)
make asan        # rebuild with AddressSanitizer + UndefinedBehaviorSanitizer
```

The integration suite (`tests/integration_test.py`) starts a real server
process against a throwaway document root and talks to it over raw TCP
sockets — including sending an *unnormalized* `GET /../secret.txt` that a
well-behaved HTTP client like `curl` would silently rewrite before sending,
so the test actually exercises the traversal guard the way an attacker
would hit it.

## How request resolution works

```mermaid
flowchart TD
    A[accept connection] --> B[read until full request head]
    B -->|malformed / too large| E1[400 Bad Request]
    B -->|method not GET/HEAD| E2[405 Method Not Allowed]
    B --> C[percent-decode request-target]
    C -->|bad %-escape or embedded NUL| E1
    C --> D[join with doc_root, realpath canonicalize]
    D -->|outside doc_root or symlink escape| E3[403 Forbidden]
    D -->|does not exist| E4[404 Not Found]
    D -->|is a directory with index.html| F[serve index.html]
    D -->|is a regular file| F
    F --> G[200 OK + file bytes]
```

The key design decision: **percent-decoding happens before any traversal
check**, and the final check is "does the canonical, symlink-resolved path
still live under the canonical document root" — not a string match on `..`.
String-matching `..` in the *raw* request-target is the classic mistake that
encoded traversal (`%2e%2e`) and symlinks both defeat.

## How the event loop works

A single `epoll` instance per worker process drives every connection through
an explicit state machine — there is exactly one thread, and it never calls
a blocking `read()`/`write()`/`accept()`:

```mermaid
flowchart TD
    L[listen_fd EPOLLIN] -->|accept4, loop until EAGAIN| N[new connection_t, EPOLLIN]
    N --> R[CONN_STATE_READING]
    R -->|EPOLLIN: read more, try to parse| R
    R -->|full request parsed or error| W0[build response into wbuf / open file_fd]
    W0 --> W[CONN_STATE_WRITING, switch to EPOLLOUT]
    W -->|EPOLLOUT: drain wbuf, then sendfile file_fd| W
    W -->|fully sent, keep-alive| R
    W -->|fully sent, Connection: close| X[close fd, free connection_t]
    R -->|EAGAIN on read, peer closed, or error| X
```

Every transition is driven by what `epoll_wait()` reports as ready, so a
connection that's mid-read (a slow client trickling in a request byte by
byte) holds no thread hostage — the loop simply moves on to whichever other
fd is ready next. A 1-second `epoll_wait` timeout also drives a periodic
sweep that closes any connection idle longer than `-t` seconds. `-w N`
runs N of these loops in independent processes, each with its own socket
bound via `SO_REUSEPORT`; the kernel hands each new TCP connection to one
of them.

## Architecture

| File | Responsibility |
|---|---|
| `src/main.c` | CLI args, signal handling, the `epoll_wait` dispatch loop, `-w` multi-worker fork/supervise |
| `src/event_loop.c` | thin `epoll` wrapper: create, add/mod/del fd interest, wait |
| `src/server.c` | listening socket setup (`socket`/`bind`/`listen`, `SO_REUSEADDR`, `SO_REUSEPORT`) |
| `src/connection.c` | per-connection state machine: non-blocking read → parse → build response → non-blocking write/`sendfile` → keep-alive or close; access log |
| `src/http_parser.c` | request-line + header parsing, no body handling |
| `src/files.c` | percent-decoding + symlink-safe path resolution |
| `src/http_response.c` | pure header/body formatting (no I/O — the connection state machine owns all reads/writes) |
| `src/mime.c` | extension → `Content-Type` lookup table |

## Known limitations

- No HTTP pipelining: bytes received after one complete request (before its
  response is sent) are discarded. No mainstream browser pipelines over
  plain HTTP/1.1 anyway, but a client that does will see that request hang
  until the idle timeout.
- No chunked transfer-encoding or request bodies — this server only ever
  serves static files in response to `GET`/`HEAD`, so it never needs to read
  one.
- IPv4 only.

## Roadmap

- [x] **Week 1** — blocking sockets, parser, static files, MIME, path-traversal
      hardening, HEAD, `index.html`, unit + integration tests, CI
- [x] **Week 2** — `epoll` event loop + non-blocking I/O, keep-alive,
      `sendfile()` zero-copy, idle timeouts, `SO_REUSEPORT` multi-worker
      processes, access log
- [ ] **Week 3** — `/metrics` + live `/dashboard` (SSE), Docker one-command
      demo, `wrk` benchmark table, demo GIF
- [ ] Stretch — range requests, gzip, mini reverse-proxy

## License

[MIT](LICENSE)
