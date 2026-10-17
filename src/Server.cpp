#include "Server.hpp"
#include "Configuration.hpp"
#include "RequestHandler.hpp"
#include "Log.hpp"
#include <sstream>

// We need these headers for socket operations.
#include <sys/socket.h> // socket(), bind(), listen(), accept()
// Needed for sockaddr_in and network byte order functions like htons()
#include <netinet/in.h>
// Standard POSIX API for close()
#include <unistd.h>
// fcntl() is used to manipulate file descriptors (like setting them to
// non-blocking)
#include <fcntl.h>

#include <cerrno>    // errno access
#include <cstring>   // strerror(), memset()
#include <iostream>  // std::cout, std::cerr
#include <stdexcept> // std::runtime_error


#if !defined(USE_KQUEUE) && !defined(__APPLE__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif
namespace {

  // Sets the given file descriptor to non-blocking mode.
  // By default, reading from a socket will block the entire program until data
  // arrives. Since our server must handle many clients at once with a single
  // thread, a blocking socket would freeze the whole server.
  // Uses fcntl to set the O_NONBLOCK flag. The subject only allows F_SETFL,
  // O_NONBLOCK and FD_CLOEXEC, so we cannot read the current flags with
  // F_GETFL: we simply set O_NONBLOCK.
  // Returns false on failure instead of throwing, so a single bad client
  // can never bring the whole server down.
  bool setNonBlocking(int fd) {
    return fcntl(fd, F_SETFL, O_NONBLOCK) != -1;
  }

} // namespace

volatile sig_atomic_t g_shutdown_requested = 0;

// Constructor initializes the config
Server::Server(Configuration const &config)
    : _config(config), requests_served_(0) {}

// Destructor cleans up all the listening sockets
Server::~Server() {
  for (std::map<int, std::vector<ServerConfig const *> >::iterator it =
           listen_sockets_.begin();
       it != listen_sockets_.end(); ++it) {
    close(it->first);
  }
}

// Binds the sockets and starts listening on the required ports based on the
// Configuration.
void Server::setupListeningSockets() {
  // First, group configs by unique IP:Port pairs
  std::map<std::pair<in_addr_t, int>, std::vector<ServerConfig const *> > binds;
  std::vector<ServerConfig> const &servers = _config.getServers();

  for (size_t i = 0; i < servers.size(); ++i) {
    binds[servers.at(i).getListenAddress()].push_back(&servers.at(i));
  }

  // Create a socket for each unique IP:Port pair
  for (std::map<std::pair<in_addr_t, int>,
                std::vector<ServerConfig const *> >::iterator it = binds.begin();
       it != binds.end(); ++it) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      throw std::runtime_error(strerror(errno));

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      close(fd);
      throw std::runtime_error(strerror(errno));
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = it->first.first;
    // htons() converts the port number to network byte order (Big Endian)
    address.sin_port = htons(it->first.second);

    // Bind the socket
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      close(fd);
      throw std::runtime_error(strerror(errno));
    }

    // Start listening. SOMAXCONN lets the kernel queue as many pending
    // connections as possible, which matters under stress tests.
    if (listen(fd, SOMAXCONN) < 0) {
      close(fd);
      throw std::runtime_error(strerror(errno));
    }

    if (!setNonBlocking(fd)) {
      close(fd);
      throw std::runtime_error(strerror(errno));
    }

    // Store the mapping from fd to the ServerConfigs
    listen_sockets_[fd] = it->second;

    std::cout << "  " << GREEN << "▸" << NC << " Listening on " << BOLD
              << "http://localhost:" << it->first.second << NC << GRAY << "  ("
              << logfmt::ipToString(it->first.first) << ":" << it->first.second
              << ", fd " << fd << ")" << NC << std::endl;
  }
}

// Called when the multiplexer indicates the listening socket is ready for
// reading, meaning a new client is trying to connect. We accept() the
// connection, make it non-blocking, and register it for read events.
void Server::handleNewConnection(int listen_fd, int multiplexer_fd) {
  // accept() gives us a new file descriptor specifically for this client
  int new_socket = accept(listen_fd, NULL, NULL);
  if (new_socket < 0) {
    std::cerr << "Accept failed: " << strerror(errno) << std::endl;
    return; // We don't crash, just ignore the failed connection
  }

  // New client sockets MUST be non-blocking before we add them to the
  // multiplexer
  if (!setNonBlocking(new_socket)) {
    std::cerr << "Failed to set client socket non-blocking" << std::endl;
    close(new_socket);
    return;
  }

#if defined(USE_KQUEUE) || defined(__APPLE__)
  struct kevent ev;
  // EV_ADD to add to kqueue, EV_ENABLE to activate it. We watch for read events
  // (EVFILT_READ).
  EV_SET(&ev, new_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  if (kevent(multiplexer_fd, &ev, 1, NULL, 0, NULL) < 0) {
    std::cerr << "Failed to add new socket to kqueue: " << strerror(errno)
              << std::endl;
    close(new_socket);
    return;
  }
#else
  struct epoll_event client_ev;
  // EPOLLIN for reading. EPOLLRDHUP is useful to detect peer shutdown early.
  client_ev.events = EPOLLIN | EPOLLRDHUP;
  client_ev.data.fd = new_socket;
  if (epoll_ctl(multiplexer_fd, EPOLL_CTL_ADD, new_socket, &client_ev) < 0) {
    std::cerr << "Failed to add new socket to epoll: " << strerror(errno)
              << std::endl;
    close(new_socket);
    return;
  }
#endif

  // Track the client only once it is registered in the multiplexer
  ClientSession session;
  session.virtual_host = listen_sockets_[listen_fd];
  clients_[new_socket] = session;
}

// Cleans up the client connection.
// Unregisters the socket from the multiplexer and closes the file descriptor.
void Server::handleClientDisconnect(int client_fd, int multiplexer_fd)
{
  // std::cout << GRAY << "Client disconnected: " << client_fd << NC << std::endl;

  // If a CGI was running for this client, nobody will read its output: kill it
  std::map<int, ClientSession>::iterator it = clients_.find(client_fd);
  if (it != clients_.end() && it->second.cgi.active())
    cleanupCgi(it->second, multiplexer_fd, true);
  clients_.erase(client_fd);

#if defined(USE_KQUEUE) || defined(__APPLE__)
  // With kqueue, closing the file descriptor automatically removes it from the
  // queue. We cast to void to prevent unused variable warnings from the
  // compiler.
  (void)multiplexer_fd;
  close(client_fd);
#else
  // EPOLL_CTL_DEL removes the socket from epoll's watch list
  epoll_ctl(multiplexer_fd, EPOLL_CTL_DEL, client_fd, NULL);
  close(client_fd);
#endif
}

// Closes every client that has been silent for more than CLIENT_TIMEOUT
// seconds (half-sent requests, idle keep-alive connections...), and kills
// CGI scripts running for more than CGI_TIMEOUT (the client gets a 504).
// We first collect the fds because handleClientDisconnect() erases from
// clients_.
void Server::closeIdleClients(int multiplexer_fd)
{
  time_t now = time(NULL);
  std::vector<int> expired;
  std::vector<int> cgi_expired;

  for (std::map<int, ClientSession>::iterator it = clients_.begin();
       it != clients_.end(); ++it) {
    ClientSession& client = it->second;
    if (client.cgi.active()) {
      if (now - client.cgi.last_io > CGI_TIMEOUT)
        cgi_expired.push_back(it->first);
      continue;
    }
    // Headers received but the body is incomplete and nothing is pending to
    // send: short BODY_TIMEOUT. Otherwise (idle keep-alive, half-sent
    // headers...): CLIENT_TIMEOUT.
    bool waiting_body = client.bytes_sent >= client.response.size()
        && client.request_buffer.find("\r\n\r\n") != std::string::npos;
    if (waiting_body ? now - client.last_activity >= BODY_TIMEOUT
                     : now - client.last_activity > CLIENT_TIMEOUT)
      expired.push_back(it->first);
  }
  for (size_t i = 0; i < cgi_expired.size(); ++i) {
    cleanupCgi(clients_[cgi_expired[i]], multiplexer_fd, true);
    RequestHandler handler;
    if (!clients_[cgi_expired[i]].virtual_host.empty())
      handler.setServerConfig(clients_[cgi_expired[i]].virtual_host[0]);   // su error_page
    HttpResponse timeout = handler.buildError(504, "");
    std::ostringstream note;
    note << "CGI killed after " << CGI_TIMEOUT << "s";
    timeout.setNote(note.str());
    finishCgi(cgi_expired[i], multiplexer_fd, timeout);
  }
  for (size_t i = 0; i < expired.size(); ++i) {
    ClientSession& session = clients_[expired[i]];
    bool partial_request = !session.request_buffer.empty();
    bool response_pending = session.bytes_sent < session.response.size();

    // Half-sent request and nothing pending to send: answer 408 Request
    // Timeout and close, instead of hanging up without a word.
    if (partial_request && !response_pending) {
      RequestHandler handler;
      if (!session.virtual_host.empty())
        handler.setServerConfig(session.virtual_host[0]);   // su error_page
      HttpResponse timeout = handler.buildError(408, "");
      std::ostringstream note;
      note << "request incomplete (no data for "
           << now - session.last_activity << "s)";
      timeout.setNote(note.str());
      timeout.setHeader("Connection", "close");
      session.log_method = "???";
      session.log_target = "(incomplete request)";
      session.head_request = false;
      std::string().swap(session.request_buffer);
      session.chunk = ChunkProgress();
      session.last_activity = now;
      queueResponse(expired[i], multiplexer_fd, timeout);
      continue;
    }

    std::cout << GRAY << logfmt::clock() << "  " << NC << YELLOW << DIM
              << "#" << expired[i] << " closed: idle for more than "
              << CLIENT_TIMEOUT << "s" << NC << std::endl;
    handleClientDisconnect(expired[i], multiplexer_fd);
  }
  reapChildren();
}

// Called when the event loop ends (Ctrl+C): kills running CGI scripts,
// closes every client connection and the multiplexer itself. The listening
// sockets are closed by the destructor.
void Server::shutdown(int multiplexer_fd)
{
  size_t clients = clients_.size();
  size_t cgis = 0;

  for (std::map<int, ClientSession>::iterator it = clients_.begin();
       it != clients_.end(); ++it) {
    if (it->second.cgi.active()) {
      cleanupCgi(it->second, multiplexer_fd, true);
      ++cgis;
    }
    close(it->first);
  }
  clients_.clear();
  close(multiplexer_fd);

  std::cout << "\n  " << BOLD << "Shutting down" << NC << GRAY << "  ·  "
            << requests_served_ << " requests served  ·  " << clients
            << " connections closed  ·  " << cgis << " CGI killed" << NC
            << std::endl;
}

// The main Event Demultiplexer loop.
// Waits for events and delegates them to the appropriate handler.
void Server::run() {

#if defined(USE_KQUEUE) || defined(__APPLE__)
  int kq = kqueue();
  if (kq < 0) {
    throw std::runtime_error(strerror(errno));
  }

  // Register all listening sockets for read events
  for (std::map<int, std::vector<ServerConfig const *> >::iterator it =
           listen_sockets_.begin();
       it != listen_sockets_.end(); ++it) {
    struct kevent ev;
    EV_SET(&ev, it->first, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
      close(kq);
      throw std::runtime_error(strerror(errno));
    }
  }

  struct kevent events[WORKER_CONNECTIONS];
  // Wake up at least twice per second so we can apply the timeouts (idle
  // clients, BODY_TIMEOUT, CGI) even when no events arrive.
  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 500000000;

  // The event loop, until Ctrl+C (SIGINT) or SIGTERM
  while (!g_shutdown_requested) {
    int nev = kevent(kq, NULL, 0, events, WORKER_CONNECTIONS, &timeout);
    if (nev < 0) {
      // Interrupted by a signal (e.g. Ctrl+C): the loop condition decides.
      if (errno == EINTR)
        continue;
      close(kq);
      throw std::runtime_error(strerror(errno));
    }

    // Loop through all the events that occurred
    for (int n = 0; n < nev; ++n) {
      int current_fd = events[n].ident;

      try {
        // If the fd is one of our listening sockets, it's a new connection!
        if (listen_sockets_.find(current_fd) != listen_sockets_.end()) {
          handleNewConnection(current_fd, kq);
        }
        // A pipe of a running CGI script. We don't look at EOF/HUP flags here:
        // read() returning 0 is what tells us the script has finished.
        else if (cgi_pipes_.find(current_fd) != cgi_pipes_.end()) {
          handleCgiEvent(current_fd, kq);
        }
        // The client may have been closed by an earlier event of this batch
        else if (clients_.find(current_fd) == clients_.end()) {
          continue;
        }
        // EV_EOF indicates the client closed the connection
        else if (events[n].flags & EV_EOF) {
          handleClientDisconnect(current_fd, kq);
        }
        // EVFILT_READ means there is data waiting to be read()
        else if (events[n].filter == EVFILT_READ) {
          handleClientRead(current_fd, kq);
        }
        // EVFILT_WRITE means we can send() without blocking
        else if (events[n].filter == EVFILT_WRITE) {
          handleClientWrite(current_fd, kq);
        }
      } catch (std::exception const &e) {
        // Anything unexpected (e.g. std::bad_alloc) only costs this client
        // its connection; the server keeps running.
        std::cerr << RED << "Error handling fd " << current_fd << ": "
                  << e.what() << NC << std::endl;
        // If the fd was a CGI pipe, the affected client is its owner
        int client_fd = current_fd;
        std::map<int, int>::iterator pipe = cgi_pipes_.find(current_fd);
        if (pipe != cgi_pipes_.end())
          client_fd = pipe->second;
        if (clients_.find(client_fd) != clients_.end())
          handleClientDisconnect(client_fd, kq);
      }
    }
    closeIdleClients(kq);
  }
  shutdown(kq);
#else
  // epoll_create1(0) is standard modern Linux. epoll_create(1) is older.
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    throw std::runtime_error(strerror(errno));
  }

  // Register all listening sockets
  // for (std::map<int, std::vector<ServerConfig const *>>::iterator it =
  for (std::map<int, std::vector<ServerConfig const *> >::iterator it =
           listen_sockets_.begin();
       it != listen_sockets_.end(); ++it) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = it->first;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, it->first, &ev) < 0) {
      close(epoll_fd);
      throw std::runtime_error(strerror(errno));
    }
  }

  struct epoll_event events[WORKER_CONNECTIONS];

  // The event loop, until Ctrl+C (SIGINT) or SIGTERM
  while (!g_shutdown_requested) {
    // Wait at most 0.5 s so we can apply the timeouts even when no events
    // arrive.
    int nfds = epoll_wait(epoll_fd, events, WORKER_CONNECTIONS, 500);
    if (nfds < 0) {
      // Interrupted by a signal (e.g. Ctrl+C): the loop condition decides.
      if (errno == EINTR)
        continue;
      close(epoll_fd);
      throw std::runtime_error(strerror(errno));
    }

    // Loop through all the events that occurred
    for (int n = 0; n < nfds; ++n) {
      int current_fd = events[n].data.fd;

      try {
        // If the fd is one of our listening sockets, it's a new connection!
        if (listen_sockets_.find(current_fd) != listen_sockets_.end()) {
          handleNewConnection(current_fd, epoll_fd);
        }
        // A pipe of a running CGI script. We don't look at EOF/HUP flags here:
        // read() returning 0 is what tells us the script has finished.
        else if (cgi_pipes_.find(current_fd) != cgi_pipes_.end()) {
          handleCgiEvent(current_fd, epoll_fd);
        }
        // The client may have been closed by an earlier event of this batch
        else if (clients_.find(current_fd) == clients_.end()) {
          continue;
        }
        // EPOLLERR (Error), EPOLLHUP (Hang up), EPOLLRDHUP (Peer closed
        // connection)
        else if (events[n].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          handleClientDisconnect(current_fd, epoll_fd);
        }
        // EPOLLIN means data is ready to be read()
        else if (events[n].events & EPOLLIN) {
          handleClientRead(current_fd, epoll_fd);
        }
        // EPOLLOUT means socket is ready to send()
        else if (events[n].events & EPOLLOUT) {
          handleClientWrite(current_fd, epoll_fd);
        }
      } catch (std::exception const &e) {
        // Anything unexpected (e.g. std::bad_alloc) only costs this client
        // its connection; the server keeps running.
        std::cerr << RED << "Error handling fd " << current_fd << ": "
                  << e.what() << NC << std::endl;
        // If the fd was a CGI pipe, the affected client is its owner
        int client_fd = current_fd;
        std::map<int, int>::iterator pipe = cgi_pipes_.find(current_fd);
        if (pipe != cgi_pipes_.end())
          client_fd = pipe->second;
        if (clients_.find(client_fd) != clients_.end())
          handleClientDisconnect(client_fd, epoll_fd);
      }
    }
    closeIdleClients(epoll_fd);
  }
  shutdown(epoll_fd);
#endif
}