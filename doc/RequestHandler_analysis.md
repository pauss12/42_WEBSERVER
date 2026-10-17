# Analysis Report: `src/RequestHandler.cpp`

## 1. Executive Summary

The file [`src/RequestHandler.cpp`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp) is responsible for routing HTTP requests and generating responses:
- Request method routing: `RequestHandler::handle()`
- Static file serving: `RequestHandler::handleStaticFile()`
- File deletion: `RequestHandler::handleDeleteRequest()`
- Upload directory listing: `RequestHandler::handleUploadsListingRequest()`
- Error response generation: `RequestHandler::handleError()`

A code audit of `RequestHandler.cpp` reveals why requests to the root path (`GET /`) return **404 Not Found**, as well as several architectural defects regarding configuration integration, autoindex handling, and file validation.

---

## 2. Root Cause Analysis: 404 Not Found on `GET /`

### 2.1 Hardcoded Web Root Prefix
- **Location**: [`src/RequestHandler.cpp:L166-L175`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L166-L175)
- **Problem**:
  ```cpp
  HttpResponse RequestHandler::handleStaticFile(const HttpRequest& req) 
  {
      std::string path = req.getPath();
      if (path == "/")
          path = "/index.html";
      
      std::string full_path = "www" + path;
      std::string content = readFile(full_path);

      if (content.empty()) {
          return handleError(404, "Not Found", req.getRequestId());
      }
      ...
  ```
- **Execution Trace for `GET /`**:
  1. The client sends `GET / HTTP/1.1`.
  2. `path == "/"` is rewritten to `"/index.html"`.
  3. `full_path` is constructed as `"www" + "/index.html"` $\rightarrow$ `"www/index.html"`.
  4. The server attempts to read `./www/index.html` from the workspace filesystem.
  5. In the project structure:
     ```
     www/
     ├── cgi/
     ├── cgi-bin/
     ├── err/
     ├── img/
     └── my_website/
         ├── index.html       <-- Actual index file is located here
         ├── link.html
         └── new-page.html
     ```
     There is no `index.html` directly inside `www/`. It is located inside `www/my_website/`.
  6. `readFile("www/index.html")` fails and returns an empty string.
  7. `handleStaticFile()` interprets the empty string as file not found and returns `404 Not Found`.

### 2.2 Complete Disconnect from Server and Location Configurations
- In [`etc/default.conf`](file:///Users/p/Desktop/webserv/etc/default.conf), the administrator configures the document root for the server and location blocks:
  ```nginx
  server {
      listen 0.0.0.0:8080;
      root www/my_website/;

      location / {
          root www/my_website/;
          autoindex on;
          allowed_methods GET;
      }
  }
  ```
- **The Defect**: `handleStaticFile()` never reads `ServerConfig::getRoot()` nor `LocationConfig::getRoot()`. It even ignores `req.getWwwRoot()`. Because `"www"` is hardcoded directly in the source code, any custom `root` directive configured in the `.conf` file is completely ignored by the static file handler.

---

## 3. Missing `autoindex` Implementation

- In [`etc/default.conf`](file:///Users/p/Desktop/webserv/etc/default.conf#L10), `autoindex on;` is explicitly enabled for `location /`.
- **The Defect**:
  1. `handleStaticFile()` unconditionally rewrites `path = "/index.html"`.
  2. If `index.html` does not exist in a directory, an NGINX-compliant web server with `autoindex on` must generate an HTML directory listing (`listing.html` or dynamic `<ul><li>...</li></ul>`).
  3. `RequestHandler` has no directory listing logic for general static paths; it immediately drops to `404 Not Found` when `index.html` is absent.

---

## 4. Hardcoded Method Routing & Ignored `allowed_methods` (Cause of 405 on `POST /`)

### 4.1 Root Cause for `POST /` Failure
- **Location**: [`src/RequestHandler.cpp:L29-L70`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L29-L70)
- **Problem**:
  Even when `allowed_methods GET POST;` is added to `location /` in [`etc/default.conf`](file:///Users/p/Desktop/webserv/etc/default.conf#L11), a `POST /` request returns **405 Method Not Allowed**.
- **Root Cause**:
  1. **`LocationConfig::getAllowedMethods()` is never queried anywhere in the request pipeline.** While `Configuration.cpp` parses the directive into `LocationConfig::_allowed_methods`, neither `Server`, `HttpRequest`, nor `RequestHandler` ever accesses this getter.
  2. **Methods are hardcoded by URL strings in `RequestHandler::handle()`**:
     ```cpp
     // Only POST is allowed for /upload
     if (path == "/upload" || path == "/upload/") {
         if (method == "POST") return handleUploadRequest(req);
         return handleError(405, ERROR_405, req.getRequestId());
     }
     ...
     // Only GET is allowed for all other routes (including root /)
     if (method == "GET") {
         return handleStaticFile(req);
     }
     return handleError(405, ERROR_405, req.getRequestId());
     ```
  3. When `POST /` arrives:
     - It does not match `path == "/upload"`.
     - It falls through to `if (method == "GET")`, which evaluates to `false`.
     - It unconditionally returns `handleError(405, ERROR_405, req.getRequestId())`.

### 4.2 Where Does `POST` Actually Work in the Codebase?

Under the current implementation, **`POST` is only accepted if the path is explicitly `/upload` or `/upload/`**.

Here is the exact routing behavior for `POST` across all endpoints:

| Request Path | HTTP Method | Behavior & Handler | Response |
| :--- | :--- | :--- | :--- |
| **`/upload` or `/upload/`** | `POST` | Dispatches to [`RequestHandler::handleUploadRequest()`](file:///Users/p/Desktop/webserv/src/handleUpload.cpp#L168). | **`200 OK`** (if valid body) |
| **`/upload` or `/upload/`** | `GET`, `DELETE`, etc. | Hardcoded check fails `if (method == "POST")`. | **`405 Method Not Allowed`** |
| **`/uploads` or `/uploads/`** | `POST` | Only accepts `GET` (`handleUploadsListingRequest`). | **`405 Method Not Allowed`** |
| **`/uploads/<file>`** | `POST` | Only accepts `GET` or `DELETE`. | **`405 Method Not Allowed`** |
| **`/cgi/*`** | `POST` (or any) | Dispatches to [`handleCgiRequest()`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L151) (stub). | **`501 Not Implemented`** |
| **`/` or any static file** | `POST` | Fallback only allows `GET` (`handleStaticFile`). | **`405 Method Not Allowed`** |

#### Internal Execution Flow for Successful `POST /upload`:
Defined in [`src/handleUpload.cpp`](file:///Users/p/Desktop/webserv/src/handleUpload.cpp):
1. **Body Check** (`L138-L142`): Validates `!req.getBody().empty()`. Returns `400` if empty.
2. **Directory Resolution** (`L144-L149`): Validates `req.getUploadsDirectory()`. Returns `500` if missing.
3. **Payload Extraction** (`L151-L157`):
   - Tries `parseMultipartFormData()` using the `boundary` in `Content-Type`.
   - If not multipart, falls back to raw body data (`req.getDirectFilename()`).
4. **Collision Check** (`L98-L103`): Checks `fileExists(full_path)`. If the target file already exists, it returns **`409 Conflict`** to prevent overwriting.
5. **Disk Write** (`L105-L125`): Writes bytes via `std::ofstream`. Returns **`200 OK`** on success.

#### Testing a Working `POST`:
```bash
# Upload a raw binary or text file to /upload:
curl -X POST http://localhost:8080/upload --data-binary @small.txt

# Upload using multipart/form-data:
curl -X POST http://localhost:8080/upload -F "file=@small.txt"
```

### 4.3 Remediation
Pass the matched `LocationConfig` (or its list of allowed methods) to `RequestHandler`. Check whether `req.getMethod()` is present in `location->getAllowedMethods()` before dispatching, rather than hardcoding method checks by path prefix.

---

## 5. Other File-Serving Flaws

### 5.1 Empty Files (0 Bytes) Mistaken for 404
- **Problem**:
  ```cpp
  std::string content = readFile(full_path);
  if (content.empty()) {
      return handleError(404, "Not Found", req.getRequestId());
  }
  ```
- **Impact**:
  If a valid, existing file on disk has a file size of 0 bytes (e.g. `touch www/empty.txt`), `content.empty()` evaluates to `true`. The server returns `404 Not Found` instead of returning `200 OK` with `Content-Length: 0`.
- **Remediation**:
  Use `stat()` or `access()` to test for file existence and accessibility before reading the contents.

### 5.2 Missing URL Decoding & Path Sanitization
- While [`src/showUploadedFiles.cpp`](file:///Users/p/Desktop/webserv/src/showUploadedFiles.cpp#L36-L44) implements `decodeSafePath()`, [`RequestHandler::handleStaticFile()`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L164) directly concatenates `req.getPath()`.
- Requests with URL-encoded characters (e.g. `/my%20file.html`) will fail to resolve on disk.

### 5.3 Hardcoded Error Template Path
- **Location**: [`src/RequestHandler.cpp:L201`](file:///Users/p/Desktop/webserv/src/RequestHandler.cpp#L201)
- `std::string tpl_path = "www/err/template.html";` is hardcoded relative to the process working directory. If custom error pages are configured in `default.conf` (`error_page 400 ...`), they are bypassed unless wired through the request session.

---

## 6. Recommended Architecture & Remediation

1. **Pass Effective Root to `HttpRequest` or `RequestHandler`**:
   In `switchMode.cpp`, assign `wwwRoot` from the matched virtual host or location:
   ```cpp
   std::string effective_root = (active_server && !active_server->getRoot().empty())
                                ? active_server->getRoot() : "www";
   request.setWwwRoot(effective_root);
   ```

2. **Dynamically Resolve Document Root in `handleStaticFile`**:
   ```cpp
   std::string root = req.getWwwRoot().empty() ? "www" : req.getWwwRoot();
   if (!root.empty() && root[root.size() - 1] == '/' && !path.empty() && path[0] == '/')
       root.erase(root.size() - 1);
   std::string full_path = root + path;
   ```

3. **Check Existence Before Read**:
   ```cpp
   struct stat st;
   if (stat(full_path.c_str(), &st) != 0) {
       return handleError(404, "Not Found", req.getRequestId());
   }
   if (S_ISDIR(st.st_mode)) {
       // Check for index.html or trigger autoindex listing
   }
   ```
