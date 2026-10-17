# Verificación contra el subject

- **Verificado sobre:** `ee5d79f` (rama `temp`), en un **clon nuevo** del repo · **Fecha:** 16/10/2026
- **Plataformas:** macOS (kqueue) y Linux (epoll, contenedores ARM y x86-64)

**Completado:** ✅ sí · ⚠️ parcialmente · ❌ no

---

## II. General rules

| Lo que pide el subject | Comprobación realizada | Resultado | Completado |
|---|---|---|---|
| *"Your program must not crash under any circumstances (even if it runs out of memory)"* | `tst/tester.py` (200 conexiones simultáneas, clientes que se van a mitad, CGI infinito…); `tst/stress.py` con 100 clientes; arrancar con 16 configs mal formadas | El servidor no muere en ningún test ni tras 91 000 peticiones; las 16 configs rotas se rechazan con un mensaje, sin crash | ✅ |
| *"You must submit a Makefile that compiles your source files. It must not perform unnecessary relinking."* | `make` dos veces seguidas | El segundo `make` dice `Nothing to be done for 'all'` | ✅ |
| *"Your Makefile must at least contain the rules: $(NAME), all, clean, fclean and re."* | Revisar el Makefile | Están las cinco reglas | ✅ |
| *"Compile your code with c++ and the flags -Wall -Wextra -Werror"* | `make re` en un clon nuevo | Compila con `c++ -Wall -Wextra -Werror` sin warnings | ✅ |
| *"Your code must comply with the C++ 98 standard and should still compile when adding the flag -std=c++98."* | `make re` (el Makefile ya usa `-std=c++98`) | Compila sin errores | ✅ |
| *"choose \<cstring\> over \<string.h\>"* | Buscar includes de C que tienen versión C++ (`<string.h>` → `<cstring>`, `<stdlib.h>` → `<cstdlib>`…):<br>`grep -nE '#include <(stdio\|stdlib\|string\|signal\|time\|errno\|ctype)\.h>' src/*.cpp inc/*.hpp app/*.cpp` | Sin resultados: el código usa siempre `<cstring>`, `<cstdlib>`, `<csignal>`… Los `.h` que aparecen (`<unistd.h>`, `<sys/socket.h>`, `<fcntl.h>`…) son cabeceras POSIX, que no tienen versión C++ | ✅ |
| *"Any external library and Boost libraries are forbidden."* | Revisar los includes y el enlazado | Solo la biblioteca estándar y las llamadas al sistema | ✅ |

## IV. Mandatory part

| Lo que pide el subject | Comprobación realizada | Resultado | Completado |
|---|---|---|---|
| *"Program name: webserv"* · *"./webserv [configuration file]"* | `make` y `ls` | El ejecutable es `./webserv`, en la raíz del repo | ✅ |
| Solo las funciones externas de la lista (`execve`, `pipe`, `fork`, `socket`, `kevent`…) | `grep` de funciones no permitidas (`inet_addr`, `realpath`, `getcwd`, `mkdir`, `gettimeofday`…) | Ninguna. `inet_addr` y `realpath` se sustituyeron por código propio | ✅ |
| *"Your program must use a configuration file, provided as an argument on the command line, or available in a default path."* | `./webserv` sin argumento y `./webserv etc/default.conf` | Sin argumento carga `etc/default.conf`; con argumento, el fichero indicado | ✅ |
| *"You cannot execve another web server."* | Revisar los `execve` | El único `execve` ejecuta el script CGI | ✅ |
| *"use only 1 poll() (or equivalent) for all the I/O operations between the clients and the server (listen included)"* | `bash tst/check_single_poll.sh`: `grep` en el código y `lsof` del proceso con 3 CGIs y 5 clientes a la vez | Ningún `poll()`/`select()`; un único punto de espera (`kevent` en `Server::run`); el proceso tiene **1 KQUEUE** con sockets y pipes | ✅ |
| *"poll() (or equivalent) must monitor both reading and writing simultaneously."* | Revisar `Server::run` y `_switchToWriteMode`/`_switchToReadMode` | El mismo kqueue vigila `EVFILT_READ` y `EVFILT_WRITE` | ✅ |
| *"You must never do a read or a write operation without going through poll()"* | Revisar `handleClientRead`, `handleClientWrite` y `handleCgiEvent` | Un solo `recv`/`send`/`read`/`write` por evento; los ficheros regulares quedan fuera (el subject lo permite) | ✅ |
| *"Checking the value of errno to adjust the server behaviour is strictly forbidden after performing a read or write operation."* | `grep errno` | Solo aparece tras `socket`/`bind`/`listen`/`kevent`/`accept`/`opendir`, nunca tras read/write | ✅ |
| *"Your server must remain non-blocking at all times and properly handle client disconnections"* | Tests `client_disconnects_mid_response` y `client_disconnects_mid_request`; `cgi_timeout_does_not_block` | 30 clientes que cierran a mitad de una descarga de 5 MB → el servidor sigue vivo; con un CGI infinito, otro cliente responde en 0,0006 s | ✅ |
| *"A request to your server should never hang indefinitely."* | Tests `idle_timeout` (`--slow`) y `cgi_timeout_does_not_block` | Una petición a medias se cierra a los 60 s; un CGI infinito da 504 a los 10 s | ✅ |
| *"Your server must be compatible with standard web browsers"* | Navegar por `http://localhost:8080` | Las páginas, el formulario de upload, el listado y el borrado funcionan | ✅ |
| *"Your HTTP response status codes must be accurate."* | Tests `standard_reason_phrases`, `empty_file`, `delete_missing_file`, `unknown_method`, `chunked_malformed`, `unsupported_transfer_encoding`; `curl` con `default.conf` | 200/301/400/403/404/405 (con cabecera `Allow`)/409/413/414/501/502/504/505 correctos: fichero vacío → 200, DELETE de algo inexistente → 404, método desconocido → 501, directorio sin index ni autoindex → 403 | ✅ |
| *"Your server must have default error pages if none are provided."* | Test `default_error_page`; pedir una ruta inexistente | Se sirve la plantilla `www/err/template.html`; si falta, un HTML mínimo | ✅ |
| *"You can't use fork for anything other than CGI"* | `grep fork` | Un único `fork()`, en `CgiHandler::start` | ✅ |
| *"You must be able to serve a fully static website."* | Test `get_index`; navegador | El body coincide byte a byte con `www/index.html`, con `Content-Type` y `Content-Length` correctos | ✅ |
| *"Clients must be able to upload files."* | Tests `upload_get_list_delete` y `upload_binary`; upload en un clon nuevo | La subida multipart funciona (también la binaria) y en un clon nuevo (`www/uploads/.gitkeep`) | ✅ |
| *"You need at least the GET, POST, and DELETE methods."* | Test `upload_get_list_delete`; `curl` con `default.conf` | GET 200, POST 200, DELETE 200 → GET otra vez 404 | ✅ |
| *"Stress test your server to ensure it remains available at all times."* | `python3 tst/stress.py -c 100 -t 8`; sin keep-alive; contra el CGI | 100 % de disponibilidad en los tres casos (~11 300, ~9 900 y ~180 req/s) | ✅ |
| *"Your server must be able to listen to multiple ports to deliver different content"* | Test `multiple_ports`; `curl` a `:8080` y `:8081` con `default.conf` | Cada puerto sirve su propio root (`www/` y `www/site2/`) y su propia config | ✅ |
| *(macOS)* *"you are allowed to use fcntl() only with the following flags: F_SETFL, O_NONBLOCK and, FD_CLOEXEC"* | `grep fcntl` | Las 3 llamadas son `fcntl(fd, F_SETFL, O_NONBLOCK)` | ✅ |

## IV.3 Configuration file

| Lo que pide el subject | Comprobación realizada | Resultado | Completado |
|---|---|---|---|
| *"Define all the interface:port pairs on which your server will listen to"* | Configs con `listen 8080;`, `listen 127.0.0.1:8080;` e IPs o puertos inválidos | Se escucha en la interfaz y el puerto indicados; `300.1.1.1`, `99999` y `-5` dan error al arrancar | ✅ |
| *"Set up default error pages."* (`error_page`) | Test `custom_error_page` | Con `error_page 404 <fichero>` se sirve ese fichero (con código 404); sin `error_page`, o si el fichero no existe, la plantilla `www/err/template.html` | ✅ |
| *"Set the maximum allowed size for client request bodies."* | Test `body_too_large_early`; POST de 200 bytes a `:8081` (máximo 100) | 413 en cuanto llegan las cabeceras, sin esperar al body; se rechazan los valores negativos | ✅ |
| *"List of accepted HTTP methods for the route."* | Test `method_not_allowed`; `POST /` con `default.conf` | 405 si el método no está en `allowed_methods` | ✅ |
| *"HTTP redirection."* | Test `redirect`; `curl /old` | `301 Moved Permanently` con `Location: /` | ✅ |
| *"Directory where the requested file should be located (e.g., /kapouet rooted to /tmp/www…)"* | Location `/uploads` con `root www/uploads`: `GET` y `DELETE /uploads/fichero` | Se busca en `www/uploads/fichero`, como en el ejemplo de `/kapouet` | ✅ |
| *"Enabling or disabling directory listing."* | Test `autoindex_listing`; `curl /uploads/` y `/cgi/` con `default.conf` | `autoindex on` → listado HTML con enlaces (JSON si se pide con `Accept: application/json`); `autoindex off` (por defecto) y sin index → 403 | ✅ |
| *"Default file to serve when the requested resource is a directory."* | Test `index_directive`; `curl /showUploads` con `default.conf` | Se sirve el fichero de `index` (por defecto `index.html`); un directorio pedido sin `/` final → 301 a la URL con `/` | ✅ |
| *"Uploading files from the clients to the server is authorized, and storage location is provided."* | Tests del grupo `upload` y `chunked_upload` | Solo se acepta POST de upload en locations con `upload_enabled on`; se guarda en su `upload_path`. Sin `upload_enabled` → 405 | ✅ |
| *"Execution of CGI, based on file extension (for example .php)."* | Tests del grupo `cgi` con `cgi .py /usr/bin/python3;` en la location | El CGI se decide por la extensión del fichero y la directiva `cgi <ext> <intérprete>` de la location; el script es `root` + URL | ✅ |
| *"The full request and arguments provided by the client must be available to the CGI."* | Test `cgi_get_environment` | El script recibe `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SERVER_PORT` y las cabeceras como `HTTP_*` | ✅ |
| *"for chunked requests, your server needs to un-chunk them, the CGI will expect EOF as the end of the body."* | Grupo `chunked` del tester (8 tests); `curl -H 'Transfer-Encoding: chunked'` | El script recibe el body des-chunkeado por stdin, con `CONTENT_LENGTH` real y EOF al final; también troceado en muchos paquetes; 413 si supera el límite, 400 si está mal formado | ✅ |
| *"If no content_length is returned from the CGI, EOF will mark the end of the returned data."* | Tests `cgi_get_environment` y `cgi_big_post` | La salida se lee hasta EOF y el `Content-Length` lo calcula el servidor | ✅ |
| *"The CGI should be run in the correct directory for relative path file access."* | Test `cgi_get_environment` | El script ve `CWD=…/www/cgi` | ✅ |
| *"Your server should support at least one CGI"* | Tests del grupo `cgi` | Python funciona: GET, POST, POST de 2,5 MB, `Status`, 404/502/504 | ✅ |
| *"You must provide configuration files and default files to test and demonstrate that every feature works"* | Clon nuevo + `default.conf` en otros puertos | Dos sitios, upload, DELETE, CGI por extensión, redirect, autoindex (`/uploads/`), `index` (`/showUploads/`) y 413 en `:8081`. `error_page` está como ejemplo comentado: por defecto se usa la plantilla del equipo | ✅ |

## V. README

| Lo que pide el subject | Comprobación realizada | Resultado | Completado |
|---|---|---|---|
| *"The very first line must be italicized and read: This project has been created as part of the 42 curriculum by \<login1\>[, \<login2\>…]"* | Comparar la línea 1 con el formato exacto (expresión regular) | `*This project has been created as part of the 42 curriculum by pepaloma, pmendez-.*` coincide con el formato | ✅ |
| *"A 'Description' section that clearly presents the project"* | Leer `README.md` | Existe y describe el proyecto | ✅ |
| *"An 'Instructions' section containing any relevant information about compilation, installation, and/or execution."* | Leer `README.md` | Compilación (`make`…), ejecución (`./webserv [config]`, ruta por defecto), URLs de demostración y testers | ✅ |
| *"A 'Resources' section listing classic references… as well as a description of how AI was used"* | Leer `README.md` | Referencias (RFC 9110/9112/3875, Beej, man pages, NGINX…) y sección **How AI was used** con tareas y partes | ✅ |
| *"Your README must be written in English."* | Leer `README.md` | En inglés, con Features, Technical Choices y Configuration File Structure | ✅ |

---

## Resumen

| Capítulo | ✅ | ⚠️ | ❌ |
|---|---|---|---|
| II. General rules | 7 | 0 | 0 |
| IV. Mandatory part | 20 | 0 | 0 |
| IV.3 Configuration file | 16 | 0 | 0 |
| V. README | 5 | 0 | 0 |

**Comprobaciones finales (clon nuevo, `ee5d79f`):**

| Comprobación | Resultado |
|---|---|
| `make` / `make` otra vez | 0 avisos · `Nothing to be done` |
| Funciones no permitidas, `poll`/`select`, includes de C | Ninguno |
| `python3 tst/tester.py` | 52/52 |
| `python3 tst/tester.py --tester42` | 25/25 |
| `bash tst/check_single_poll.sh` | 1 kqueue con clientes y CGIs a la vez |
| `tst/full_test_suite.py --config etc/testing.conf` | 95/100 (el único fallo es una suposición errónea del propio tester: dos cabeceras válidas) |
| Demostración con `etc/default.conf` | 14/14 respuestas correctas |
| Estrés sin keep-alive | 100 % de disponibilidad |
| `leaks --atExit` | 0 fugas |
| Config inválida / duplicada / dos instancias | Error claro al arrancar; la primera instancia sigue funcionando |

**Pendiente:**
- Volver a pasar el **tester oficial de 42** en Linux x86-64 (`make mac-x86` o un ordenador de 42) después del arreglo del límite en chunked (`dc63469`).
- Revisar los logins del README si el equipo tiene más miembros.
