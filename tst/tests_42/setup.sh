#!/usr/bin/env bash
# Prepara todo lo que pide el tester oficial de 42 (tst/tests_42/tester).
#
#   bash tst/tests_42/setup.sh        (desde la raíz del repo)
#
# Después, en dos terminales (en un ordenador de 42: los binarios son de Linux):
#   ./webserv tst/tests_42/tester.conf
#   ./tst/tests_42/tester http://localhost:8080
#
# Lo que pide el tester ("Setup the configuration file as follow"):
#   - / must answer to GET request ONLY
#   - Any file with .bla as extension must answer to POST request by calling the cgi_test executable
#   - /post_body must answer anything to POST request with a maxBody of 100
#   - /directory/ must answer to GET request and the root of it would be the repository
#     YoupiBanane and if no file are requested, it should search for youpi.bad_extension files

cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"

# Los binarios vienen sin permiso de ejecución
chmod +x tester cgi_tester

# Directorio YoupiBanane, exactamente como lo describe el tester
rm -rf YoupiBanane
mkdir -p YoupiBanane/nop YoupiBanane/Yeah
echo "youpi.bad_extension"            > YoupiBanane/youpi.bad_extension
# youpi.bla es un CGI mínimo en Python que hace lo mismo que cgi_tester
# (devuelve el body en mayúsculas). cgi_tester no lee el fichero del script,
# así que con el tester de 42 da igual su contenido; y con etc/testing.conf
# ("cgi .bla /usr/bin/python3") funciona también en macOS o en un contenedor
# ARM, donde el cgi_tester de 42 (Linux x86-64) no se puede ejecutar.
cat > YoupiBanane/youpi.bla <<'PY'
import sys
data = sys.stdin.buffer.read()
sys.stdout.buffer.write(b"Status: 200 OK\r\nContent-Type: text/plain\r\n\r\n" + data.upper())
PY
echo "nop/youpi.bad_extension"        > YoupiBanane/nop/youpi.bad_extension
echo "nop/other.pouic"                > YoupiBanane/nop/other.pouic
echo "Yeah/not_happy.bad_extension"   > YoupiBanane/Yeah/not_happy.bad_extension
# El tester también hace POST a /directory/youpla.bla, que no está en su lista.
# cgi_tester no lee el fichero del script, pero nuestro servidor responde 404
# si el script no existe, así que lo creamos para que se ejecute el CGI.
cp YoupiBanane/youpi.bla YoupiBanane/youpla.bla

# Donde se guardan los POST a /post_body
rm -rf post_body_uploads
mkdir -p post_body_uploads

# Config con rutas absolutas (el intérprete de cgi tiene que serlo: el CGI se
# ejecuta dentro del directorio del script)
cat > tester.conf <<EOF
# Generado por tst/tests_42/setup.sh: config que pide el tester de 42
server {
    listen 8080;
    root www/;
    client_max_body_size 200M;

    # / must answer to GET request ONLY
    location / {
        allowed_methods GET;
    }

    # /post_body must answer anything to POST request with a maxBody of 100
    location /post_body {
        client_max_body_size 100;
        upload_enabled on;
        upload_path $HERE/post_body_uploads;
        allowed_methods POST;
    }

    # /directory/ -> YoupiBanane, default file youpi.bad_extension,
    # .bla files -> cgi_tester
    location /directory {
        root $HERE/YoupiBanane;
        index youpi.bad_extension;
        cgi .bla $HERE/cgi_tester;
        allowed_methods GET POST;
    }
}
EOF

echo "Listo:"
echo "  YoupiBanane/        $(find YoupiBanane -type f | wc -l | tr -d ' ') ficheros"
echo "  tester.conf         config para el tester"
echo
echo "Ahora (en Linux):"
echo "  ./webserv tst/tests_42/tester.conf"
echo "  ./tst/tests_42/tester http://localhost:8080"
