# Webserv-42: Project Walkthrough & Checkpoint

## 1. Current State: The "Transportation" Layer
At this stage, the **Transport Layer (TCP)** and **I/O Multiplexing** are fully functional. The `Server` class successfully:
- Parses the configuration file and creates `ServerConfig` and `LocationConfig` objects.
- Creates sockets and binds them to the specified IP/Ports.
- Sets all sockets to non-blocking mode.
- Uses an OS-level event loop (`epoll` or `kqueue`) to monitor sockets.
- Accepts incoming connections, detects data arrival, and detects client disconnects.

The server can physically transport data back and forth securely and efficiently.

## 2. The Next Step: The Application Layer (HTTP)
Currently, `handleClientRead()` blindly reads raw text strings, and `handleClientWrite()` replies with a hardcoded `"hola\n"`. The next major milestone is implementing the **HTTP Protocol**.

### Phase A: The HTTP Request Parser
When a browser connects, it sends a structured HTTP Request (Method, URI, Headers, Body). 
Inside `handleClientRead()`, the raw buffer needs to be fed into an `HttpRequest` class that parses the string and splits it into accessible C++ variables (e.g., `method = "GET"`, `uri = "/images/cat.jpg"`).

### Phase B: The Router (Matching Request to Configuration)
Once the request is parsed, the server must decide how to handle it by checking the `ServerConfig` and `LocationConfig` objects. A router component will:
- Check if the requested URI matches a specific `location` block.
- Verify if the HTTP Method (e.g., `GET`) is in the `allowed_methods`.
- Determine the physical path of the requested file by combining the `root` or `alias` with the URI.

### Phase C: The HTTP Response Builder
After determining the physical file (or generating an error like 404), the server must format the response so the browser understands it.
An `HttpResponse` class will generate a formatted HTTP string containing the status code, headers (like `Content-Length`), and the body payload.
Inside `handleClientWrite()`, this fully formatted string is sent back over the socket.

### Phase D: CGI Execution
If the requested file is a dynamic script (e.g., Python, PHP), the server must use `fork()`, `execve()`, and `pipe()` to execute the script in a child process and capture its output, rather than just returning the script's raw text.

## 3. Summary Perspective
The project currently has the **Rules (Configuration)** and the **Highway (Server Sockets)**. The final major piece is building the **Traffic Controller (HTTP Parser & Router)** that reads the HTTP requests and directs them to their proper destinations based on the rules.
