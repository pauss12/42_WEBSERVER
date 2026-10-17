#!/usr/bin/env python3
"""Functional tester for webserv.

Usage (from the repository root, after `make`):

    python3 tst/tester.py              # all tests (~15 s)
    python3 tst/tester.py --slow       # also the idle-timeout test (+60 s)
    python3 tst/tester.py -k cgi       # only tests whose name contains "cgi"
    python3 tst/tester.py -v           # show details of passing tests too
    python3 tst/tester.py --tester42   # simulate the official 42 tester (add --big for 100 MB)

It starts its own ./webserv with a temporary config on ports 18480/18481, so it
never collides with a server you already have running. Temporary CGI scripts
and files are created under www/ and removed at the end.

Results:
    PASS     the server behaves as expected
    FAIL     a mandatory behaviour is broken (exit code 1)
    PENDING  a feature that is not implemented yet (does not affect exit code)
    CRASH    the server process died -> grade 0 in the evaluation
"""

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import httpclient as http  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 18480           # main server (change with --port)
PORT2 = PORT + 1       # second server (multi-port tests)
MAX_BODY = 3000000     # client_max_body_size of the main server
TAG = "_t%d" % os.getpid()   # unique prefix for every temporary file

GREEN, RED, YELLOW, GRAY, BOLD, NC = (
    "\033[32m", "\033[31m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")

# --------------------------------------------------------------------------------------
# Test fixtures: config, CGI scripts and files created for the run
# --------------------------------------------------------------------------------------

CONFIG = """
server {
    listen 127.0.0.1:%(port)d;
    root www/;
    client_max_body_size %(max_body)d;
    error_page 404 www/err/%(tag)s_404.html;

    location / {
        allowed_methods GET POST;
    }
    location /cgi {
        root www/cgi;
        cgi .py /usr/bin/python3;
        allowed_methods GET POST;
    }
    location /upload {
        upload_enabled on;
        upload_path www/uploads;
        allowed_methods POST;
    }
    location /uploads {
        root www/uploads;
        autoindex on;
        allowed_methods GET DELETE;
    }
    location /old {
        redirect 301 /;
        allowed_methods GET;
    }
    location /%(tag)s_dir {
        root www/%(tag)s_dir;
        autoindex on;
        allowed_methods GET;
    }
    location /%(tag)s_idx {
        root www/%(tag)s_idx;
        index page.html;
        autoindex off;
        allowed_methods GET;
    }
}

server {
    listen 127.0.0.1:%(port2)d;
    root www/;
    location / {
        allowed_methods GET;
    }
}
"""

CGI_SCRIPTS = {
    # Prints the CGI environment variables we care about
    "env.py": r'''
import os
print("Content-Type: text/plain")
print()
for key in ("REQUEST_METHOD", "QUERY_STRING", "CONTENT_LENGTH", "CONTENT_TYPE",
            "SERVER_PROTOCOL", "GATEWAY_INTERFACE", "SCRIPT_NAME", "SERVER_PORT",
            "HTTP_X_CUSTOM"):
    print("%s=%s" % (key, os.environ.get(key, "<unset>")))
print("CWD=" + os.getcwd())
''',
    # Sends the request body back
    "echo.py": r'''
import sys
data = sys.stdin.buffer.read()
sys.stdout.write("Content-Type: application/octet-stream\r\n\r\n")
sys.stdout.flush()
sys.stdout.buffer.write(data)
''',
    # Prints only the number of body bytes received
    "len.py": r'''
import sys
data = sys.stdin.buffer.read()
print("Content-Type: text/plain")
print()
print(len(data))
''',
    # Uses the CGI "Status" header and a custom header
    "status.py": r'''
print("Status: 201 Created")
print("X-From-Cgi: yes")
print("Content-Type: text/plain")
print()
print("created")
''',
    # Exits without printing anything
    "silent.py": "import sys\nsys.exit(3)\n",
    # Never finishes
    "loop.py": "while True:\n    pass\n",
}


class Context:
    def __init__(self, args):
        self.args = args
        self.tmpdir = tempfile.mkdtemp(prefix="webserv-tester-")
        self.log_path = os.path.join(self.tmpdir, "server.log")
        self.conf_path = os.path.join(self.tmpdir, "test.conf")
        self.proc = None
        self.created = []   # files/dirs to remove at the end

    def cgi(self, name):
        return "/cgi/%s_%s" % (TAG, name)

    # -- setup ---------------------------------------------------------------------
    def create_fixtures(self):
        def write(rel_path, content, mode="w"):
            full = os.path.join(ROOT, rel_path)
            with open(full, mode) as f:
                f.write(content)
            self.created.append(full)

        for name, code in CGI_SCRIPTS.items():
            write("www/cgi/%s_%s" % (TAG, name), code.lstrip())
        write("www/err/%s_404.html" % TAG, "<html><body>CUSTOM-404-PAGE</body></html>")
        write("www/%s_empty.txt" % TAG, "")
        write("www/%s_big.bin" % TAG, b"x" * (5 * 1024 * 1024), "wb")
        for d, files in (("%s_dir" % TAG, {"listed-file.txt": "hi"}),
                         ("%s_idx" % TAG, {"page.html": "INDEX-PAGE-CONTENT"})):
            full = os.path.join(ROOT, "www", d)
            os.makedirs(full, exist_ok=True)
            self.created.append(full)
            for fname, content in files.items():
                with open(os.path.join(full, fname), "w") as f:
                    f.write(content)
        os.makedirs(os.path.join(ROOT, "www", "uploads"), exist_ok=True)

        with open(self.conf_path, "w") as f:
            f.write(CONFIG % {"port": PORT, "port2": PORT2, "max_body": MAX_BODY, "tag": TAG})

    def start_server(self):
        binary = os.path.join(ROOT, self.args.binary)
        if not os.path.exists(binary):
            sys.exit("%s not found: run `make` first" % binary)
        for port in (PORT, PORT2):
            if port_in_use(port):
                sys.exit("port %d is already in use: stop the process using it" % port)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([binary, self.conf_path], cwd=ROOT,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.proc.poll() is not None:
                print(open(self.log_path).read())
                sys.exit("webserv exited during startup (code %s)" % self.proc.returncode)
            if port_in_use(PORT) and port_in_use(PORT2):
                return
            time.sleep(0.1)
        sys.exit("webserv did not start listening on %d/%d" % (PORT, PORT2))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    # -- teardown ------------------------------------------------------------------
    def cleanup(self):
        if self.alive():
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        for path in reversed(self.created):
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            elif os.path.exists(path):
                os.remove(path)
        for name in os.listdir(os.path.join(ROOT, "www", "uploads")):
            if name.startswith(TAG):
                os.remove(os.path.join(ROOT, "www", "uploads", name))


def port_in_use(port):
    try:
        socket.create_connection((http.HOST, port), timeout=0.2).close()
        return True
    except OSError:
        return False


# --------------------------------------------------------------------------------------
# Tiny test framework
# --------------------------------------------------------------------------------------

TESTS = []


def test(group, pending=False, slow=False):
    """Registers a test. pending=True: feature not implemented yet."""
    def register(func):
        TESTS.append({"name": func.__name__, "group": group, "func": func,
                      "pending": pending, "slow": slow, "doc": (func.__doc__ or "").strip()})
        return func
    return register


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_status(response, status):
    expect(response.status == status,
           "expected %d, got %d %s" % (status, response.status, response.reason))


# --------------------------------------------------------------------------------------
# Basic behaviour
# --------------------------------------------------------------------------------------

@test("basic")
def get_index(ctx):
    """GET / serves www/index.html with correct headers"""
    r = http.get(PORT, "/")
    expect_status(r, 200)
    expect(r.header("content-type").startswith("text/html"), "Content-Type: %r" % r.header("content-type"))
    expect(int(r.header("content-length", "-1")) == len(r.body), "Content-Length does not match body")
    with open(os.path.join(ROOT, "www", "index.html"), "rb") as f:
        expect(r.body == f.read(), "body differs from www/index.html")
    return "%d bytes" % len(r.body)


@test("basic")
def not_found(ctx):
    """GET of a missing file -> 404 Not Found"""
    r = http.get(PORT, "/%s_does_not_exist.html" % TAG)
    expect_status(r, 404)
    expect(r.reason == "Not Found", "reason phrase %r" % r.reason)


@test("basic")
def standard_reason_phrases(ctx):
    """Status lines use standard reason phrases"""
    cases = [(http.build("GET", "/"), 200, "OK"),
             (http.build("DELETE", "/"), 405, "Method Not Allowed"),
             (b"GARBAGE\r\n\r\n", 400, "Bad Request"),
             (http.build("GET", "/old"), 301, "Moved Permanently")]
    for data, status, reason in cases:
        r = http.request(PORT, data)
        expect(r.status == status and r.reason == reason,
               "expected '%d %s', got '%d %s'" % (status, reason, r.status, r.reason))


@test("basic")
def multiple_ports(ctx):
    """Second server block answers on its own port"""
    expect_status(http.get(PORT2, "/"), 200)
    r = http.request(PORT2, http.build("POST", "/", body="x"))
    expect_status(r, 405)
    return "POST allowed on :%d but not on :%d" % (PORT, PORT2)


@test("basic")
def redirect(ctx):
    """location with redirect -> 301 + Location header"""
    r = http.get(PORT, "/old")
    expect_status(r, 301)
    expect(r.header("location") == "/", "Location: %r" % r.header("location"))


@test("basic")
def method_not_allowed(ctx):
    """DELETE on a GET/POST location -> 405"""
    expect_status(http.request(PORT, http.build("DELETE", "/index.html")), 405)


@test("basic")
def default_error_page(ctx):
    """Error responses carry an HTML page (default error pages)"""
    r = http.request(PORT, http.build("DELETE", "/"))
    expect(b"<html" in r.body.lower(), "no HTML body in the error response")


# --------------------------------------------------------------------------------------
# Request parsing / limits
# --------------------------------------------------------------------------------------

@test("parsing")
def malformed_request_line(ctx):
    """Garbage request line -> 400"""
    expect_status(http.request(PORT, b"THIS IS NOT HTTP AT ALL\r\n\r\n"), 400)


@test("parsing")
def missing_host(ctx):
    """HTTP/1.1 without Host -> 400"""
    expect_status(http.request(PORT, http.build("GET", "/", host=False)), 400)


@test("parsing")
def http10_without_host(ctx):
    """HTTP/1.0 without Host is valid and the connection is closed after"""
    sock = http.connect(PORT)
    sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
    r, _ = http.read_response(sock)
    expect_status(r, 200)
    expect(http.is_closed_by_server(sock), "server kept the HTTP/1.0 connection open")
    sock.close()


@test("parsing")
def unsupported_version(ctx):
    """HTTP/2.0 -> 505"""
    expect_status(http.request(PORT, http.build("GET", "/", version="HTTP/2.0")), 505)


@test("parsing")
def uri_too_long(ctx):
    """URI longer than 2048 chars -> 414"""
    expect_status(http.get(PORT, "/" + "a" * 3000), 414)


@test("parsing")
def path_traversal(ctx):
    """/../ and /%2e%2e/ are rejected (cannot escape the root)"""
    for path in ("/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/cgi/../../Makefile"):
        r = http.get(PORT, path)
        expect(r.status in (400, 403), "%s -> %d" % (path, r.status))
        expect(b"root:" not in r.body and b"CXXFLAGS" not in r.body, "%s leaked a file!" % path)


@test("parsing")
def body_too_large_early(ctx):
    """Content-Length > client_max_body_size -> 413 before the body is sent"""
    start = time.time()
    data = http.build("POST", "/upload", headers={"Content-Length": str(MAX_BODY * 10)})
    r = http.request(PORT, data)   # we never send the body
    expect_status(r, 413)
    expect(time.time() - start < 2, "413 took %.1fs (server waited for the body)" % (time.time() - start))


@test("parsing")
def headers_too_large(ctx):
    """Headers that never end (>16 KB) -> 431 instead of buffering forever"""
    sock = http.connect(PORT)
    try:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + b"a" * 20000)
        r, _ = http.read_response(sock)
        expect_status(r, 431)
    finally:
        sock.close()


@test("parsing")
def url_encoded_path(ctx):
    """%XX in the path is decoded (/index%2Ehtml == /index.html)"""
    expect_status(http.get(PORT, "/index%2Ehtml"), 200)


# --------------------------------------------------------------------------------------
# Connections: keep-alive, pipelining, slow / rude clients
# --------------------------------------------------------------------------------------

@test("connection")
def keep_alive(ctx):
    """Two requests on the same connection"""
    sock = http.connect(PORT)
    try:
        for _ in range(2):
            sock.sendall(http.build("GET", "/"))
            r, rest = http.read_response(sock)
            expect_status(r, 200)
    finally:
        sock.close()


@test("connection")
def pipelining(ctx):
    """Two requests in one packet -> two responses, in order"""
    sock = http.connect(PORT)
    try:
        sock.sendall(http.build("GET", "/") + http.build("GET", "/%s_nope" % TAG))
        first, rest = http.read_response(sock)
        second, _ = http.read_response(sock, rest)
        expect(first.status == 200 and second.status == 404,
               "got %d then %d" % (first.status, second.status))
    finally:
        sock.close()


@test("connection")
def connection_close(ctx):
    """'Connection: close' -> server closes after the response"""
    sock = http.connect(PORT)
    sock.sendall(http.build("GET", "/", headers={"Connection": "close"}))
    r, _ = http.read_response(sock)
    expect_status(r, 200)
    expect(http.is_closed_by_server(sock), "connection still open")
    sock.close()


@test("connection")
def slow_client(ctx):
    """Request sent one byte at a time"""
    sock = http.connect(PORT)
    try:
        for byte in http.build("GET", "/"):
            sock.send(bytes([byte]))
            time.sleep(0.002)
        r, _ = http.read_response(sock)
        expect_status(r, 200)
    finally:
        sock.close()


@test("connection")
def client_disconnects_mid_response(ctx):
    """30 clients close while receiving a 5 MB file (SIGPIPE must not kill us)"""
    for _ in range(30):
        sock = http.connect(PORT)
        sock.sendall(http.build("GET", "/%s_big.bin" % TAG))
        sock.recv(1024)
        sock.close()
    time.sleep(0.5)
    expect(ctx.alive(), "server died")
    expect_status(http.get(PORT, "/"), 200)


@test("connection")
def client_disconnects_mid_request(ctx):
    """Clients that send half a request and vanish"""
    for _ in range(20):
        sock = http.connect(PORT)
        sock.sendall(b"POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\nabc")
        sock.close()
    time.sleep(0.3)
    expect_status(http.get(PORT, "/"), 200)


@test("connection")
def many_concurrent_connections(ctx):
    """200 connections open at the same time, all answered"""
    socks = [http.connect(PORT, timeout=10) for _ in range(200)]
    try:
        for s in socks:
            s.sendall(http.build("GET", "/"))
        statuses = [http.read_response(s)[0].status for s in socks]
        expect(statuses.count(200) == len(socks), "only %d/%d got 200" % (statuses.count(200), len(socks)))
    finally:
        for s in socks:
            s.close()


@test("connection", slow=True)
def idle_timeout(ctx):
    """A half-sent request is closed by the server (CLIENT_TIMEOUT, ~60 s)"""
    sock = http.connect(PORT)
    sock.sendall(b"GET / HTTP/1.1\r\n")
    expect(http.is_closed_by_server(sock, wait=75), "still open after 75 s")
    sock.close()


# --------------------------------------------------------------------------------------
# Upload / DELETE
# --------------------------------------------------------------------------------------

@test("upload")
def upload_get_list_delete(ctx):
    """POST multipart upload -> GET it -> appears in /uploads -> DELETE -> 404"""
    name = "%s_upload.txt" % TAG
    content = "uploaded by the tester " * 50
    body, ctype = http.multipart("file", name, content)
    r = http.request(PORT, http.build("POST", "/upload", headers={"Content-Type": ctype}, body=body))
    expect(r.status in (200, 201), "upload -> %d" % r.status)

    r = http.get(PORT, "/uploads/" + name)
    expect_status(r, 200)
    expect(r.body == content.encode(), "downloaded content differs")

    r = http.get(PORT, "/uploads/")
    expect(name.encode() in r.body, "file missing from the /uploads/ listing (autoindex)")
    r = http.get(PORT, "/uploads/", headers={"Accept": "application/json"})
    expect(r.header("content-type").startswith("application/json") and name.encode() in r.body,
           "JSON listing (Accept: application/json) missing the file")

    r = http.request(PORT, http.build("DELETE", "/uploads/" + name))
    expect(r.status in (200, 204), "DELETE -> %d" % r.status)
    expect_status(http.get(PORT, "/uploads/" + name), 404)


@test("upload")
def upload_binary(ctx):
    """Binary upload (all 256 byte values) arrives intact"""
    name = "%s_binary.bin" % TAG
    content = bytes(range(256)) * 400
    body, ctype = http.multipart("file", name, content)
    r = http.request(PORT, http.build("POST", "/upload", headers={"Content-Type": ctype}, body=body))
    expect(r.status in (200, 201), "upload -> %d" % r.status)
    r = http.get(PORT, "/uploads/" + name)
    expect(r.body == content, "binary content corrupted (%d vs %d bytes)" % (len(r.body), len(content)))


@test("upload")
def upload_multiple_files(ctx):
    """Several files in ONE multipart request (like <input multiple>) are all saved"""
    boundary = "----webservMulti"
    files = [("%s_multi_%d.txt" % (TAG, i), ("file number %d " % i).encode() * 50) for i in range(3)]
    files.append(("%s_multi_bin.bin" % TAG, bytes(range(256)) * 300))
    body = b""
    for name, content in files:
        body += ("--%s\r\nContent-Disposition: form-data; name=\"files\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n" % (boundary, name)).encode() + content + b"\r\n"
    body += ("--%s--\r\n" % boundary).encode()
    r = http.request(PORT, http.build("POST", "/upload",
                                      headers={"Content-Type": "multipart/form-data; boundary=" + boundary}, body=body))
    expect(r.status in (200, 201), "upload -> %d" % r.status)
    for name, content in files:
        got = http.get(PORT, "/uploads/" + name).body
        expect(got == content, "%s: got %d bytes, expected %d" % (name, len(got), len(content)))
    return "%d files" % len(files)


@test("upload")
def upload_existing_file(ctx):
    """Uploading the same name twice -> 409 Conflict"""
    name = "%s_twice.txt" % TAG
    body, ctype = http.multipart("file", name, "x")
    data = http.build("POST", "/upload", headers={"Content-Type": ctype}, body=body)
    http.request(PORT, data)
    expect_status(http.request(PORT, data), 409)


# --------------------------------------------------------------------------------------
# CGI
# --------------------------------------------------------------------------------------

@test("cgi")
def cgi_get_environment(ctx):
    """GET with query string: method, query, protocol, custom header, cwd"""
    r = http.get(PORT, ctx.cgi("env.py") + "?name=paula&x=1", headers={"X-Custom": "hello"})
    expect_status(r, 200)
    text = r.body.decode()
    for line in ("REQUEST_METHOD=GET", "QUERY_STRING=name=paula&x=1", "GATEWAY_INTERFACE=CGI/1.1",
                 "HTTP_X_CUSTOM=hello", "SERVER_PORT=%d" % PORT):
        expect(line in text, "missing %r in:\n%s" % (line, text))
    expect(re.search(r"CWD=.*/www/cgi\s", text), "script not run inside its directory:\n" + text)


@test("cgi")
def cgi_post_body(ctx):
    """POST body reaches the script's stdin, CONTENT_LENGTH is set"""
    r = http.request(PORT, http.build("POST", ctx.cgi("echo.py"), body=b"hello cgi"))
    expect_status(r, 200)
    expect(r.body == b"hello cgi", "echo returned %r" % r.body[:100])
    r = http.request(PORT, http.build("POST", ctx.cgi("env.py"), body=b"12345",
                                      headers={"Content-Type": "text/plain"}))
    expect(b"CONTENT_LENGTH=5" in r.body and b"CONTENT_TYPE=text/plain" in r.body, r.body.decode())


@test("cgi")
def cgi_big_post(ctx):
    """2.5 MB POST to a CGI (much bigger than a pipe buffer) arrives complete"""
    size = 2500000
    r = http.request(PORT, http.build("POST", ctx.cgi("len.py"), body=b"z" * size), timeout=15)
    expect_status(r, 200)
    expect(r.body.strip() == str(size).encode(), "script received %r bytes" % r.body.strip())


@test("cgi")
def cgi_status_header(ctx):
    """'Status: 201 Created' and custom headers from the script are honoured"""
    r = http.get(PORT, ctx.cgi("status.py"))
    expect_status(r, 201)
    expect(r.header("x-from-cgi") == "yes", "custom header lost")


@test("cgi")
def cgi_not_found(ctx):
    """Missing script -> 404"""
    expect_status(http.get(PORT, "/cgi/%s_missing.py" % TAG), 404)


@test("cgi")
def cgi_no_output(ctx):
    """Script that exits without output -> 502"""
    expect_status(http.get(PORT, ctx.cgi("silent.py")), 502)


@test("cgi")
def cgi_timeout_does_not_block(ctx):
    """Infinite script -> 504, and the server keeps serving others meanwhile"""
    result = {}

    def slow_request():
        try:
            result["r"] = http.request(PORT, http.build("GET", ctx.cgi("loop.py")), timeout=30)
        except Exception as e:   # noqa: BLE001
            result["error"] = e

    thread = threading.Thread(target=slow_request)
    thread.start()
    time.sleep(1)
    start = time.time()
    expect_status(http.get(PORT, "/"), 200)
    elapsed = time.time() - start
    thread.join(30)
    expect(elapsed < 1, "GET / took %.2fs while the CGI was running (server blocked)" % elapsed)
    expect("r" in result, "no response for the infinite CGI: %r" % result.get("error"))
    expect_status(result["r"], 504)
    return "other request answered in %.4fs" % elapsed


@test("cgi")
def cgi_client_leaves(ctx):
    """Client disconnects while the CGI runs -> script is killed"""
    sock = http.connect(PORT)
    sock.sendall(http.build("GET", ctx.cgi("loop.py")))
    time.sleep(0.5)
    sock.close()
    time.sleep(1.5)
    out = subprocess.run(["pgrep", "-f", "%s_loop.py" % TAG], capture_output=True, text=True).stdout
    expect(out.strip() == "", "loop.py still running: pids %s" % out.split())


@test("cgi")
def cgi_keep_alive(ctx):
    """CGI response followed by a static one on the same connection"""
    sock = http.connect(PORT)
    try:
        sock.sendall(http.build("GET", ctx.cgi("env.py")))
        r1, rest = http.read_response(sock)
        sock.sendall(http.build("GET", "/"))
        r2, _ = http.read_response(sock, rest)
        expect(r1.status == 200 and r2.status == 200, "got %d, %d" % (r1.status, r2.status))
    finally:
        sock.close()


@test("cgi")
def cgi_concurrent(ctx):
    """10 CGI requests at the same time"""
    socks = [http.connect(PORT, timeout=15) for _ in range(10)]
    try:
        for i, s in enumerate(socks):
            s.sendall(http.build("POST", ctx.cgi("echo.py"), body=("req-%d" % i).encode()))
        for i, s in enumerate(socks):
            r, _ = http.read_response(s)
            expect(r.body == ("req-%d" % i).encode(), "request %d got %r" % (i, r.body))
    finally:
        for s in socks:
            s.close()


# --------------------------------------------------------------------------------------
# Transfer-Encoding: chunked
# --------------------------------------------------------------------------------------

def chunked(*parts):
    """Encodes the given byte strings as a chunked body (last chunk included)."""
    out = b""
    for part in parts:
        out += ("%x\r\n" % len(part)).encode() + part + b"\r\n"
    return out + b"0\r\n\r\n"


@test("chunked")
def chunked_request_to_cgi(ctx):
    """Chunked body is un-chunked before reaching the CGI (stdin + EOF)"""
    data = http.build("POST", ctx.cgi("echo.py"), headers={"Transfer-Encoding": "chunked"},
                      body=chunked(b"hello", b" world"))
    r = http.request(PORT, data)
    expect_status(r, 200)
    expect(r.body == b"hello world", "CGI received %r" % r.body)


@test("chunked")
def chunked_content_length_for_cgi(ctx):
    """The CGI gets CONTENT_LENGTH of the decoded body"""
    data = http.build("POST", ctx.cgi("env.py"), headers={"Transfer-Encoding": "chunked"},
                      body=chunked(b"abc", b"defgh"))
    r = http.request(PORT, data)
    expect(b"CONTENT_LENGTH=8" in r.body, r.body.decode())


@test("chunked")
def chunked_split_in_packets(ctx):
    """Chunked body arriving in many small packets (incl. split chunk sizes)"""
    payload = bytes(range(256)) * 200
    body = chunked(*[payload[i:i + 1000] for i in range(0, len(payload), 1000)])
    data = http.build("POST", ctx.cgi("echo.py"), headers={"Transfer-Encoding": "chunked"}, body=body)
    sock = http.connect(PORT, timeout=10)
    try:
        for i in range(0, len(data), 333):
            sock.send(data[i:i + 333])
            time.sleep(0.001)
        r, _ = http.read_response(sock)
        expect_status(r, 200)
        expect(r.body == payload, "got %d bytes, expected %d" % (len(r.body), len(payload)))
    finally:
        sock.close()


@test("chunked")
def chunked_upload(ctx):
    """Chunked multipart upload is saved intact"""
    name = "%s_chunked.bin" % TAG
    content = bytes(range(256)) * 100
    body, ctype = http.multipart("file", name, content)
    data = http.build("POST", "/upload", headers={"Content-Type": ctype, "Transfer-Encoding": "chunked"},
                      body=chunked(body[:5000], body[5000:]))
    r = http.request(PORT, data)
    expect(r.status in (200, 201), "upload -> %d" % r.status)
    expect(http.get(PORT, "/uploads/" + name).body == content, "uploaded content differs")


@test("chunked")
def chunked_too_large(ctx):
    """Chunked body bigger than client_max_body_size -> 413"""
    big = b"x" * (MAX_BODY + 1000)
    data = http.build("POST", ctx.cgi("len.py"), headers={"Transfer-Encoding": "chunked"},
                      body=chunked(big[:MAX_BODY // 2], big[MAX_BODY // 2:]))
    sock = http.connect(PORT, timeout=10)
    try:
        try:
            sock.sendall(data)
        except (BrokenPipeError, ConnectionResetError):
            pass   # the server may answer and close before we finish sending
        r, _ = http.read_response(sock)
        expect_status(r, 413)
    finally:
        sock.close()


@test("chunked")
def chunked_malformed(ctx):
    """Invalid chunk size -> 400"""
    data = http.build("POST", ctx.cgi("echo.py"), headers={"Transfer-Encoding": "chunked"},
                      body=b"zz\r\nhello\r\n0\r\n\r\n")
    expect_status(http.request(PORT, data), 400)


@test("chunked")
def unsupported_transfer_encoding(ctx):
    """Transfer-Encoding other than chunked -> 501"""
    data = http.build("POST", ctx.cgi("echo.py"), headers={"Transfer-Encoding": "gzip"}, body=b"")
    expect_status(http.request(PORT, data), 501)


@test("chunked")
def chunked_then_next_request(ctx):
    """A request after a chunked one on the same connection is not lost"""
    sock = http.connect(PORT)
    try:
        sock.sendall(http.build("POST", ctx.cgi("echo.py"), headers={"Transfer-Encoding": "chunked"},
                                body=chunked(b"first"))
                     + http.build("GET", "/"))
        r1, rest = http.read_response(sock)
        r2, _ = http.read_response(sock, rest)
        expect(r1.body == b"first" and r2.status == 200, "got %r / %d" % (r1.body, r2.status))
    finally:
        sock.close()


# --------------------------------------------------------------------------------------
# Configuration features: error_page, autoindex, index, status codes
# (to mark a test for a feature that is not implemented yet, use
#  @test("group", pending=True): it is reported as PENDING instead of FAIL)
# --------------------------------------------------------------------------------------

@test("config")
def custom_error_page(ctx):
    """error_page 404 from the config is used"""
    r = http.get(PORT, "/%s_nope.html" % TAG)
    expect_status(r, 404)
    expect(b"CUSTOM-404-PAGE" in r.body, "custom error page not served")


@test("config")
def autoindex_listing(ctx):
    """autoindex on -> directory listing"""
    r = http.get(PORT, "/%s_dir/" % TAG)
    expect_status(r, 200)
    expect(b"listed-file.txt" in r.body, "file not listed")


@test("config")
def index_directive(ctx):
    """index page.html is served for the directory"""
    r = http.get(PORT, "/%s_idx/" % TAG)
    expect_status(r, 200)
    expect(b"INDEX-PAGE-CONTENT" in r.body, "index file not served")


@test("config")
def empty_file(ctx):
    """An empty file is 200 with Content-Length: 0 (not 404)"""
    r = http.get(PORT, "/%s_empty.txt" % TAG)
    expect_status(r, 200)
    expect(r.header("content-length") == "0", "Content-Length %r" % r.header("content-length"))


@test("config")
def delete_missing_file(ctx):
    """DELETE of a file that does not exist -> 404 (not 500)"""
    expect_status(http.request(PORT, http.build("DELETE", "/uploads/%s_nope" % TAG)), 404)


@test("config")
def unknown_method(ctx):
    """Unknown method -> 501 Not Implemented"""
    expect_status(http.request(PORT, http.build("FOO", "/")), 501)


# --------------------------------------------------------------------------------------
# Final checks
# --------------------------------------------------------------------------------------

@test("final")
def no_zombie_processes(ctx):
    """No zombie CGI children left behind"""
    time.sleep(2)   # reaping happens in the event loop, once per second
    ps = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,stat="], capture_output=True, text=True).stdout
    zombies = [line for line in ps.splitlines()
               if len(line.split()) >= 3 and line.split()[1] == str(ctx.proc.pid) and "Z" in line.split()[2]]
    expect(not zombies, "zombies: %s" % zombies)


# --------------------------------------------------------------------------------------
# --tester42: reproduces the official 42 tester (tst/tests_42/tester) locally.
# The real tester and cgi_tester are Linux binaries; this mode lets you check the
# same scenario on macOS, with a fake cgi_tester written in Python.
# --------------------------------------------------------------------------------------

FAKE_CGI_TESTER = r'''
import sys
data = sys.stdin.buffer.read()
out = sys.stdout.buffer
out.write(b"Status: 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n")
out.write(data.upper())
'''


def run_tester42(args):
    port = args.port + 20
    big = args.big
    body_size = 100000000 if big else 10000000
    tmp = tempfile.mkdtemp(prefix="webserv-tester42-")
    youpi = os.path.join(tmp, "YoupiBanane")
    for d in ("nop", "Yeah"):
        os.makedirs(os.path.join(youpi, d))
    files = {"youpi.bad_extension": "youpi.bad_extension", "nop/youpi.bad_extension": "nop/youpi.bad_extension",
             "nop/other.pouic": "nop/other.pouic", "Yeah/not_happy.bad_extension": "Yeah/not_happy.bad_extension",
             "youpi.bla": FAKE_CGI_TESTER, "youpla.bla": FAKE_CGI_TESTER}
    for rel, content in files.items():
        with open(os.path.join(youpi, rel), "w") as f:
            f.write(content)
    os.makedirs(os.path.join(tmp, "post_body_uploads"))
    conf = os.path.join(tmp, "tester42.conf")
    with open(conf, "w") as f:
        f.write("""
server {
    listen 127.0.0.1:%(port)d;
    root www/;
    client_max_body_size 200M;
    location / { allowed_methods GET; }
    location /post_body {
        client_max_body_size 100;
        upload_enabled on;
        upload_path %(tmp)s/post_body_uploads;
        allowed_methods POST;
    }
    location /directory {
        root %(tmp)s/YoupiBanane;
        index youpi.bad_extension;
        cgi .bla /usr/bin/python3;
        allowed_methods GET POST;
    }
}
""" % {"port": port, "tmp": tmp})

    if port_in_use(port):
        sys.exit("port %d is already in use" % port)
    log_path = os.path.join(tmp, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([os.path.join(ROOT, args.binary), conf], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        if port_in_use(port):
            break
        time.sleep(0.1)

    def get(path, follow=True, method="GET", headers=None):
        r = http.request(port, http.build(method, path, headers=headers))
        if follow and r.status in (301, 302) and r.header("location"):
            return get(r.header("location"), False, method, headers)
        return r

    def post_chunked(path, size, headers=None, chunk=65536):
        payload = (b"abcdefghij" * (size // 10 + 1))[:size]
        hdrs = {"Transfer-Encoding": "chunked", "Content-Type": "test/file"}
        hdrs.update(headers or {})
        body = b"".join(("%x\r\n" % len(payload[i:i + chunk])).encode() + payload[i:i + chunk] + b"\r\n"
                        for i in range(0, len(payload), chunk)) + b"0\r\n\r\n"
        sock = http.connect(port, timeout=120)
        try:
            try:
                sock.sendall(http.build("POST", path, headers=hdrs) + body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            r, _ = http.read_response(sock)
        finally:
            sock.close()
        return r, payload

    results = []

    def check(name, func):
        start = time.time()
        try:
            func()
            results.append(True)
            print("  %sPASS%s  %-62s %s(%.1fs)%s" % (GREEN, NC, name, GRAY, time.time() - start, NC))
        except Exception as e:   # noqa: BLE001
            results.append(False)
            print("  %sFAIL%s  %-62s %s" % (RED, NC, name, e))
        if proc.poll() is not None:
            print("%s%sCRASH: webserv died%s" % (RED, BOLD, NC))
            raise SystemExit(1)

    def status(path, expected, method="GET", body_check=None):
        def run():
            r = get(path, method=method)
            expect(r.status == expected, "expected %d, got %d" % (expected, r.status))
            if body_check is not None:
                expect(r.body.strip() == body_check, "body %r" % r.body[:60])
        return run

    def post_cgi(path, size, headers=None):
        def run():
            r, payload = post_chunked(path, size, headers)
            expect(r.status == 200, "expected 200, got %d" % r.status)
            expect(r.body == payload.upper(), "bad cgi returned body content (%d bytes)" % len(r.body))
        return run

    def post_body(size, expected):
        def run():
            r, _ = post_chunked("/post_body", size)
            expect(r.status == expected, "expected %d, got %d" % (expected, r.status))
        return run

    def workers(n_workers, times, func):
        def run():
            errors = []

            def worker():
                for _ in range(times):
                    try:
                        func()
                    except Exception as e:   # noqa: BLE001
                        errors.append(str(e))
            threads = [threading.Thread(target=worker) for _ in range(n_workers)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            expect(not errors, "%d errors, first: %s" % (len(errors), errors[0] if errors else ""))
        return run

    def keepalive_get(path):
        local = threading.local()

        def run():
            if not hasattr(local, "sock"):
                local.sock, local.rest = http.connect(port, timeout=30), b""
            local.sock.sendall(http.build("GET", path))
            r, local.rest = http.read_response(local.sock, local.rest)
            if r.header("connection") == "close":
                local.sock.close()
                del local.sock
            expect(r.status == 200, "got %d" % r.status)
        return run

    print("%swebserv · 42 tester simulation%s  ·  body %s  ·  log: %s\n"
          % (BOLD, NC, "100 MB" if big else "10 MB (use --big for 100 MB)", log_path))
    try:
        check("GET /", status("/", 200))
        check("POST / with a size of 0 -> 405", status("/", 405, method="POST"))
        check("HEAD / -> 405", status("/", 405, method="HEAD"))
        check("GET /directory", status("/directory", 200, body_check=b"youpi.bad_extension"))
        check("GET /directory/youpi.bad_extension", status("/directory/youpi.bad_extension", 200))
        check("GET /directory/youpi.bla", status("/directory/youpi.bla", 200))
        check("GET Expected 404 on /directory/oulalala", status("/directory/oulalala", 404))
        check("GET /directory/nop", status("/directory/nop", 200, body_check=b"nop/youpi.bad_extension"))
        check("GET /directory/nop/", status("/directory/nop/", 200))
        check("GET /directory/nop/other.pouic", status("/directory/nop/other.pouic", 200))
        check("GET Expected 404 on /directory/nop/other.pouac", status("/directory/nop/other.pouac", 404))
        check("GET Expected 404 on /directory/Yeah", status("/directory/Yeah", 404))
        check("GET /directory/Yeah/not_happy.bad_extension", status("/directory/Yeah/not_happy.bad_extension", 200))
        check("POST /directory/youpi.bla with a size of %d" % body_size, post_cgi("/directory/youpi.bla", body_size))
        check("POST /directory/youpla.bla with a size of %d" % body_size, post_cgi("/directory/youpla.bla", body_size))
        check("POST /directory/youpi.bla size 100000 with special headers",
              post_cgi("/directory/youpi.bla", 100000, {"X-Secret-Header-For-Test": "1"}))
        check("POST /post_body with a size of 0", post_body(0, 200))
        check("POST /post_body with a size of 100", post_body(100, 200))
        check("POST /post_body with a size of 200 -> 413", post_body(200, 413))
        check("POST /post_body with a size of 101 -> 413", post_body(101, 413))

        def post_body_split():
            # 100 bytes (the exact limit) where the chunk's closing "\r" arrives
            # with the data and the "\n" later: must be 200, not a false 413
            sock = http.connect(port, timeout=10)
            try:
                sock.sendall(http.build("POST", "/post_body", headers={"Transfer-Encoding": "chunked"}))
                for part in (b"64\r\n", b"x" * 100 + b"\r", b"\n0\r\n", b"\r\n"):
                    time.sleep(0.05)
                    sock.sendall(part)
                r, _ = http.read_response(sock)
                expect(r.status == 200, "expected 200, got %d" % r.status)
            finally:
                sock.close()
        check("POST /post_body 100 bytes arriving in pieces (exact limit)", post_body_split)
        check("multiple workers(5) x 15: GET /", workers(5, 15, keepalive_get("/")))
        check("multiple workers(20) x %d: GET /" % (5000 if big else 500),
              workers(20, 5000 if big else 500, keepalive_get("/")))
        check("multiple workers(128) x 50: GET /directory/nop/", workers(128, 50, keepalive_get("/directory/nop/")))
        check("multiple workers(20) x 5: POST /directory/youpi.bla size %d" % body_size,
              workers(20 if big else 5, 5 if big else 2, post_cgi("/directory/youpi.bla", body_size)))
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    failed = results.count(False)
    print("\n%s%d passed%s  ·  %s%d failed%s" % (GREEN, results.count(True), NC, RED if failed else GRAY, failed, NC))
    sys.exit(1 if failed else 0)


# --------------------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------------------

def main():
    global PORT, PORT2
    parser = argparse.ArgumentParser(description="webserv functional tester")
    parser.add_argument("-k", metavar="TEXT", help="only run tests whose name or group contains TEXT")
    parser.add_argument("-v", "--verbose", action="store_true", help="show details of passing tests")
    parser.add_argument("--slow", action="store_true", help="include slow tests (idle timeout, ~60 s)")
    parser.add_argument("--binary", default="webserv", help="path to the executable (default: ./webserv)")
    parser.add_argument("--port", type=int, default=PORT, help="first port to use (default: %d)" % PORT)
    parser.add_argument("--tester42", action="store_true",
                        help="simulate the official 42 tester (tst/tests_42) with a fake cgi_tester")
    parser.add_argument("--big", action="store_true", help="with --tester42: 100 MB bodies and full worker counts")
    args = parser.parse_args()
    if args.tester42:
        run_tester42(args)

    PORT, PORT2 = args.port, args.port + 1

    tests = [t for t in TESTS if (args.slow or not t["slow"])
             and (not args.k or args.k in t["name"] or args.k in t["group"])]

    ctx = Context(args)
    counts = {"PASS": 0, "FAIL": 0, "PENDING": 0}
    crashed = False
    try:
        ctx.create_fixtures()
        ctx.start_server()
        print("%swebserv tester%s  ·  %d tests  ·  server log: %s\n"
              % (BOLD, NC, len(tests), ctx.log_path))
        group = None
        for t in tests:
            if t["group"] != group:
                group = t["group"]
                print("%s%s%s" % (BOLD, group.upper(), NC))
            start = time.time()
            try:
                detail = t["func"](ctx)
                status, color = "PASS", GREEN
                if t["pending"]:
                    detail = "implemented!"
            except Exception as e:   # noqa: BLE001
                status, color = ("PENDING", YELLOW) if t["pending"] else ("FAIL", RED)
                detail = "%s: %s" % (type(e).__name__, e) if not isinstance(e, AssertionError) else str(e)
            counts[status] += 1
            line = "  %s%-7s%s %-34s %s%s%s" % (color, status, NC, t["name"], GRAY, t["doc"], NC)
            print(line + ("%s  (%.1fs)%s" % (GRAY, time.time() - start, NC)))
            if detail and (status != "PASS" or args.verbose or t["pending"]):
                for detail_line in str(detail).splitlines()[:6]:
                    print("          %s%s%s" % (color if status != "PASS" else GRAY, detail_line, NC))
            if not ctx.alive():
                crashed = True
                print("\n%s%sCRASH: webserv died during '%s' (exit code %s)%s"
                      % (RED, BOLD, t["name"], ctx.proc.returncode, NC))
                print(open(ctx.log_path).read()[-2000:])
                break
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        ctx.cleanup()

    print("\n%s%d passed%s  ·  %s%d failed%s  ·  %s%d pending%s%s"
          % (GREEN, counts["PASS"], NC, RED if counts["FAIL"] else GRAY, counts["FAIL"], NC,
             YELLOW if counts["PENDING"] else GRAY, counts["PENDING"], NC,
             ("  ·  %s%sSERVER CRASHED%s" % (RED, BOLD, NC)) if crashed else ""))
    sys.exit(1 if counts["FAIL"] or crashed else 0)


if __name__ == "__main__":
    main()
