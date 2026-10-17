#include "HttpRequest.hpp"
#include <cstring>
#include <cctype>

HttpRequest::HttpRequest()
    : client_id(0), _complete(false), _chunked_status(CHUNKED_DONE), _errorCode(0), _maxBodySize(0),
      _server_config(NULL), _location_config(NULL)
{
}

/**
 * Destructor for the HttpRequest class. Currently, it does not perform any specific cleanup, but it's defined to allow for future resource management if needed.
 * @addtogroup HttpRequest
 */
HttpRequest::~HttpRequest()
{
}

// GETTERS ---------------------------------------------------------------
const std::string& HttpRequest::getHeader(const std::string& key) const
{
    std::map<std::string, std::string>::const_iterator it = _headers.find(normalizeHeaderName(key));
    if (it == _headers.end()) {
        static const std::string empty = "";
        return empty;
    }
    return it->second;
}

const std::string& HttpRequest::getBody() const
{
    return _body;
}

void HttpRequest::takeBody(std::string& out)
{
    out.swap(_body);
    std::string().swap(_body);
}

const std::string& HttpRequest::getStartLine() const
{
    return _start_line;
}

const std::map<std::string, std::string>& HttpRequest::getHeaders() const
{
    return _headers;
}

const std::string& HttpRequest::getVersion() const
{
    return _version;
}

const std::string& HttpRequest::getWwwRoot() const
{
    return _www_root;
}

void HttpRequest::setWwwRoot(const std::string& www_root)
{
    _www_root = www_root;
}

void HttpRequest::setUploadsDirectory(const std::string& uploads_directory)
{
    _uploads_directory = uploads_directory;
}

int HttpRequest::getErrorCode() const
{
    return (_errorCode);
}

const std::string& HttpRequest::getMethod() const
{
    return (_method);
}

const std::string& HttpRequest::getPath() const
{
    return (_path);
}

const std::string& HttpRequest::getResolvedPath() const
{
    return (_resolved_path.empty() ? _path : _resolved_path);
}

const ServerConfig* HttpRequest::getServerConfig() const
{
    return _server_config;
}

const LocationConfig* HttpRequest::getLocationConfig() const
{
    return _location_config;
}

const std::string& HttpRequest::getQueryString() const
{
    return (_query_string);
}

const std::string& HttpRequest::getUploadsDirectory() const
{
    return (_uploads_directory);
}

size_t HttpRequest::getmaxBodySize() const
{
    return (_maxBodySize);
}

size_t HttpRequest::getClientId() const
{
    return (client_id);
}

const std::string& HttpRequest::getRequestId() const
{
    return (_request_id);
}

// SETTERS ------------------------------------------------------
void HttpRequest::setErrorCode(int code)
{
    _errorCode = code;
}

void HttpRequest::setErrorCode(int code, const std::string& note)
{
    _errorCode = code;
    _errorNote = note;
}

const std::string& HttpRequest::getErrorNote() const
{
    return _errorNote;
}

void HttpRequest::setHeader(const std::string& key, const std::string& value)
{
    std::string normalized_key = normalizeHeaderName(key);
    if (!normalized_key.empty())
        _headers[normalized_key] = value;
}

void HttpRequest::setStartLine(const std::string &startLine)
{
    _start_line = startLine;
}

void HttpRequest::setmaxBodySize(size_t maxBodySize)
{
    _maxBodySize = maxBodySize;
}

void HttpRequest::setClientId(size_t id)
{
    client_id = id;
}

void HttpRequest::setRequestId(const std::string& req_id)
{
    _request_id = req_id;
}

void HttpRequest::setResolvedPath(const std::string& path)
{
    _resolved_path = path;
}

void HttpRequest::setServerConfig(const ServerConfig* config)
{
    _server_config = config;
}

void HttpRequest::setLocationConfig(const LocationConfig* config)
{
    _location_config = config;
}

// OTHER FUNCTIONS
std::string HttpRequest::getDirectFilename() const
{
    std::string filename = this->getHeader("X-Filename");
    if (filename.empty())
        filename = "uploaded_file.bin";
    return filename;
}

std::string HttpRequest::trim(const std::string &value)
{
    std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string HttpRequest::normalizeHeaderName(const std::string &name) const
{
    std::string normalized = trim(name);
    for (std::string::size_type index = 0; index < normalized.size(); ++index)
        normalized[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[index])));
    return normalized;
}

// Parse Header Line
// Caracteres válidos en el nombre de una cabecera ("token" de RFC 9110)
static bool isTokenChar(unsigned char c)
{
    // c != '\0': strchr() would find the terminating NUL of the set
    return std::isalnum(c) || (c != '\0' && std::strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

void HttpRequest::parseHeaderLine(const std::string &line)
{
    if (line.size() > MAX_HEADER_LINE)
    {
        setErrorCode(431, "header line longer than 8 KB");
        return ;
    }
    std::string::size_type colon = line.find(':');
    if (colon == std::string::npos)
    {
        setErrorCode(400, "header line without ':'");
        return ;
    }
    if (colon > 0 && (line[colon - 1] == ' ' || line[colon - 1] == '\t'))
    {
        setErrorCode(400, "whitespace before ':' in header");
        return ;
    }
    std::string key = normalizeHeaderName(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (key.empty())
    {
        setErrorCode(400, "empty header name");
        return ;
    }
    // Nombre: solo caracteres "token". Valor: nada de caracteres de control
    // (NUL, CR, LF...) salvo el tabulador.
    for (std::string::size_type i = 0; i < key.size(); ++i)
    {
        if (!isTokenChar(static_cast<unsigned char>(key[i])))
        {
            setErrorCode(400, "invalid character in header name");
            return ;
        }
    }
    for (std::string::size_type i = 0; i < value.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if ((c < 0x20 && c != '\t') || c == 0x7f)
        {
            setErrorCode(400, "control character in header value");
            return ;
        }
    }

    if (_headers.find(key) != _headers.end())
    {
        if (key == "content-length" || key == "host")
        {
            setErrorCode(400, "duplicate " + key + " header");
            return ;
        }
    }
    _headers[key] = value;
}

/**
 * Parse Message: Parses the raw HTTP request string into its components: start line, headers, and body.
 * @param raw_request The raw HTTP request string to be parsed.
 * @addtogroup HttpRequest
 */
void HttpRequest::parseMessage(const std::string& raw_request)
{
    // --- 1. Inline Message::parse logic ---
    _headers.clear();
    _body.clear();
    _start_line.clear();
    _complete = false;
    _errorCode = 0;
    _chunked_status = CHUNKED_DONE;

    std::string::size_type header_end = raw_request.find("\r\n\r\n");
    std::string header_block = raw_request;
    if (header_end != std::string::npos)
    {
        header_block = raw_request.substr(0, header_end);
        _complete = true;
    }

    std::istringstream stream(header_block);
    std::string line;

    if (std::getline(stream, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        _start_line = line;
    }

    while (std::getline(stream, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        if (line.empty())
            break;
        parseHeaderLine(line);
    }

    // Body: si es chunked se decodifica directamente desde la petición (una
    // sola copia, el body puede ser de 100 MB); si no, se copia tal cual.
    if (header_end != std::string::npos)
    {
        if (normalizeHeaderName(getHeader("Transfer-Encoding")) == "chunked")
        {
            size_t consumed = 0;
            size_t decoded = 0;
            _chunked_status = decodeChunked(raw_request, header_end + 4, &_body, consumed, decoded);
        }
        else
            _body = raw_request.substr(header_end + 4);
    }
}

/**
 * Parses the raw HTTP request string and extracts the method, path, version, headers, and body.
 * @param raw_request The raw HTTP request string to be parsed.
 * @addtogroup HttpRequest
 */
void HttpRequest::parse(const std::string& raw_request)
{
    parseMessage(raw_request);

    this->parse_request_line(this->getStartLine());
    if (this->getErrorCode() != 0)
        return;

    // 2. Validaciones de Headers específicas para HTTP/1.1
    bool has_content_length = !this->getHeader("Content-Length").empty();
    bool has_transfer_encoding = !this->getHeader("Transfer-Encoding").empty();

    // Host es obligatorio en HTTP/1.1
    if (this->_version == "HTTP/1.1" && this->getHeader("Host").empty()) {
        this->setErrorCode(400, "missing Host header");
        return;
    }

    // Conflicto de cabeceras de cuerpo (HTTP Smuggling protection)
    size_t content_length = 0;
    if (has_content_length && !parseContentLength(this->getHeader("Content-Length"), content_length)) {
        this->setErrorCode(400, "invalid Content-Length '" + this->getHeader("Content-Length") + "'");
        return;
    }

    if (has_content_length && has_transfer_encoding) {
        this->setErrorCode(400, "both Content-Length and Transfer-Encoding");
        // std::cout << "[ERROR] Conflicto de cabeceras de cuerpo (HTTP Smuggling protection)" << std::endl;
        return;
    }

    // Body chunked: lo des-chunkeamos aquí, así el CGI y el upload reciben el
    // body real (el subject: "your server needs to un-chunk them, the CGI will
    // expect EOF as the end of the body").
    if (has_transfer_encoding)
    {
        std::string encoding = normalizeHeaderName(this->getHeader("Transfer-Encoding"));
        if (encoding != "chunked")
        {
            this->setErrorCode(501, "Transfer-Encoding '" + encoding + "' not supported");
            return;
        }
        // Ya decodificado en parseMessage()
        if (_chunked_status != CHUNKED_DONE)
        {
            this->setErrorCode(400, "malformed chunked body");
            return;
        }
    }

    // Validar tamaño máximo del Body
    if (this->getBody().size() > this->getmaxBodySize())
    {
        // std::cout << RED << "[ERROR 413] Body size exceeds maximum allowed size: " << this->getBody().size() << " > " << this->getmaxBodySize() << NC << std::endl;
        std::ostringstream note;
        note << "body " << this->getBody().size() << " B > client_max_body_size " << this->getmaxBodySize() << " B";
        this->setErrorCode(413, note.str()); // Payload Too Large
        return;
    }
}


// SHOW REQUEST INFO
void HttpRequest::showRequestInfo() const 
{
    std::cout << GRAY << "------------------------------------ " << NC << std::endl;
    std::cout << "Parsed HTTP Request: " << std::endl;
    std::cout << "Method: " << this->getMethod() << std::endl;
    std::cout << "Path: " << this->getPath() << std::endl;
    std::cout << "Version: " << this->getVersion() << std::endl;
    std::cout << "Body: " << this->getBody() << std::endl;
    std::cout << "Headers: " << std::endl;
    
    const std::map<std::string, std::string>& headers = this->getHeaders();
    for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
        std::cout << it->first << ": " << it->second << std::endl;

    std::cout << GRAY << "------------------------------------ " << NC << std::endl << std::endl;    
}