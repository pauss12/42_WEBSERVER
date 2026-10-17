#!/usr/bin/env python3
"""Pruebas "como con telnet": peticiones HTTP escritas a mano, en crudo.

    python3 tst/telnet_tester.py
    PORT1=9080 PORT2=9081 python3 tst/telnet_tester.py   # otros puertos

Muestra cada línea que se escribiría en telnet (">") y la línea de estado
que responde el servidor ("<"), para poder repetirlo a mano con:

    telnet 127.0.0.1 8080        (o: nc -c 127.0.0.1 8080)

Usa etc/default.conf. Si el servidor no está arrancado, lo arranca y lo para
al terminar.
"""

import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get("PORT1", "8080"))
PORT2 = int(os.environ.get("PORT2", "8081"))
HOST = "127.0.0.1"

GREEN, RED, YELLOW, GRAY, BOLD, NC = (
    "\033[32m", "\033[31m", "\033[33m", "\033[90m", "\033[1m", "\033[0m")
results = []


# ----------------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------------

def section(title, port=None):
    print("\n%s%s%s" % (BOLD, title, NC))
    print("  %stelnet %s %d%s" % (GRAY, HOST, port or PORT, NC))


def show_request(raw):
    """Prints the request as it would be typed in telnet."""
    text = raw.decode("latin-1")
    lines = text.split("\r\n")
    if text.endswith("\r\n\r\n"):
        lines = lines[:-1]          # the final empty line is shown once
    for line in lines:
        print("  %s> %s%s" % (GRAY, line, NC))


def check(description, ok, detail=""):
    results.append(ok)
    mark = "%s✔%s" % (GREEN, NC) if ok else "%s✘%s" % (RED, NC)
    print("  %s %s%s" % (mark, description, ("  %s(%s)%s" % (GRAY, detail, NC)) if detail else ""))


def connect(timeout=5):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    return s


def read_response(sock, buffer=b""):
    """Reads one response. Returns (status_line, headers, body, rest)."""
    while b"\r\n\r\n" not in buffer:
        chunk = sock.recv(65536)
        if not chunk:
            return None, {}, b"", b""
        buffer += chunk
    head, buffer = buffer.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    length = int(headers.get("content-length", "0"))
    while len(buffer) < length:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buffer += chunk
    return lines[0], headers, buffer[:length], buffer[length:]


def status_of(line):
    m = re.match(r"HTTP/\d\.\d (\d{3})", line or "")
    return int(m.group(1)) if m else None


def show_status(line):
    print("  %s< %s%s" % (GRAY, line if line else "(sin respuesta)", NC))


def closed_by_server(sock, wait=2.0):
    sock.settimeout(wait)
    try:
        return sock.recv(1) == b""
    except socket.timeout:
        return False
    except OSError:
        return True


def send_and_read(raw):
    show_request(raw)
    s = connect()
    s.sendall(raw)
    line, headers, body, _ = read_response(s)
    show_status(line)
    return s, line, headers, body


def port_open(port):
    try:
        socket.create_connection((HOST, port), timeout=0.5).close()
        return True
    except OSError:
        return False


# ----------------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------------

def run():
    req = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"

    section("1. Petición normal: la conexión sigue abierta (keep-alive)")
    s, line, headers, _ = send_and_read(req)
    check("200 OK", status_of(line) == 200, line)
    check("la conexión sigue abierta para otra petición", not closed_by_server(s, 0.5))
    print("  %s(misma conexión, segunda petición:)%s" % (GRAY, NC))
    req2 = b"GET /noexiste HTTP/1.1\r\nHost: localhost\r\n\r\n"
    show_request(req2)
    s.sendall(req2)
    line, headers, _, _ = read_response(s)
    show_status(line)
    check("404 por la misma conexión", status_of(line) == 404, line)
    check("tras un error, el servidor cierra (Connection: close)", closed_by_server(s))
    s.close()

    section("2. HTTP/1.1 sin cabecera Host")
    s, line, _, _ = send_and_read(b"GET / HTTP/1.1\r\n\r\n")
    check("400 Bad Request (Host es obligatorio en HTTP/1.1)", status_of(line) == 400, line)
    s.close()

    section("3. HTTP/1.0 sin Host")
    s, line, _, _ = send_and_read(b"GET / HTTP/1.0\r\n\r\n")
    check("200 OK (en HTTP/1.0 Host no es obligatorio)", status_of(line) == 200, line)
    check("el servidor cierra la conexión (HTTP/1.0 no hace keep-alive)", closed_by_server(s))
    s.close()

    section("4. Petición mal escrita")
    s, line, _, _ = send_and_read(b"HOLA QUE TAL\r\n\r\n")
    check("400 Bad Request", status_of(line) == 400, line)
    s.close()

    section("5. Método desconocido")
    s, line, _, _ = send_and_read(b"FOO / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    check("501 Not Implemented (y no crashea)", status_of(line) == 501, line)
    s.close()

    section("6. Cabecera sin ':'")
    s, line, _, _ = send_and_read(b"GET / HTTP/1.1\r\nHost: localhost\r\nCabeceraRota\r\n\r\n")
    check("400 Bad Request", status_of(line) == 400, line)
    s.close()

    section("7. POST con body escrito a mano, a un CGI")
    s, line, _, body = send_and_read(
        b"POST /cgi/example.py HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhola!")
    check("200 OK", status_of(line) == 200, line)
    check("el CGI recibe el body", b"Body (5 bytes): hola!" in body)
    s.close()

    section("8. Dos peticiones escritas de golpe (pipelining)")
    both = req + b"GET /noexiste HTTP/1.1\r\nHost: localhost\r\n\r\n"
    show_request(both)
    s = connect()
    s.sendall(both)
    line1, _, _, rest = read_response(s)
    line2, _, _, _ = read_response(s, rest)
    show_status(line1)
    show_status(line2)
    check("dos respuestas, en orden (200 y 404)", status_of(line1) == 200 and status_of(line2) == 404)
    s.close()

    section("9. Escribiendo la petición letra a letra (como en telnet)")
    show_request(req)
    s = connect()
    for byte in req:
        s.send(bytes([byte]))
        time.sleep(0.01)
    line, _, _, _ = read_response(s)
    show_status(line)
    check("200 OK aunque llegue byte a byte", status_of(line) == 200, line)
    s.close()

    section("10. Un cliente a medias NO bloquea a los demás")
    print("  %s> GET / HTTP/1.1%s   %s(sin terminar: la conexión se queda a medias)%s" % (GRAY, NC, GRAY, NC))
    slow = connect()
    slow.sendall(b"GET / HTTP/1.1\r\n")
    time.sleep(0.3)
    start = time.time()
    other = connect()
    other.sendall(req)
    line, _, _, _ = read_response(other)
    elapsed = time.time() - start
    other.close()
    print("  %s(mientras tanto, otro cliente pide GET /:)%s" % (GRAY, NC))
    show_status(line)
    check("el otro cliente recibe respuesta enseguida", status_of(line) == 200 and elapsed < 1,
          "%.3f s" % elapsed)
    print("  %s> Host: localhost%s   %s(ahora el primero termina su petición)%s" % (GRAY, NC, GRAY, NC))
    print("  %s>%s" % (GRAY, NC))
    slow.sendall(b"Host: localhost\r\n\r\n")
    line, _, _, _ = read_response(slow)
    show_status(line)
    check("y el primero también recibe su 200", status_of(line) == 200, line)
    slow.close()

    section("11. Content-Length mayor que el body enviado")
    raw = b"POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n1234"
    show_request(raw)
    print("  %s(se envían 4 bytes de los 10 anunciados y se espera...)%s" % (GRAY, NC))
    s = connect(timeout=10)
    s.sendall(raw)
    start = time.time()
    line, _, _, _ = read_response(s)
    show_status(line)
    check("408 Request Timeout al cabo de unos segundos", status_of(line) == 408,
          "%.1f s" % (time.time() - start))
    s.close()

    section("12. Una cabecera enorme (más de 8 KB)")
    raw = b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Larga: " + b"A" * 10000 + b"\r\n\r\n"
    print("  %s> GET / HTTP/1.1%s" % (GRAY, NC))
    print("  %s> Host: localhost%s" % (GRAY, NC))
    print("  %s> X-Larga: AAAA... (10000 caracteres)%s" % (GRAY, NC))
    print("  %s>%s" % (GRAY, NC))
    s = connect()
    s.sendall(raw)
    line, _, _, _ = read_response(s)
    show_status(line)
    check("431 Request Header Fields Too Large", status_of(line) == 431, line)
    s.close()

    section("13. Segundo sitio, en otra interfaz y puerto", PORT2)
    show_request(req)
    s = socket.create_connection((HOST, PORT2), timeout=5)
    s.sendall(req)
    line, _, body, _ = read_response(s)
    show_status(line)
    check("200 OK con el contenido del segundo sitio", status_of(line) == 200 and b"Second site" in body, line)
    s.close()

    section("14. El servidor sigue vivo después de todo")
    s, line, _, _ = send_and_read(req)
    check("200 OK", status_of(line) == 200, line)
    s.close()


# ----------------------------------------------------------------------------------
# Main: use the running server or start one
# ----------------------------------------------------------------------------------

def main():
    proc = None
    if port_open(PORT):
        print("%sUsando el servidor que ya escucha en el puerto %d: tiene que estar arrancado con\n"
              "etc/default.conf y con el código actual (si no, reinícialo o usa PORT1/PORT2).%s"
              % (YELLOW, PORT, NC))
    else:
        binary = os.path.join(ROOT, "webserv")
        if not os.path.exists(binary):
            subprocess.run(["make", "-s"], cwd=ROOT, check=True)
        tmp = tempfile.mkdtemp(prefix="webserv-telnet-")   # copy of default.conf with the ports
        conf = os.path.join(tmp, "default.conf")
        with open(os.path.join(ROOT, "etc", "default.conf")) as f:
            text = f.read().replace(":8080;", ":%d;" % PORT).replace(":8081;", ":%d;" % PORT2)
        with open(conf, "w") as f:
            f.write(text)
        proc = subprocess.Popen([binary, conf], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(30):
            if port_open(PORT):
                break
            time.sleep(0.1)
        print("%sServidor arrancado con etc/default.conf en %d/%d (pid %d); se parará al terminar.%s"
              % (GRAY, PORT, PORT2, proc.pid, NC))
    try:
        run()
    except Exception as e:   # noqa: BLE001
        results.append(False)
        print("\n%sError durante las pruebas: %s%s" % (RED, e, NC))
    finally:
        if proc is not None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    failed = results.count(False)
    print("\n%sResultado:%s %s%d correctas%s%s" % (
        BOLD, NC, GREEN, results.count(True), NC,
        (" · %s%d fallidas%s" % (RED, failed, NC)) if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
