#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make clean >/dev/null
make >/dev/null

PORT=8099
valgrind --error-exitcode=99 --leak-check=full --track-fds=yes \
  ./build/httpd -p "$PORT" -r www -t 2 &
VPID=$!
sleep 1

# Plain sequential requests.
for i in $(seq 1 5); do curl -sS -o /dev/null "http://127.0.0.1:$PORT/"; done

# Keep-alive: several requests over one connection, then close.
python3 - "$PORT" <<'EOF'
import socket, sys
port = int(sys.argv[1])
with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
    for path in ("/", "/about.html", "/css/style.css", "/", "/does-not-exist"):
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        head, _, rest = buf.partition(b"\r\n\r\n")
        cl = 0
        for line in head.split(b"\r\n")[1:]:
            if line.lower().startswith(b"content-length:"):
                cl = int(line.split(b":", 1)[1].strip())
        while len(rest) < cl:
            rest += s.recv(4096)
EOF

# Idle keep-alive connection left to time out server-side.
python3 - "$PORT" <<'EOF'
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=2)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
time.sleep(3)
s.close()
EOF

kill -INT "$VPID"
wait "$VPID"
