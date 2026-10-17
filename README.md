*This project has been created as part of the 42 curriculum by pepaloma, pmendez-.*

# Web Server (webserv)

## Description

webserv is an HTTP/1.1 web server written from scratch in C++98. The goal of the project is to understand how a web server works under the hood: the HTTP protocol, TCP sockets, non-blocking I/O and event-driven programming.

The server is single-threaded and handles many clients at the same time with **one** event multiplexer (`kqueue` on macOS, `epoll` on Linux) for every I/O operation: listening sockets, client sockets and CGI pipes. Its behaviour is defined by an NGINX-like configuration file: ports, roots, allowed methods, redirects, directory listing, uploads, error pages, body size limits and CGI.

## Instructions

### Compilation

```bash
make          # builds ./webserv
make clean    # removes the object files
make fclean   # removes the object files and the executable
make re       # rebuilds everything
```

The code compiles with `c++ -Wall -Wextra -Werror -std=c++98`. On macOS the Makefile uses `kqueue`; on Linux, `epoll`.

### Execution

```bash
./webserv [configuration_file]
```

If no configuration file is given, `etc/default.conf` is used (the subject allows a default path).

With `etc/default.conf`:

| URL | What it shows |
|---|---|
| `http://localhost:8080/` | Static website |
| `http://localhost:8080/showUploads` | Upload, list, download and delete files |
| `http://localhost:8080/uploads/` | Directory listing (autoindex) |
| `http://localhost:8080/cgi/example.py` | Python CGI (GET and POST) |
| `http://localhost:8080/old` | 301 redirect to `/` |
| `http://127.0.0.1:8081/` | Second site: another interface, port and root, with a 100-byte body limit |

Stop the server with `Ctrl+C`: it closes every connection and CGI process cleanly.

### Tests

```bash
python3 tst/tester.py                 # functional tests (the server is started automatically)
python3 tst/tester.py --tester42      # simulation of the official 42 tester (add --big for 100 MB bodies)
bash tst/check_single_poll.sh         # shows that a single kqueue/epoll is used
./webserv & python3 tst/stress.py     # stress test against a running server
```

Official 42 tester (Linux binaries, see `doc/testers.md`):

```bash
bash tst/tests_42/setup.sh
./webserv tst/tests_42/tester.conf
./tst/tests_42/tester http://localhost:8080
```

## Features

- **Methods:** GET, HEAD, POST and DELETE; other methods answer `501 Not Implemented`
- **Static website**, with a default `index` file for directories and optional **directory listing** (`autoindex`), as HTML or JSON
- **File uploads:** `multipart/form-data` (several files per request) or raw bodies
- **CGI by file extension** (`cgi .py /usr/bin/python3;`), run in the script's directory, without blocking the server; several CGI types can be configured
- **`Transfer-Encoding: chunked`** requests (un-chunked before reaching the CGI or the upload)
- **Several servers** on different interfaces and ports; two servers on the same interface:port are rejected (virtual hosts are not implemented)
- **Default error pages** (template `www/err/template.html`) and custom pages with `error_page`
- **Keep-alive and pipelining**; `Connection: close` and HTTP/1.0 are honoured
- **Timeouts:** idle clients (60 s), incomplete request bodies (2 s, `408`), CGI scripts that stop responding (10 s, `504`)
- **Limits and validation:** `client_max_body_size` per server or per location (`413`, checked before the body arrives), header size (`431`), URI length (`414`), invalid headers or `Content-Length` (`400`), path traversal (`400`)
- **Coloured log**, one line per request with its status code and the reason of errors

## Technical Choices

- **One multiplexer for everything.** `kqueue` (macOS) or `epoll` (Linux), selected at compile time. Listening sockets, clients and CGI pipes are registered in the same instance, and the only blocking wait is in `Server::run()`.
- **One read or write per event.** After an event, each handler does a single `accept`/`recv`/`send`/`read`/`write` and returns to the loop; whatever is left is done on the next event. `errno` is never checked after a read or write. Regular disk files (configuration, static files, uploads) use `std::ifstream`/`std::ofstream`, as the subject allows.
- **Non-blocking CGI.** `fork()` + `execve()` with non-blocking pipes on the server side. The request body is written to the script and its output is read through the same event loop, so the server keeps serving other clients while a script runs. Children are reaped with `waitpid(WNOHANG)`.
- **Configuration-driven routing.** Each request is matched to the longest-prefix `location`, which decides the allowed methods, redirect, root, index, autoindex, upload and CGI. Nothing is hardcoded in the request handler.
- **Large bodies.** Responses are sent from an offset instead of erasing the sent bytes, chunked bodies are scanned incrementally, and request bodies and CGI output are moved with `swap()` instead of being copied, so 100 MB requests are handled efficiently.
- **C++98 and allowed functions only.** No external libraries. Functions such as `inet_addr` or `realpath`, which are not in the allowed list, are replaced by small helpers of our own.

## Configuration File Structure

```nginx
server {
    listen 127.0.0.1:8080;            # interface:port (or just the port)
    root www/;
    client_max_body_size 10M;         # K, M and G suffixes are accepted
    error_page 404 www/err/404.html;  # optional; the default template is used otherwise

    location / {
        index index.html;             # default: index.html
        allowed_methods GET;
    }
    location /uploads {
        root www/uploads;             # /uploads/a.png -> www/uploads/a.png
        autoindex on;                 # default: off
        upload_enabled on;
        upload_path www/uploads;
        client_max_body_size 1M;      # overrides the server limit
        allowed_methods GET POST DELETE;
    }
    location /cgi {
        root www/cgi;
        cgi .py /usr/bin/python3;     # extension -> interpreter (absolute path)
        allowed_methods GET POST;
    }
    location /old {
        redirect 301 /;
        allowed_methods GET;
    }
}
```

More examples are in `etc/` (`default.conf` is the demonstration configuration) and the full grammar is documented in `doc/`.

## Resources

- **Beej's Guide to Network Programming:** sockets, ports and network communication in C/C++.
- **RFC 9110 and RFC 9112 (HTTP/1.1):** HTTP semantics, message syntax, status codes and chunked transfer encoding.
- **RFC 3875 (CGI 1.1):** the environment variables and the input/output contract between the server and a CGI script.
- **kqueue/kevent and epoll man pages:** event-driven, non-blocking I/O on macOS/BSD and Linux.
- **NGINX documentation:** reference for the configuration file syntax and for the expected behaviour (directory redirects, `index`, `autoindex`, `client_max_body_size`).
- https://dev.to/frosnerd/writing-a-simple-tcp-server-using-kqueue-cah
- https://dev.to/frosnerd/explain-non-blocking-i-o-like-i-m-five-2a5f
- https://www.youtube.com/watch?v=Qx6wgxrvG3s

### How AI was used

AI tools were used as a support tool alongside our own work:

- **Reference and debugging:** questions about HTTP, sockets and kqueue/epoll, `Makefile` recipes and syntax errors.
- **Code review:** checking the code against the subject sheet, and support with some of the fixes (I/O handling, CGI and routing).
- **Tests and documentation:** the test scripts in `tst/` and parts of `doc/`.
