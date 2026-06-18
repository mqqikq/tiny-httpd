#!/usr/bin/env python3
"""Integration tests for the httpd binary.

Spins up a real server process against a throwaway document root and talks
to it over a TCP socket, exercising the parts unit tests can't reach: actual
accept()/read()/write() behavior, response headers, and -- using a raw
socket so the client can't "fix up" a malicious request the way curl or
Python's http.client normalize dot-segments -- the path-traversal guard as
seen by a real attacker.

Usage: integration_test.py [path-to-httpd-binary]
"""
import http.client
import os
import shutil
import socket
import subprocess
import sys
import tempfile
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


def run_tests(host, port):
    print("test_get_index")
    conn = http.client.HTTPConnection(host, port, timeout=2)
    conn.request("GET", "/")
    resp = conn.getresponse()
    body = resp.read()
    check(resp.status == 200, "GET / returns 200")
    check(resp.getheader("Content-Type", "").startswith("text/html"), "index.html has text/html Content-Type")
    check(resp.getheader("Content-Length") == str(len(body)), "Content-Length matches body length")
    check(resp.getheader("Connection") == "close", "Connection: close is present (week-1 baseline)")
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
    proc = None
    try:
        os.makedirs(docroot)
        with open(os.path.join(docroot, "index.html"), "w") as f:
            f.write("<h1>home</h1>")
        os.makedirs(os.path.join(docroot, "sub"))
        with open(os.path.join(docroot, "sub", "page.txt"), "w") as f:
            f.write("hello from sub")
        with open(os.path.join(docroot, "style.css"), "w") as f:
            f.write("body { color: red; }")
        with open(os.path.join(workspace, "secret.txt"), "w") as f:
            f.write("should never be served")

        port = find_free_port()
        proc = subprocess.Popen(
            [binary, "-p", str(port), "-r", docroot],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

        if not wait_for_port("127.0.0.1", port):
            print("error: server did not start listening in time")
            return 1

        run_tests("127.0.0.1", port)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
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
