#include "RequestHandler.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>

RequestHandler::RequestHandler() : _server(NULL) {}
RequestHandler::~RequestHandler() {}

namespace {
    // "/www/cgi/test.py" -> ".py"  ("" si no tiene extensión)
    std::string extensionOf(const std::string& path)
    {
        size_t slash = path.find_last_of('/');
        size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        return path.substr(dot);
    }

    // "GET, POST, DELETE" para la cabecera Allow de un 405
    std::string joinMethods(const std::vector<std::string>& methods)
    {
        std::string out;
        for (size_t i = 0; i < methods.size(); ++i)
            out += (i ? ", " : "") + methods[i];
        return out;
    }
}

/**
 * @brief Decide qué hacer con la petición, siempre según su location:
 *   1. error de parseo                       -> ese código (400/413/414/505)
 *   2. método que no implementamos           -> 501 (implementados: GET, HEAD, POST, DELETE)
 *   3. ninguna location coincide             -> 404
 *   4. método no permitido en la location    -> 405 (+ cabecera Allow)
 *   5. la location tiene redirect            -> 301/302/...
 *   6. la extensión está en "cgi .ext ..."   -> CGI (lo lanza el Server)
 *   7. GET/HEAD -> fichero / directorio (index, autoindex); HEAD sin body
 *      POST -> upload si upload_enabled, si no 405
 *      DELETE -> borrar el fichero
 */
HttpResponse RequestHandler::handle(const HttpRequest& req)
{
    _cgi_script.clear();
    _cgi_interpreter.clear();
    _server = req.getServerConfig();
    const std::string& id = req.getRequestId();

    // 1. Errores de parseo detectados por HttpRequest
    if (req.getErrorCode() != 0)
        return withNote(buildError(req.getErrorCode(), id), req.getErrorNote());

    // 2. Solo implementamos GET, HEAD, POST y DELETE
    const std::string& method = req.getMethod();
    if (method != "GET" && method != "HEAD" && method != "POST" && method != "DELETE")
        return withNote(buildError(501, id), method + " is not implemented");

    // 3. Location que corresponde a la URL (la elige Server::applyRequestConfiguration)
    const LocationConfig* location = req.getLocationConfig();
    if (!location)
        return withNote(buildError(404, id), "no location matches " + req.getPath());

    // 4. Método permitido en esta location
    const std::vector<std::string>& allowed = location->getAllowedMethods();
    if (std::find(allowed.begin(), allowed.end(), method) == allowed.end())
    {
        HttpResponse res = withNote(buildError(405, id),
                                    method + " not allowed in location " + location->getLocationMatch());
        res.setHeader("Allow", joinMethods(allowed));
        return res;
    }

    // 5. Redirección
    if (location->getRedirect().first != 0)
    {
        HttpResponse redirect;
        redirect.setStatusCode(location->getRedirect().first);
        redirect.setStatusMessage(HttpResponse::reasonPhrase(location->getRedirect().first));
        redirect.setHeader("Location", location->getRedirect().second);
        redirect.setHeader("Content-Length", "0");
        redirect.setNote("-> " + location->getRedirect().second);
        return redirect;
    }

    // Ruta real en disco: root (o alias) de la location + resto de la URL
    std::string fs_path = req.getWwwRoot() + req.getResolvedPath();

    // 6. CGI por extensión. No se ejecuta aquí: el Server lo lanza con
    //    CgiHandler::start() y lee su salida a través de kqueue/epoll.
    std::map<std::string, std::string>::const_iterator cgi = location->getCgi().find(extensionOf(fs_path));
    if (cgi != location->getCgi().end())
    {
        _cgi_script = fs_path;
        _cgi_interpreter = cgi->second;
        return HttpResponse();
    }

    // 7. Según el método. HEAD responde lo mismo que GET; el Server quita el
    //    body antes de enviarlo (Server::queueResponse).
    if (method == "GET" || method == "HEAD")
        return handleGet(req, fs_path);
    if (method == "DELETE")
        return handleDelete(req, fs_path);
    if (location->getUploadEnabled())
        return handleUploadRequest(req);
    HttpResponse res = withNote(buildError(405, id),
                                "POST needs 'upload_enabled on' or a CGI script in location "
                                + location->getLocationMatch());
    res.setHeader("Allow", "GET, DELETE");
    return res;
}

// DELETE: solo ficheros normales.
HttpResponse RequestHandler::handleDelete(const HttpRequest& req, const std::string& fs_path)
{
    const std::string& id = req.getRequestId();
    struct stat st;

    if (stat(fs_path.c_str(), &st) != 0)
        return withNote(buildError(404, id), "nothing to delete at " + fs_path);
    if (S_ISDIR(st.st_mode))
        return withNote(buildError(403, id), "refusing to delete a directory: " + fs_path);

    // remove() trabaja sobre el disco, no sobre un socket/pipe: aquí sí se
    // puede mirar errno para distinguir "sin permiso" de otros fallos.
    if (std::remove(fs_path.c_str()) != 0)
    {
        if (errno == EACCES || errno == EPERM)
            return withNote(buildError(403, id), "no permission to delete " + fs_path);
        return withNote(buildError(500, id), "could not delete " + fs_path);
    }

    HttpResponse res;
    res.setStatusCode(200);
    res.setStatusMessage("OK");
    res.setHeader("Content-Length", "0");
    res.setNote("deleted " + fs_path);
    return res;
}

// Devuelve `res` con una nota para el log del servidor (no se envía al cliente).
HttpResponse RequestHandler::withNote(HttpResponse res, const std::string& note)
{
    res.setNote(note);
    return res;
}

void RequestHandler::setServerConfig(const ServerConfig* server)
{
    _server = server;
}

bool RequestHandler::wantsCgi() const
{
    return !_cgi_script.empty();
}

const std::string& RequestHandler::getCgiScript() const
{
    return _cgi_script;
}

const std::string& RequestHandler::getCgiInterpreter() const
{
    return _cgi_interpreter;
}

HttpResponse RequestHandler::buildError(int code, const std::string& req_id)
{
    switch (code)
    {
        case 403: return handleError(403, ERROR_403, req_id);
        case 404: return handleError(404, ERROR_404, req_id);
        case 405: return handleError(405, ERROR_405, req_id);
        case 408: return handleError(408, ERROR_408, req_id);
        case 409: return handleError(409, ERROR_409, req_id);
        case 413: return handleError(413, ERROR_413, req_id);
        case 414: return handleError(414, ERROR_414, req_id);
        case 431: return handleError(431, ERROR_431, req_id);
        case 500: return handleError(500, ERROR_500, req_id);
        case 501: return handleError(501, ERROR_501, req_id);
        case 502: return handleError(502, ERROR_502, req_id);
        case 504: return handleError(504, ERROR_504, req_id);
        case 505: return handleError(505, ERROR_505, req_id);
        default:  return handleError(400, ERROR_400, req_id);
    }
}

/**
 * @brief Genera una respuesta de error HTTP.
 * 1. Si la config del server tiene "error_page <code> <fichero>" y el fichero
 *    se puede leer, se envía ese fichero.
 * 2. Si no, la plantilla www/err/template.html (con el chiste en el mensaje).
 * 3. Si tampoco existe la plantilla, un HTML mínimo.
 * La línea de estado siempre lleva el código original y su reason phrase.
 */
HttpResponse RequestHandler::handleError(int code, const std::string& message, const std::string & req_id)
{
    HttpResponse res;
    res.setStatusCode(code);
    // La línea de estado lleva el reason phrase estándar ("Method Not
    // Allowed"); el mensaje divertido solo va dentro de la página HTML.
    res.setStatusMessage(HttpResponse::reasonPhrase(code));

    std::string html;

    // 1. Página propia de la config (error_page)
    if (_server)
    {
        std::map<int, std::string>::const_iterator page = _server->getErrorPages().find(code);
        if (page != _server->getErrorPages().end() && readFile(page->second, html))
        {
            res.setHeader("Content-Type", getMimeType(page->second));
            res.setHeader("Content-Length", intToString(html.size()));
            res.setHeader("Connection", "close");
            res.setBody(html);
            return res;
        }
    }

    // 2. Plantilla por defecto: www/err/template.html
    if (readFile("www/err/template.html", html))
    {
        std::string icon = "";
        std::string image = "";
        std::string visual_class = "";

        RequestHandler::setErrorVisual(code, icon, visual_class, image);

        replaceAll(html, "{{status}}", intToString(code));
        replaceAll(html, "{{title}}", HttpResponse::reasonPhrase(code));
        replaceAll(html, "{{message}}", message);
        replaceAll(html, "{{request_id}}", req_id.empty() ? "N/A" : req_id);

        replaceAll(html, "{{image}}", image);
        replaceAll(html, "{{icon}}", icon);
        replaceAll(html, "{{visual_class}}", visual_class);
        replaceAll(html, "{{favicon}}", "/favicon.ico");

        res.setHeader("Content-Type", "text/html; charset=UTF-8");
    }
    // 3. Sin plantilla: HTML mínimo
    else
    {
        html = "<html><body><h1>" + intToString(code) + " " + HttpResponse::reasonPhrase(code)
             + "</h1><p>" + message + "</p></body></html>";
        res.setHeader("Content-Type", "text/html");
    }

    res.setHeader("Content-Length", intToString(html.size()));
    res.setHeader("Connection", "close");
    res.setBody(html);
    return res;
}

// ################################################################################################
// GET: ficheros estáticos, directorios, index y autoindex
// ################################################################################################

// GET de cualquier ruta:
//   no existe            -> 404
//   es un directorio     -> handleDirectory (index / autoindex / 403)
//   no se puede leer     -> 403
//   fichero normal       -> 200 con su contenido (un fichero vacío también es 200)
HttpResponse RequestHandler::handleGet(const HttpRequest& req, const std::string& fs_path)
{
    const std::string& id = req.getRequestId();
    struct stat st;

    if (stat(fs_path.c_str(), &st) != 0)
        return withNote(buildError(404, id), "no file at " + fs_path);
    if (S_ISDIR(st.st_mode))
        return handleDirectory(req, fs_path);
    if (!S_ISREG(st.st_mode) || access(fs_path.c_str(), R_OK) != 0)
        return withNote(buildError(403, id), "cannot read " + fs_path);
    return serveFile(req, fs_path);
}

// Directorio:
//   existe el fichero "index"       -> se sirve (por defecto index.html), con o
//                                      sin '/' final en la URL
//   autoindex on                    -> listado (HTML, o JSON si se pide); si la
//                                      URL no acaba en '/', antes 301 a URL + "/",
//                                      porque los enlaces del listado son relativos
//   si no                           -> 404 (no hay nada que servir; es lo que
//                                      espera el tester de 42 en /directory/Yeah)
HttpResponse RequestHandler::handleDirectory(const HttpRequest& req, const std::string& fs_path)
{
    const std::string& id = req.getRequestId();
    const std::string& url = req.getPath();
    bool trailing_slash = !url.empty() && url[url.size() - 1] == '/';

    std::string dir = fs_path;
    if (dir.empty() || dir[dir.size() - 1] != '/')
        dir += "/";

    const LocationConfig* location = req.getLocationConfig();
    if (!location->getIndex().empty())
    {
        std::string index_path = dir + location->getIndex();
        struct stat st;
        if (stat(index_path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        {
            if (access(index_path.c_str(), R_OK) != 0)
                return withNote(buildError(403, id), "cannot read " + index_path);
            return serveFile(req, index_path);
        }
    }

    if (location->getAutoindex())
    {
        if (!trailing_slash)
        {
            std::string target = url + "/";
            if (!req.getQueryString().empty())
                target += "?" + req.getQueryString();
            HttpResponse redirect;
            redirect.setStatusCode(301);
            redirect.setStatusMessage(HttpResponse::reasonPhrase(301));
            redirect.setHeader("Location", target);
            redirect.setHeader("Content-Length", "0");
            redirect.setNote("directory -> " + target);
            return redirect;
        }
        // La página /showUploads pide el listado en JSON con "Accept: application/json"
        if (req.getHeader("Accept").find("application/json") != std::string::npos)
            return autoindexJson(req, dir);
        return autoindexHtml(req, dir);
    }

    return withNote(buildError(404, id), "directory without " + location->getIndex()
                                         + " and autoindex off: " + dir);
}

HttpResponse RequestHandler::serveFile(const HttpRequest& req, const std::string& fs_path)
{
    std::string content;
    if (!readFile(fs_path, content))
        return withNote(buildError(500, req.getRequestId()), "error reading " + fs_path);

    HttpResponse res;
    res.setStatusCode(200);
    res.setStatusMessage("OK");
    res.setHeader("Content-Type", getMimeType(fs_path));
    res.setHeader("Content-Length", intToString(content.size()));
    res.setBody(content);
    return res;
}

namespace {
    struct Entry {
        std::string name;
        bool        is_dir;
        off_t       size;
        time_t      mtime;
        bool operator<(Entry const& other) const {
            if (is_dir != other.is_dir)
                return is_dir;              // directorios primero
            return name < other.name;
        }
    };

    // Lee las entradas de un directorio (sin ".", ".." ni ficheros ocultos
    // como .gitkeep). Devuelve false si no se puede abrir.
    bool listDirectory(const std::string& dir, std::vector<Entry>& entries)
    {
        DIR* d = opendir(dir.c_str());
        if (!d)
            return false;
        struct dirent* item;
        while ((item = readdir(d)) != NULL)
        {
            std::string name = item->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            struct stat st;
            if (stat((dir + name).c_str(), &st) != 0)
                continue;
            Entry entry;
            entry.name = name;
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size = st.st_size;
            entry.mtime = st.st_mtime;
            entries.push_back(entry);
        }
        closedir(d);
        std::sort(entries.begin(), entries.end());
        return true;
    }
}

// autoindex on: página HTML con un enlace por entrada del directorio
HttpResponse RequestHandler::autoindexHtml(const HttpRequest& req, const std::string& dir)
{
    std::vector<Entry> entries;
    if (!listDirectory(dir, entries))
        return withNote(buildError(403, req.getRequestId()), "cannot open directory " + dir);

    const std::string& url = req.getPath();
    std::ostringstream html;
    html << "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
         << "<title>Index of " << htmlEscape(url) << "</title>\n"
         << "<style>body{font-family:system-ui,sans-serif;margin:2rem}"
         << "td{padding:.2rem 1.5rem .2rem 0}a{text-decoration:none}</style>\n"
         << "</head>\n<body>\n<h1>Index of " << htmlEscape(url) << "</h1>\n<table>\n";
    if (url != "/")
        html << "<tr><td><a href=\"../\">../</a></td><td></td></tr>\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        std::string name = entries[i].name + (entries[i].is_dir ? "/" : "");
        html << "<tr><td><a href=\"" << urlEncode(entries[i].name) << (entries[i].is_dir ? "/" : "")
             << "\">" << htmlEscape(name) << "</a></td><td>";
        if (!entries[i].is_dir)
            html << static_cast<unsigned long>(entries[i].size) << " bytes";
        html << "</td></tr>\n";
    }
    html << "</table>\n</body>\n</html>\n";

    HttpResponse res;
    res.setStatusCode(200);
    res.setStatusMessage("OK");
    res.setHeader("Content-Type", "text/html; charset=UTF-8");
    res.setHeader("Content-Length", intToString(html.str().size()));
    res.setBody(html.str());
    res.setNote("autoindex of " + dir);
    return res;
}

// autoindex on + "Accept: application/json": la misma lista en JSON
// {"files":[{"name":"foto.png","size":1234,"mtime":1700000000}, ...]}
HttpResponse RequestHandler::autoindexJson(const HttpRequest& req, const std::string& dir)
{
    std::vector<Entry> entries;
    if (!listDirectory(dir, entries))
        return withNote(buildError(403, req.getRequestId()), "cannot open directory " + dir);

    std::ostringstream json;
    json << "{\"files\":[";
    bool first = true;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].is_dir)
            continue;   // solo ficheros
        if (!first)
            json << ",";
        first = false;
        json << "{\"name\":\"" << jsonEscape(entries[i].name) << "\","
             << "\"size\":" << static_cast<unsigned long>(entries[i].size) << ","
             << "\"mtime\":" << static_cast<unsigned long>(entries[i].mtime) << "}";
    }
    json << "]}";

    HttpResponse res;
    res.setStatusCode(200);
    res.setStatusMessage("OK");
    res.setHeader("Content-Type", "application/json; charset=UTF-8");
    res.setHeader("Content-Length", intToString(json.str().size()));
    res.setBody(json.str());
    res.setNote("autoindex (JSON) of " + dir);
    return res;
}
