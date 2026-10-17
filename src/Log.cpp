#include "Log.hpp"

#include <ctime>
#include <sstream>

namespace logfmt {

std::string humanSize(size_t bytes)
{
  std::ostringstream oss;
  if (bytes < 1024)
    oss << bytes << " B";
  else
  {
    double value = bytes / 1024.0;
    const char* unit = "KB";
    if (value >= 1024.0)
    {
      value /= 1024.0;
      unit = "MB";
    }
    if (value >= 1024.0)
    {
      value /= 1024.0;
      unit = "GB";
    }
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << value << " " << unit;
  }
  return oss.str();
}

std::string clock()
{
  char buffer[16];
  time_t now = time(NULL);
  if (strftime(buffer, sizeof(buffer), "%H:%M:%S", localtime(&now)) == 0)
    return "--:--:--";
  return buffer;
}

std::string ipToString(in_addr_t ip)
{
  // The address is stored in network byte order: the first byte in memory
  // is the first number of the dotted notation.
  unsigned char const* bytes = reinterpret_cast<unsigned char const*>(&ip);
  std::ostringstream oss;
  oss << static_cast<int>(bytes[0]) << "." << static_cast<int>(bytes[1]) << "."
      << static_cast<int>(bytes[2]) << "." << static_cast<int>(bytes[3]);
  return oss.str();
}

std::string pad(std::string const& text, size_t width)
{
  if (text.size() >= width)
    return text;
  return text + std::string(width - text.size(), ' ');
}

} // namespace logfmt
