#include "HttpRequest.hpp"
#include <cctype>
#include <cstdlib>

static bool is_valid_request_line(const std::string& line) {
    // 1. No permite tabuladores ni bytes nulos
    if (line.find('\t') != std::string::npos || line.find('\0') != std::string::npos)
        return false;

    // 2. No permite espacios dobles o consecutivos
    if (line.find("  ") != std::string::npos)
        return false;

    // 3. Contar que existan EXACTAMENTE dos espacios en toda la línea
    size_t space_count = 0;
    for (size_t i = 0; i < line.length(); ++i) {
        if (line[i] == ' ')
            space_count++;
    }
    if (space_count != 2)
        return false;

    return true;
}

// Convierte las secuencias %XX de la URL en el byte que representan
// (p.ej. "%20" -> ' '), para poder abrir ficheros con espacios o tildes.
static std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size()
            && std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(s[i + 2])))
        {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), NULL, 16));
            i += 2;
        }
        else
            out += s[i];
    }
    return out;
}

// La ruta debe empezar por '/', no puede contener bytes nulos ni ningun
// segmento "..": si no, un cliente podria pedir /../../etc/passwd y salirse
// del root. Se comprueba DESPUES de decodificar para que %2e%2e no se cuele.
static bool isSafePath(const std::string& path)
{
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos)
        return false;
    std::istringstream segments(path);
    std::string segment;
    while (std::getline(segments, segment, '/'))
    {
        if (segment == "..")
            return false;
    }
    return true;
}

/**
 * @brief Parsea la primera línea (Request Line) y separa la Query String si existe.
 */
void HttpRequest::parse_request_line(const std::string& raw_line)
{
    std::string line = raw_line;

    // 1. Strip trailing carriage return if present
    size_t end_pos = line.find_first_of("\r\n");
    if (end_pos != std::string::npos) {
        line = line.substr(0, end_pos);
    }

    this->setStartLine(line);

    // Validar formato básico de espacios y caracteres
    if (!is_valid_request_line(line)) {
        this->setErrorCode(400, "malformed request line"); // 400 Bad Request
        return;
    }

    // Extraer los 3 componentes según los espacios
    size_t first_space = line.find(' ');
    size_t last_space = line.rfind(' ');

    this->_method = line.substr(0, first_space);
    std::string raw_path = line.substr(first_space + 1, last_space - (first_space + 1));
    this->_version = line.substr(last_space + 1);

    // --- LIMPIEZA CLAVE DE LA VERSIÓN ---
    // Elimina cualquier \r, \n, tabulador o espacio sobrante al final de _version
    size_t ver_end = this->_version.find_first_of(" \t\r\n");
    if (ver_end != std::string::npos)
        this->_version = this->_version.substr(0, ver_end);

    // 4. Validar longitud de la URI (Límite 2048)
    if (raw_path.length() > 2048) {
        this->setErrorCode(414, "URI longer than 2048 characters"); // 414 URI Too Long
        return ;
    }

    // 5. Validar versión HTTP
    //    Mal escrita (no es "HTTP/<dígito>.<dígito>", p.ej. "HOLA QUE TAL") -> 400.
    //    Bien escrita pero no soportada (p.ej. HTTP/2.0)                     -> 505.
    const std::string& v = this->_version;
    if (v.size() != 8 || v.compare(0, 5, "HTTP/") != 0
        || !std::isdigit(static_cast<unsigned char>(v[5])) || v[6] != '.'
        || !std::isdigit(static_cast<unsigned char>(v[7]))) {
        this->setErrorCode(400, "malformed HTTP version '" + v + "'");
        return;
    }
    if (v != "HTTP/1.0" && v != "HTTP/1.1") {
        this->setErrorCode(505, "unsupported version " + v); // 505 HTTP Version Not Supported
        return;
    }

    // 6. Extraer Path y Query String
    size_t question_mark = raw_path.find('?');
    if (question_mark != std::string::npos) {
        this->_path = urlDecode(raw_path.substr(0, question_mark));
        this->_query_string = raw_path.substr(question_mark + 1);
    } else {
        this->_path = urlDecode(raw_path);
        this->_query_string = "";
    }

    // "OPTIONS * HTTP/1.1" (asterisk-form, RFC 9112): '*' es una ruta válida
    // solo para OPTIONS. El handler responderá 501 (OPTIONS no implementado).
    if (raw_path == "*" && this->_method == "OPTIONS")
        return;

    // 7. Evitar path traversal
    if (!isSafePath(this->_path)) {
        this->setErrorCode(400, "unsafe path (\"..\" or not starting with '/')");
        return;
    }
}