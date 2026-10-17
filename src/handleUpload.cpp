#include "RequestHandler.hpp"
#include <ctime>
#include <vector>
#include <algorithm>

// Un fichero dentro de un body multipart/form-data
struct UploadPart
{
    std::string filename;
    size_t      data_start;    // dónde empiezan sus bytes dentro del body
    size_t      data_length;
};

/**
 * Lee TODAS las partes de un body multipart/form-data (un formulario con
 * <input type="file" multiple> manda una parte por fichero):
 *
 *     --BOUNDARY\r\n
 *     Content-Disposition: form-data; name="files"; filename="a.txt"\r\n
 *     Content-Type: text/plain\r\n
 *     \r\n
 *     ...bytes de a.txt...\r\n
 *     --BOUNDARY\r\n
 *     ...cabeceras y bytes de b.png...\r\n
 *     --BOUNDARY--\r\n
 *
 * Las partes sin filename (campos normales del formulario) se ignoran.
 * @return false si el body no es multipart (o está mal formado); true si se
 *         ha podido leer, aunque no traiga ningún fichero.
 */
static bool parseMultipartFormData(const std::string& body_str, const std::string& content_type,
                                   std::vector<UploadPart>& parts)
{
    std::string boundary_key = "boundary=";
    size_t b_pos = content_type.find(boundary_key);
    if (b_pos == std::string::npos)
        return (false);

    // boundary=XYZ  o  boundary="XYZ"  (puede ir seguido de "; otro=parametro")
    std::string boundary = content_type.substr(b_pos + boundary_key.length());
    boundary = boundary.substr(0, boundary.find(';'));
    boundary.erase(boundary.find_last_not_of(" \t\r") + 1);
    if (boundary.size() >= 2 && boundary[0] == '"' && boundary[boundary.size() - 1] == '"')
        boundary = boundary.substr(1, boundary.size() - 2);
    if (boundary.empty())
        return (false);
    std::string delimiter = "--" + boundary;

    size_t pos = body_str.find(delimiter);
    if (pos == std::string::npos)
        return (false);

    while (true)
    {
        pos += delimiter.size();
        if (body_str.compare(pos, 2, "--") == 0)    // "--BOUNDARY--": fin del body
            return (true);
        if (body_str.compare(pos, 2, "\r\n") != 0)
            return (false);
        pos += 2;

        // Cabeceras de esta parte
        size_t header_end = body_str.find("\r\n\r\n", pos);
        if (header_end == std::string::npos)
            return (false);
        std::string headers = body_str.substr(pos, header_end - pos);

        // Los datos acaban justo antes de "\r\n--BOUNDARY"
        size_t data_start = header_end + 4;
        size_t next = body_str.find("\r\n" + delimiter, data_start);
        if (next == std::string::npos)
            return (false);

        // filename="..." solo dentro de las cabeceras de esta parte
        std::string filename_key = "filename=\"";
        size_t fn_start = headers.find(filename_key);
        if (fn_start != std::string::npos)
        {
            fn_start += filename_key.length();
            size_t fn_end = headers.find('"', fn_start);
            if (fn_end != std::string::npos && fn_end > fn_start)
            {
                UploadPart part;
                part.filename = headers.substr(fn_start, fn_end - fn_start);
                part.data_start = data_start;
                part.data_length = next - data_start;
                parts.push_back(part);
            }
        }
        pos = next + 2;    // apunta al siguiente "--BOUNDARY"
    }
}

static std::string sanitizeFilename(const std::string& raw_filename)
{
    std::string filename = raw_filename;

    size_t slash_pos = filename.find_last_of("/\\");
    if (slash_pos != std::string::npos)
        filename = filename.substr(slash_pos + 1);

    if (filename.empty() || filename == "." || filename == "..")
        filename = "uploaded_file.bin";

    return (filename);
}

static bool fileExists(const std::string& path)
{
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// Nombre libre para un body sin nombre de fichero: upload_<hora>_<n>.bin
static std::string uniqueUploadName(const std::string& uploads_dir)
{
    static unsigned long counter = 0;
    while (true)
    {
        std::ostringstream name;
        name << "upload_" << time(NULL) << "_" << counter++ << ".bin";
        if (!fileExists(uploads_dir + "/" + name.str()))
            return (name.str());
    }
}

static bool saveFileToDisk(const std::string& full_path, const std::string& body_str, size_t data_start, size_t data_length, int& out_error_code, bool allow_overwrite = false)
{
    // 1. Verificar si el archivo ya existe para evitar sobrescribirlo
    //    (salvo cuando se sube a una ruta concreta: POST /uploads/foto.png)
    if (!allow_overwrite && fileExists(full_path))
    {
        // std::cerr << RED << "[ERROR 409] El fichero ya existe en el servidor: " << full_path << NC << std::endl;
        out_error_code = 409;
        return (false);
    }

    std::ofstream file(full_path.c_str(), std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        // std::cerr << RED << "[ERROR 500] Error opening file for writing: " << full_path << " (" << strerror(errno) << ")" << NC << std::endl;
        out_error_code = 500;
        return (false);
    }

    file.write(body_str.data() + data_start, data_length);
    file.close();

    if (!file.good())
    {
        // std::cerr << RED << "[ERROR 500] Error escribiendo datos en el disco: " << full_path << NC << std::endl;
        out_error_code = 500;
        return (false);
    }

    // std::cout << GREEN << "File uploaded successfully: " << full_path << " (" << data_length << " bytes)" << NC << std::endl;
    out_error_code = 200;
    return (true);
}

// Guarda el/los fichero(s) del body. Devuelve el código HTTP (200 si todo
// fue bien) y en `saved` cuántos ficheros se han guardado.
int RequestHandler::handleFileUpload(const HttpRequest& req, size_t& saved)
{
    int error_code = 200;
    saved = 0;

    // Referencia, no copia: el body puede ser muy grande
    const std::string& body = req.getBody();

    std::string uploads_dir = req.getUploadsDirectory();
    if (uploads_dir.empty())
        return (500);

    std::vector<UploadPart> parts;
    if (parseMultipartFormData(body, req.getHeader("Content-Type"), parts))
    {
        // Formulario multipart: un fichero por parte
        if (parts.empty())
            return (400);    // multipart sin ningún fichero

        // Antes de escribir nada, comprobamos que ningún nombre choca (con un
        // fichero existente o con otro de la misma petición): así no queda
        // una subida a medias.
        std::vector<std::string> paths;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            std::string path = uploads_dir + "/" + sanitizeFilename(parts[i].filename);
            if (fileExists(path) || std::find(paths.begin(), paths.end(), path) != paths.end())
                return (409);
            paths.push_back(path);
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (!saveFileToDisk(paths[i], body, parts[i].data_start, parts[i].data_length, error_code))
                return (error_code);
            ++saved;
        }
        return (200);
    }

    // Body "en crudo" (no multipart), también vacío. El nombre sale de:
    //  1. la cabecera X-Filename (si ya existe -> 409);
    //  2. la URL: POST /uploads/foto.png -> foto.png. Como un PUT: si ya
    //     existe se sobrescribe;
    //  3. si no hay ninguno, un nombre único (upload_<hora>_<n>.bin).
    std::string filename = req.getHeader("X-Filename");
    bool overwrite = false;
    if (filename.empty())
    {
        const std::string& target = req.getResolvedPath();
        if (!target.empty() && target != "/" && target[target.size() - 1] != '/')
        {
            filename = target.substr(target.find_last_of('/') + 1);
            overwrite = true;
        }
    }
    if (filename.empty())
        filename = uniqueUploadName(uploads_dir);
    filename = sanitizeFilename(filename);

    if (!saveFileToDisk(uploads_dir + "/" + filename, body, 0, body.size(), error_code, overwrite))
        return (error_code);
    saved = 1;
    return (200);
}

HttpResponse RequestHandler::handleUploadRequest(const HttpRequest& req)
{
    size_t saved = 0;
    int out_code = handleFileUpload(req, saved);

    if (out_code == 200)
    {
        std::ostringstream note;
        note << saved << (saved == 1 ? " file" : " files") << " saved in " << req.getUploadsDirectory();
        HttpResponse res;
        res.setStatusCode(200);
        res.setStatusMessage("OK");
        res.setHeader("Content-Length", "0");
        res.setHeader("Connection", "close");
        res.setNote(note.str());
        return (res);
    }
    else if (out_code == 409)
        return withNote(handleError(409, ERROR_409, req.getRequestId()), "file already exists");
    else if (out_code == 400)
        return withNote(handleError(400, ERROR_400, req.getRequestId()), "multipart body without any file");
    else
        return withNote(handleError(500, ERROR_500, req.getRequestId()), "could not write in " + req.getUploadsDirectory());
}