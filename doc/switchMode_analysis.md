# Analysis Report: `src/switchMode.cpp`

## 1. Executive Summary

The file [`src/switchMode.cpp`](file:///Users/p/Desktop/webserv/src/switchMode.cpp) contains core socket event handling routines for client connections in the `Server` class:
- `Server::handleClientRead(int client_fd, int multiplexer_fd)`
- `Server::handleClientWrite(int client_fd, int multiplexer_fd)`
- `Server::_switchToWriteMode(int client_fd, int multiplexer_fd)`
- `Server::_switchToReadMode(int client_fd, int multiplexer_fd)`

While named `switchMode.cpp`, this file acts as the primary client I/O controller. A code audit reveals critical logical ordering bugs, buffer handling flaws, and deviations from 42 subject rules / non-blocking I/O specifications.

---

## 2. Critical Bugs & Logical Flaws

### 2.1 Request Parsing Order Bug (Path Uninitialized)
- **Location**: [`src/switchMode.cpp:L74-L83`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L74-L83)
- **Problem**:
  ```cpp
  std::string upload_dir = resolveUploadDirectory(active_server, request);
  request.setWwwRoot(upload_dir);
  request.setUploadsDirectory(upload_dir);
  request.setClientId(client_fd);
  ...
  request.parse(session.request_buffer);
  ```
  `resolveUploadDirectory()` relies on `request.getPath()` to find the matching `LocationConfig`. However, `request.parse()` has **not** been executed yet at line 74. At this point, `request.getPath()` is an empty string (`""`).
- **Impact**:
  - The server always fails to match the targeted location block (e.g. `/upload`, `/cgi-bin`) during path resolution.
  - Setting `wwwRoot` to `upload_dir` (`request.setWwwRoot(upload_dir)`) assigns the upload directory (e.g. `"www/uploads"`) to `wwwRoot` instead of the configured document root (e.g. `"www/my_website"`), contributing to 404 errors on static file requests (see [`doc/RequestHandler_analysis.md`](file:///Users/p/Desktop/webserv/doc/RequestHandler_analysis.md)).
- **Remediation**:
  Move `request.parse(session.request_buffer)` before any routing, directory resolution, or location matching. Set `request.setWwwRoot()` to the server or location root, not the upload directory.

### 2.2 Buffer Truncation & Pipelining Data Loss
- **Location**: [`src/switchMode.cpp:L94`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L94)
- **Problem**:
  ```cpp
  session.request_buffer.clear();
  ```
  Once a request is complete and parsed, `request_buffer.clear()` wipes all data stored in the buffer.
- **Impact**:
  If a client sends multiple requests over a persistent connection (HTTP keep-alive/pipelining) or if extra bytes arrived in the same read buffer, all subsequent bytes belonging to the next request are permanently destroyed.
- **Remediation**:
  Only erase the consumed bytes:
  ```cpp
  session.request_buffer.erase(0, session.request_size);
  ```

### 2.3 Duplication of Mode-Switch Logic
- **Location**: [`src/switchMode.cpp:L97-L104`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L97-L104)
- **Problem**:
  ```cpp
  #ifdef USE_KQUEUE
          struct kevent ev;
          EV_SET(&ev, client_fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, NULL);
          kevent(multiplexer_fd, &ev, 1, NULL, 0, NULL);
  #else
          _switchToWriteMode(client_fd, multiplexer_fd);
  #endif
  ```
  `_switchToWriteMode(client_fd, multiplexer_fd)` already implements this exact `#ifdef USE_KQUEUE` branch. Inlining it in `handleClientRead` bypasses encapsulation and duplicates event registration code.
- **Remediation**:
  Directly call `_switchToWriteMode(client_fd, multiplexer_fd);`.

---

## 3. Compliance with Non-Blocking I/O & 42 Rules

### 3.1 Prohibited `errno` Checks After I/O
- **Location**: [`src/switchMode.cpp:L29-L34`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L29-L34)
- **Problem**:
  ```cpp
  if (errno == EINTR)
      continue;
  if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
  ```
  Per the project's non-blocking rules ([`non-blocking-io/SKILL.md`](file:///Users/p/Desktop/webserv/.agents/skills/non-blocking-io/SKILL.md#L21-L24)):
  > *"Checking the value of errno to adjust the server behaviour is strictly forbidden after performing a read or write operation (because readiness guarantees they won't block)."*
- **Impact**:
  In 42 evaluations, inspecting `errno` after `read/recv` or `write/send` is a common failure criterion.

### 3.2 Loop Starvation (`while (true)` Read Loop)
- **Location**: [`src/switchMode.cpp:L23-L58`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L23-L58)
- **Problem**:
  The `while (true)` loop repeatedly calls `read()` until `EAGAIN` or `EWOULDBLOCK` is returned.
- **Impact**:
  In a level-triggered single-threaded multiplexer (`epoll` or `kqueue`), looping inside a single connection handler can starve other clients if a connection continuously streams data (or on high load).
- **Remediation**:
  Read once per multiplexer notification (up to `BUFFER_SIZE`) and let the multiplexer trigger another event when more data is available.

### 3.3 Asymmetry in Simultaneous Read/Write Monitoring
- **Location**: [`src/switchMode.cpp:L166`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L166) vs [`L162`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L162)
- **Problem**:
  - In `kqueue`, `EVFILT_READ` remains registered while `EVFILT_WRITE` is added with `EV_ONESHOT`.
  - In `epoll`, `_switchToWriteMode` modifies the event mask to `EPOLLOUT | EPOLLRDHUP`, disabling `EPOLLIN`.
- **Impact**:
  While a large response is being transmitted over epoll, incoming client messages or standard disconnects on `EPOLLIN` are temporarily ignored, violating the requirement to monitor read and write simultaneously.

---

## 4. Architecture & Virtual Host Selection

1. **Hardcoded Virtual Host**:
   - [`switchMode.cpp:L67`](file:///Users/p/Desktop/webserv/src/switchMode.cpp#L67):
     ```cpp
     const ServerConfig* active_server = session.virtual_host.empty() ? NULL : session.virtual_host[0];
     ```
     `session.virtual_host` contains all `ServerConfig`s bound to that specific IP/Port. Choosing `[0]` ignores the HTTP `Host:` header, breaking multi-host configurations.
2. **Dead / Unused Session Fields**:
   - In [`inc/Server.hpp`](file:///Users/p/Desktop/webserv/inc/Server.hpp#L28-L46), `ClientSession` contains `response_buffer` and `bytes_sent`. `handleClientWrite` instead mutates `session.response` using `erase()`. Either remove the unused fields or standardize on buffered offset tracking.
