#ifndef SERVER_HPP
#define SERVER_HPP

#include "Configuration.hpp"
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <ctime>


#include <unistd.h>        // para close()
#include <csignal>         // sig_atomic_t

// Se pone a 1 desde el handler de SIGINT/SIGTERM (Ctrl+C) para que run()
// salga del bucle y cierre todo de forma ordenada.
extern volatile sig_atomic_t g_shutdown_requested;

#ifndef WORKER_CONNECTIONS
#define WORKER_CONNECTIONS 64
#endif
#ifndef BUFFER_SIZE
#define BUFFER_SIZE 65536
#endif
// Segundos sin actividad tras los que cerramos una conexion, para que ninguna
// peticion se quede colgada indefinidamente.
#ifndef CLIENT_TIMEOUT
#define CLIENT_TIMEOUT 60
#endif
// Segundos sin recibir nada cuando ya llegaron las cabeceras pero falta parte
// del body (p.ej. anuncia Content-Length: 10 y envía 4): se responde 408.
// Es corto a propósito; ojo: curl espera 1 s antes de enviar bodies de más de
// 1 KB (Expect: 100-continue), así que no debe bajar de 2.
#ifndef BODY_TIMEOUT
#define BODY_TIMEOUT 2
#endif

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "CgiHandler.hpp"

// Holds the state of a single client connection between non-blocking I/O
// events.
struct ClientSession 
{
    std::vector<const ServerConfig*> virtual_host;
  
    std::string request_buffer;
    bool request_complete;
    bool request_parsed;
    size_t request_size;
    std::string response;

    std::string response_buffer;
    size_t bytes_sent;        // Bytes de `response` ya enviados. No se borra lo
                              // enviado del principio: con 100 MB sería muy lento
    ChunkProgress chunk;      // Por dónde va la lectura de un body chunked
    bool head_request;        // HEAD: se envían las cabeceras sin el body

    time_t last_activity;   // Ultima vez que leimos o escribimos (timeouts)
    bool close_after_send;  // Cerrar la conexion al terminar de enviar

    CgiProcess cgi;         // Script CGI en ejecucion para este cliente
    std::string log_method;   // "GET", para el log de la respuesta
    std::string log_target;   // "/index.html", para el log de la respuesta

    ClientSession() 
        : request_complete(false), 
          request_parsed(false), 
          request_size(0), 
          bytes_sent(0),
          head_request(false),
          last_activity(time(NULL)),
          close_after_send(false) {}
};


// Represents the core HTTP Server.
// Responsible for initializing listening sockets, accepting incoming client
// connections, and managing the non-blocking I/O event loop (epoll/kqueue).
class Server
{
	private:
		// Hide copy semantics to prevent accidental copying
		// since the Server manages a unique listening socket.
		Server(Server const &rhs);
		Server &operator=(Server const &rhs);

		Configuration const &_config;

		// Maps a listening socket FD to all the ServerConfigs that share it (IP:Port)
		std::map<int, std::vector<ServerConfig const *> > listen_sockets_;
		
  		// Maps a connected client socket FD to its current session state
  		std::map<int, ClientSession> clients_;

		// Maps a CGI pipe FD (stdin or stdout of a script) to the client FD
		// whose request launched that script
		std::map<int, int> cgi_pipes_;

		// CGI children that had not exited yet when we finished with them;
		// we reap them later with waitpid(WNOHANG) so they don't stay zombies
		std::vector<pid_t> pending_children_;

		// NEW ---------------------------------------------------------------------------------------------

		// Cambia el socket del cliente a modo lectura (EPOLLIN)
		void _switchToReadMode(int client_fd, int multiplexer_fd);
		// Cambia el socket del cliente a modo escritura (EPOLLOUT)
		void _switchToWriteMode(int client_fd, int multiplexer_fd);

		const LocationConfig* findMatchingLocation(const ServerConfig* server, const std::string& path);
		void applyRequestConfiguration(HttpRequest& request, const ServerConfig* server);
    	std::string resolveUploadDirectory(const ServerConfig* server, const HttpRequest& req);

		// -------------------------------------------------------------------------------------------------

		// Event handlers for the multiplexer loop.
		void handleNewConnection(int listen_fd, int multiplexer_fd);
		void handleClientDisconnect(int client_fd, int multiplexer_fd);
		void handleClientRead(int client_fd, int multiplexer_fd);
		void handleClientWrite(int client_fd, int multiplexer_fd);

		// Procesa la siguiente peticion del buffer del cliente si ya esta
		// completa (o si ya sabemos que es invalida) y pasa a modo escritura.
		void processRequestBuffer(int client_fd, int multiplexer_fd);
		// Serializa la respuesta en la sesión y pasa el socket a modo escritura
		void queueResponse(int client_fd, int multiplexer_fd, HttpResponse& response);
		// client_max_body_size que se aplica a la petición del buffer: el de su
		// location si lo tiene, si no el del server (src/obtainServerConfigToRequest.cpp)
		size_t maxBodySizeFor(const ServerConfig* server, const std::string& buffer);
		// Cierra las conexiones que llevan mas de CLIENT_TIMEOUT sin actividad
		// y mata los CGI que superan CGI_TIMEOUT.
		void closeIdleClients(int multiplexer_fd);

		// Imprime una linea por peticion con hora, puerto, metodo, ruta,
		// codigo, tamano y el motivo (si lo hay).
		void logResponse(int client_fd, HttpResponse const& response);

		// Peticiones respondidas desde que arranco el servidor
		size_t requests_served_;

		// Cierra clientes y CGIs al salir del bucle (Ctrl+C)
		void shutdown(int multiplexer_fd);

		// CGI (src/cgiEvents.cpp)
		int  startCgi(int client_fd, int multiplexer_fd, HttpRequest& request,
		              std::string const& script, std::string const& interpreter);
		void handleCgiEvent(int pipe_fd, int multiplexer_fd);
		void finishCgi(int client_fd, int multiplexer_fd, HttpResponse& response);
		void cleanupCgi(ClientSession& session, int multiplexer_fd, bool kill_child);
		void closeCgiPipe(int& pipe_fd, int multiplexer_fd);
		void reapChildren();
		bool watchFd(int fd, int multiplexer_fd, bool for_writing);

	public:
		// Constructs a Server using the provided global Configuration.
		Server(Configuration const &config);

		~Server();

		// Initializes, binds, and starts listening on all required sockets
		// based on the provided Configuration.
		// Throws std::runtime_error if socket creation, binding, or listening fails.
		void setupListeningSockets();

		// Enters the infinite event loop, waiting for and dispatching network events
		// to the appropriate handlers. Blocks indefinitely.
		void run();
};

#endif
