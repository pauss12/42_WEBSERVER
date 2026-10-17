#ifndef LOG_HPP
# define LOG_HPP

#include <string>
#include <cstddef>
#include <netinet/in.h>

// Colores ANSI para la salida por terminal
# define GREEN "\033[0;32m"
# define WHITE "\033[37m"
# define RED "\033[0;31m\033[1m"
# define ORANGE "\001\033[38;5;208m\002"
# define BLUE "\033[0;34m"
# define PURPLE "\033[0;35m"
# define CYAN "\033[0;36m"
# define YELLOW  "\x1b[33m"
# define ROSE    "\x1B[38;2;255;151;203m"
# define LIGHT_BLUE   "\x1B[38;2;53;149;240m"
# define LIGHT_GREEN  "\x1B[38;2;17;245;120m"
# define GRAY    "\x1B[38;2;176;174;174m"
# define NC "\033[0m"
# define BOLD "\033[1m"
# define DIM "\033[2m"

// Small helpers to make the server output readable.
namespace logfmt {
  // 512 -> "512 B", 15880 -> "15.5 KB", 5000000 -> "4.8 MB"
  std::string humanSize(size_t bytes);
  // Current local time as "HH:MM:SS"
  std::string clock();
  // in_addr_t in network byte order -> "127.0.0.1"
  std::string ipToString(in_addr_t ip);
  // Pads `text` with spaces on the right up to `width` characters
  std::string pad(std::string const& text, size_t width);
}

#endif
