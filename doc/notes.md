## Webserv Roadmap

This changes things significantly. The Webserv project at 42 is a rite of passage, and because it is built in C++98, you do not get modern conveniences like `auto`, lambdas, or `std::shared_ptr`.

At a high level, you are building a low-level engine that behaves a bit like Nginx or Apache. The first mistake to avoid is jumping straight into sockets without an architecture for non-blocking I/O.

## Phase 1: Foundation

Before touching `epoll`, build a server that can listen for a single connection. Do not worry about multiple clients yet.

### 1. Configuration Parsing

Start by parsing the config file, for example:

```conf
server {
		listen 8080;
		...
}
```

You need a data structure that stores the server configuration.

### 2. Socket Creation

Set up the socket in this order:

- `socket()` to create the file descriptor.
- `setsockopt()` to enable `SO_REUSEADDR` so you can restart the server without waiting for the kernel to release the port.
- `fcntl()` to set the socket to `O_NONBLOCK`. This is mandatory for `epoll` to work effectively.
- `bind()` to attach the socket to an IP and port.
- `listen()` to put the socket into passive mode and wait for connections.

### Goal

Successfully run a server that prints `Connected` when you `curl` it, even if it hangs immediately after.

## Phase 2: Event Loop

This is the heart of the server. `epoll` lets you monitor hundreds of file descriptors simultaneously without creating a thread per connection.

### Workflow

1. Initialize with `epoll_create()`.
2. Register the listening socket with `epoll_ctl(..., EPOLL_CTL_ADD, ...)` and monitor `EPOLLIN`.
3. Enter a `while (true)` loop.
4. Call `epoll_wait()`, which blocks until a socket has activity.
5. Iterate through the returned events.
6. Branch based on the socket type:
	 - If the event is on the listening socket, accept a new client, set it to `O_NONBLOCK`, and add it to the interest list with `epoll_ctl()`.
	 - If the event is on a client socket:
		 - `EPOLLIN`: data is available to read, so read it into a buffer.
		 - `EPOLLOUT`: the socket is ready to send the HTTP response.
		 - `EPOLLHUP` or `EPOLLRDHUP`: the client closed the connection, so remove it from `epoll` and call `close()`.

## Phase 3: HTTP Engine

Once you have raw bytes from the client socket, turn them into something useful.

### 1. Request Parsing

Write a parser that walks the raw buffer and identifies:

- The request line: method, URI, and version.
- Headers: store them in a `std::map<std::string, std::string>`.
- Body: handle it based on `Content-Length`.

### 2. Request Object

Create a class or struct to hold the parsed request data.

### 3. Response Generation

Build a string that strictly follows the HTTP/1.1 protocol, for example:

```http
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
...
```

## Phase 4: CGI

This is often the hardest part of Webserv. The server needs to act as a gateway to external scripts such as PHP or Python.

### Steps

1. Use `fork()` and `execve()` to run the CGI script.
2. Use `pipe()` to send the request body to the CGI script's standard input and read its standard output.
3. Keep CGI non-blocking by polling the pipe file descriptor with `epoll`, so the server does not freeze while waiting for the script to finish.

## Phase 5: Error Handling and Cleanup

- Timeouts: track how long a client has been idle. If it has not sent data in X seconds, call `close()` on the socket and remove it from `epoll`.
- Partial reads and writes: never assume `read()` or `write()` finishes in one call. Buffer the data. If `write()` returns less than the buffer size, store the remainder and wait for the next `EPOLLOUT` event.

## Component Summary

| Component | Responsibility | When to call |
| --- | --- | --- |
| Listening socket | Accepting new clients | When `epoll_wait()` shows activity on the listening FD |
| Client socket | Transferring data | Managed by `epoll` after `accept()` |
| `epoll` | Orchestrator | The engine of the main loop |
| HTTP parser | Data interpretation | After `read()` returns data from a socket |
| CGI module | Executing scripts | When the requested URI matches a CGI configuration |

## Pro Tip For C++98

Since you do not have smart pointers, be disciplined with memory management. Every `new` must have a corresponding `delete`.

One good pattern is to create a `Connection` class that holds the socket FD, the read buffer, and the response buffer. When you remove a connection from `epoll`, explicitly destroy that object to avoid leaks.

## Closing Question

Does this roadmap help clarify the order of operations for your Webserv implementation, or would you like to dive deeper into the `epoll` loop logic specifically?
