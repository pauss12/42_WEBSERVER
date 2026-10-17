#include "Server.hpp"

const LocationConfig* Server::findMatchingLocation(const ServerConfig* server, const std::string& path)
{
    if (!server) return NULL;
    
    const std::vector<LocationConfig>& locations = server->getLocations();
    const LocationConfig* best_match = NULL;
    size_t longest_match = 0;

    for (size_t i = 0; i < locations.size(); ++i) {
        const std::string& match_str = locations[i].getLocationMatch();
        if (path.rfind(match_str, 0) == 0)
        {
            if (match_str.length() > longest_match) {
                longest_match = match_str.length();
                best_match = &locations[i];
            }
        }
    }
    return best_match;
}

static bool isInsideWorkspace(const std::string& p)
{
    if (p.empty() || p[0] == '/')              // rutas absolutas fuera
        return false;
    if (p == ".." || p.find("../") != std::string::npos
        || (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0))
        return false;                          // nada de salir con ..
    return true;
}

std::string Server::resolveUploadDirectory(const ServerConfig* server, const HttpRequest& req)
{
    const LocationConfig* loc = findMatchingLocation(server, req.getPath());
    std::string root;

    // upload_path viene de la config (lo escribe quien administra el servidor,
    // no el cliente): se respeta tal cual, también si es una ruta absoluta.
    if (loc && !loc->getUploadPath().empty())
        root = loc->getUploadPath();
    else
    {
        if (loc && !loc->getRoot().empty())
            root = loc->getRoot() + "/uploads";
        else if (server && !server->getRoot().empty())
            root = server->getRoot() + "/uploads";

        // Ruta deducida: si no hay nada o se sale del workspace, usamos la local
        if (!isInsideWorkspace(root))
            root = "www/uploads";
    }

    if (!root.empty() && root[root.length() - 1] == '/')
        root = root.substr(0, root.length() - 1);

    return root;
}

void Server::applyRequestConfiguration(HttpRequest& request, const ServerConfig* server)
{
    const LocationConfig* loc = findMatchingLocation(server, request.getPath());
    request.setServerConfig(server);
    request.setLocationConfig(loc);

    std::string root = server ? server->getRoot() : "www";
    std::string resolved_path = request.getPath();
    if (loc)
    {
        if (!loc->getRoot().empty())
            root = loc->getRoot();
        if (!loc->getAlias().empty())
            root = loc->getAlias();

        std::string match = loc->getLocationMatch();
        if (match != "/" && resolved_path.compare(0, match.size(), match) == 0)
            resolved_path = resolved_path.substr(match.size());
        if (resolved_path.empty() || resolved_path[0] != '/')
            resolved_path = "/" + resolved_path;
    }
    if (!root.empty() && root[root.size() - 1] == '/')
        root.erase(root.size() - 1);
    request.setWwwRoot(root);
    request.setResolvedPath(resolved_path);
}

// client_max_body_size that applies to the request at the start of `buffer`:
// the one of its location if it sets one, otherwise the server's. Only the
// request line is needed ("POST /post_body HTTP/1.1"), so it can be applied
// before the body arrives.
size_t Server::maxBodySizeFor(const ServerConfig* server, const std::string& buffer)
{
    size_t server_max = server ? server->getClientMaxBodySize() : 50 * 1024 * 1024;

    size_t line_end = buffer.find("\r\n");
    if (line_end == std::string::npos)
        return server_max;
    std::string line = buffer.substr(0, line_end);
    size_t first_space = line.find(' ');
    if (first_space == std::string::npos)
        return server_max;
    size_t path_end = line.find_first_of(" ?", first_space + 1);
    std::string path = line.substr(first_space + 1, path_end - first_space - 1);

    const LocationConfig* loc = findMatchingLocation(server, path);
    if (loc && loc->hasClientMaxBodySize())
        return loc->getClientMaxBodySize();
    return server_max;
}
