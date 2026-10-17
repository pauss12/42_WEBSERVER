3. Serious 42 Non-Blocking I/O Violations
Per 

.agents/skills/non-blocking-io/SKILL.md
:

Second Multiplexer Violation:
The rule states: "Your server must use only 1 poll(), epoll, or kqueue for ALL the I/O operations between the clients and the server".


src/CgiHandler.cpp:L336-L353
 invokes an independent poll() loop inside execute().
Blocking the Server:
The poll(&pfd, 1, 500) loop blocks synchronously for up to 5 seconds. Because webserv is single-threaded, all other connected clients and new connections freeze while any CGI script is executing.
Pipe Deadlock on Large POST bodies:
write(stdin_pipe[1], body.c_str(), body.length()) writes the entire request body synchronously. If a client POSTs a payload larger than the OS pipe buffer capacity (typically 64KB), write() blocks indefinitely waiting for the child to read, while the child blocks waiting for execve or output, causing a permanent deadlock.