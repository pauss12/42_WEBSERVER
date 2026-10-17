# Server Architecture and Memory Safety

## Why the `Server` Class is Non-Copyable

In our event-driven architecture, the `Server` class represents the entire web server program, not just a single virtual host. It is designed to be **non-copyable** (by declaring the copy constructor and assignment operator as private). 

Here is exactly why this is critical for C++ memory safety:

### 1. The Architecture
We use an event multiplexer (`kqueue` or `epoll`). A single `Server` object runs a single `run()` loop that manages **all** incoming connections across **all** configurations simultaneously. 

If we have 5 different `server {}` blocks in the config (listening on ports 80, 8080, 443, etc.), we don't create 5 `Server` objects. We create **one** `Configuration` object (which holds 5 `ServerConfig`s), pass it to **one** `Server` object, and that single `Server` opens all 5 sockets and listens to them all at the same time in its `run()` loop.

### 2. The Danger of Copying File Descriptors
The `Server` class holds a map of file descriptors (`listen_sockets_`), which are raw integer IDs handed out by the operating system for the network sockets.

If we made a copy of the `Server` object, the new copy would hold the exact same integers. This creates a lethal "shallow copy" bug:
1. `Server B` is a copy of `Server A`. They both think they own socket `fd = 5`.
2. `Server B` goes out of scope, so its destructor runs and calls `close(5)`.
3. Now `Server A` is still running, but its socket has been destroyed out from under it by the OS. The moment it tries to accept a connection, the entire program crashes (a classic use-after-free scenario).

By keeping the copy constructor private, we force the compiler to throw an error if anyone ever tries to accidentally copy a `Server` object, completely preventing this category of bug.
