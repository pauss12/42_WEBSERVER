#ifndef CGIHANDLER_HPP
#define CGIHANDLER_HPP

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "Configuration.hpp"

#include <string>
#include <map>
#include <ctime>
#include <sys/types.h>

// Seconds a CGI script may stay silent (no reads/writes on its pipes) before
// we kill it and answer 504. Measured from the last I/O, not from the start,
// so a script that is busy processing a 100 MB body is not killed.
#ifndef CGI_TIMEOUT
#define CGI_TIMEOUT 10
#endif

// State of a running CGI script, stored in the ClientSession of the client
// that requested it. The Server registers both pipe ends in its single
// kqueue/epoll, so the script runs while the server keeps serving others.
struct CgiProcess {
  pid_t       pid;        // Child process, -1 when no CGI is running
  int         stdin_fd;   // We write the request body here (-1 once closed)
  int         stdout_fd;  // We read the script output here (-1 once closed)
  std::string body;       // Request body still to be sent to the script
  size_t      body_sent;  // Bytes of body already written
  std::string output;     // Everything the script has printed so far
  time_t      last_io;    // Last read/write on its pipes: a script silent for
                          // more than CGI_TIMEOUT seconds is killed (504)

  CgiProcess()
    : pid(-1), stdin_fd(-1), stdout_fd(-1), body_sent(0), last_io(0) {}

  bool active() const { return pid > 0; }
};

// Static utility class responsible for:
// 1. Setting up the RFC 3875 environment variables.
// 2. Forking and setting up non-blocking stdin/stdout pipes.
// 3. Executing the script via execve.
// 4. Parsing the CGI output into an HttpResponse.
// It never reads from or writes to the pipes: that is the Server's job,
// always after kqueue/epoll says the pipe is ready.
class CgiHandler {
private:
  CgiHandler();
  ~CgiHandler();
  CgiHandler(CgiHandler const& rhs);
  CgiHandler& operator=(CgiHandler const& rhs);

  static std::map<std::string, std::string> setupEnvironment(
    HttpRequest const&                        req,
    std::string const&                        script_path,
    ServerConfig const*                       server
  );

  static char** createEnvArray(
    std::map<std::string, std::string> const& env
  );

  static void freeEnvArray(
    char**                                    envp
  );

public:
  // Forks the script and fills `cgi` with its pid and pipe fds.
  // `interpreter` comes from the config ("cgi .py /usr/bin/python3;"); if it
  // is empty the script is executed directly (it needs execute permission).
  // Returns 0 on success, or the HTTP error code to answer with
  // (404 script not found, 403 not readable/executable, 500 pipe/fork failed
  // or interpreter missing).
  static int start(
    HttpRequest const&        req,
    std::string const&        script_path,
    std::string const&        interpreter,
    ServerConfig const*       server,
    CgiProcess&               cgi
  );

  // Turns the raw script output (CGI headers + body) into an HttpResponse.
  // `output` is consumed: its body is moved into the response, not copied
  // (it can be 100 MB).
  static HttpResponse parseOutput(
    std::string&              output
  );
};

#endif
