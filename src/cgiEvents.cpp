#include "../inc/Server.hpp"
#include "../inc/RequestHandler.hpp"

#include <sys/wait.h>
#include <csignal>
#include <iostream>

#if defined(USE_KQUEUE) || defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

// ################################################################################################
// CGI NO BLOQUEANTE
// Los dos pipes del script (stdin y stdout) se registran en el MISMO kqueue/epoll
// que los sockets. Asi solo hay un multiplexor, nunca hacemos read/write sobre un
// pipe sin que nos avise antes, y el servidor sigue atendiendo a otros clientes
// mientras el script se ejecuta.
// ################################################################################################

// Registers a pipe fd in the multiplexer, for reading or for writing.
bool Server::watchFd(int fd, int multiplexer_fd, bool for_writing)
{
#if defined(USE_KQUEUE) || defined(__APPLE__)
    struct kevent ev;
    EV_SET(&ev, fd, for_writing ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    return kevent(multiplexer_fd, &ev, 1, NULL, 0, NULL) != -1;
#else
    struct epoll_event ev;
    ev.events = for_writing ? EPOLLOUT : EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(multiplexer_fd, EPOLL_CTL_ADD, fd, &ev) != -1;
#endif
}

// Unregisters and closes one of the CGI pipes, and marks it as closed (-1).
void Server::closeCgiPipe(int& pipe_fd, int multiplexer_fd)
{
    if (pipe_fd == -1)
        return;
#if defined(USE_KQUEUE) || defined(__APPLE__)
    // With kqueue, closing the fd removes it from the queue automatically.
    (void)multiplexer_fd;
#else
    epoll_ctl(multiplexer_fd, EPOLL_CTL_DEL, pipe_fd, NULL);
#endif
    cgi_pipes_.erase(pipe_fd);
    close(pipe_fd);
    pipe_fd = -1;
}

// Forks the script and registers its pipes in the multiplexer.
// Returns 0 on success or the HTTP error code to answer with.
int Server::startCgi(int client_fd, int multiplexer_fd, HttpRequest& request,
                     std::string const& script, std::string const& interpreter)
{
    ClientSession& session = clients_[client_fd];

    int code = CgiHandler::start(request, script, interpreter, request.getServerConfig(), session.cgi);
    if (code != 0)
        return code;

    CgiProcess& cgi = session.cgi;
    request.takeBody(cgi.body);   // the body goes to the script without being copied
    cgi_pipes_[cgi.stdout_fd] = client_fd;
    bool ok = watchFd(cgi.stdout_fd, multiplexer_fd, false);

    if (cgi.body.empty())
        closeCgiPipe(cgi.stdin_fd, multiplexer_fd); // Nothing to send: EOF right away
    else
    {
        cgi_pipes_[cgi.stdin_fd] = client_fd;
        ok = watchFd(cgi.stdin_fd, multiplexer_fd, true) && ok;
    }

    if (!ok)
    {
        cleanupCgi(session, multiplexer_fd, true);
        return 500;
    }
    // std::cout << GRAY << "CGI started (pid " << cgi.pid << ") for client " << client_fd << NC << std::endl;
    return 0;
}

// Called when one of the CGI pipes is ready. ONE write() or read() per event.
//  - stdin pipe:  send the next piece of the request body. When it has all
//                 been sent we close the pipe, so the script sees EOF.
//  - stdout pipe: append whatever the script printed. read() == 0 means the
//                 script closed its stdout (it finished): build the response.
void Server::handleCgiEvent(int pipe_fd, int multiplexer_fd)
{
    std::map<int, int>::iterator it = cgi_pipes_.find(pipe_fd);
    if (it == cgi_pipes_.end())
        return;
    int client_fd = it->second;
    CgiProcess& cgi = clients_[client_fd].cgi;
    clients_[client_fd].last_activity = time(NULL);

    if (pipe_fd == cgi.stdin_fd)
    {
        ssize_t written = write(pipe_fd, cgi.body.data() + cgi.body_sent, cgi.body.size() - cgi.body_sent);
        if (written <= 0)
        {
            // The script stopped reading its input: stop sending, keep its output
            closeCgiPipe(cgi.stdin_fd, multiplexer_fd);
            return;
        }
        cgi.body_sent += written;
        cgi.last_io = time(NULL);
        if (cgi.body_sent >= cgi.body.size())
        {
            closeCgiPipe(cgi.stdin_fd, multiplexer_fd);
            std::string().swap(cgi.body);   // everything sent: free the memory now
        }
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t count = read(pipe_fd, buffer, sizeof(buffer));
    if (count > 0)
    {
        cgi.output.append(buffer, count);
        cgi.last_io = time(NULL);
        return;
    }

    // EOF (or error): the script is done. No output at all means it failed
    // (e.g. execve error or crash) -> 502 Bad Gateway.
    if (cgi.output.empty())
    {
        RequestHandler handler;
        if (!clients_[client_fd].virtual_host.empty())
            handler.setServerConfig(clients_[client_fd].virtual_host[0]);   // su error_page
        HttpResponse error = handler.buildError(502, "");
        error.setNote("CGI exited without output");
        finishCgi(client_fd, multiplexer_fd, error);
    }
    else
    {
        HttpResponse res = CgiHandler::parseOutput(cgi.output);   // consumes cgi.output
        std::ostringstream note;
        note << "CGI pid " << cgi.pid;
        res.setNote(note.str());
        finishCgi(client_fd, multiplexer_fd, res);
    }
}

// The CGI is over (finished, failed or timed out): free its resources and
// send `response` to the client.
void Server::finishCgi(int client_fd, int multiplexer_fd, HttpResponse& response)
{
    ClientSession& session = clients_[client_fd];
    cleanupCgi(session, multiplexer_fd, false);

    if (session.close_after_send)
        response.setHeader("Connection", "close");
    queueResponse(client_fd, multiplexer_fd, response);
}

// Closes both pipes and, if asked, kills the script. The child is then
// reaped with waitpid(WNOHANG) (now or in a later loop iteration), so the
// server never blocks waiting for it.
void Server::cleanupCgi(ClientSession& session, int multiplexer_fd, bool kill_child)
{
    CgiProcess& cgi = session.cgi;

    closeCgiPipe(cgi.stdin_fd, multiplexer_fd);
    closeCgiPipe(cgi.stdout_fd, multiplexer_fd);
    if (cgi.pid > 0)
    {
        if (kill_child)
            kill(cgi.pid, SIGKILL);
        pending_children_.push_back(cgi.pid);
    }
    cgi = CgiProcess();
    reapChildren();
}

// Collects the exit status of finished CGI children (avoids zombies).
void Server::reapChildren()
{
    size_t i = 0;
    while (i < pending_children_.size())
    {
        // > 0: reaped, -1: no such child anymore -> forget it. 0: still running.
        if (waitpid(pending_children_[i], NULL, WNOHANG) != 0)
            pending_children_.erase(pending_children_.begin() + i);
        else
            ++i;
    }
}
