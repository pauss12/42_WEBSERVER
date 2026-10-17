#!/usr/bin/env bash
# Demuestra que webserv usa UN solo multiplexor (poll() o equivalente).
#
#   bash tst/check_single_poll.sh            (desde la raíz del repo, tras `make`)
#
# 1. Prueba estática: busca en el código poll()/select() y cuántos multiplexores
#    se crean y dónde se espera a los eventos.
# 2. Prueba en ejecución: arranca webserv, le lanza clientes y CGIs a la vez, y
#    cuenta cuántos kqueue (macOS) / epoll (Linux) tiene abiertos el proceso.

cd "$(dirname "$0")/.." || exit 1
GREEN=$'\033[32m'; RED=$'\033[31m'; BOLD=$'\033[1m'; GRAY=$'\033[90m'; NC=$'\033[0m'
PORT=18496
SRC="src/*.cpp app/*.cpp inc/*.hpp"
ok=1

title() { printf "\n${BOLD}%s${NC}\n" "$1"; }
pass()  { printf "  ${GREEN}✔${NC} %s\n" "$1"; }
fail()  { printf "  ${RED}✘ %s${NC}\n" "$1"; ok=0; }
# grep sin las líneas que son comentarios
code_grep() { grep -nE "$1" $SRC | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)'; }

# ------------------------------------------------------------------------------
title "1. Prueba estática (código fuente)"

calls=$(code_grep '\b(poll|select)[[:space:]]*\(')
if [ -z "$calls" ]; then pass "ninguna llamada a poll() ni select()"
else fail "llamadas a poll()/select():"; echo "$calls"; fi

printf "\n  ${GRAY}Creación del multiplexor (kqueue en macOS, epoll en Linux; excluyentes por #if):${NC}\n"
code_grep '\b(kqueue|epoll_create1?)[[:space:]]*\(' | sed 's/^/    /'

printf "\n  ${GRAY}Único punto donde se ESPERA a los eventos (Server::run):${NC}\n"
code_grep '\bepoll_wait[[:space:]]*\(|kevent\([^)]*events' | sed 's/^/    /'

waits=$(code_grep '\bepoll_wait[[:space:]]*\(|kevent\([^)]*events' | wc -l | tr -d ' ')
[ "$waits" -le 2 ] && pass "un punto de espera por plataforma (kevent en macOS / epoll_wait en Linux)" \
                   || fail "$waits puntos de espera"

printf "\n  ${GRAY}El resto de kevent()/epoll_ctl() solo REGISTRAN fds (no esperan):${NC}\n"
code_grep '\bepoll_ctl[[:space:]]*\(|kevent\([^)]*NULL, 0, NULL\)' | sed 's/^/    /'

# ------------------------------------------------------------------------------
title "2. Prueba en ejecución (proceso real)"

if [ ! -x ./webserv ]; then
    printf "  ${GRAY}./webserv no existe: compilando con make...${NC}\n"
    make -s > /dev/null 2>&1 || { fail "make ha fallado: ejecuta make para ver el error"; exit 1; }
fi
tmp=$(mktemp -d)
cat > "$tmp/demo.conf" <<EOF
server {
    listen 127.0.0.1:$PORT;
    root www/;
    location / { allowed_methods GET POST; }
    location /cgi { root www/cgi; cgi .py /usr/bin/python3; allowed_methods GET POST; }
}
EOF
script="www/cgi/_poll_demo_$$.py"
printf 'import time\ntime.sleep(3)\nprint("Content-Type: text/plain\\n")\nprint("ok")\n' > "$script"

./webserv "$tmp/demo.conf" > "$tmp/server.log" 2>&1 &
pid=$!
trap 'kill -INT $pid 2>/dev/null; rm -f "$script"; rm -rf "$tmp"' EXIT
sleep 1

# 3 CGIs (3 s cada uno) + 5 clientes normales, todo a la vez
curls=""
for _ in 1 2 3; do curl -s -o /dev/null "http://127.0.0.1:$PORT/cgi/$(basename "$script")" & curls="$curls $!"; done
for _ in 1 2 3 4 5; do curl -s -o /dev/null "http://127.0.0.1:$PORT/" & curls="$curls $!"; done
sleep 1

if [ "$(uname)" = "Darwin" ]; then
    printf "  ${GRAY}lsof -p %s  (sockets, pipes de CGI y multiplexores):${NC}\n" "$pid"
    lsof -p "$pid" 2>/dev/null | awk '/KQUEUE|PIPE|TCP/ {printf "    %-5s %-7s %s %s %s\n", $4, $5, $9, $10, $11}'
    count=$(lsof -p "$pid" 2>/dev/null | grep -c KQUEUE)
    kind="kqueue"
else
    printf "  ${GRAY}ls -l /proc/%s/fd:${NC}\n" "$pid"
    ls -l "/proc/$pid/fd" | awk 'NR>1 {print "    " $9, $10, $11}'
    count=$(ls -l "/proc/$pid/fd" | grep -c eventpoll)
    kind="epoll"
fi
pipes=$(lsof -p "$pid" 2>/dev/null | grep -c PIPE)

echo
[ "$count" -eq 1 ] && pass "el proceso tiene exactamente 1 $kind abierto (con $pipes pipes de CGI y varios clientes a la vez)" \
                   || fail "el proceso tiene $count $kind abiertos"
wait $curls 2>/dev/null   # solo los curl: webserv sigue corriendo

# ------------------------------------------------------------------------------
echo
if [ $ok -eq 1 ]; then printf "${GREEN}${BOLD}Un solo multiplexor para todo: sockets de escucha, clientes y pipes de CGI.${NC}\n"
else printf "${RED}${BOLD}Revisar los puntos marcados con ✘${NC}\n"; exit 1; fi
