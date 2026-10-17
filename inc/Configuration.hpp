#ifndef CONFIGURATION_HPP
# define CONFIGURATION_HPP

#include <string>
#include <map>
#include <netinet/in.h>
#include <vector>

// Represents a single route rule inside a server configuration.
// (Equivalent to a `location {}` block in NGINX)
class LocationConfig {
public:
	enum RouteType {
		STATIC_FILE,
		REDIRECT,
		CGI
	};

private:
	std::string _location_match;
	RouteType   _type;
	std::string _root;
	std::string _alias;
	std::string _index;
	bool _autoindex;
	std::string _upload_path;
	bool _upload_enabled;
	std::pair<int, std::string> _redirect;
	std::vector<std::string> _allowed_methods;
	// CGI por extensión: ".py" -> "/usr/bin/python3" (directiva "cgi .py /usr/bin/python3;")
	std::map<std::string, std::string> _cgi;
	// client_max_body_size propio de la location (si no, se usa el del server)
	size_t _client_max_body_size;
	bool _has_client_max_body_size;

public:
	LocationConfig(std::string const & root, std::string const & location_match);
	~LocationConfig();

	// Getters
	std::string const & getLocationMatch() const;
	RouteType getType() const;
	std::string const & getRoot() const;
	std::string const & getAlias() const;
	std::string const & getIndex() const;
	bool getAutoindex() const;
	std::string const & getUploadPath() const;
	bool getUploadEnabled() const;
	std::pair<int, std::string> const & getRedirect() const;
	std::vector<std::string> const & getAllowedMethods() const;
	std::map<std::string, std::string> const & getCgi() const;
	bool hasClientMaxBodySize() const;
	size_t getClientMaxBodySize() const;

	friend class Configuration;
};

// Represents a single virtual host / server.
// (Equivalent to a `server {}` block in NGINX)
class ServerConfig {
private:
	std::pair<in_addr_t, int> _listen_address;
	std::map<int, std::string> _error_pages;
	size_t _client_max_body_size;
	std::string _root;
	std::vector<LocationConfig> _locations;

public:
	ServerConfig();
	~ServerConfig();

	// Getters
	std::pair<in_addr_t, int> const & getListenAddress() const;
	std::map<int, std::string> const & getErrorPages() const;
	size_t getClientMaxBodySize() const;
	std::string const & getRoot() const;
	std::vector<LocationConfig> const & getLocations() const;
	friend class Configuration;
};

// Represents the global configuration parsed from the file.
// Contains multiple ServerConfigs.
class Configuration {
private:
	std::vector<ServerConfig> _servers;

	// Parser Helpers
	static std::vector<std::string> tokenize(std::string const& configFilePath);
	static ServerConfig parseServerBlock(std::vector<std::string> const& tokens, size_t& pos);
	static LocationConfig parseLocationBlock(std::vector<std::string> const& tokens, size_t& pos, std::string const& root);
	static void expectToken(std::vector<std::string> const& tokens, size_t& pos, std::string const& expected);
	// Devuelve el siguiente token como valor de una directiva. Lanza excepción
	// si se acaba el fichero o si es '{', '}' o ';' (falta el valor).
	static std::string const& nextValue(std::vector<std::string> const& tokens, size_t& pos);

public:
	Configuration();
	~Configuration();

	static void parse(std::string const& configFilePath, Configuration& config);

	std::vector<ServerConfig> const & getServers() const;

	// Prints a coloured summary of the configuration. `source` is the file it
	// was read from (optional, only shown in the header).
	void print(std::string const& source = "") const;
};

#endif