#!/usr/bin/env python3
"""Integration tests for the httpd binary.

Spins up real server processes against a throwaway document root and talks
to them over TCP sockets, exercising the parts unit tests can't reach: actual
accept()/epoll/read()/write() behavior, response headers, keep-alive across
multiple requests on one connection, true concurrency (a slow, half-sent
request must not stall other connections), idle-connection timeouts, and --
using a raw socket so the client can't "fix up" a malicious request the way
curl or Python's http.client normalize dot-segments -- the path-traversal
guard as seen by a real attacker.

Usage: integration_test.py [path-to-httpd-binary]
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

FAILURES = []


def check(cond, msg):
    status = "PASS" if cond else "FAIL"
    print(f"  {status}: {msg}")
    if not cond:
        FAILURES.append(msg)


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def raw_request(host, port, raw_bytes, timeout=2.0):
    """Sends raw bytes over a fresh TCP connection and returns whatever the
    server writes back before closing (or until timeout)."""
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(raw_bytes)
        s.settimeout(timeout)
        chunks = []
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        except socket.timeout:
            pass
        return b"".join(chunks)


def status_line(response_bytes):
    return response_bytes.split(b"\r\n", 1)[0].decode("latin-1")


def recv_one_response(sock, timeout=2.0):
    """Reads exactly one HTTP response off an already-connected socket,
    using Content-Length to know where it ends -- the connection itself may
    stay open (keep-alive), so EOF can't be used as the end-of-response
    signal the way raw_request() uses it."""
    sock.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("connection closed before headers completed")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = lines[0].decode("latin-1")
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, _, v = line.partition(b":")
            headers[k.strip().lower().decode("latin-1")] = v.strip().decode("latin-1")
    content_length = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk
    return status, headers, body


def run_tests(host, port):
    print("test_get_index")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/")
    resp = conn.getresponse()
    body = resp.read()
    check(resp.status == 200, "GET / returns 200")
    check(resp.getheader("Content-Type", "").startswith("text/html"), "index.html has text/html Content-Type")
    check(resp.getheader("Content-Length") == str(len(body)), "Content-Length matches body length")
    check(resp.getheader("Connection") == "keep-alive", "Connection: keep-alive is the HTTP/1.1 default (week 2)")
    conn.close()

    print("test_get_subdirectory_file")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/sub/page.txt")
    resp = conn.getresponse()
    body = resp.read()
    check(resp.status == 200, "GET /sub/page.txt returns 200")
    check(body == b"hello from sub", "subdirectory file content is correct")
    conn.close()

    print("test_mime_type_css")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/style.css")
    resp = conn.getresponse()
    resp.read()
    check(resp.getheader("Content-Type", "").startswith("text/css"), "style.css has text/css Content-Type")
    conn.close()

    print("test_404")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/does-not-exist.html")
    resp = conn.getresponse()
    resp.read()
    check(resp.status == 404, "missing file returns 404")
    conn.close()

    print("test_405")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("POST", "/")
    resp = conn.getresponse()
    resp.read()
    check(resp.status == 405, "POST returns 405 (only GET/HEAD are supported)")
    conn.close()

    print("test_head_matches_get_headers_with_no_body")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("HEAD", "/")
    resp = conn.getresponse()
    body = resp.read()
    check(resp.status == 200, "HEAD / returns 200")
    check(body == b"", "HEAD response has no body")
    check(resp.getheader("Content-Length") is not None, "HEAD response still reports Content-Length")
    conn.close()

    print("test_path_traversal_blocked_raw_socket")
    raw = b"GET /../secret.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
    resp_bytes = raw_request(host, port, raw)
    line = status_line(resp_bytes)
    check(" 403 " in line or " 404 " in line, f"unnormalized '../secret.txt' is rejected (got: {line!r})")
    check(b"should never be served" not in resp_bytes, "secret file content is never returned")

    print("test_path_traversal_encoded_blocked")
    raw = b"GET /%2e%2e/secret.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
    resp_bytes = raw_request(host, port, raw)
    check(b"should never be served" not in resp_bytes, "percent-encoded traversal never returns secret content")

    print("test_malformed_request_line")
    raw = b"NOTAVALIDREQUESTLINE\r\n\r\n"
    resp_bytes = raw_request(host, port, raw)
    line = status_line(resp_bytes)
    check(" 400 " in line, f"malformed request line returns 400 (got: {line!r})")

    print("test_bad_http_version_rejected")
    raw = b"GET / HTTP/9.9\r\nHost: x\r\n\r\n"
    resp_bytes = raw_request(host, port, raw)
    line = status_line(resp_bytes)
    check(" 400 " in line, f"unsupported HTTP version returns 400 (got: {line!r})")

    print("test_concurrent_sequential_connections_dont_wedge_server")
    ok = True
    for _ in range(20):
        conn = http.client.HTTPConnection(host, port, timeout=2)
        conn.request("GET", "/")
        resp = conn.getresponse()
        resp.read()
        if resp.status != 200:
            ok = False
        conn.close()
    check(ok, "20 sequential connections all succeed (no fd/state leak between requests)")

    print("test_keep_alive_reuses_connection")
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        status1, headers1, _ = recv_one_response(s)
        check(" 200 " in status1, "first request on a keep-alive connection returns 200")
        check(headers1.get("connection") == "keep-alive", "first response advertises keep-alive")

        s.sendall(b"GET /sub/page.txt HTTP/1.1\r\nHost: x\r\n\r\n")
        status2, _, body2 = recv_one_response(s)
        check(" 200 " in status2, "second request on the SAME connection also returns 200")
        check(body2 == b"hello from sub", "second response body is correct (state reset between requests)")

    print("test_connection_close_header_closes_socket")
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        _, headers, _ = recv_one_response(s)
        check(headers.get("connection") == "close", "response honors an explicit Connection: close")
        s.settimeout(2)
        trailing = s.recv(4096)
        check(trailing == b"", "server actually closes the socket after Connection: close")

    print("test_http10_defaults_to_connection_close")
    raw = b"GET / HTTP/1.0\r\nHost: x\r\n\r\n"
    resp_bytes = raw_request(host, port, raw)
    line = status_line(resp_bytes)
    check(" 200 " in line, "HTTP/1.0 request without a Connection header still succeeds")
    check(b"Connection: keep-alive" not in resp_bytes, "HTTP/1.0 without explicit keep-alive is not kept alive")

    print("test_metrics_endpoint")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/")
    conn.getresponse().read()
    conn.close()
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/metrics")
    resp = conn.getresponse()
    body = resp.read()
    conn.close()
    check(resp.status == 200, "GET /metrics returns 200")
    check(resp.getheader("Content-Type", "").startswith("application/json"),
          "/metrics has application/json Content-Type")
    metrics = json.loads(body)
    check(metrics.get("requests_total", 0) >= 1, "metrics reflect at least the prior GET /")
    check("connections_active" in metrics and "uptime_sec" in metrics and "pid" in metrics,
          "metrics JSON has the expected fields")

    print("test_dashboard_serves_html")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/dashboard")
    resp = conn.getresponse()
    body = resp.read()
    conn.close()
    check(resp.status == 200, "GET /dashboard returns 200")
    check(resp.getheader("Content-Type", "").startswith("text/html"), "/dashboard has text/html Content-Type")
    check(b"metrics/stream" in body, "dashboard page wires up the SSE endpoint")

    print("test_metrics_stream_sse")
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(b"GET /metrics/stream HTTP/1.1\r\nHost: x\r\n\r\n")
        s.settimeout(3)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed before SSE headers completed")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        check(b"200 OK" in head, "SSE stream responds 200 OK")
        check(b"Content-Type: text/event-stream" in head, "SSE stream sets text/event-stream Content-Type")
        check(b"Content-Length" not in head, "SSE stream has no Content-Length (indefinite body)")

        # The server pushes one frame per second; wait for at least one.
        while b"\n\n" not in rest:
            chunk = s.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed before any SSE frame arrived")
            rest += chunk
        frame, _, _ = rest.partition(b"\n\n")
        check(frame.startswith(b"data: "), "SSE frame starts with 'data: '")
        payload = json.loads(frame[len(b"data: "):])
        check("requests_total" in payload, "SSE frame payload is the same metrics JSON shape")

    print("test_slow_client_does_not_block_other_connections")
    results = {}

    def slow_client():
        try:
            with socket.create_connection((host, port), timeout=5) as s:
                start = time.time()
                # Trickle the request in slowly (well within the server's
                # idle timeout) to prove the epoll loop doesn't stall other
                # connections behind a half-sent one -- a single-threaded
                # blocking-accept-loop server could never pass this.
                for part in (b"GET ", b"/ ", b"HTTP/1.1\r\n", b"Host: x\r\n", b"\r\n"):
                    s.sendall(part)
                    time.sleep(0.3)
                status, _, _ = recv_one_response(s)
                results["slow_status"] = status
                results["slow_elapsed"] = time.time() - start
        except Exception as e:  # pragma: no cover - failure surfaced via check()
            results["slow_error"] = str(e)

    t = threading.Thread(target=slow_client)
    t.start()
    time.sleep(0.15)  # let the slow client connect and send its first fragment

    fast_start = time.time()
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/")
    resp = conn.getresponse()
    resp.read()
    fast_elapsed = time.time() - fast_start
    conn.close()
    t.join(timeout=5)

    check(resp.status == 200, "fast request succeeds while a slow client is mid-request")
    check(fast_elapsed < 1.0, f"fast request was not blocked behind the slow client (took {fast_elapsed:.2f}s)")
    check("slow_error" not in results, f"slow client did not error out: {results.get('slow_error')}")
    check(" 200 " in results.get("slow_status", ""), "the slow client's request eventually completes too")


def run_multiworker_test(binary, docroot):
    print("test_multiworker_reuseport")
    proc, port = start_server(binary, docroot, extra_args=["-w", "4"])
    try:
        ok = True
        for _ in range(30):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            conn.request("GET", "/")
            resp = conn.getresponse()
            resp.read()
            if resp.status != 200:
                ok = False
            conn.close()
        check(ok, "30 requests against a 4-worker (SO_REUSEPORT) server all succeed")
    finally:
        stop_start = time.time()
        stop_server(proc)
        stopped_in = time.time() - stop_start
        check(stopped_in < 3.0,
              f"multi-worker supervisor shuts down all workers promptly on SIGTERM (took {stopped_in:.2f}s)")


def run_timeout_test(host, port):
    print("test_idle_connection_is_closed_after_timeout")
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        status, headers, _ = recv_one_response(s)
        check(" 200 " in status, "initial request on the short-timeout server succeeds")
        check(headers.get("connection") == "keep-alive", "connection is kept alive initially")
        # Go idle past the server's 1-second --timeout and confirm it closes
        # the connection on its own instead of waiting forever.
        s.settimeout(3)
        trailing = s.recv(4096)
        check(trailing == b"", "server closes an idle keep-alive connection after the configured timeout")


def start_server(binary, docroot, extra_args=()):
    port = find_free_port()
    proc = subprocess.Popen(
        [binary, "-p", str(port), "-r", docroot, *extra_args],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if not wait_for_port("127.0.0.1", port):
        proc.terminate()
        raise RuntimeError("server did not start listening in time")
    return proc, port


def stop_server(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/httpd"
    binary = os.path.abspath(binary)
    if not os.path.isfile(binary):
        print(f"error: server binary not found at {binary}")
        return 1

    workspace = tempfile.mkdtemp(prefix="httpd_it_")
    # secret.txt lives one level *above* the document root, so a successful
    # "/../secret.txt" escape would actually reach real content -- a 404
    # that's merely missing-by-coincidence wouldn't prove the guard works.
    docroot = os.path.join(workspace, "root")
    main_proc = None
    timeout_proc = None
    try:
        os.makedirs(docroot)
        with open(os.path.join(docroot, "index.html"), "w") as f:
            f.write("<h1>home</h1>")
        os.makedirs(os.path.join(docroot, "sub"))
        with open(os.path.join(docroot, "sub", "page.txt"), "w") as f:
            f.write("hello from sub")
        with open(os.path.join(docroot, "style.css"), "w") as f:
            f.write("body { color: red; }")
        with open(os.path.join(docroot, "dashboard.html"), "w") as f:
            f.write("<!doctype html><title>dashboard</title>EventSource('/metrics/stream')")
        with open(os.path.join(workspace, "secret.txt"), "w") as f:
            f.write("should never be served")

        main_proc, port = start_server(binary, docroot)
        run_tests("127.0.0.1", port)

        timeout_proc, timeout_port = start_server(binary, docroot, extra_args=["-t", "1"])
        run_timeout_test("127.0.0.1", timeout_port)

        run_multiworker_test(binary, docroot)
    finally:
        stop_server(main_proc)
        stop_server(timeout_proc)
        shutil.rmtree(workspace, ignore_errors=True)

    if FAILURES:
        print(f"\n{len(FAILURES)} integration test(s) FAILED:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1

    print("\nAll integration tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
