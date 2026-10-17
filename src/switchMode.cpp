# include "../inc/Server.hpp"
#include "../inc/RequestHandler.hpp"

#include <sys/socket.h>

#if defined(USE_KQUEUE) || defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

// ################################################################################################
// ###################### HANDLER DE READ PARA CLIENTES ###################################
// ################################################################################################

// Called when the multiplexer says the client socket is readable.
// We do exactly ONE recv() per event: the subject forbids reading without
// going through poll() (or equivalent) first, and forbids checking errno after
// a read/write. So:
//   > 0  -> append the data to the buffer and try to process it
//   <= 0 -> the client closed the connection (0) or something failed (-1);
//           in both cases we close it.
void Server::handleClientRead(int client_fd, int multiplexer_fd)
{
    std::map<int, ClientSession>::iterator it = clients_.find(client_fd);
    if (it == clients_.end())
        return;
    ClientSession &session = it->second;

    char buffer[BUFFER_SIZE];
    ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);
    if (count <= 0)
    {
        handleClientDisconnect(client_fd, multiplexer_fd);
        return;
    }

    session.request_buffer.append(buffer, count);
    session.last_activity = time(NULL);
    processRequestBuffer(client_fd, multiplexer_fd);
}

// Looks at the client's buffer. If it contains a complete request (or we
// already know it is invalid, e.g. the body is too big), it builds the
// response and switches the socket to write mode. Otherwise it does nothing
// and we keep waiting for more data.
void Server::processRequestBuffer(int client_fd, int multiplexer_fd)
{
    ClientSession &session = clients_[client_fd];

    // Mientras su CGI se ejecuta, la siguiente petición del cliente espera en
    // el buffer: se procesará cuando hayamos enviado la respuesta del CGI.
    if (session.cgi.active())
        return;

    const ServerConfig* active_server = session.virtual_host.empty() ? NULL : session.virtual_host[0];
    // Límite de body: el de la location de esta URL si lo tiene, si no el del server
    size_t max_body = maxBodySizeFor(active_server, session.request_buffer);

    HttpRequest request;
    request.setmaxBodySize(max_body);
    request.setClientId(client_fd);

    std::stringstream ss;
    ss << "webserv-" << client_fd << "-" << time(NULL);
    request.setRequestId(ss.str());

    HttpResponse response;
    RequestHandler handler;

    if (HttpRequest::isCompleteRequest(session.request_buffer, session.request_size, session.chunk))
    {
        // Only the bytes of THIS request: anything after it belongs to the next
        // one (pipelining) and stays in the buffer. If the buffer holds exactly
        // one request (the usual case) we take it with swap(), without copying
        // (it can be a 100 MB body).
        std::string raw;
        if (session.request_size >= session.request_buffer.size())
            raw.swap(session.request_buffer);
        else
        {
            raw = session.request_buffer.substr(0, session.request_size);
            session.request_buffer.erase(0, session.request_size);
        }
        session.chunk = ChunkProgress();

        request.parse(raw);
        std::string().swap(raw);   // free the raw copy now
        if (request.getMethod().empty())
        {
            session.log_method = "???";
            session.log_target = "(malformed request)";
        }
        else
        {
            session.log_method = request.getMethod();
            session.log_target = request.getPath();
        }
        session.head_request = (request.getMethod() == "HEAD");

        applyRequestConfiguration(request, active_server);
        request.setUploadsDirectory(resolveUploadDirectory(active_server, request));
        response = handler.handle(request);

        // HTTP/1.0 closes by default; HTTP/1.1 keeps the connection alive
        // unless the client asks otherwise.
        const std::string& connection = request.getHeader("Connection");
        bool close_connection = connection == "close"
            || (request.getVersion() == "HTTP/1.0" && connection != "keep-alive");

        if (handler.wantsCgi())
        {
            int error_code = startCgi(client_fd, multiplexer_fd, request,
                                      handler.getCgiScript(), handler.getCgiInterpreter());
            if (error_code == 0)
            {
                // The response will be built in finishCgi() once the script
                // ends; until then the client just waits.
                session.close_after_send = close_connection;
                return;
            }
            response = handler.buildError(error_code, request.getRequestId());
            if (error_code == 404)
                response.setNote("CGI script not found: " + handler.getCgiScript());
            else if (error_code == 403)
                response.setNote("CGI script not readable/executable: " + handler.getCgiScript());
            else
                response.setNote("could not start CGI (interpreter missing or pipe/fork failed)");
        }

        if (close_connection)
            response.setHeader("Connection", "close");
    }
    else
    {
        int error_code = HttpRequest::earlyErrorCode(session.request_buffer, max_body, session.chunk);
        if (error_code == 0)
            return; // Incomplete but valid so far: wait for more data

        // We will not read the rest of this request, so the connection cannot
        // be reused: answer with the error and close.
        std::ostringstream note;
        if (error_code == 413)
            note << "body > client_max_body_size (" << logfmt::humanSize(max_body) << ")";
        else if (error_code == 431)
            note << "headers larger than " << logfmt::humanSize(MAX_HEADER_SIZE);
        else
            note << "invalid Content-Length";
        request.setErrorCode(error_code, note.str());
        request.setServerConfig(active_server);   // para usar su error_page
        session.log_method = "???";
        session.log_target = "(rejected before completion)";
        session.head_request = false;
        response = handler.handle(request);
        response.setHeader("Connection", "close");
        std::string().swap(session.request_buffer);
        session.chunk = ChunkProgress();
    }

    queueResponse(client_fd, multiplexer_fd, response);
}

// Prepares `response` to be sent and switches the socket to write mode.
// HEAD requests get the same headers as a GET (Content-Length included) but
// no body, as HTTP requires.
void Server::queueResponse(int client_fd, int multiplexer_fd, HttpResponse& response)
{
    ClientSession &session = clients_[client_fd];

    std::map<std::string, std::string>::const_iterator conn = response.getHeaders().find("Connection");
    session.close_after_send = (conn != response.getHeaders().end() && conn->second == "close");
    logResponse(client_fd, response);
    if (session.head_request)
        response.setBody("");

    std::string serialized = response.serialize();
    session.response.swap(serialized);     // without copying (can be 100 MB)
    session.bytes_sent = 0;
    _switchToWriteMode(client_fd, multiplexer_fd);
}

// One line per request:
//   14:02:11  :8080 #5   GET     /index.html                  200  15.5 KB
//   14:02:12  :8080 #5   DELETE  /uploads/x                   405  2.1 KB  DELETE not allowed...
// The status code is coloured by class: 2xx green, 3xx cyan, 4xx yellow, 5xx red.
void Server::logResponse(int client_fd, HttpResponse const& response)
{
    ClientSession& session = clients_[client_fd];
    int status = response.getStatusCode();
    ++requests_served_;

    const char* color = GREEN;
    if (status >= 500)
        color = RED;
    else if (status >= 400)
        color = YELLOW;
    else if (status >= 300)
        color = CYAN;

    int port = session.virtual_host.empty() ? 0 : session.virtual_host[0]->getListenAddress().second;
    std::ostringstream fd;
    fd << "#" << client_fd;

    std::cout << GRAY << logfmt::clock() << "  " << NC
              << LIGHT_BLUE << ":" << port << NC << " "
              << GRAY << logfmt::pad(fd.str(), 4) << NC << " "
              << BOLD << logfmt::pad(session.log_method, 7) << NC << " "
              << logfmt::pad(session.log_target, 28) << " "
              << color << BOLD << status << NC << "  "
              << GRAY << logfmt::pad(logfmt::humanSize(response.getBody().size()), 9) << NC;
    if (!response.getNote().empty())
        std::cout << color << DIM << response.getNote() << NC;
    std::cout << std::endl;
}

// ################################################################################################
// ###################### HANDLER DE WRITE PARA CLIENTES ##########################################
// ################################################################################################

// Called when the socket is ready to be written to. Sends as much of the
// response as the kernel accepts (ONE send() per event). When everything has
// been sent we either close the connection or go back to reading.
void Server::handleClientWrite(int client_fd, int multiplexer_fd)
{
    std::map<int, ClientSession>::iterator it = clients_.find(client_fd);
    if (it == clients_.end())
        return;
    ClientSession &session = it->second;

    // Si por alguna razón no queda nada por enviar, volvemos a modo lectura
    if (session.bytes_sent >= session.response.size()) {
        _switchToReadMode(client_fd, multiplexer_fd);
        return ;
    }

    const std::string &response = session.response;
    ssize_t sent = send(client_fd, response.data() + session.bytes_sent,
                        response.size() - session.bytes_sent, 0);

    if (sent <= 0)
    {
        // std::cerr << GRAY << "Send failed on socket " << client_fd << NC << std::endl;
        handleClientDisconnect(client_fd, multiplexer_fd);
        return;
    }

    session.last_activity = time(NULL);
    session.bytes_sent += sent;

    // Envío parcial: el buffer del SO se llenó. Seguimos en modo escritura y
    // mandaremos el resto en el siguiente evento. No borramos lo enviado del
    // principio del string (con 100 MB moveríamos memoria en cada evento):
    // solo avanzamos bytes_sent.
    if (session.bytes_sent < response.size())
        return;

    // std::cout << GRAY << "Sent response to socket " << client_fd << NC << std::endl;
    std::string().swap(session.response);   // free the memory
    session.bytes_sent = 0;

    if (session.close_after_send)
    {
        handleClientDisconnect(client_fd, multiplexer_fd);
        return;
    }

    _switchToReadMode(client_fd, multiplexer_fd);
    // Si el cliente ya nos mandó la siguiente petición (pipelining), la
    // procesamos sin esperar a otro evento de lectura.
    if (!session.request_buffer.empty())
        processRequestBuffer(client_fd, multiplexer_fd);
}

// ################################################################################################
// HANDLE SWITCH MODE FROM READ TO WRITE
// ################################################################################################
// While a response is pending we stop listening for reads, so a new request
// can never overwrite a response that has not been fully sent yet.
void Server::_switchToWriteMode(int client_fd, int multiplexer_fd) {
#if defined(USE_KQUEUE) || defined(__APPLE__)
	struct kevent ev[2];
	EV_SET(&ev[0], client_fd, EVFILT_READ, EV_DISABLE, 0, 0, NULL);
	EV_SET(&ev[1], client_fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
	kevent(multiplexer_fd, ev, 2, NULL, 0, NULL);
#else
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLRDHUP;
	ev.data.fd = client_fd;
	epoll_ctl(multiplexer_fd, EPOLL_CTL_MOD, client_fd, &ev);
#endif
}

//################################################################################################
// HANDLE SWITCH MODE FROM WRITE TO READ
// ################################################################################################
void Server::_switchToReadMode(int client_fd, int multiplexer_fd) {
#if defined(USE_KQUEUE) || defined(__APPLE__)
	struct kevent ev[2];
	EV_SET(&ev[0], client_fd, EVFILT_WRITE, EV_DISABLE, 0, 0, NULL);
	EV_SET(&ev[1], client_fd, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
	kevent(multiplexer_fd, ev, 2, NULL, 0, NULL);
#else
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLRDHUP;
	ev.data.fd = client_fd;
	epoll_ctl(multiplexer_fd, EPOLL_CTL_MOD, client_fd, &ev);
#endif
}
