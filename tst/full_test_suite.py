#!/usr/bin/env python3
import argparse
import re
import socket
import subprocess
import sys
import time
import threading
from pathlib import Path
from urllib.parse import urljoin

import requests

DEFAULT_BASE = "http://127.0.0.1:8080"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "config" / "testing.conf"
DEFAULT_SERVER_BIN = ROOT / "webserv"

TOTAL = 0
PASSED = 0
SKIPPED = 0

# =========================
# OUTPUT
# =========================

def print_block(title):
    print("\n" + "=" * 70)
    print(title)
    print("=" * 70)


def print_result(name, status, expected="", actual="", hint=""):
    global TOTAL, PASSED, SKIPPED
    TOTAL += 1
    if status == "PASS":
        PASSED += 1
        icon = "✅"
    elif status == "SKIP":
        SKIPPED += 1
        icon = "⚪️"
    else:
        icon = "❌"
    print(f"{name}: {icon} {status}")
    print(f"   ↳ Info: Verifica {name}")
    if expected:
        print(f"   ↳ Expected: {expected}")
    if actual:
        print(f"   ↳ Actual: {actual}")
    if hint:
        print(f"   ↳ Possible cause: {hint}")


def print_summary():
    print_block("SUMMARY")
    print(f"Passed: {PASSED}/{TOTAL}")
    print(f"Skipped: {SKIPPED}/{TOTAL}")
    print(f"Failed: {TOTAL - PASSED - SKIPPED}/{TOTAL}")


# =========================
# CONFIG HELPERS
# =========================

def read_config_text(path: Path):
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return ""


def config_has(text, token):
    return token in text


def count_server_blocks(text):
    return len(re.findall(r"\bserver\s*\{", text))


def parse_server_names(text):
    names = []
    for line in text.splitlines():
        if "server_name" in line:
            parts = line.strip().rstrip(";").split()
            if len(parts) > 1:
                names.extend(parts[1:])
    return [n for n in names if n and n != "_" and n != "localhost"]


def _strip_comments(line):
    return line.split("#", 1)[0].strip()


def parse_directives(block_text):
    directives = {}
    for raw in block_text.splitlines():
        line = _strip_comments(raw)
        if not line or line.startswith("}") or line.startswith("{"):
            continue
        if line.endswith(";"):
            line = line[:-1]
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if key == "server":
            continue
        values = parts[1:]
        if key == "error_page":
            directives.setdefault(key, []).append(values)
        else:
            directives[key] = values
    return directives


def parse_locations(text):
    locations = {}
    for match in re.finditer(r"location\s+([^\s{]+)\s*\{([^}]*)\}", text, flags=re.S):
        path = match.group(1).strip()
        body = match.group(2)
        locations[path] = parse_directives(body)
    return locations


def parse_server_directives(text):
    stripped = re.sub(r"location\s+[^\s{]+\s*\{[^}]*\}", "", text, flags=re.S)
    return parse_directives(stripped)


def resolve_path(conf_path):
    if not conf_path:
        return None
    p = conf_path[0] if isinstance(conf_path, list) else conf_path
    if p.startswith("./"):
        return (ROOT / p[2:]).resolve()
    return (ROOT / p).resolve()


def resolve_error_page_file(error_path, server_cfg):
    if not error_path:
        return None
    if error_path.startswith("/"):
        root = server_cfg.get("root", ["./"])[0] if server_cfg else "./"
        base = resolve_path(root)
        return (base / error_path.lstrip("/")).resolve() if base else None
    return resolve_path(error_path)


def get_location_config(locations, uri):
    best = None
    for loc in locations:
        if uri.startswith(loc):
            if best is None or len(loc) > len(best):
                best = loc
    return locations.get(best) if best else None


def get_max_body_size(server_cfg, locations, uri):
    loc = get_location_config(locations, uri)
    if loc and "client_max_body_size" in loc:
        try:
            return int(loc["client_max_body_size"][0])
        except Exception:
            return 0
    if server_cfg and "client_max_body_size" in server_cfg:
        try:
            return int(server_cfg["client_max_body_size"][0])
        except Exception:
            return 0
    return 0


# =========================
# HTTP HELPERS
# =========================

def safe_request(method, url, **kwargs):
    try:
        return requests.request(method, url, timeout=5, **kwargs)
    except Exception as exc:
        return exc


def has_status(resp, *codes):
    return hasattr(resp, "status_code") and resp.status_code in codes


def raw_request(host, port, payload, timeout=5):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        s.sendall(payload.encode("utf-8", errors="ignore"))
        chunks = []
        while True:
            try:
                data = s.recv(4096)
                if not data:
                    break
                chunks.append(data)
            except socket.timeout:
                break
        raw = b"".join(chunks)
    finally:
        try:
            s.close()
        except Exception:
            pass

    text = raw.decode("iso-8859-1", errors="replace")
    status = None
    headers = {}
    body = ""
    if "\r\n\r\n" in text:
        head, body = text.split("\r\n\r\n", 1)
        lines = head.split("\r\n")
        if lines:
            parts = lines[0].split(" ")
            if len(parts) >= 2 and parts[1].isdigit():
                status = int(parts[1])
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip()] = v.strip()
    return status, headers, body, text


# =========================
# BUILD / CONFIG TESTS
# =========================

def test_build(make_cmd, expect_flags=None):
    name = "Build: make"
    try:
        result = subprocess.run(make_cmd, cwd=str(ROOT), capture_output=True, text=True)
        if result.returncode != 0:
            print_result(name, "FAIL", "make should succeed", "non-zero exit", "Compiler errors or missing deps")
            return
        if expect_flags:
            makefile = (ROOT / "Makefile").read_text(encoding="utf-8", errors="ignore")
            missing = [f for f in expect_flags if f not in makefile]
            if missing:
                print_result(name + " flags", "FAIL", f"flags present: {', '.join(expect_flags)}", f"missing: {', '.join(missing)}", "CXXFLAGS not set")
            else:
                print_result(name + " flags", "PASS", f"flags present: {', '.join(expect_flags)}")
        print_result(name, "PASS", "make should succeed", "ok")
    except Exception as exc:
        print_result(name, "FAIL", "make should succeed", f"{type(exc).__name__}: {exc}", "Build tool failure")


def test_invalid_config(server_bin: Path):
    name = "Config: invalid syntax returns error"
    bad_conf = "server { listen 8080; invalid_directive on; }"
    tmp_path = ROOT / ".tmp_bad_conf.conf"
    try:
        tmp_path.write_text(bad_conf, encoding="utf-8")
        result = subprocess.run([str(server_bin), str(tmp_path)], capture_output=True, text=True)
        if result.returncode == 0:
            print_result(name, "FAIL", "non-zero exit", "exit=0", "Parser not validating unknown directives")
        else:
            print_result(name, "PASS", "non-zero exit", f"exit={result.returncode}")
    except Exception as exc:
        print_result(name, "FAIL", "non-zero exit", f"{type(exc).__name__}: {exc}", "Server failed to run")
    finally:
        try:
            tmp_path.unlink()
        except Exception:
            pass


# =========================
# TESTS
# =========================

def test_basic_methods(base):
    print_block("BASIC METHODS")
    r = safe_request("GET", urljoin(base, "/"))
    print_result("GET /", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "Root not served or routing error")

    r = safe_request("HEAD", urljoin(base, "/"))
    print_result("HEAD /", "PASS" if has_status(r, 200, 405) else "FAIL", "200 or 405", f"status={getattr(r, 'status_code', r)}", "HEAD method handling")

    r = safe_request("BREW", urljoin(base, "/"))
    print_result("UNKNOWN method", "PASS" if has_status(r, 400, 405, 501) else "FAIL", "400/405/501", f"status={getattr(r, 'status_code', r)}", "Unknown method not rejected")


def test_http_parsing(base, host, port):
    print_block("HTTP PARSING")

    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Request line válida", "PASS" if status == 200 else "FAIL", "200", f"status={status}", "Request line parsing")

    payload = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"X-Test: one\r\n"
        f"X-Test: two\r\n\r\n"
    )
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Headers duplicados", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "Header merge or rejection missing")

    huge_header = "A" * 16000
    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\nX-Long: {huge_header}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload, timeout=3)
    print_result("Header muy largo", "PASS" if status in (400, 413, 414, 431) else "FAIL", "400/413/414/431", f"status={status}", "No header size limits")

    payload = f"GET /?q=hola%20mundo&x=1 HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Query string parsing", "PASS" if status == 200 else "FAIL", "200", f"status={status}", "URI parsing failed")

    payload = f"GET /%7Euser HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("URI percent-encoding", "PASS" if status in (200, 404) else "FAIL", "200/404", f"status={status}", "Percent decoding or routing issue")

    payload = f"GET  HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Request line inválida", "PASS" if status in (400, 405) else "FAIL", "400/405", f"status={status}", "Malformed request line accepted")

    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\nX-BadHeader\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Header sin ':'", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Invalid header accepted")

    payload = f"GET / HTTP/1.1\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Host header requerido", "PASS" if status in (400, 403) else "FAIL", "400/403", f"status={status}", "Missing Host not rejected")

    payload = f"GeT / HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Método con token inválido", "PASS" if status in (400, 405, 501) else "FAIL", "400/405/501", f"status={status}", "Method token validation missing")

    long_uri = "/" + ("a" * 9000)
    payload = f"GET {long_uri} HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("URI muy larga", "PASS" if status in (400, 414, 431) else "FAIL", "400/414/431", f"status={status}", "URI length limit not enforced")

    payload = f"GET http://{host}/ HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Absolute-form URI", "PASS" if status in (200, 400, 404) else "FAIL", "200/400/404", f"status={status}", "Absolute-form not handled")

    payload = f"GET / HTTP/1.1\r\nHost : {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Header con espacio antes de ':'", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Header parsing too permissive")

    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\nX-Obs: a\r\n\tb\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Obs-fold (line folding)", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Obs-fold not rejected")

    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\nX-In\x00valid: a\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Header con NULL", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Invalid header accepted")

    payload = f"POST /post_body HTTP/1.1\r\nHost: {host}\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n1234"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Content-Length duplicado", "PASS" if status in (400, 411) else "FAIL", "400/411", f"status={status}", "Duplicate CL not rejected")

    payload = f"GET /%2e%2e/%2e%2e/secret HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Path traversal encoded", "PASS" if status in (400, 403, 404) else "FAIL", "400/403/404", f"status={status}", "Encoded traversal not blocked")

    # Slowloris (partial request)
    try:
        s = socket.create_connection((host, port), timeout=3)
        s.sendall(f"GET / HTTP/1.1\r\nHost: {host}\r\n".encode("utf-8"))
        time.sleep(1.5)
        s.close()
        print_result("Requests incompletas (slowloris)", "PASS", "No crash", "no crash")
    except Exception as exc:
        print_result("Requests incompletas (slowloris)", "FAIL", "No crash", f"{type(exc).__name__}: {exc}", "Socket handling error")


def test_methods_and_codes(base, server_cfg, locations):
    print_block("METHODS + CODES")

    r = safe_request("GET", urljoin(base, "/"))
    print_result("200 OK", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "Root not served")

    r = safe_request("POST", urljoin(base, "/"), data="x")
    print_result("405 Method Not Allowed", "PASS" if has_status(r, 405) else "FAIL", "405", f"status={getattr(r, 'status_code', r)}", "Allowed methods misconfigured")

    allow = getattr(r, "headers", {}).get("Allow") if hasattr(r, "headers") else None
    expected_allow = None
    root_loc = get_location_config(locations, "/")
    if root_loc and "allowed_methods" in root_loc:
        expected_allow = ", ".join(root_loc["allowed_methods"])
    if has_status(r, 405):
        ok = allow is not None and (expected_allow is None or all(m in allow for m in expected_allow.split(", ")))
        print_result("Allow header en 405", "PASS" if ok else "FAIL", f"Allow: {expected_allow or 'methods'}", f"Allow={allow}", "Allow header missing")
    else:
        print_result("Allow header en 405", "FAIL", "Allow: GET", f"status={getattr(r, 'status_code', r)}", "Expected 405 response")

    error_pages = server_cfg.get("error_page", []) if server_cfg else []
    error_405_path = None
    for entry in error_pages:
        if "405" in entry[:-1]:
            error_405_path = entry[-1]
            break
    if error_405_path:
        error_file = resolve_error_page_file(error_405_path, server_cfg)
        if error_file and error_file.exists() and has_status(r, 405) and hasattr(r, "text"):
            expected_snippet = error_file.read_text(encoding="utf-8", errors="ignore")[:50].strip()
            ok = expected_snippet in r.text
            print_result("Custom 405 body", "PASS" if ok else "FAIL", "405 + custom body", f"status={getattr(r, 'status_code', r)}", "error_page not applied")
        else:
            print_result("Custom 405 body", "SKIP", "error_page 405 file exists", "missing", "File not found")

    r = safe_request("GET", urljoin(base, "/this_path_should_not_exist_42"))
    print_result("404 Not Found", "PASS" if has_status(r, 404) else "FAIL", "404", f"status={getattr(r, 'status_code', r)}", "Routing should return 404")

    post_loc = get_location_config(locations, "/post_body")
    if post_loc and "allowed_methods" in post_loc and "POST" in post_loc["allowed_methods"]:
        r = safe_request("POST", urljoin(base, "/post_body"), data="x" * 10)
        print_result("POST /post_body", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "POST not handled")

        max_body = get_max_body_size(server_cfg, locations, "/post_body")
        if max_body > 0:
            r = safe_request("POST", urljoin(base, "/post_body"), data="x" * max_body)
            print_result("POST /post_body (limit)", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "Max body rejected")

            r = safe_request("POST", urljoin(base, "/post_body"), data="x" * (max_body + 1))
            print_result("POST /post_body (limit+1)", "PASS" if has_status(r, 413) else "FAIL", "413", f"status={getattr(r, 'status_code', r)}", "Max body not enforced")
        else:
            print_result("POST /post_body (limit)", "SKIP", "client_max_body_size", "not configured", "No max body size")
            print_result("POST /post_body (limit+1)", "SKIP", "client_max_body_size", "not configured", "No max body size")
    else:
        print_result("POST /post_body", "SKIP", "POST allowed", "not allowed", "Config disallows POST")

    r = safe_request("POST", urljoin(base, "/uploads/upload.txt"), data="hello")
    print_result("201/200 Created", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "Upload handler not working")

    r = safe_request("DELETE", urljoin(base, "/uploads/upload.txt"))
    print_result("204 No Content", "PASS" if has_status(r, 200, 202, 204, 404) else "FAIL", "200/202/204/404", f"status={getattr(r, 'status_code', r)}", "Delete handler not working")

    r = safe_request("POST", urljoin(base, "/uploads"), data="x" * 200)
    print_result("413 Payload Too Large", "PASS" if has_status(r, 413) else "FAIL", "413", f"status={getattr(r, 'status_code', r)}", "client_max_body_size not enforced")

    r = safe_request("OPTIONS", urljoin(base, "/"))
    print_result("OPTIONS", "PASS" if has_status(r, 200, 405, 501) else "FAIL", "200/405/501", f"status={getattr(r, 'status_code', r)}", "OPTIONS handling")

    r = safe_request("PUT", urljoin(base, "/"), data="x")
    print_result("PUT /", "PASS" if has_status(r, 405, 501) else "FAIL", "405/501", f"status={getattr(r, 'status_code', r)}", "PUT not rejected")

    r = safe_request("TRACE", urljoin(base, "/"))
    print_result("TRACE /", "PASS" if has_status(r, 405, 501) else "FAIL", "405/501", f"status={getattr(r, 'status_code', r)}", "TRACE not rejected")

    r = safe_request("DELETE", urljoin(base, "/"))
    print_result("DELETE /", "PASS" if has_status(r, 405, 501) else "FAIL", "405/501", f"status={getattr(r, 'status_code', r)}", "DELETE on / should be blocked")


def test_routes_and_files(base, server_cfg, locations):
    print_block("ROUTES + FILES")

    r = safe_request("GET", urljoin(base, "/"))
    ok = has_status(r, 200) and hasattr(r, "text") and ("index" in r.text.lower() or "html" in r.text.lower())
    print_result("Servir index.html", "PASS" if ok else "FAIL", "200 + body", f"status={getattr(r, 'status_code', r)}", "index not found or wrong root")

    dir_loc = get_location_config(locations, "/directory/")
    dir_root = resolve_path(dir_loc.get("root")[0]) if dir_loc and "root" in dir_loc else None
    dir_index = dir_loc.get("index", [None])[0] if dir_loc else None
    index_file = (dir_root / dir_index) if dir_root and dir_index else None

    r = safe_request("GET", urljoin(base, "/directory/"))
    if index_file and index_file.exists():
        expected_snippet = index_file.read_text(encoding="utf-8", errors="ignore")[:50].strip()
        ok = has_status(r, 200) and hasattr(r, "text") and (expected_snippet in r.text)
        print_result("Index en /directory/", "PASS" if ok else "FAIL", "200 + index file", f"status={getattr(r, 'status_code', r)}", "index not served")
    else:
        ok = has_status(r, 200) and hasattr(r, "text") and ("index of" in r.text.lower() or "youpi" in r.text.lower())
        print_result("Autoindex on", "PASS" if ok else "FAIL", "200 + listing", f"status={getattr(r, 'status_code', r)}", "autoindex disabled or wrong root")

    has_dir = "/directory" in locations
    r = safe_request("GET", urljoin(base, "/directory"), allow_redirects=False)
    if has_dir:
        ok = has_status(r, 200)
        print_result("/directory directo", "PASS" if ok else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "Unexpected redirect or error")
    else:
        redirect_ok = has_status(r, 301, 302, 307, 308) and hasattr(r, "headers") and "Location" in r.headers
        print_result("Directory redirect (/directory -> /directory/)", "PASS" if redirect_ok else "FAIL", "301/302/307/308 + Location", f"status={getattr(r, 'status_code', r)}", "Missing trailing slash redirect")

    r = safe_request("GET", urljoin(base, "/../secret"))
    print_result("Path traversal bloqueado", "PASS" if has_status(r, 400, 403, 404) else "FAIL", "400/403/404", f"status={getattr(r, 'status_code', r)}", "Path traversal not blocked")

    r = safe_request("GET", urljoin(base, "/img"))
    print_result("Archivos binarios/imagenes", "PASS" if has_status(r, 200, 301, 302, 404) else "FAIL", "200/301/302/404", f"status={getattr(r, 'status_code', r)}", "Static files or routing issue")

    r = safe_request("GET", urljoin(base, "/img/"))
    print_result("Directorio /img/", "PASS" if has_status(r, 200, 403, 404) else "FAIL", "200/403/404", f"status={getattr(r, 'status_code', r)}", "Directory handling for /img")

    r = safe_request("GET", urljoin(base, "/errors/405.html"))
    print_result("Custom error 405 file", "PASS" if has_status(r, 200, 404) else "FAIL", "200/404", f"status={getattr(r, 'status_code', r)}", "errors root missing")

    error_pages = server_cfg.get("error_page", []) if server_cfg else []
    error_404_path = None
    for entry in error_pages:
        if "404" in entry[:-1]:
            error_404_path = entry[-1]
            break

    if error_404_path:
        error_file = resolve_error_page_file(error_404_path, server_cfg)
        r = safe_request("GET", urljoin(base, error_404_path))
        print_result("Custom error page file", "PASS" if has_status(r, 200, 404) else "FAIL", "200/404", f"status={getattr(r, 'status_code', r)}", "errors root missing")

        if error_file and error_file.exists():
            expected_snippet = error_file.read_text(encoding="utf-8", errors="ignore")[:50].strip()
            r = safe_request("GET", urljoin(base, "/this_path_should_not_exist_42"))
            ok = has_status(r, 404) and hasattr(r, "text") and expected_snippet and expected_snippet in r.text
            print_result("Custom 404 body", "PASS" if ok else "FAIL", "404 + custom body", f"status={getattr(r, 'status_code', r)}", "error_page not applied")
        else:
            print_result("Custom 404 body", "SKIP", "error_page 404 file exists", "missing", "File not found")
    else:
        print_result("Custom error page file", "SKIP", "error_page 404 configured", "not configured", "No error_page 404")
        print_result("Custom 404 body", "SKIP", "error_page 404 configured", "not configured", "No error_page 404")


def test_cgi(base, server_cfg, locations):
    print_block("CGI")
    root_loc = get_location_config(locations, "/")
    cgi_ext = root_loc.get("cgi_extension", [None])[0] if root_loc else None
    cgi_path = root_loc.get("cgi_path", [None])[0] if root_loc else None
    if cgi_ext and cgi_path:
        r = safe_request("POST", urljoin(base, f"/archivo1{cgi_ext}"), data="test")
        print_result("CGI extension", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "CGI mapping or extension not configured")
    else:
        print_result("CGI extension", "SKIP", "cgi_extension + cgi_path", "not configured", "No CGI in config")


def test_uploads(base, server_cfg, locations):
    print_block("UPLOADS")

    r = safe_request("POST", urljoin(base, "/uploads/upload.txt"), data="hello")
    print_result("Upload simple", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "Upload dir or method not allowed")

    r = safe_request("GET", urljoin(base, "/uploads/upload.txt"))
    ok = has_status(r, 200) and hasattr(r, "text") and "hello" in r.text
    print_result("GET uploaded file", "PASS" if ok else "FAIL", "200 + body", f"status={getattr(r, 'status_code', r)}", "Upload not persisted")

    r = safe_request("POST", urljoin(base, "/uploads/upload%20space.txt"), data="hello")
    print_result("Upload nombre con espacios", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "URL decoding or file creation issue")

    r = safe_request("POST", urljoin(base, "/uploads/upload.txt"), data="overwrite")
    print_result("Upload overwrite", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "Overwrite not supported")

    r = safe_request("POST", urljoin(base, "/uploads"), data="x" * 200)
    print_result("Upload supera límite", "PASS" if has_status(r, 413) else "FAIL", "413", f"status={getattr(r, 'status_code', r)}", "client_max_body_size not enforced")

    max_body = get_max_body_size(server_cfg, locations, "/uploads")
    if max_body > 0:
        r = safe_request("POST", urljoin(base, "/uploads"), data="x" * max_body)
        print_result("Límite exacto permitido", "PASS" if has_status(r, 200, 201, 204) else "FAIL", "200/201/204", f"status={getattr(r, 'status_code', r)}", "Exact limit rejected")

        r = safe_request("POST", urljoin(base, "/uploads"), data="x" * (max_body + 1))
        print_result("Límite +1 rechazado", "PASS" if has_status(r, 413) else "FAIL", "413", f"status={getattr(r, 'status_code', r)}", "Limit not enforced")
    else:
        print_result("Límite exacto permitido", "SKIP", "client_max_body_size", "not configured", "No max body size in config")
        print_result("Límite +1 rechazado", "SKIP", "client_max_body_size", "not configured", "No max body size in config")


def test_concurrency(base):
    print_block("CONCURRENCY")
    errors = []

    def worker(idx):
        r = safe_request("GET", urljoin(base, "/"))
        if not has_status(r, 200):
            errors.append(idx)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(10)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    print_result("Múltiples clientes simultáneos", "PASS" if not errors else "FAIL", "all 200", f"errors={len(errors)}", "Concurrency handling issue")

    session = requests.Session()
    r1 = session.get(urljoin(base, "/"))
    r2 = session.get(urljoin(base, "/"))
    ok = has_status(r1, 200) and has_status(r2, 200)
    print_result("Keep-alive", "PASS" if ok else "FAIL", "2x 200", f"r1={getattr(r1, 'status_code', r1)} r2={getattr(r2, 'status_code', r2)}", "Connection reuse issue")


def test_pipelining(host, port):
    print_block("PIPELINING")
    payload = (
        f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: keep-alive\r\n\r\n"
        f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    )
    status, _, _, raw = raw_request(host, port, payload)
    count = raw.count("HTTP/1.1") if raw else 0
    ok = status == 200 and count >= 2
    print_result("Múltiples requests en 1 conexión", "PASS" if ok else "FAIL", ">=2 responses", f"responses={count}", "Pipelining not supported or parser bug")


def test_security(base, host, port):
    print_block("SECURITY")

    # Header injection attempt
    payload = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"X-Test: ok\r\n"
        f"X-Injection: a\r\nB: c\r\n\r\n"
    )
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Header injection bloqueado", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Header validation missing")

    payload = f"GET / HTTP/2.0\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("HTTP Version no soportada", "PASS" if status in (400, 505) else "FAIL", "400/505", f"status={status}", "Version check missing")

    payload = (
        f"POST /post_body HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Content-Length: 10\r\n\r\n"
        f"1234"
    )
    status, _, _, _ = raw_request(host, port, payload, timeout=3)
    print_result("Content-Length mismatch", "PASS" if status in (400, 408, 411, 413) else "FAIL", "400/408/411/413", f"status={status}", "Body length not enforced")

    payload = (
        f"POST /post_body HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Content-Length: 4\r\n"
        f"Transfer-Encoding: chunked\r\n\r\n"
        f"4\r\nWiki\r\n0\r\n\r\n"
    )
    status, _, _, _ = raw_request(host, port, payload, timeout=3)
    print_result("CL+TE smuggling", "PASS" if status in (400, 501) else "FAIL", "400/501", f"status={status}", "CL+TE ambiguity not rejected")

    payload = (
        f"POST /post_body HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Transfer-Encoding: chunked\r\n\r\n"
        f"Z\r\nWiki\r\n0\r\n\r\n"
    )
    status, _, _, _ = raw_request(host, port, payload, timeout=3)
    print_result("Chunk size inválido", "PASS" if status in (400, 501) else "FAIL", "400/501", f"status={status}", "Chunk parser too permissive")

    payload = f"GET /%00 HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("NULL byte en URI", "PASS" if status in (400, 404) else "FAIL", "400/404", f"status={status}", "NULL byte not rejected")

    payload = f"GET /%252e%252e/secret HTTP/1.1\r\nHost: {host}\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("Double-encoding traversal", "PASS" if status in (400, 403, 404) else "FAIL", "400/403/404", f"status={status}", "Double-encoded traversal not blocked")


def test_compatibility(base, host, port):
    print_block("COMPATIBILITY")

    # Connection: close
    payload = f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    status, headers, _, raw = raw_request(host, port, payload)
    closed = "connection: close" in raw.lower()
    print_result("Connection: close", "PASS" if status == 200 and closed else "FAIL", "200 + Connection: close", f"status={status}", "Connection header not respected")

    # Chunked request (if supported)
    payload = (
        f"POST /post_body HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Transfer-Encoding: chunked\r\n\r\n"
        f"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
    )
    status, _, _, _ = raw_request(host, port, payload)
    ok = status in (200, 400, 411, 501)
    print_result("Transfer-Encoding: chunked", "PASS" if ok else "FAIL", "200/400/411/501", f"status={status}", "Chunked handling not supported")

    status, headers, body, _ = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}\r\n\r\n")
    if headers.get("Content-Length") is not None:
        ok = headers.get("Content-Length").isdigit() and int(headers.get("Content-Length")) == len(body.encode("utf-8", errors="ignore"))
        print_result("Content-Length correcto", "PASS" if ok else "FAIL", "CL == body length", f"cl={headers.get('Content-Length')} body={len(body)}", "Content-Length mismatch")
    else:
        print_result("Content-Length correcto", "SKIP", "Content-Length header", "missing", "Chunked or unknown length")

    payload = f"GET / HTTP/1.0\r\n\r\n"
    status, _, _, _ = raw_request(host, port, payload)
    print_result("HTTP/1.0 request", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "HTTP/1.0 handling")


def test_misc(base, host, port):
    print_block("MISC")

    status, headers, body, raw = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Status line format", "PASS" if raw.startswith("HTTP/") else "FAIL", "HTTP/1.x", f"line={raw.splitlines()[0] if raw else ''}", "Invalid status line")

    status, headers, body, _ = raw_request(host, port, f"HEAD / HTTP/1.1\r\nHost: {host}\r\n\r\n")
    ok = body == "" or body == "\r\n"
    print_result("HEAD sin body", "PASS" if ok else "FAIL", "empty body", f"len={len(body)}", "HEAD returned body")

    status, _, _, _ = raw_request(host, port, f"GET / HTTP/1.2\r\nHost: {host}\r\n\r\n")
    print_result("HTTP/1.2 inválido", "PASS" if status in (400, 505) else "FAIL", "400/505", f"status={status}", "Version not rejected")

    status, _, _, _ = raw_request(host, port, f"OPTIONS * HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("OPTIONS *", "PASS" if status in (200, 405, 501) else "FAIL", "200/405/501", f"status={status}", "OPTIONS * not handled")

    status, _, _, _ = raw_request(host, port, f"GET    / HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Espacios extra en request line", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "Request line parsing")

    status, _, _, _ = raw_request(host, port, f"GET\t/ HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Tab en request line", "PASS" if status in (400, 405) else "FAIL", "400/405", f"status={status}", "Invalid request line accepted")

    status, _, _, _ = raw_request(host, port, f"GET /#frag HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Fragment en URI", "PASS" if status in (200, 400, 404) else "FAIL", "200/400/404", f"status={status}", "Fragment handling")

    status, _, _, _ = raw_request(host, port, f"GET /%ZZ HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Percent inválido", "PASS" if status in (400, 404) else "FAIL", "400/404", f"status={status}", "Invalid percent not rejected")

    status, _, _, _ = raw_request(host, port, f"GET /;param HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("Params en path", "PASS" if status in (200, 404) else "FAIL", "200/404", f"status={status}", "Path params handling")

    status, _, _, _ = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}\r\nX-Empty:\r\n\r\n")
    print_result("Header vacío", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "Empty header handling")

    status, _, _, _ = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}:1234\r\n\r\n")
    print_result("Host con puerto", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "Host header parsing")

    status, _, _, _ = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}\r\nHost: {host}\r\n\r\n")
    print_result("Host duplicado", "PASS" if status in (400, 431) else "FAIL", "400/431", f"status={status}", "Duplicate Host not rejected")

    status, _, _, _ = raw_request(host, port, f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n")
    print_result("Connection duplicado", "PASS" if status in (200, 400) else "FAIL", "200/400", f"status={status}", "Duplicate Connection handling")

    status, _, _, _ = raw_request(host, port, f"POST /post_body HTTP/1.1\r\nHost: {host}\r\nContent-Length: -1\r\n\r\n")
    print_result("Content-Length negativo", "PASS" if status in (400, 411) else "FAIL", "400/411", f"status={status}", "Negative CL not rejected")

    status, _, _, _ = raw_request(host, port, f"POST /post_body HTTP/1.1\r\nHost: {host}\r\nContent-Length: abc\r\n\r\n")
    print_result("Content-Length no numérico", "PASS" if status in (400, 411) else "FAIL", "400/411", f"status={status}", "Non-numeric CL not rejected")

    status, _, _, _ = raw_request(host, port, f"POST /post_body HTTP/1.1\r\nHost: {host}\r\nContent-Length: 0\r\n\r\n")
    print_result("POST con CL=0", "PASS" if status in (200, 204, 400) else "FAIL", "200/204/400", f"status={status}", "Zero-length POST handling")

    status, _, _, _ = raw_request(host, port, f"GET /uploads/ HTTP/1.1\r\nHost: {host}\r\n\r\n")
    print_result("GET /uploads/", "PASS" if status in (200, 403, 404) else "FAIL", "200/403/404", f"status={status}", "Uploads directory handling")


def test_redirects(config_text, base):
    print_block("REDIRECTS")
    if not config_has(config_text, "return") and not config_has(config_text, "redirect"):
        print_result("Redirects", "SKIP", "redirect configured", "not configured", "No redirect directive in config")
        return

    r = safe_request("GET", urljoin(base, "/redirect"), allow_redirects=False)
    ok = has_status(r, 301, 302, 307, 308)
    print_result("Redirects", "PASS" if ok else "FAIL", "301/302/307/308", f"status={getattr(r, 'status_code', r)}", "Redirect not configured")


def test_logs(config_text):
    print_block("LOGS")
    if not ("access_log" in config_text or "error_log" in config_text):
        print_result("Logs", "SKIP", "log files configured", "not configured", "No log files configured")
        return
    print_result("Logs", "SKIP", "log files exist", "not checked", "Log checks depend on configured paths")


def test_multiple_servers(config_text):
    print_block("CONFIG CHECKS")
    servers = count_server_blocks(config_text)
    if servers < 2:
        print_result("Múltiples servidores", "SKIP", "2+ server blocks", f"found={servers}", "Only one server configured")
    else:
        print_result("Múltiples servidores", "PASS", "2+ server blocks", f"found={servers}")

    names = parse_server_names(config_text)
    if len(names) >= 2:
        print_result("server_name routing", "PASS", "2+ names", f"names={','.join(names[:3])}")
    else:
        print_result("server_name routing", "SKIP", "2+ names", f"names={','.join(names) if names else 'none'}", "Not enough server_name entries")


def test_tester_output(output_path):
    print_block("TESTER OUTPUT")
    if not output_path:
        print_result("Tester output", "SKIP", "--tester-output file", "missing", "Provide ./tester output")
        return
    path = Path(output_path)
    if not path.exists():
        print_result("Tester output", "SKIP", "--tester-output file", "not found", "File path invalid")
        return
    text = path.read_text(encoding="utf-8", errors="ignore")
    fails = len(re.findall(r"\b(FAIL|KO|ERROR)\b", text))
    print_result("Tester output", "PASS" if fails == 0 else "FAIL", "0 FAIL/KO/ERROR", f"count={fails}", "Tester indicates failures")


def test_tester_scenarios(base, server_cfg, locations):
    print_block("TESTER SCENARIOS")
    dir_loc = locations.get("/directory/") or locations.get("/directory")
    if not dir_loc:
        print_result("/directory config", "SKIP", "location /directory", "not configured", "Missing location")
        return

    dir_root = resolve_path(dir_loc.get("root")[0]) if "root" in dir_loc else None
    dir_index = dir_loc.get("index", [None])[0]

    r = safe_request("GET", urljoin(base, "/directory"))
    print_result("GET /directory", "PASS" if has_status(r, 200, 301, 302, 307, 308) else "FAIL", "200/3xx", f"status={getattr(r, 'status_code', r)}", "Directory handling")

    r = safe_request("GET", urljoin(base, "/directory/"))
    if dir_index and dir_root and (dir_root / dir_index).exists():
        expected_snippet = (dir_root / dir_index).read_text(encoding="utf-8", errors="ignore")[:50].strip()
        ok = has_status(r, 200) and hasattr(r, "text") and expected_snippet in r.text
        print_result("GET /directory/ (index)", "PASS" if ok else "FAIL", "200 + index", f"status={getattr(r, 'status_code', r)}", "Index not served")
    else:
        ok = has_status(r, 200)
        print_result("GET /directory/", "PASS" if ok else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "Directory not served")

    if dir_root:
        youpi_bad = dir_root / "youpi.bad_extension"
        youpi_bla = dir_root / "youpi.bla"
        nop_other = dir_root / "nop" / "other.pouic"
        yeah_bad = dir_root / "Yeah" / "not_happy.bad_extension"

        if youpi_bad.exists():
            r = safe_request("GET", urljoin(base, "/directory/youpi.bad_extension"))
            print_result("GET /directory/youpi.bad_extension", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "File not served")
        else:
            print_result("GET /directory/youpi.bad_extension", "SKIP", "file exists", "missing", "Missing fixture")

        if youpi_bla.exists():
            r = safe_request("GET", urljoin(base, "/directory/youpi.bla"))
            print_result("GET /directory/youpi.bla", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "File not served")

            r = safe_request("POST", urljoin(base, "/directory/youpi.bla"), data="x" * 100)
            print_result("POST /directory/youpi.bla", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "CGI or POST handling")

            max_body = get_max_body_size(server_cfg, locations, "/directory/")
            if max_body > 0:
                r = safe_request("POST", urljoin(base, "/directory/youpi.bla"), data="x" * (max_body + 1))
                print_result("POST /directory/youpi.bla (limit+1)", "PASS" if has_status(r, 413) else "FAIL", "413", f"status={getattr(r, 'status_code', r)}", "Max body not enforced")
            else:
                print_result("POST /directory/youpi.bla (limit+1)", "SKIP", "client_max_body_size", "not configured", "No max body size")
        else:
            print_result("GET /directory/youpi.bla", "SKIP", "file exists", "missing", "Missing fixture")
            print_result("POST /directory/youpi.bla", "SKIP", "file exists", "missing", "Missing fixture")

        r = safe_request("GET", urljoin(base, "/directory/oulalala"))
        print_result("GET /directory/oulalala", "PASS" if has_status(r, 404) else "FAIL", "404", f"status={getattr(r, 'status_code', r)}", "Not found not handled")

        r = safe_request("GET", urljoin(base, "/directory/nop"))
        print_result("GET /directory/nop", "PASS" if has_status(r, 200, 301, 302, 307, 308) else "FAIL", "200/3xx", f"status={getattr(r, 'status_code', r)}", "Directory handling")

        r = safe_request("GET", urljoin(base, "/directory/nop/"))
        print_result("GET /directory/nop/", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "Directory not served")

        if nop_other.exists():
            r = safe_request("GET", urljoin(base, "/directory/nop/other.pouic"))
            print_result("GET /directory/nop/other.pouic", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "File not served")
        else:
            print_result("GET /directory/nop/other.pouic", "SKIP", "file exists", "missing", "Missing fixture")

        r = safe_request("GET", urljoin(base, "/directory/nop/other.pouac"))
        print_result("GET /directory/nop/other.pouac", "PASS" if has_status(r, 404) else "FAIL", "404", f"status={getattr(r, 'status_code', r)}", "Not found not handled")

        r = safe_request("GET", urljoin(base, "/directory/Yeah"))
        print_result("GET /directory/Yeah", "PASS" if has_status(r, 404) else "FAIL", "404", f"status={getattr(r, 'status_code', r)}", "Should be not found")

        if yeah_bad.exists():
            r = safe_request("GET", urljoin(base, "/directory/Yeah/not_happy.bad_extension"))
            print_result("GET /directory/Yeah/not_happy.bad_extension", "PASS" if has_status(r, 200) else "FAIL", "200", f"status={getattr(r, 'status_code', r)}", "File not served")
        else:
            print_result("GET /directory/Yeah/not_happy.bad_extension", "SKIP", "file exists", "missing", "Missing fixture")
    else:
        print_result("/directory root", "SKIP", "root configured", "missing", "Missing root directive")


def run_all(base, host, port, config_text, server_cfg, locations, tester_output):
    test_basic_methods(base)
    test_http_parsing(base, host, port)
    test_methods_and_codes(base, server_cfg, locations)
    test_routes_and_files(base, server_cfg, locations)
    test_cgi(base, server_cfg, locations)
    test_uploads(base, server_cfg, locations)
    test_tester_scenarios(base, server_cfg, locations)
    test_concurrency(base)
    test_pipelining(host, port)
    test_security(base, host, port)
    test_compatibility(base, host, port)
    test_misc(base, host, port)
    test_redirects(config_text, base)
    test_logs(config_text)
    test_multiple_servers(config_text)
    test_tester_output(tester_output)


# =========================
# MAIN
# =========================

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Full test suite for Webserv")
    parser.add_argument("--base", default=DEFAULT_BASE)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--server-bin", default=str(DEFAULT_SERVER_BIN))
    parser.add_argument("--run-build", action="store_true")
    parser.add_argument("--run-config-tests", action="store_true")
    parser.add_argument("--tester-output", default="")
    args = parser.parse_args()

    config_text = read_config_text(Path(args.config))
    server_cfg = parse_server_directives(config_text)
    locations = parse_locations(config_text)

    print_block("INFO")
    print(f"Base: {args.base}")
    print(f"Host: {args.host}:{args.port}")
    print(f"Config: {args.config}")

    if args.run_build:
        print_block("BUILD")
        test_build(["make", "-s"], expect_flags=["-Wall", "-Wextra", "-Werror"])

    if args.run_config_tests:
        print_block("CONFIGURATION")
        test_invalid_config(Path(args.server_bin))

    run_all(args.base, args.host, args.port, config_text, server_cfg, locations, args.tester_output)
    print_summary()

    if TOTAL - PASSED - SKIPPED > 0:
        sys.exit(1)
