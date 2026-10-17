#!/usr/bin/env python3
# CGI de ejemplo: muestra el metodo, la query string y el body recibidos.
import os
import sys

method = os.environ.get("REQUEST_METHOD", "")
query = os.environ.get("QUERY_STRING", "")
body = sys.stdin.read()

print("Content-Type: text/html")
print()
print("<html><body>")
print("<h1>Hello from CGI</h1>")
print("<p>Method: %s</p>" % method)
print("<p>Query string: %s</p>" % query)
print("<p>Body (%d bytes): %s</p>" % (len(body), body[:200]))
print("<p>Working directory: %s</p>" % os.getcwd())
print("</body></html>")
