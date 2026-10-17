# ClientSession Documentation

## The Problem with Non-Blocking I/O
When using non-blocking sockets (which are required by the 42 project constraints), the operating system does not guarantee that a single `read()` call will fetch the entire payload sent by a client. 

For example, if a user uploads a large image or sends an HTTP request over a slow connection, `read()` might retrieve the data in multiple small chunks. Because `read()` returns immediately (thanks to `O_NONBLOCK`), any variables created inside the `handleClientRead` function will be destroyed when the function returns. This means partial data is lost between consecutive reads.

## The Solution: `ClientSession`
To prevent data loss, we must track the state of each connected client persistently across multiple multiplexer events (like `epoll` or `kqueue` triggers). 

The `ClientSession` structure acts as a dedicated storage unit for a single client connection:

```cpp
struct ClientSession {
	std::vector<ServerConfig const*> virtual_hosts;
	std::string request_buffer;
	std::string response_buffer;
	bool request_complete;

	ClientSession() : request_complete(false) {}
};
```

### Components:
1. **`virtual_hosts`**: Contains the configurations corresponding to the IP and Port the client connected to. This is used by the Router to determine which rules apply to the request.
2. **`request_buffer`**: An accumulating `std::string`. As the multiplexer wakes the server up with new incoming bytes, they are safely appended to this buffer. The server parses this buffer only when it detects the end of the HTTP headers (e.g., `\r\n\r\n`).
3. **`response_buffer`**: When sending large files, `send()` might only write a fraction of the file before the OS queue fills up. The remaining unsent data is stored here, waiting for the multiplexer to signal that it is safe to write again.
4. **`request_complete`**: A boolean flag indicating whether the server has received the full request and can safely proceed to generate an HTTP response.

By mapping a client's socket descriptor to a `ClientSession` instance (`std::map<int, ClientSession> clients_`), the server can safely handle thousands of simultaneous, fragmented connections without blocking or losing data.
