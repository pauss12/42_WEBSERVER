"""Minimal raw-socket HTTP client shared by tester.py and stress.py.

We use raw sockets instead of urllib/requests on purpose: that way we can send
malformed requests, pipeline several requests in one packet, send bytes one at
a time, close the connection in the middle of a response, etc.
"""

import socket

HOST = "127.0.0.1"


class Response:
    def __init__(self, status, reason, headers, body, raw_head):
        self.status = status        # 200
        self.reason = reason        # "OK"
        self.headers = headers      # {"content-length": "15880", ...} (lowercase keys)
        self.body = body            # bytes
        self.raw_head = raw_head    # status line + headers, as text

    def header(self, name, default=""):
        return self.headers.get(name.lower(), default)

    def __repr__(self):
        return "<Response %d %s, %d bytes>" % (self.status, self.reason, len(self.body))


class ServerClosed(Exception):
    """The server closed the connection before sending a full response."""


def connect(port, timeout=5.0, host=HOST):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def build(method="GET", path="/", headers=None, body=b"", version="HTTP/1.1", host=True):
    """Builds a raw HTTP request (bytes)."""
    if isinstance(body, str):
        body = body.encode()
    lines = ["%s %s %s" % (method, path, version)]
    hdrs = dict(headers or {})
    if host and "Host" not in hdrs:
        hdrs["Host"] = "localhost"
    if body and "Content-Length" not in hdrs and "Transfer-Encoding" not in hdrs:
        hdrs["Content-Length"] = str(len(body))
    for key, value in hdrs.items():
        lines.append("%s: %s" % (key, value))
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + body


def read_response(sock, buffer=b"", is_head=False):
    """Reads exactly one response from `sock`.

    is_head=True: response to a HEAD request, which has headers (Content-Length
    included) but never a body.

    Returns (Response, leftover_bytes). Leftover bytes belong to the next
    response (keep-alive / pipelining). Raises ServerClosed if the connection
    ends before a full response arrives.
    """
    buffer = bytearray(buffer)   # bytes += bytes copies everything: too slow with 100 MB
    while b"\r\n\r\n" not in buffer:
        chunk = sock.recv(65536)
        if not chunk:
            raise ServerClosed("connection closed before headers (got %d bytes)" % len(buffer))
        buffer += chunk

    head, buffer = bytes(buffer).split(b"\r\n\r\n", 1)
    buffer = bytearray(buffer)
    head_text = head.decode("latin-1")
    lines = head_text.split("\r\n")
    parts = lines[0].split(" ", 2)
    if len(parts) < 2 or not parts[0].startswith("HTTP/"):
        raise ValueError("invalid status line: %r" % lines[0])
    status = int(parts[1])
    reason = parts[2] if len(parts) > 2 else ""

    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()

    if is_head:
        body = b""
    elif "content-length" in headers:
        length = int(headers["content-length"])
        while len(buffer) < length:
            chunk = sock.recv(65536)
            if not chunk:
                raise ServerClosed("connection closed in the middle of the body")
            buffer += chunk
        body, buffer = bytes(buffer[:length]), bytes(buffer[length:])
    else:
        # No Content-Length: the body ends when the server closes
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
        body, buffer = bytes(buffer), b""

    return Response(status, reason, headers, body, head_text), buffer


def request(port, data, timeout=5.0):
    """Opens a connection, sends `data` (bytes), returns one Response."""
    sock = connect(port, timeout)
    try:
        sock.sendall(data)
        response, _ = read_response(sock, is_head=data.startswith(b"HEAD "))
        return response
    finally:
        sock.close()


def get(port, path="/", **kwargs):
    return request(port, build("GET", path, **kwargs))


def is_closed_by_server(sock, wait=2.0):
    """True if the server closes the connection within `wait` seconds."""
    sock.settimeout(wait)
    try:
        return sock.recv(1) == b""
    except socket.timeout:
        return False
    except (ConnectionResetError, BrokenPipeError):
        return True


def multipart(field, filename, content, boundary="----webservTestBoundary"):
    """Builds a multipart/form-data body like a browser upload form does."""
    if isinstance(content, str):
        content = content.encode()
    body = (
        "--%s\r\n"
        'Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n" % (boundary, field, filename)
    ).encode() + content + ("\r\n--%s--\r\n" % boundary).encode()
    return body, "multipart/form-data; boundary=%s" % boundary
