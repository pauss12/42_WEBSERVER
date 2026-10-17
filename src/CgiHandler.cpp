#include "../inc/CgiHandler.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sstream>
#include <iostream>

namespace {
std::string headerToEnvName(std::string const& header) {
  std::string result = "HTTP_";
  for (size_t i = 0; i < header.length(); ++i) {
    char c = header[i];
    if (c == '-') {
      result += '_';
    } else {
      result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  return result;
}

std::string intToString(int val) {
  std::ostringstream ss;
  ss << val;
  return ss.str();
}

std::string sizeToString(size_t val) {
  std::ostringstream ss;
  ss << val;
  return ss.str();
}
} // namespace

CgiHandler::CgiHandler() {}
CgiHandler::~CgiHandler() {}

std::map<std::string, std::string> CgiHandler::setupEnvironment(
    HttpRequest const& req, std::string const& script_path,
    ServerConfig const* server) {
  std::map<std::string, std::string> env;

  // 1. Core CGI / RFC 3875 variables
  env["GATEWAY_INTERFACE"] = "CGI/1.1";
  env["SERVER_PROTOCOL"] = "HTTP/1.1";
  env["SERVER_SOFTWARE"] = "webserv/1.0";

  // 2. Request method
  std::string method = req.getMethod();
  env["REQUEST_METHOD"] = method.empty() ? "GET" : method;

  // 3. Query string and script naming
  std::string path = req.getPath();
  std::string query_str = req.getQueryString();
  std::string uri = query_str.empty() ? path : (path + "?" + query_str);

  env["REQUEST_URI"] = uri;
  env["QUERY_STRING"] = query_str;
  env["SCRIPT_NAME"] = path;
  env["PATH_INFO"] = path;
  // The child runs inside the script's directory (see start()), so the
  // script is referenced by its file name only.
  std::string script_file = script_path.substr(script_path.find_last_of('/') + 1);
  env["PATH_TRANSLATED"] = script_file;
  env["SCRIPT_FILENAME"] = script_file;

  // 4. Content headers
  if (method == "POST" || !req.getBody().empty()) {
    env["CONTENT_LENGTH"] = sizeToString(req.getBody().length());
  }
  std::string content_type = req.getHeader("content-type");
  if (!content_type.empty()) {
    env["CONTENT_TYPE"] = content_type;
  }

  // 5. Server details
  if (server) {
    int port = server->getListenAddress().second;
    env["SERVER_PORT"] = intToString(port);
  } else {
    env["SERVER_PORT"] = "8080";
  }

  std::string host = req.getHeader("host");
  if (!host.empty()) {
    size_t colon = host.find(':');
    env["SERVER_NAME"] = (colon != std::string::npos) ? host.substr(0, colon) : host;
  } else {
    env["SERVER_NAME"] = "localhost";
  }

  env["REMOTE_ADDR"] = "127.0.0.1";
  env["REDIRECT_STATUS"] = "200"; // Required by PHP-CGI

  char* system_path = std::getenv("PATH");
  if (system_path) {
    env["PATH"] = system_path;
  } else {
    env["PATH"] = "/usr/local/bin:/usr/bin:/bin";
  }

  // 6. Forward HTTP request headers
  std::map<std::string, std::string> const& headers = req.getHeaders();
  for (std::map<std::string, std::string>::const_iterator it = headers.begin();
       it != headers.end(); ++it) {
    if (it->first == "content-type" || it->first == "content-length") {
      continue;
    }
    env[headerToEnvName(it->first)] = it->second;
  }

  return env;
}

char** CgiHandler::createEnvArray(std::map<std::string, std::string> const& env) {
  char** envp = new char*[env.size() + 1];
  size_t i = 0;
  for (std::map<std::string, std::string>::const_iterator it = env.begin();
       it != env.end(); ++it, ++i) {
    std::string env_entry = it->first + "=" + it->second;
    envp[i] = new char[env_entry.length() + 1];
    std::strcpy(envp[i], env_entry.c_str());
  }
  envp[i] = NULL;
  return envp;
}

void CgiHandler::freeEnvArray(char** envp) {
  if (!envp) return;
  for (size_t i = 0; envp[i] != NULL; ++i) {
    delete[] envp[i];
  }
  delete[] envp;
}

HttpResponse CgiHandler::parseOutput(std::string& output) {
  HttpResponse res;

  // Search for header separator: \r\n\r\n or \n\n
  size_t header_end = output.find("\r\n\r\n");
  size_t delim_len = 4;
  if (header_end == std::string::npos) {
    header_end = output.find("\n\n");
    delim_len = 2;
  }

  if (header_end == std::string::npos) {
    // No headers found: default to 200 OK with text/html
    res.setStatusCode(200);
    res.setStatusMessage("OK");
    res.setHeader("Content-Type", "text/html");
    res.setHeader("Content-Length", sizeToString(output.length()));
    res.swapBody(output);
    return res;
  }

  std::string headers_str = output.substr(0, header_end);
  // The rest of the output is the body: remove the headers in place and move
  // it into the response, without making another copy.
  output.erase(0, header_end + delim_len);

  std::istringstream stream(headers_str);
  std::string line;
  int status_code = 200;
  std::string status_msg = "OK";

  while (std::getline(stream, line)) {
    if (!line.empty() && line[line.length() - 1] == '\r') {
      line.erase(line.length() - 1);
    }
    if (line.empty()) continue;

    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      std::string val = line.substr(colon + 1);

      // Trim leading spaces from header value
      size_t first = val.find_first_not_of(" \t");
      if (first != std::string::npos) {
        val = val.substr(first);
      }

      if (key == "Status" || key == "status") {
        std::istringstream s_stream(val);
        s_stream >> status_code;
        std::getline(s_stream, status_msg);
        size_t m_first = status_msg.find_first_not_of(" \t");
        if (m_first != std::string::npos) {
          status_msg = status_msg.substr(m_first);
        } else {
          status_msg = "OK";
        }
      } else {
        res.setHeader(key, val);
      }
    }
  }

  res.setStatusCode(status_code);
  res.setStatusMessage(status_msg);
  res.setHeader("Content-Length", sizeToString(output.length()));
  res.swapBody(output);

  return res;
}

namespace {
void closePipe(int fds[2]) {
  close(fds[0]);
  close(fds[1]);
}
} // namespace

int CgiHandler::start(HttpRequest const& req, std::string const& script_path,
                      std::string const& interpreter, ServerConfig const* server,
                      CgiProcess& cgi) {
  if (access(script_path.c_str(), F_OK) != 0) {
    // std::cerr << "CGI script not found: " << script_path << std::endl;
    return 404;
  }
  if (access(script_path.c_str(), R_OK) != 0) {
    // std::cerr << "CGI script not readable: " << script_path << std::endl;
    return 403;
  }
  // Without interpreter the script is executed directly: it must be executable
  if (interpreter.empty() && access(script_path.c_str(), X_OK) != 0)
    return 403;
  // The interpreter from the config ("cgi .py /usr/bin/python3;") must exist
  if (!interpreter.empty() && access(interpreter.c_str(), X_OK) != 0)
    return 500;

  int stdin_pipe[2];
  int stdout_pipe[2];
  if (pipe(stdin_pipe) < 0)
    return 500;
  if (pipe(stdout_pipe) < 0) {
    closePipe(stdin_pipe);
    return 500;
  }

  // Our ends of the pipes must be non-blocking: we only touch them when
  // kqueue/epoll says they are ready, exactly like the client sockets.
  // The child's ends stay blocking, which is what a normal script expects.
  if (fcntl(stdin_pipe[1], F_SETFL, O_NONBLOCK) == -1 ||
      fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
    closePipe(stdin_pipe);
    closePipe(stdout_pipe);
    return 500;
  }

  std::map<std::string, std::string> env = setupEnvironment(req, script_path, server);
  char** envp = createEnvArray(env);

  // Anything still buffered in std::cout would be duplicated in the child.
  std::cout.flush();
  pid_t pid = fork();
  if (pid < 0) {
    closePipe(stdin_pipe);
    closePipe(stdout_pipe);
    freeEnvArray(envp);
    return 500;
  }

  if (pid == 0) {
    // Child process: redirect standard I/O to pipes
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);

    // Close every other inherited fd (both pipe ends, listening sockets,
    // other clients...). Otherwise the script would keep those connections
    // alive, and we would never see EOF on its stdout.
    for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd)
      close(fd);

    // Run the script inside its own directory, so relative paths work.
    std::string dir = ".";
    std::string file = script_path;
    size_t slash_pos = script_path.find_last_of('/');
    if (slash_pos != std::string::npos) {
      dir = script_path.substr(0, slash_pos);
      file = script_path.substr(slash_pos + 1);
    }
    if (dir.empty())
      dir = "/";
    if (chdir(dir.c_str()) != 0)
      std::exit(1);

    // The interpreter comes from the location's "cgi <ext> <interpreter>;"
    std::string exec_name = "./" + file;
    char* argv[3];
    if (!interpreter.empty()) {
      argv[0] = const_cast<char*>(interpreter.c_str());
      argv[1] = const_cast<char*>(file.c_str());
      argv[2] = NULL;
      execve(interpreter.c_str(), argv, envp);
    } else {
      argv[0] = const_cast<char*>(exec_name.c_str());
      argv[1] = NULL;
      execve(exec_name.c_str(), argv, envp);
    }
    // execve only returns on failure: no output -> the server answers 502
    std::exit(1);
  }

  // Parent process: keep only our ends of the pipes
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  freeEnvArray(envp);

  cgi.pid = pid;
  cgi.stdin_fd = stdin_pipe[1];
  cgi.stdout_fd = stdout_pipe[0];
  cgi.body.clear();   // the Server moves the request body here (no copy)
  cgi.body_sent = 0;
  cgi.output.clear();
  cgi.last_io = time(NULL);
  return 0;
}
