Here is a consolidated summary of all findings across the three reports ([switchMode_analysis.md](file:///Users/p/Desktop/webserv/doc/switchMode_analysis.md), [Server_analysis.md](file:///Users/p/Desktop/webserv/doc/Server_analysis.md), and [RequestHandler_analysis.md](file:///Users/p/Desktop/webserv/doc/RequestHandler_analysis.md)), categorized by impact and type.

---

### 1. 42 Rules & Subject Violations (Instant Evaluation Failures)

These are strict requirements from the 42 subject and your [non-blocking I/O guidelines](file:///Users/p/Desktop/webserv/.agents/skills/non-blocking-io/SKILL.md) that are currently broken:

* **Checking `errno` after `read` / `write`**:
  * *Where*: [src/switchMode.cpp:L29-L34](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L29-L34) (`errno == EINTR`, `errno == EAGAIN || errno == EWOULDBLOCK`).
  * *Violation*: Checking `errno` to adjust server behavior after I/O is strictly forbidden. The multiplexer's job is to guarantee readiness before reading.
* **Simultaneous Read/Write Monitoring (Epoll)**:
  * *Where*: [src/switchMode.cpp:L166](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L166).
  * *Violation*: In epoll, `_switchToWriteMode` sets `EPOLLOUT | EPOLLRDHUP` and strips `EPOLLIN`. The subject requires monitoring read and write simultaneously so incoming messages and disconnections aren't missed during writes.
* **Loop Starvation / Single-read violation**:
  * *Where*: [src/switchMode.cpp:L23-L58](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L23-L58).
  * *Violation*: `while (true)` loops until `read()` fails. During wire-speed uploads on `127.0.0.1`, this starves the event loop, freezing all other clients. The rule is to read once per multiplexer event up to `BUFFER_SIZE`.

---

### 2. Critical Logic Bugs & Data Loss

* **Request parsed AFTER upload directory resolution** ([src/switchMode.cpp:L74-L83](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L74-L83)):
  * `resolveUploadDirectory()` calls `req.getPath()` *before* `request.parse()`. At this point, the path is an empty string `""`, corrupting location matching on every request.
* **Pipelining & Body Truncation** ([src/switchMode.cpp:L94](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L94)):
  * Calling `session.request_buffer.clear()` wipes all data in the buffer. If a client sends pipelined HTTP requests or extra data in the same packet, everything past the first request is permanently deleted.
* **Premature Disconnect on Half-Closed Sockets (`EV_EOF` / `EPOLLRDHUP`)** ([src/Server.cpp:L226](file:///Users/p/Desktop/webserv/src/Server.cpp#L226), [L283](file:///Users/p/Desktop/webserv/src/Server.cpp#L283)):
  * In the event loop, disconnect flags are evaluated in an `if ... else if` before `EVFILT_READ` / `EPOLLIN`. When a client sends a request and immediately shuts down its writing half (like `curl -d`), the server closes the connection and discards the unread request data.
* **Empty 0-Byte Files Cause False 404** ([src/RequestHandler.cpp:L173](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L173)):
  * `if (content.empty()) return handleError(404, ...);`. Valid files that are 0 bytes on disk are mistaken for non-existent files.

---

### 3. Configuration & Routing Disconnects

* **Hardcoded Web Root (Causes 404 on `GET /`)** ([src/RequestHandler.cpp:L170](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L170)):
  * `handleStaticFile()` hardcodes `"www" + path` (looking for `./www/index.html`). It completely ignores `root` directives defined in `default.conf` (e.g. `root www/my_website/`).
* **Misassigned `wwwRoot`** ([src/switchMode.cpp:L75](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L75)):
  * `switchMode.cpp` sets `request.setWwwRoot(upload_dir)`, pointing the web root to `"www/uploads"` instead of the server's root.
* **Ignored `allowed_methods` (Causes 405 on `POST /`)** ([src/RequestHandler.cpp:L29-L70](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L29-L70)):
  * `LocationConfig::getAllowedMethods()` is parsed from the config but **never queried**. `RequestHandler` hardcodes allowed methods by URL: `/upload` only allows `POST`, while `/` only allows `GET`.
* **Missing `autoindex` Implementation** ([src/RequestHandler.cpp:L166-L175](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L166-L175)):
  * Even with `autoindex on;` in the config, missing `index.html` immediately returns 404 instead of generating a directory listing.
* **Virtual Host Selection Bypassed** ([src/switchMode.cpp:L67](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L67)):
  * Unconditionally selects `session.virtual_host[0]` without checking the HTTP `Host:` header, breaking multi-virtual-host setups sharing an IP/Port.

---

### 4. Stability, Process Crashes & Resource Leaks

* **Missing `SIGPIPE` Ignore**:
  * Nowhere in the project is `SIGPIPE` ignored. If a client terminates before the server finishes sending a response, the next `send()` kills the entire server process instantly.
* **Unhandled `EINTR` Crashes the Server** ([src/Server.cpp:L211](file:///Users/p/Desktop/webserv/src/Server.cpp#L211), [L267](file:///Users/p/Desktop/webserv/src/Server.cpp#L267)):
  * When `kevent()` or `epoll_wait()` is interrupted by a signal (e.g., CGI exit `SIGCHLD`), it returns `-1` with `errno == EINTR`. The server throws `std::runtime_error` and aborts instead of continuing the loop.
* **Low Listen Backlog (`listen(fd, 10)`)** ([src/Server.cpp:L99](file:///Users/p/Desktop/webserv/src/Server.cpp#L99)):
  * A backlog queue of 10 causes the kernel to reject connections with `ECONNREFUSED` during concurrency / stress tests.
* **Resource Leaks on Server Destruction** ([src/Server.cpp:L52-L58](file:///Users/p/Desktop/webserv/src/Server.cpp#L52-L58)):
  * `~Server()` closes listening sockets, but leaks all open client file descriptors in `clients_`.
* **Stale Map Entry on Epoll/Kqueue Registration Failure** ([src/Server.cpp:L136](file:///Users/p/Desktop/webserv/src/Server.cpp#L136)):
  * `clients_[new_socket]` is inserted before registration; if `kevent()` / `epoll_ctl()` fails, the fd is closed but the session remains orphaned in `clients_`.

---

### Summary Matrix

| Category | Primary File | Impact | Priority |
| :--- | :--- | :--- | :--- |
| **`errno` checking after read/write** | `switchMode.cpp` | 42 evaluation penalty | 🔴 High |
| **Premature EOF disconnect** | `Server.cpp` | Drops valid requests on close | 🔴 High |
| **Parsing order (`req.getPath()` empty)** | `switchMode.cpp` | Corrupts location matching & roots | 🔴 High |
| **Missing `SIGPIPE` ignore** | `Server.cpp` / `main.cpp` | Server crashes on client reset | 🔴 High |
| **Hardcoded `"www"` & ignored config root** | `RequestHandler.cpp` | `404 Not Found` on `GET /` | 🟡 Medium |
| **Hardcoded methods & ignored `allowed_methods`** | `RequestHandler.cpp` | `405 Method Not Allowed` on `POST /` | 🟡 Medium |
| **Pipelining data loss (`buffer.clear()`)** | `switchMode.cpp` | Drops subsequent HTTP requests | 🟡 Medium |
| **Low listen backlog (`listen(fd, 10)`)** | `Server.cpp` | Fails stress tests | 🟡 Medium |