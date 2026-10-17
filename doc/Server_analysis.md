# Analysis Report: `src/Server.cpp`

## 1. Executive Summary

The file [`src/Server.cpp`](file:///Users/p/Desktop/webserv/src/Server.cpp) implements the foundational networking and lifecycle components of the web server:
- Socket creation, binding, and listening: `Server::setupListeningSockets()`
- Event demultiplexing loop: `Server::run()` (`kqueue` on macOS / `epoll` on Linux)
- Connection handling: `Server::handleNewConnection()`
- Disconnection cleanup: `Server::handleClientDisconnect()`
- Non-blocking socket configuration helper: `setNonBlocking(int fd)`

This analysis evaluates `Server.cpp` against the project's non-blocking I/O guidelines, concurrency requirements, error handling, and reliability under load.

---

## 2. Critical Bugs & Concurrency Hazards

### 2.1 Premature Disconnect on Pending Data (`EV_EOF` / `EPOLLRDHUP`)
- **Location**:
  - Kqueue loop: [`src/Server.cpp:L226-L232`](file:///Users/p/Desktop/webserv/src/Server.cpp#L226-L232)
  - Epoll loop: [`src/Server.cpp:L283-L289`](file:///Users/p/Desktop/webserv/src/Server.cpp#L283-L289)
- **Problem**:
  ```cpp
  // Kqueue:
  if (events[n].flags & EV_EOF) {
    handleClientDisconnect(current_fd, kq);
  }
  else if (events[n].filter == EVFILT_READ) {
    handleClientRead(current_fd, kq);
  }

  // Epoll:
  if (events[n].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    handleClientDisconnect(current_fd, epoll_fd);
  }
  else if (events[n].events & EPOLLIN) {
    handleClientRead(current_fd, epoll_fd);
  }
  ```
- **Impact**:
  When an HTTP client sends data and closes its write end of the socket (sending a `FIN` packet, common with HTTP clients like `curl -d`, benchmarking tools, or scripts), both `EVFILT_READ` and `EV_EOF` (or `EPOLLIN` and `EPOLLRDHUP`) are set simultaneously.
  Because the disconnect check is in the `if` and the read check is in the `else if`, the server immediately closes the socket and **discards unread request data** without processing it. In kqueue, `events[n].data` may still hold pending bytes that must be drained before closing.
- **Remediation**:
  Always process `EVFILT_READ` / `EPOLLIN` before closing on EOF, or only disconnect when `read()` returns 0 bytes.

### 2.2 Unhandled Signal Interruption (`EINTR`) Crashes Server
- **Location**:
  - Kqueue: [`src/Server.cpp:L211-L215`](file:///Users/p/Desktop/webserv/src/Server.cpp#L211-L215)
  - Epoll: [`src/Server.cpp:L267-L271`](file:///Users/p/Desktop/webserv/src/Server.cpp#L267-L271)
- **Problem**:
  ```cpp
  int nev = kevent(kq, NULL, 0, events, WORKER_CONNECTIONS, NULL);
  if (nev < 0) {
    close(kq);
    throw std::runtime_error(strerror(errno));
  }
  ```
  If any signal (such as `SIGCHLD` from child CGI processes, `SIGWINCH` on terminal resize, or profiler signals) is received while the multiplexer is blocked, `kevent` and `epoll_wait` return `-1` with `errno == EINTR`.
- **Impact**:
  The server throws `std::runtime_error` and terminates unexpectedly instead of retrying the wait loop.
- **Remediation**:
  Check `if (errno == EINTR) continue;` before treating `nev < 0` / `nfds < 0` as a fatal error.

### 2.3 Stale Session Map Entry on Multiplexer Registration Failure
- **Location**: [`src/Server.cpp:L136-L157`](file:///Users/p/Desktop/webserv/src/Server.cpp#L136-L157)
- **Problem**:
  ```cpp
  clients_[new_socket].virtual_host = listen_sockets_[listen_fd];
  ...
  if (kevent(multiplexer_fd, &ev, 1, NULL, 0, NULL) < 0) {
    std::cerr << "Failed to add new socket to kqueue: " << strerror(errno) << std::endl;
    close(new_socket);
  }
  ```
  If `kevent()` or `epoll_ctl()` fails to register the new client socket, `new_socket` is closed, but `clients_[new_socket]` was already inserted.
- **Impact**:
  A stale `ClientSession` remains in `clients_`, leaking memory and corrupting state if the OS reuses the same file descriptor integer for a future socket.
- **Remediation**:
  Only insert into `clients_` after `kevent()` / `epoll_ctl()` succeeds, or call `clients_.erase(new_socket)` on failure.

---

## 3. Resource Management & Leaks

### 3.1 Client Sockets Leaked on Server Destruction
- **Location**: [`src/Server.cpp:L52-L58`](file:///Users/p/Desktop/webserv/src/Server.cpp#L52-L58)
- **Problem**:
  ```cpp
  Server::~Server() {
    for (std::map<int, std::vector<ServerConfig const *> >::iterator it =
             listen_sockets_.begin();
         it != listen_sockets_.end(); ++it) {
      close(it->first);
    }
  }
  ```
  The destructor closes listening sockets in `listen_sockets_`, but completely ignores active client file descriptors stored in `clients_`.
- **Remediation**:
  Iterate over `clients_` and close all connected client sockets in `~Server()`.

### 3.2 Socket Leak if `setNonBlocking()` Throws
- **Location**: [`src/Server.cpp:L104`](file:///Users/p/Desktop/webserv/src/Server.cpp#L104) & [`L132`](file:///Users/p/Desktop/webserv/src/Server.cpp#L132)
- **Problem**:
  In `setupListeningSockets()`, if `setNonBlocking(fd)` throws, `fd` is not stored in `listen_sockets_` yet and is not closed before throwing.
  In `handleNewConnection()`, if `setNonBlocking(new_socket)` throws, the exception escapes uncaught from `run()`, terminating the server without closing `new_socket`.

---

## 4. Scalability & Performance Bottlenecks

### 4.1 Restrictive Listen Backlog (`listen(fd, 10)`)
- **Location**: [`src/Server.cpp:L99`](file:///Users/p/Desktop/webserv/src/Server.cpp#L99)
- **Problem**:
  `listen(fd, 10)` sets a maximum pending connection queue of only 10.
- **Impact**:
  During stress tests (e.g. `siege -c 50`, `wrk`, or concurrent evaluation scripts), incoming connections beyond 10 are immediately rejected by the OS kernel (`ECONNREFUSED`) before `accept()` has an opportunity to handle them.
- **Remediation**:
  Increase backlog to `SOMAXCONN` (or at least `128`): `listen(fd, SOMAXCONN)`.

### 4.2 Single `accept()` per Event Notification
- **Location**: [`src/Server.cpp:L121`](file:///Users/p/Desktop/webserv/src/Server.cpp#L121)
- **Problem**:
  `handleNewConnection()` calls `accept()` only once when the listen socket is ready. If multiple connections arrive in a burst, connection ramp-up is delayed across multiple event loop cycles.
- **Remediation**:
  Loop `accept()` until it returns `< 0` with `EWOULDBLOCK`/`EAGAIN` (since listen socket is non-blocking), accepting all pending connections in the queue.

### 4.3 Stale Event References in Batch Processing
- **Location**: [`src/Server.cpp:L218-L237`](file:///Users/p/Desktop/webserv/src/Server.cpp#L218-L237)
- **Problem**:
  The event loop processes up to `WORKER_CONNECTIONS` events returned in `events[]`. If an earlier event in the batch causes `current_fd` to be disconnected and closed, a subsequent event in the same batch could reference that same `current_fd`.
- **Impact**:
  Calling `handleClientWrite` or `handleClientRead` on an already-closed fd will invoke `clients_[current_fd]`, creating a phantom entry in `clients_`.
- **Remediation**:
  Verify `clients_.find(current_fd) != clients_.end()` before invoking handlers in the event loop.

---

## 5. Missing Process-Level Safety: `SIGPIPE`

- **Problem**:
  Neither `Server.cpp` nor `app/main.cpp` ignores `SIGPIPE`.
- **Impact**:
  If a client unexpectedly terminates or resets a connection while the server is writing a response, the subsequent `send()` or `write()` will emit a `SIGPIPE` signal. The default action for `SIGPIPE` is **instant process termination**.
- **Remediation**:
  Add `signal(SIGPIPE, SIG_IGN);` in `main()` or in `Server::setupListeningSockets()`.
