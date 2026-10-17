#include "Configuration.hpp"
#include "Log.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <cstdlib>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>

namespace {
	int stringToInt(std::string const& str) {
		std::istringstream iss(str);
		int val;
		if (!(iss >> val) || !iss.eof()) {
			throw std::runtime_error("Invalid integer format: " + str);
		}
		return val;
	}

	// "127.0.0.1" -> address in network byte order. We parse it ourselves
	// because inet_addr() is not in the subject's list of allowed functions.
	in_addr_t parseIPv4(std::string const& str) {
		std::istringstream iss(str);
		unsigned long parts[4];
		char dot;
		for (int i = 0; i < 4; ++i) {
			if (iss.peek() < '0' || iss.peek() > '9' || !(iss >> parts[i]) || parts[i] > 255)
				throw std::runtime_error("Invalid IP address: " + str);
			if (i < 3 && (!(iss >> dot) || dot != '.'))
				throw std::runtime_error("Invalid IP address: " + str);
		}
		if (iss.peek() != EOF)
			throw std::runtime_error("Invalid IP address: " + str);
		return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
	}

	int parsePort(std::string const& str) {
		int port = stringToInt(str);
		if (port < 1 || port > 65535)
			throw std::runtime_error("Invalid port (must be 1-65535): " + str);
		return port;
	}

	// "8080" or "127.0.0.1:8080"
	std::pair<in_addr_t, int> parseEndpoint(std::string const& token) {
		size_t colon = token.find(':');
		if (colon == std::string::npos)
			return std::make_pair(static_cast<in_addr_t>(INADDR_ANY), parsePort(token));
		return std::make_pair(parseIPv4(token.substr(0, colon)), parsePort(token.substr(colon + 1)));
	}

	// "1000", "10K", "10M", "1G" -> bytes. Negative or malformed values are
	// rejected instead of silently becoming "no limit".
	size_t parseSize(std::string const& str) {
		size_t digits = 0;
		while (digits < str.size() && str[digits] >= '0' && str[digits] <= '9')
			++digits;
		if (digits == 0 || digits + 1 < str.size())
			throw std::runtime_error("Invalid size: " + str);

		size_t multiplier = 1;
		if (digits < str.size()) {
			char unit = static_cast<char>(std::toupper(static_cast<unsigned char>(str[digits])));
			if (unit == 'K') multiplier = 1024;
			else if (unit == 'M') multiplier = 1024 * 1024;
			else if (unit == 'G') multiplier = 1024 * 1024 * 1024;
			else throw std::runtime_error("Invalid size unit: " + str);
		}

		size_t value = 0;
		for (size_t i = 0; i < digits; ++i) {
			size_t digit = static_cast<size_t>(str[i] - '0');
			if (value > (static_cast<size_t>(-1) - digit) / 10)
				throw std::runtime_error("Size too large: " + str);
			value = value * 10 + digit;
		}
		if (value > static_cast<size_t>(-1) / multiplier)
			throw std::runtime_error("Size too large: " + str);
		return value * multiplier;
	}

	// "on" -> true, "off" -> false, anything else is an error
	bool parseOnOff(std::string const& directive, std::string const& value) {
		if (value == "on")
			return true;
		if (value == "off")
			return false;
		throw std::runtime_error(directive + " must be 'on' or 'off', got '" + value + "'");
	}
}

// --- LocationConfig Implementation ---

LocationConfig::LocationConfig(std::string const & root, std::string const & location_match) :
	_location_match(location_match),
	_type(STATIC_FILE),
	_root(root),
	_index("index.html"), // Like NGINX: default file for a directory
	_autoindex(false), // Like NGINX: directory listing only when asked for
	_upload_enabled(false),
	_redirect(0, ""),
	_client_max_body_size(0),
	_has_client_max_body_size(false)
{
	_allowed_methods.push_back("GET");
}

LocationConfig::~LocationConfig() {}

std::string const & LocationConfig::getLocationMatch() const { return _location_match; }
LocationConfig::RouteType LocationConfig::getType() const { return _type; }
std::string const & LocationConfig::getRoot() const { return _root; }
std::string const & LocationConfig::getAlias() const { return _alias; }
std::string const & LocationConfig::getIndex() const { return _index; }
bool LocationConfig::getAutoindex() const { return _autoindex; }
std::string const & LocationConfig::getUploadPath() const { return _upload_path; }
bool LocationConfig::getUploadEnabled() const { return _upload_enabled; }
std::pair<int, std::string> const & LocationConfig::getRedirect() const { return _redirect; }
std::vector<std::string> const & LocationConfig::getAllowedMethods() const { return _allowed_methods; }
std::map<std::string, std::string> const & LocationConfig::getCgi() const { return _cgi; }
bool LocationConfig::hasClientMaxBodySize() const { return _has_client_max_body_size; }
size_t LocationConfig::getClientMaxBodySize() const { return _client_max_body_size; }

// --- ServerConfig Implementation ---

ServerConfig::ServerConfig() :
	_listen_address(INADDR_ANY, 8080),
	_client_max_body_size(50 * 1024 * 1024), // 50 MB
	_root("www/")
{
	LocationConfig default_loc(_root, "/");
	_locations.push_back(default_loc);
}

ServerConfig::~ServerConfig() {}

std::pair<in_addr_t, int> const & ServerConfig::getListenAddress() const { return _listen_address; }
std::map<int, std::string> const & ServerConfig::getErrorPages() const { return _error_pages; }
size_t ServerConfig::getClientMaxBodySize() const { return _client_max_body_size; }
std::string const & ServerConfig::getRoot() const { return _root; }
std::vector<LocationConfig> const & ServerConfig::getLocations() const { return _locations; }

// --- Configuration Implementation ---

Configuration::Configuration() {}
Configuration::~Configuration() {}

std::vector<ServerConfig> const & Configuration::getServers() const { return _servers; }

std::vector<std::string> Configuration::tokenize(std::string const& configFilePath) {
	std::vector<std::string> tokens;
	std::ifstream file(configFilePath.c_str());
	if (!file.is_open()) {
		throw std::runtime_error("Failed to open configuration file: " + configFilePath);
	}

	std::string line;
	while (std::getline(file, line)) {
		size_t hash_pos = line.find('#');
		if (hash_pos != std::string::npos) {
			line = line.substr(0, hash_pos);
		}
		
		std::istringstream iss(line);
		std::string word;
		while (iss >> word) {
			std::string current_token = "";
			for (size_t i = 0; i < word.length(); ++i) {
				char c = word[i];
				if (c == '{' || c == '}' || c == ';') {
					if (!current_token.empty()) {
						tokens.push_back(current_token);
						current_token = "";
					}
					tokens.push_back(std::string(1, c));
				} else {
					current_token += c;
				}
			}
			if (!current_token.empty()) {
				tokens.push_back(current_token);
			}
		}
	}
	return tokens;
}

void Configuration::expectToken(std::vector<std::string> const& tokens, size_t& pos, std::string const& expected) {
	if (pos >= tokens.size()) {
		throw std::runtime_error("Unexpected end of file, expected '" + expected + "'");
	}
	if (tokens[pos] != expected) {
		throw std::runtime_error("Expected '" + expected + "' but found '" + tokens[pos] + "'");
	}
	pos++;
}

std::string const& Configuration::nextValue(std::vector<std::string> const& tokens, size_t& pos) {
	if (pos >= tokens.size())
		throw std::runtime_error("Unexpected end of file, expected a value");
	std::string const& token = tokens[pos];
	if (token == "{" || token == "}" || token == ";")
		throw std::runtime_error("Expected a value but found '" + token + "'");
	++pos;
	return token;
}

LocationConfig Configuration::parseLocationBlock(std::vector<std::string> const& tokens, size_t& pos, std::string const& root) {
	std::string match = nextValue(tokens, pos);
	if (match[0] != '/')
		throw std::runtime_error("Location path must start with '/': " + match);
	
	LocationConfig loc(root, match);
	expectToken(tokens, pos, "{");
	
	while (pos < tokens.size() && tokens[pos] != "}") {
		std::string directive = tokens[pos++];
		
		if (directive == "root") {
			loc._root = nextValue(tokens, pos);
			expectToken(tokens, pos, ";");
		} else if (directive == "alias") {
			loc._alias = nextValue(tokens, pos);
			expectToken(tokens, pos, ";");
		} else if (directive == "index") {
			loc._index = nextValue(tokens, pos);
			expectToken(tokens, pos, ";");
		} else if (directive == "autoindex") {
			loc._autoindex = parseOnOff(directive, nextValue(tokens, pos));
			expectToken(tokens, pos, ";");
		} else if (directive == "upload_path") {
			loc._upload_path = nextValue(tokens, pos);
			expectToken(tokens, pos, ";");
		} else if (directive == "upload_enabled") {
			loc._upload_enabled = parseOnOff(directive, nextValue(tokens, pos));
			expectToken(tokens, pos, ";");
		} else if (directive == "redirect") {
			int code = stringToInt(nextValue(tokens, pos));
			if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
				throw std::runtime_error("Invalid redirect code (use 301, 302, 303, 307 or 308)");
			std::string url = nextValue(tokens, pos);
			loc._redirect = std::make_pair(code, url);
			expectToken(tokens, pos, ";");
		} else if (directive == "cgi") {
			// cgi <extension> <interpreter>;   e.g. cgi .py /usr/bin/python3;
			std::string extension = nextValue(tokens, pos);
			if (extension.size() < 2 || extension[0] != '.')
				throw std::runtime_error("cgi extension must look like '.py', got '" + extension + "'");
			std::string interpreter = nextValue(tokens, pos);
			// The child does chdir() to the script's directory before execve(),
			// so a relative interpreter path would not be found there.
			if (interpreter[0] != '/')
				throw std::runtime_error("cgi interpreter must be an absolute path: " + interpreter);
			loc._cgi[extension] = interpreter;
			expectToken(tokens, pos, ";");
		} else if (directive == "client_max_body_size") {
			// Overrides the server's limit for this location
			loc._client_max_body_size = parseSize(nextValue(tokens, pos));
			loc._has_client_max_body_size = true;
			expectToken(tokens, pos, ";");
		} else if (directive == "allowed_methods") {
			loc._allowed_methods.clear();
			while (pos < tokens.size() && tokens[pos] != ";") {
				loc._allowed_methods.push_back(nextValue(tokens, pos));
			}
			if (loc._allowed_methods.empty())
				throw std::runtime_error("allowed_methods needs at least one method");
			expectToken(tokens, pos, ";");
		} else {
			throw std::runtime_error("Unknown location directive: " + directive);
		}
	}
	expectToken(tokens, pos, "}");
	return loc;
}

ServerConfig Configuration::parseServerBlock(std::vector<std::string> const& tokens, size_t& pos) {
	ServerConfig server;
	server._locations.clear(); // Remove the default location

	expectToken(tokens, pos, "{");
	while (pos < tokens.size() && tokens[pos] != "}") {
		std::string directive = tokens[pos++];
		if (directive == "location") {
			server._locations.push_back(parseLocationBlock(tokens, pos, server._root));
		} else if (directive == "listen") {
			server._listen_address = parseEndpoint(nextValue(tokens, pos));
			expectToken(tokens, pos, ";");
		} else if (directive == "error_page") {
			int code = stringToInt(nextValue(tokens, pos));
			if (code < 300 || code > 599)
				throw std::runtime_error("Invalid error_page code (must be 300-599)");
			std::string page = nextValue(tokens, pos);
			server._error_pages[code] = page;
			expectToken(tokens, pos, ";");
		} else if (directive == "client_max_body_size") {
			server._client_max_body_size = parseSize(nextValue(tokens, pos));
			expectToken(tokens, pos, ";");
		} else if (directive == "root") {
			server._root = nextValue(tokens, pos);
			expectToken(tokens, pos, ";");
		} else {
			throw std::runtime_error("Unknown server directive: " + directive);
		}
	}
	expectToken(tokens, pos, "}");
	return server;
}

void Configuration::parse(std::string const& configFilePath, Configuration& config) {
	std::vector<std::string> tokens = tokenize(configFilePath);
	
	// Backward compatibility for test files without a 'server' block wrapper
	if (!tokens.empty() && tokens[0] != "server") {
		tokens.insert(tokens.begin(), "{");
		tokens.insert(tokens.begin(), "server");
		tokens.push_back("}");
	}

	size_t pos = 0;
	
	while (pos < tokens.size()) {
		if (tokens[pos] == "server") {
			pos++;
			config._servers.push_back(parseServerBlock(tokens, pos));
		} else {
			throw std::runtime_error("Unknown top-level directive: " + tokens[pos]);
		}
	}
	
	// Two server blocks on the same interface:port: without virtual hosts
	// (server_name) there would be no way to choose between them, so it is
	// a configuration error (the evaluation sheet asks for an error here).
	for (size_t i = 0; i < config._servers.size(); ++i) {
		for (size_t j = i + 1; j < config._servers.size(); ++j) {
			std::pair<in_addr_t, int> a = config._servers[i].getListenAddress();
			if (a == config._servers[j].getListenAddress()) {
				std::ostringstream msg;
				msg << "duplicate listen " << logfmt::ipToString(a.first) << ":" << a.second
				    << " (server blocks " << i + 1 << " and " << j + 1
				    << "): virtual hosts are not supported";
				throw std::runtime_error(msg.str());
			}
		}
	}

	if (config._servers.empty()) {
		// Fallback so the app doesn't crash on an empty config file
		ServerConfig default_server;
		config._servers.push_back(default_server);
	}
}

namespace {
	// Joins the methods of a location: "GET POST DELETE"
	std::string joinMethods(std::vector<std::string> const& methods) {
		std::string out;
		for (size_t i = 0; i < methods.size(); ++i) {
			if (i)
				out += " ";
			out += methods[i];
		}
		return out.empty() ? "(none)" : out;
	}

	// Everything special about a location, in one line:
	// "autoindex  index a.html  upload -> www/uploads  redirect 301 -> /new"
	std::string locationFlags(LocationConfig const& loc, std::string const& server_root) {
		std::ostringstream out;
		if (loc.getAutoindex())
			out << "autoindex  ";
		if (loc.getIndex() != "index.html")
			out << "index " << loc.getIndex() << "  ";
		if (!loc.getAlias().empty())
			out << "alias " << loc.getAlias() << "  ";
		else if (loc.getRoot() != server_root)
			out << "root " << loc.getRoot() << "  ";
		if (loc.getUploadEnabled() || !loc.getUploadPath().empty())
			out << "upload -> " << (loc.getUploadPath().empty() ? "(default)" : loc.getUploadPath()) << "  ";
		if (loc.getRedirect().first != 0)
			out << "redirect " << loc.getRedirect().first << " -> " << loc.getRedirect().second << "  ";
		if (loc.hasClientMaxBodySize())
			out << "max body " << logfmt::humanSize(loc.getClientMaxBodySize()) << "  ";
		for (std::map<std::string, std::string>::const_iterator it = loc.getCgi().begin();
		     it != loc.getCgi().end(); ++it)
			out << "cgi " << it->first << " -> " << it->second << "  ";
		return out.str();
	}
}

void Configuration::print(std::string const& source) const {
	std::string rule;
	for (size_t i = 0; i < 60; ++i)
		rule += "─";

	std::cout << "\n  " << BOLD << "webserv" << NC;
	if (!source.empty())
		std::cout << GRAY << "  ·  " << source << NC;
	std::cout << GRAY << "  ·  " << _servers.size()
	          << (_servers.size() == 1 ? " server" : " servers") << NC << "\n";
	std::cout << GRAY << "  " << rule << NC << "\n";

	for (size_t i = 0; i < _servers.size(); ++i) {
		ServerConfig const& server = _servers[i];
		std::pair<in_addr_t, int> listen = server.getListenAddress();

		std::cout << "  " << LIGHT_BLUE << BOLD << "● " << logfmt::ipToString(listen.first)
		          << ":" << listen.second << NC
		          << GRAY << "   root " << NC << server.getRoot()
		          << GRAY << "   max body " << NC << logfmt::humanSize(server.getClientMaxBodySize())
		          << "\n";

		std::map<int, std::string> const& pages = server.getErrorPages();
		if (!pages.empty()) {
			std::cout << GRAY << "    error pages" << NC;
			for (std::map<int, std::string>::const_iterator it = pages.begin(); it != pages.end(); ++it)
				std::cout << " " << it->first;
			std::cout << "\n";
		}

		std::vector<LocationConfig> const& locs = server.getLocations();
		for (size_t j = 0; j < locs.size(); ++j) {
			std::cout << "    " << logfmt::pad(locs[j].getLocationMatch(), 16)
			          << YELLOW << logfmt::pad(joinMethods(locs[j].getAllowedMethods()), 20) << NC
			          << GRAY << locationFlags(locs[j], server.getRoot()) << NC << "\n";
		}
		if (i + 1 < _servers.size())
			std::cout << "\n";
	}
	std::cout << GRAY << "  " << rule << NC << std::endl;
}
