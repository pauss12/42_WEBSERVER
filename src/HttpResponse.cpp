#include "HttpResponse.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

HttpResponse::HttpResponse()
    : _status_code(500), _status_message("Internal Server Error")
{
}

HttpResponse::HttpResponse(const HttpResponse& other)
{
    *this = other;
}

HttpResponse::~HttpResponse()
{
}

HttpResponse& HttpResponse::operator=(const HttpResponse& other)
{
    if (this != &other) 
    {
        this->_status_code = other._status_code;
        this->_status_message = other._status_message;
        this->_headers = other._headers;
        this->_body = other._body;
        this->_start_line = other._start_line;
        this->_note = other._note;
    }
    return (*this);
}

// GETTERS
int HttpResponse::getStatusCode() const
{
    return (_status_code);
}

const std::string& HttpResponse::getStatusMessage() const
{
    return (_status_message);
}

const std::string& HttpResponse::getBody() const
{
    return _body;
}

const std::map<std::string, std::string>& HttpResponse::getHeaders() const
{
    return _headers;
}

const std::string& HttpResponse::getNote() const
{
    return _note;
}

// SETTERS
void HttpResponse::setNote(const std::string& note)
{
    _note = note;
}

void HttpResponse::setStatusCode(int code)
{
    _status_code = code;
}

void HttpResponse::setStatusMessage(const std::string& message)
{
    _status_message = message;
}

void HttpResponse::setHeader(const std::string& key, const std::string& value)
{
    _headers[key] = value;
}

void HttpResponse::setBody(const std::string& body)
{
    _body = body;
}

void HttpResponse::swapBody(std::string& body)
{
    _body.swap(body);
}

void HttpResponse::setStartLine(const std::string& start_line)
{
    _start_line = start_line;
}

std::string HttpResponse::toString(int value) const
{
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

std::string HttpResponse::reasonPhrase(int code)
{
    switch (code)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string HttpResponse::serialize() const
{
    std::ostringstream head;

    if (_start_line.empty())
        head << "HTTP/1.1 " << _status_code << " " << _status_message << "\r\n";
    else
        head << _start_line << "\r\n";

    for (std::map<std::string, std::string>::const_iterator it = _headers.begin(); it != _headers.end(); ++it)
        head << it->first << ": " << it->second << "\r\n";

    head << "\r\n";

    // Cabeceras + body en un solo string, reservando la memoria de una vez
    // (el body puede ser de 100 MB: así no se copia más de lo necesario)
    std::string out = head.str();
    out.reserve(out.size() + _body.size());
    out += _body;
    return out;
}