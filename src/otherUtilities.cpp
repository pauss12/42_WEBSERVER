#include "RequestHandler.hpp"
#include <algorithm>
#include <cctype>

void RequestHandler::replaceAll(std::string &str, const std::string &from, const std::string &to)
{
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

void RequestHandler::setErrorVisual(int status, std::string &icon, std::string &visual_class, std::string &image)
{
    if (status == 404)
    {
        icon = "⚠️";
        image = "/img/404.avif";
        visual_class = "client-error";
    } 
    else if (status >= 400 && status < 500) 
    {
        icon = "⚠️";
        visual_class = "client-error";
    } else if (status >= 500)
    {
        icon = "🔥";
        visual_class = "server-error";
        image = "favicon-500.ico";
    }
    else 
    {
        icon = "ℹ️";
        visual_class = "info-error";
    }
}

std::string RequestHandler::intToString(int val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// Content-Type según la extensión real del fichero (lo que va tras el último
// punto del nombre). Con find(".js") un "datos.json" salía como JavaScript.
std::string RequestHandler::getMimeType(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "application/octet-stream";

    std::string ext = path.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
        ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));

    static const char* types[][2] = {
        {"html", "text/html; charset=UTF-8"}, {"htm", "text/html; charset=UTF-8"},
        {"css", "text/css"}, {"js", "application/javascript"},
        {"json", "application/json"}, {"txt", "text/plain; charset=UTF-8"},
        {"xml", "application/xml"}, {"csv", "text/csv"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"ico", "image/x-icon"},
        {"webp", "image/webp"}, {"avif", "image/avif"}, {"bmp", "image/bmp"},
        {"pdf", "application/pdf"}, {"zip", "application/zip"},
        {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"}, {"webm", "video/webm"},
        {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"},
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
    {
        if (ext == types[i][0])
            return types[i][1];
    }
    return "application/octet-stream";
}

// Lee un fichero NORMAL del disco entero en `content`. Devuelve false si no
// existe, es un directorio o no se puede leer. Un fichero vacío devuelve true
// con content vacío (antes "vacío" y "no existe" eran lo mismo -> 404).
bool RequestHandler::readFile(const std::string& path, std::string& content)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open())
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();
    return true;
}

// Para meter texto dentro de HTML: < > & " no se interpretan como etiquetas
std::string RequestHandler::htmlEscape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '<')       out += "&lt;";
        else if (s[i] == '>')  out += "&gt;";
        else if (s[i] == '&')  out += "&amp;";
        else if (s[i] == '"')  out += "&quot;";
        else                   out += s[i];
    }
    return out;
}

// Para meter un nombre de fichero en un enlace: "mi foto.png" -> "mi%20foto.png"
std::string RequestHandler::urlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Escapa comillas, barras y caracteres de control para meter el nombre en JSON
std::string RequestHandler::jsonEscape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20)  out += ' ';
        else                out += s[i];
    }
    return out;
}