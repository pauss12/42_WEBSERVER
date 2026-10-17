#!/usr/bin/env bash
# Pruebas con curl, en el orden de la hoja de corrección (doc/WebserEval.pdf).
# Muestra cada comando antes de ejecutarlo, para poder repetirlo a mano.
#
#   bash tst/curl_tester.sh          # todas las pruebas (~15 s)
#   bash tst/curl_tester.sh --fast   # sin la del CGI infinito (504, tarda 10 s)
#
# Usa etc/default.conf (puertos 8080 y 8081). Si el servidor no está
# arrancado, lo arranca y lo para al terminar. Para no chocar con un servidor
# que ya tengas abierto, se pueden usar otros puertos:
#   PORT1=9080 PORT2=9081 bash tst/curl_tester.sh

cd "$(dirname "$0")/.." || exit 1
GREEN=$'\033[32m'; RED=$'\033[31m'; YELLOW=$'\033[33m'; GRAY=$'\033[90m'; BOLD=$'\033[1m'; NC=$'\033[0m'
PORT1=${PORT1:-8080}; PORT2=${PORT2:-8081}
A="http://127.0.0.1:$PORT1"    # sitio principal
B="http://127.0.0.1:$PORT2"    # segundo sitio (otra interfaz, límite de 100 bytes)
FAST=0; [ "$1" = "--fast" ] && FAST=1
PASS=0; FAIL=0; STARTED=""
TMP=$(mktemp -d)

section() { printf "\n${BOLD}%s${NC}\n" "$1"; }

# check "descripción" "esperado" "obtenido" "comando"
check() {
    printf "  ${GRAY}\$ %s${NC}\n" "$4"
    if [ "$2" = "$3" ]; then
        printf "  ${GREEN}✔${NC} %-52s %s\n" "$1" "$3"; PASS=$((PASS + 1))
    else
        printf "  ${RED}✘ %-52s esperado %s, obtenido %s${NC}\n" "$1" "$2" "$3"; FAIL=$((FAIL + 1))
    fi
}

# Código HTTP de un comando curl (se le añaden -s -o /dev/null -w '%{http_code}')
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$@"; }

cleanup() {
    rm -rf "$TMP" "www/uploads/curl_test.txt" www/cgi/_curl_loop.py www/cgi/_curl_error.py
    [ -n "$STARTED" ] && kill -INT "$STARTED" 2>/dev/null
}
trap cleanup EXIT

# --- servidor ------------------------------------------------------------------
if ! curl -s -o /dev/null --max-time 2 "$A/"; then
    [ -x ./webserv ] || { echo "Compilando..."; make -s >/dev/null || exit 1; }
    # etc/default.conf, con los puertos cambiados si se pidieron otros
    sed "s/:8080;/:$PORT1;/; s/:8081;/:$PORT2;/" etc/default.conf > "$TMP/default.conf"
    ./webserv "$TMP/default.conf" > "$TMP/server.log" 2>&1 &
    STARTED=$!
    sleep 1
    curl -s -o /dev/null --max-time 2 "$A/" || { echo "${RED}El servidor no arranca:${NC}"; cat "$TMP/server.log"; exit 1; }
    printf "${GRAY}Servidor arrancado con etc/default.conf en %s/%s (pid %s); se parará al terminar.${NC}\n" "$PORT1" "$PORT2" "$STARTED"
else
    printf "${YELLOW}Usando el servidor que ya escucha en el puerto %s: tiene que estar arrancado con\n" "$PORT1"
    printf "etc/default.conf y con el código actual (si no, reinícialo o usa PORT1/PORT2).${NC}\n"
fi

# --- 1. Basic checks -------------------------------------------------------------
section "1. Basic checks: GET, POST, DELETE, UNKNOWN"
check "GET / (sitio estático)"            200 "$(code $A/)"                     "curl -i $A/"
check "GET de una URL que no existe"       404 "$(code $A/noexiste)"             "curl -i $A/noexiste"
check "Método desconocido (no crashea)"    501 "$(code -X FOO $A/)"              "curl -i -X FOO $A/"
check "HEAD / (no permitido en /)"         405 "$(code -I $A/)"                  "curl -I $A/"

echo "hola desde curl" > "$TMP/curl_test.txt"
check "POST: subir un fichero (multipart)" 200 "$(code -F "file=@$TMP/curl_test.txt" $A/upload)" \
      "curl -i -F file=@curl_test.txt $A/upload"
check "GET: descargar el fichero subido"   "hola desde curl" "$(curl -s $A/uploads/curl_test.txt)" \
      "curl $A/uploads/curl_test.txt"
check "DELETE del fichero (permitido)"     200 "$(code -X DELETE $A/uploads/curl_test.txt)" \
      "curl -i -X DELETE $A/uploads/curl_test.txt"
check "DELETE otra vez (ya no existe)"     404 "$(code -X DELETE $A/uploads/curl_test.txt)" \
      "curl -i -X DELETE $A/uploads/curl_test.txt"

# --- 2. Configuración ------------------------------------------------------------
section "2. Configuración"
check "Segundo sitio (127.0.0.1:8081)"     200 "$(code $B/)"                     "curl -i $B/"
check "  ...con otro contenido"            "yes" "$(curl -s $B/ | grep -q 'Second site' && echo yes)" \
      "curl -s $B/ | grep 'Second site'"
check "Body por debajo del límite (100 B)" 200 "$(code -X POST -H 'Content-Type: plain/text' --data 'BODY IS HERE short' $B/)" \
      "curl -i -X POST -H \"Content-Type: plain/text\" --data \"BODY IS HERE short\" $B/"
LONG=$(printf 'x%.0s' $(seq 200))
check "Body por encima del límite"         413 "$(code -X POST -H 'Content-Type: plain/text' --data "$LONG" $B/)" \
      "curl -i -X POST -H \"Content-Type: plain/text\" --data \"<200 bytes>\" $B/"
check "DELETE sin permiso"                 405 "$(code -X DELETE $A/index.html)" "curl -i -X DELETE $A/index.html"
check "  ...con cabecera Allow"            "Allow: GET" "$(curl -s -o /dev/null -D - -X DELETE $A/index.html | tr -d '\r' | grep -i '^allow:')" \
      "curl -s -D - -o /dev/null -X DELETE $A/index.html | grep Allow"
check "Fichero por defecto de un directorio" 200 "$(code $A/showUploads)"        "curl -i $A/showUploads"
check "Ruta a otro directorio (/uploads/)" 200 "$(code $A/uploads/)"             "curl -i $A/uploads/"

# --- 3. Códigos y cabeceras ------------------------------------------------------
section "3. Códigos de estado y cabeceras"
check "Redirección"                        301 "$(code $A/old)"                  "curl -i $A/old"
check "  ...con Location"                  "Location: /" "$(curl -s -o /dev/null -D - $A/old | tr -d '\r' | grep -i '^location:')" \
      "curl -s -D - -o /dev/null $A/old | grep Location"
check "Listado de directorio (autoindex)"  "yes" "$(curl -s $A/uploads/ | grep -q 'Index of' && echo yes)" \
      "curl -s $A/uploads/ | grep 'Index of'"
check "Content-Type de una página HTML"    "text/html; charset=UTF-8" "$(curl -s -o /dev/null -w '%{content_type}' $A/)" \
      "curl -s -o /dev/null -w '%{content_type}' $A/"
SIZE=$(curl -s $A/ | wc -c | tr -d ' ')
check "Content-Length == tamaño del body"  "$SIZE" "$(curl -s -o /dev/null -D - $A/ | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}')" \
      "curl -s -D - -o /dev/null $A/ | grep Content-Length"
check "HTTP/1.1 sin cabecera Host"         400 "$(code -H 'Host:' $A/)"          "curl -i -H \"Host:\" $A/"
check "URI demasiado larga"                414 "$(code "$A/$(printf 'a%.0s' $(seq 3000))")" "curl -i $A/aaaa...(3000)"
check "Path traversal (/../)"              400 "$(code --path-as-is $A/../../etc/passwd)" "curl -i --path-as-is $A/../../etc/passwd"

# --- 4. CGI ------------------------------------------------------------------------
section "4. CGI"
check "CGI con GET y query string"         "yes" "$(curl -s "$A/cgi/example.py?x=1" | grep -q 'Query string: x=1' && echo yes)" \
      "curl \"$A/cgi/example.py?x=1\""
check "CGI con POST (body por stdin)"      "yes" "$(curl -s -d 'nombre=paula' $A/cgi/example.py | grep -q 'nombre=paula' && echo yes)" \
      "curl -d nombre=paula $A/cgi/example.py"
check "CGI con POST en chunked"            "yes" "$(curl -s -H 'Transfer-Encoding: chunked' -d 'nombre=paula' $A/cgi/example.py | grep -q 'nombre=paula' && echo yes)" \
      "curl -H \"Transfer-Encoding: chunked\" -d nombre=paula $A/cgi/example.py"
check "CGI ejecutado en su directorio"     "yes" "$(curl -s $A/cgi/example.py | grep -q 'www/cgi' && echo yes)" \
      "curl -s $A/cgi/example.py | grep 'Working directory'"
check "Script que no existe"               404 "$(code $A/cgi/noexiste.py)"      "curl -i $A/cgi/noexiste.py"
printf 'import sys\nsys.exit(1)\n' > www/cgi/_curl_error.py
check "Script que falla sin escribir nada" 502 "$(code $A/cgi/_curl_error.py)"   "curl -i $A/cgi/_curl_error.py"
if [ $FAST -eq 0 ]; then
    printf 'while True:\n    pass\n' > www/cgi/_curl_loop.py
    check "Script en bucle infinito (~10 s)" 504 "$(code $A/cgi/_curl_loop.py)"  "curl -i $A/cgi/_curl_loop.py"
else
    printf "  ${YELLOW}-${NC} Script en bucle infinito: omitido (--fast)\n"
fi
check "El servidor sigue vivo"             200 "$(code $A/)"                     "curl -i $A/"

# --- resumen -------------------------------------------------------------------------
printf "\n${BOLD}Resultado:${NC} ${GREEN}%d correctas${NC}" "$PASS"
[ $FAIL -gt 0 ] && printf " · ${RED}%d fallidas${NC}" "$FAIL"
printf "\n"
[ $FAIL -eq 0 ]
