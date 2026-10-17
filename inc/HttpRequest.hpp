#ifndef HttpRequest_HPP
# define HttpRequest_HPP

#include <map>
#include <string>
#include <sstream>
#include <iostream>
#include "Configuration.hpp"

#include "Log.hpp"

// Tamano maximo de la request line + cabeceras. Si llegan mas bytes sin
// encontrar "\r\n\r\n" cortamos la peticion en vez de seguir acumulando.
# define MAX_HEADER_SIZE 16384
// Tamaño máximo de UNA línea de cabecera (como los 8 KB de NGINX) -> 431
# define MAX_HEADER_LINE 8192

// Resultado de HttpRequest::decodeChunked()
# define CHUNKED_DONE       1   // body chunked completo (llegó el chunk "0")
# define CHUNKED_NEED_MORE  0   // faltan bytes por llegar
# define CHUNKED_ERROR     -1   // formato chunked inválido -> 400

// Progreso de la lectura de un body chunked mientras va llegando. Se guarda
// en la sesión del cliente para no recorrer otra vez desde el principio todo
// lo recibido en cada recv() (con bodies de 100 MB sería muy lento).
struct ChunkProgress
{
    size_t pos;      // Dónde empieza la siguiente línea de tamaño (0 = sin empezar)
    size_t decoded;  // Bytes de datos de los chunks ya completos
    size_t partial;  // Bytes recibidos del chunk que está llegando ahora
    ChunkProgress() : pos(0), decoded(0), partial(0) {}
};

class HttpRequest
{
    private:
        std::string _start_line;
        std::string _method;
        std::string _path;
        std::string _resolved_path;
        std::string _query_string;
        std::string _www_root;
        std::string _uploads_directory;
        size_t client_id;
        std::map<std::string, std::string> _headers;
        std::string _body;
        bool _complete;
        int _chunked_status;    // Resultado de decodificar un body chunked
        int _errorCode;
        std::string _errorNote; // Por qué falló, solo para el log del servidor
        std::string _version;
        size_t _maxBodySize;
        std::string _request_id;
        const ServerConfig* _server_config;
        const LocationConfig* _location_config;

        void                parse_request_line(const std::string& raw_line);
        void                parseMessage(const std::string& raw_request);
        void                parseHeaderLine(const std::string& line);
        std::string         normalizeHeaderName(const std::string &name) const;
        static std::string  trim(const std::string &value);
        static std::string  sanitizeFilename(const std::string& raw_filename);
        static bool         saveFileToDisk(const std::string& full_path, const std::string& body_str, size_t data_start, size_t data_length);
        static bool         parseMultipartData(const std::string& body_str, std::string& filename, size_t& data_start, size_t& data_length);

    public:
        HttpRequest();
        ~HttpRequest();

        void parse(const std::string& raw_request);
        void showRequestInfo() const;
        static bool isCompleteRequest(const std::string& raw_request, size_t& request_size,
                                      ChunkProgress& chunk);
        static int  earlyErrorCode(const std::string& raw_request, size_t max_body_size,
                                   ChunkProgress& chunk);
        static int  scanChunked(const std::string& data, ChunkProgress& chunk);
        // "1234" -> 1234. false si no son solo dígitos (p.ej. "-1", "abc", "")
        static bool parseContentLength(const std::string& value, size_t& length);
        static int  decodeChunked(const std::string& data, size_t start, std::string* out,
                                  size_t& consumed, size_t& decoded);
        std::string         getDirectFilename() const;

        // Getters
        const std::string& getHeader(const std::string& key) const;
        const std::string& getBody() const;
        // Pasa el body a `out` sin copiarlo (swap). Para cuerpos grandes (CGI).
        void takeBody(std::string& out);
        const std::string& getStartLine() const;
        const std::map<std::string, std::string>& getHeaders() const;
        const std::string& getVersion() const;
        const std::string& getMethod() const;
        const std::string& getPath() const;
        const std::string& getResolvedPath() const;
        const ServerConfig* getServerConfig() const;
        const LocationConfig* getLocationConfig() const;
        const std::string& getQueryString() const;
        const std::string& getWwwRoot() const;
        const std::string& getUploadsDirectory() const;
        size_t getmaxBodySize() const;
        int getErrorCode() const;
        const std::string& getErrorNote() const;
        size_t getClientId() const;
        const std::string& getRequestId() const;

        // Setters
        void setErrorCode(int code);
        void setErrorCode(int code, const std::string& note);
        void setHeader(const std::string& key, const std::string& value);
        void setStartLine(const std::string& start_line);
        void setWwwRoot(const std::string& www_root);
        void setUploadsDirectory(const std::string& uploads_directory);
        void setmaxBodySize(size_t maxBodySize);
        void setClientId(size_t id);
        void setRequestId(const std::string& req_id);
        void setResolvedPath(const std::string& path);
        void setServerConfig(const ServerConfig* config);
        void setLocationConfig(const LocationConfig* config);
};

#endif // HttpRequest_HPP