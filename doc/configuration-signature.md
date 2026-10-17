listen [address:]8080 ;

error_page 403 ./www/example/err/403.html ;

root ./www;

client_max_body_size 1;

# STATIC FILES
location / {
	allowed_methods GET POST DELETE;
	autoindex on;
	index /res/html/index.html;
	upload ./www/upl;
}

location /images {
	alias /img;
	autoindex on;
}

# REDIRECTION
location /etc {
	return xxx yyy;
}

# CGI --------------------------------------------------------------------------
location /cgi-bin {
	root ./www/amiwuevo;
	autoindex off;
	cgi_path /usr/bin/python3 /usr/bin/php /usr/bin/bash;
	cgi_ext .py .php .sh;
}

# We'll need an additional server for another page

This shit assumes the structure is always:
	ignore ws (repeat until not ws)
	*directive* (repeat until ws)
	ws 
	ignore ws (repeat until not ws)
	*argument*/*multiargument* (repeat until ws) (recursive)
	;

multiargument starts and finishes with braces {} and follows the same structure as the whole structure

ok so i run into some difficulties and i've decided to set the norm that everything must be separated by spaces