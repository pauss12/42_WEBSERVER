#include "HttpRequest.hpp"
#include <cstdlib>

/**
 * Returns a copy of the input string with all characters converted to lowercase.
 * @param value The string to convert.
 * @return A new string with all characters in lowercase.
 */
static std::string toLowerCopy(std::string str)
{
    // Se castea a unsigned char para evitar comportamientos indefinidos con std::tolower
	for (size_t i = 0; i < str.length(); ++i)
        str[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(str[i])));
    return (str);
}

static bool findHeaderEnd(const std::string& raw_request, size_t& header_end)
{
    header_end = raw_request.find("\r\n\r\n");
    return header_end != std::string::npos;
}

/**
 * Returns the value of header `name` (lowercase) in the header block, or "".
 * @param header_block Request line + headers (without the final \r\n\r\n).
 * @param name Header name in lowercase, e.g. "content-length".
 */
static std::string extractHeader(const std::string& header_block, const std::string& name)
{
    std::istringstream stream(header_block);
    std::string line;

    if (!std::getline(stream, line))    // skip the request line
        return ("");

    while (std::getline(stream, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue ;

        if (toLowerCopy(line.substr(0, colon)) == name)
        {
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            return (value);
        }
    }
    return ("");
}

/**
 * "1234" -> 1234. Only digits are accepted: "-1", "abc", "12x" or an empty
 * value are invalid (answered with 400), and so is a number that does not fit.
 */
bool HttpRequest::parseContentLength(const std::string& value, size_t& length)
{
    length = 0;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        return (false);
    for (size_t i = 0; i < value.size(); ++i)
    {
        size_t digit = static_cast<size_t>(value[i] - '0');
        if (length > (static_cast<size_t>(-1) - digit) / 10)
            return (false);
        length = length * 10 + digit;
    }
    return (true);
}

// Content-Length of the request (0 if there is none). false if it is invalid.
static bool extractContentLength(const std::string& header_block, size_t& content_length)
{
    std::string value = extractHeader(header_block, "content-length");
    content_length = 0;
    if (value.empty())
        return (true);
    return (HttpRequest::parseContentLength(value, content_length));
}

// True if the request uses "Transfer-Encoding: chunked"
static bool isChunked(const std::string& header_block)
{
    return toLowerCopy(extractHeader(header_block, "transfer-encoding")).find("chunked")
           != std::string::npos;
}

/**
 * Decodes a chunked body ("Transfer-Encoding: chunked"):
 *
 *     5\r\nhello\r\n   6\r\n world\r\n   0\r\n\r\n
 *     │       │         │                │
 *     size    data      next chunk       last chunk (size 0) + empty line
 *     (hex)
 *
 * @param data     Raw bytes (the whole request so far).
 * @param start    Where the body begins (after the headers' \r\n\r\n).
 * @param out      Decoded body. NULL = only check / count, without copying
 *                 (cheap enough to call after every recv()).
 * @param consumed Raw bytes used by the chunked body (when DONE).
 * @param decoded  Decoded bytes so far (also while NEED_MORE), to apply
 *                 client_max_body_size before everything has arrived.
 * @return CHUNKED_DONE, CHUNKED_NEED_MORE or CHUNKED_ERROR (malformed).
 */
int HttpRequest::decodeChunked(const std::string& data, size_t start, std::string* out,
                               size_t& consumed, size_t& decoded)
{
    size_t pos = start;
    decoded = 0;
    if (out)
        out->clear();

    while (true)
    {
        // Chunk size line: hex digits, optionally followed by ";extensions"
        size_t eol = data.find("\r\n", pos);
        if (eol == std::string::npos)
            return (data.size() - pos > 64 ? CHUNKED_ERROR : CHUNKED_NEED_MORE);
        std::string line = data.substr(pos, eol - pos);
        line = line.substr(0, line.find(';'));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty() || line.size() > 8
            || line.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return (CHUNKED_ERROR);
        size_t size = static_cast<size_t>(std::strtoul(line.c_str(), NULL, 16));
        pos = eol + 2;

        // Last chunk: skip optional trailer headers until the empty line
        if (size == 0)
        {
            while (true)
            {
                eol = data.find("\r\n", pos);
                if (eol == std::string::npos)
                    return (data.size() - pos > MAX_HEADER_SIZE ? CHUNKED_ERROR : CHUNKED_NEED_MORE);
                if (eol == pos)
                {
                    consumed = pos + 2 - start;
                    return (CHUNKED_DONE);
                }
                pos = eol + 2;
            }
        }

        // Chunk data followed by \r\n
        if (data.size() < pos + size + 2)
        {
            decoded += data.size() - pos;   // partial chunk: count what we have
            return (CHUNKED_NEED_MORE);
        }
        if (data.compare(pos + size, 2, "\r\n") != 0)
            return (CHUNKED_ERROR);
        if (out)
            out->append(data, pos, size);
        decoded += size;
        pos += size + 2;
    }
}

/**
 * Same format as decodeChunked(), but incremental and without copying: it
 * continues from chunk.pos (where it stopped after the previous recv()) and
 * only advances over chunks that have fully arrived. This way a 100 MB body
 * is not re-scanned from the beginning on every recv().
 * @return CHUNKED_DONE (chunk.pos = end of the body), CHUNKED_NEED_MORE or
 *         CHUNKED_ERROR.
 */
int HttpRequest::scanChunked(const std::string& data, ChunkProgress& chunk)
{
    chunk.partial = 0;
    while (true)
    {
        size_t eol = data.find("\r\n", chunk.pos);
        if (eol == std::string::npos)
            return (data.size() - chunk.pos > 64 ? CHUNKED_ERROR : CHUNKED_NEED_MORE);
        std::string line = data.substr(chunk.pos, eol - chunk.pos);
        line = line.substr(0, line.find(';'));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty() || line.size() > 8
            || line.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return (CHUNKED_ERROR);
        size_t size = static_cast<size_t>(std::strtoul(line.c_str(), NULL, 16));
        size_t data_start = eol + 2;

        if (size == 0)
        {
            // Last chunk: optional trailers, then an empty line
            size_t pos = data_start;
            while (true)
            {
                eol = data.find("\r\n", pos);
                if (eol == std::string::npos)
                    return (data.size() - pos > MAX_HEADER_SIZE ? CHUNKED_ERROR : CHUNKED_NEED_MORE);
                if (eol == pos)
                {
                    chunk.pos = pos + 2;
                    return (CHUNKED_DONE);
                }
                pos = eol + 2;
            }
        }

        if (data.size() < data_start + size + 2)
        {
            // This chunk is still arriving. Count only its data bytes: the
            // "\r\n" that closes it may have arrived partly (e.g. 100 bytes +
            // "\r"), and counting it would give a false 413 at the exact limit.
            size_t available = data.size() - data_start;
            chunk.partial = available < size ? available : size;
            return (CHUNKED_NEED_MORE);
        }
        if (data.compare(data_start + size, 2, "\r\n") != 0)
            return (CHUNKED_ERROR);
        chunk.decoded += size;
        chunk.pos = data_start + size + 2;
    }
}

/**
 * Determines if the raw HTTP request is complete, including headers and body.
 * The body length comes from Content-Length, or from the chunked encoding
 * (complete when the last "0" chunk has arrived).
 * @param raw_request The raw HTTP request string.
 * @param request_size Output parameter to hold the total size of the complete request.
 * @param chunk Progress of a chunked body, kept between calls (see scanChunked).
 * @return True if the request is complete (or the chunked body is malformed,
 *         so that parse() answers 400), false otherwise.
 */
bool HttpRequest::isCompleteRequest(const std::string& raw_request, size_t& request_size,
                                    ChunkProgress& chunk)
{
    size_t header_end;
    if (!findHeaderEnd(raw_request, header_end))
        return (false);

    std::string header_block = raw_request.substr(0, header_end);

    if (isChunked(header_block))
    {
        if (chunk.pos == 0)
            chunk.pos = header_end + 4;
        int status = scanChunked(raw_request, chunk);
        if (status == CHUNKED_NEED_MORE)
            return (false);
        // DONE: the request ends after the last chunk. ERROR: take everything,
        // parse() will find the malformed body and answer 400.
        request_size = (status == CHUNKED_DONE) ? chunk.pos : raw_request.size();
        return (true);
    }

    size_t content_length;
    if (!extractContentLength(header_block, content_length))
    {
        // Invalid Content-Length: the request ends with the headers and
        // parse() answers 400 (the connection is closed afterwards).
        request_size = header_end + 4;
        return (true);
    }

    if (content_length > std::string::npos - header_end - 4)
        return (false);

    request_size = header_end + 4 + content_length;

    if (raw_request.size() >= request_size)
        return (true);

    return (false);
}

/**
 * Detects, before the whole request has arrived, errors that make it pointless
 * to keep reading: headers that never end, or a body bigger than the server
 * allows (declared in Content-Length, or already received in chunks).
 * This way a client cannot make us buffer gigabytes in memory.
 * Must be called after isCompleteRequest() (which updates `chunk`).
 * @param raw_request What we have received so far.
 * @param max_body_size client_max_body_size that applies to this request.
 * @param chunk Progress of a chunked body (see scanChunked).
 * @return 0 if we should keep reading, otherwise the HTTP error code.
 */
int HttpRequest::earlyErrorCode(const std::string& raw_request, size_t max_body_size,
                                ChunkProgress& chunk)
{
    size_t header_end;
    if (!findHeaderEnd(raw_request, header_end))
        return (raw_request.size() > MAX_HEADER_SIZE ? 431 : 0);
    if (header_end > MAX_HEADER_SIZE)
        return (431);

    std::string header_block = raw_request.substr(0, header_end);
    if (isChunked(header_block))
        return (chunk.decoded + chunk.partial > max_body_size ? 413 : 0);

    size_t content_length;
    if (!extractContentLength(header_block, content_length))
        return (400);
    if (content_length > max_body_size)
        return (413);
    return (0);
}
