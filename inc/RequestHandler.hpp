#ifndef RequestHandler_HPP
# define RequestHandler_HPP

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include <string>
#include <fstream>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <sstream>
#include <cstdlib>

/**
 * @brief Clase encargada de toda la lógica de negocio.
 * Recibe una petición y devuelve una respuesta. Todo lo decide la location
 * de la configuración (métodos, redirect, root, index, autoindex, upload, CGI).
 */
class RequestHandler
{
    public:
        RequestHandler();
        ~RequestHandler();

        /**
         * @brief El método principal que decide qué hacer con cada petición.
         * @param req La petición HTTP ya parseada.
         * @return Un objeto HttpResponse listo para ser enviado al cliente.
         */
        HttpResponse handle(const HttpRequest& req);

        // Respuesta de error para un código HTTP. Usa la página de error_page
        // de la config si existe; si no, la plantilla www/err/template.html.
        HttpResponse buildError(int code, const std::string& req_id);

        // Server cuya config (error_page) usa buildError(). handle() lo pone
        // solo; hay que llamarlo si se usa buildError() sin pasar por handle().
        void setServerConfig(const ServerConfig* server);

        // Tras handle(): si la petición es CGI, handle() no ejecuta nada,
        // solo apunta aquí el script y su intérprete. El Server lo lanza de
        // forma no bloqueante.
        bool wantsCgi() const;
        const std::string& getCgiScript() const;
        const std::string& getCgiInterpreter() const;

    private:
        const ServerConfig* _server;
        std::string _cgi_script;
        std::string _cgi_interpreter;

        // Por método (src/RequestHandler.cpp y src/handleUpload.cpp)
        HttpResponse handleGet(const HttpRequest& req, const std::string& fs_path);
        HttpResponse handleDirectory(const HttpRequest& req, const std::string& fs_path);
        HttpResponse handleDelete(const HttpRequest& req, const std::string& fs_path);
        HttpResponse handleUploadRequest(const HttpRequest& req);
        HttpResponse serveFile(const HttpRequest& req, const std::string& fs_path);
        HttpResponse autoindexHtml(const HttpRequest& req, const std::string& dir);
        HttpResponse autoindexJson(const HttpRequest& req, const std::string& dir);
        HttpResponse handleError(int code, const std::string& message, const std::string& req_id);

        // Utilidades (src/otherUtilities.cpp)
        static HttpResponse withNote(HttpResponse res, const std::string& note);
        std::string getMimeType(const std::string& path);
        bool readFile(const std::string& path, std::string& content);
        std::string intToString(int val);
        std::string jsonEscape(const std::string& s);
        std::string htmlEscape(const std::string& s);
        std::string urlEncode(const std::string& s);

        void replaceAll(std::string &str, const std::string &from, const std::string &to);
        void setErrorVisual(int status, std::string &icon, std::string &visual_class, std::string &image);
        int handleFileUpload(const HttpRequest& req, size_t& saved);
};

#endif // RequestHandler_HPP
