# Cambios en webserv

Este documento explica todo lo que se ha cambiado en dos bloques:

1. **Robustez del servidor**: ya está commiteado.
2. **CGI no bloqueante, logs y códigos de estado**: todavía sin commitear.

En cada cambio se explica **qué problema había**, **por qué importa según el subject**, **qué se ha hecho** y **dónde está**, para poder defenderlo en la evaluación.

---

## Índice

- [Parte 1 — Robustez del servidor (commiteado)](#parte-1--robustez-del-servidor-commiteado)
  - [1.1 Un solo `recv`/`send` por evento y sin `errno`](#11-un-solo-recvsend-por-evento-y-sin-errno)
  - [1.2 `fcntl` solo con `F_SETFL`](#12-fcntl-solo-con-f_setfl)
  - [1.3 SIGPIPE ignorado](#13-sigpipe-ignorado)
  - [1.4 Un cliente con problemas no tumba el servidor](#14-un-cliente-con-problemas-no-tumba-el-servidor)
  - [1.5 Timeouts: ninguna petición se queda colgada](#15-timeouts-ninguna-petición-se-queda-colgada)
  - [1.6 Límites antes de acumular datos (400 / 413)](#16-límites-antes-de-acumular-datos-400--413)
  - [1.7 Path traversal](#17-path-traversal)
  - [1.8 Keep-alive, pipelining y `Connection: close`](#18-keep-alive-pipelining-y-connection-close)
  - [1.9 Makefile](#19-makefile)
- [Parte 2 — CGI no bloqueante (sin commitear)](#parte-2--cgi-no-bloqueante-sin-commitear)
- [Parte 3 — Mensajes por pantalla (sin commitear)](#parte-3--mensajes-por-pantalla-sin-commitear)
- [Parte 4 — Códigos de estado estándar (sin commitear)](#parte-4--códigos-de-estado-estándar-sin-commitear)
- [Lista de ficheros tocados](#lista-de-ficheros-tocados)
- [Preguntas probables en la evaluación](#preguntas-probables-en-la-evaluación)
- [Lo que falta por hacer](#lo-que-falta-por-hacer)

---

## Parte 1 — Robustez del servidor (commiteado)

### 1.1 Un solo `recv`/`send` por evento y sin `errno`

**Problema.** `handleClientRead` hacía `read()` dentro de un `while (true)` hasta recibir `EAGAIN`, y miraba `errno` (`EINTR`, `EAGAIN`, `EWOULDBLOCK`) para decidir qué hacer. `handleClientWrite` también usaba `errno` después de `send()`.

**Por qué importa.** El subject dice:

> *Checking the value of errno to adjust the server behaviour is strictly forbidden after performing a read or write operation.*
>
> *You must never do a read or a write operation without going through poll() (or equivalent).*

Hacer varios `read()` seguidos significa que el segundo, el tercero, etc. se hacen **sin que kqueue/epoll haya avisado**. Las dos cosas pueden dar un **0**.

**Qué se ha hecho** ([src/switchMode.cpp](src/switchMode.cpp)):

- `handleClientRead` hace **un solo `recv()` por evento**:
  - `> 0`: los datos se añaden al buffer y se intenta procesar la petición.
  - `<= 0`: el cliente cerró (0) o hubo un error (-1). En los dos casos se cierra la conexión, **sin mirar `errno`**.
- `handleClientWrite` hace **un solo `send()` por evento**:
  - si se envía todo, se termina;
  - si solo se envía una parte, se borra lo enviado y el resto sale en el siguiente evento de escritura;
  - si devuelve `<= 0`, se cierra el cliente.
- La lógica de "¿tengo ya una petición completa?" se ha separado en una función nueva, **`processRequestBuffer`**, que se llama después de leer y después de terminar de escribir.

### 1.2 `fcntl` solo con `F_SETFL`

**Problema.** `setNonBlocking` hacía `fcntl(fd, F_GETFL, 0)` para leer los flags actuales y luego añadía `O_NONBLOCK`.

**Por qué importa.** El subject (apartado *For MacOS only*):

> *you are allowed to use fcntl() only with the following flags: F_SETFL, O_NONBLOCK and, FD_CLOEXEC. Any other flag is forbidden.*

`F_GETFL` no está en esa lista.

**Qué se ha hecho** ([src/Server.cpp](src/Server.cpp)):

```cpp
bool setNonBlocking(int fd) {
    return fcntl(fd, F_SETFL, O_NONBLOCK) != -1;
}
```

Además ahora devuelve `bool` en vez de lanzar una excepción (ver 1.4).

### 1.3 SIGPIPE ignorado

**Problema.** Si un cliente cierra la conexión mientras le estamos enviando la respuesta, el `send()` siguiente genera la señal **SIGPIPE**, cuya acción por defecto es **matar el proceso**.

**Por qué importa.** *"Your program must not crash under any circumstances"*. Basta con cerrar el navegador a mitad de una descarga para tumbar el servidor.

**Qué se ha hecho** ([app/main.cpp](app/main.cpp)):

```cpp
signal(SIGPIPE, SIG_IGN);
```

Con la señal ignorada, `send()` devuelve `-1` y simplemente cerramos ese cliente.

### 1.4 Un cliente con problemas no tumba el servidor

**Problema.** Varios fallos lanzaban excepciones dentro del bucle de eventos, y una excepción sin capturar sale de `run()` y termina el programa:

- `setNonBlocking` sobre un cliente nuevo lanzaba `runtime_error`;
- si `kevent`/`epoll_wait` devolvía `-1` por una señal (`EINTR`), se lanzaba excepción.

Además, si en el mismo lote de eventos llegaban dos eventos para un fd y el primero lo cerraba, el segundo usaba `clients_[fd]`, que **crea una sesión nueva** para un fd ya cerrado.

**Qué se ha hecho** ([src/Server.cpp](src/Server.cpp)):

- `handleNewConnection` ya no lanza excepciones. Si algo falla, cierra ese socket y sigue. Además, el cliente solo se guarda en `clients_` cuando ya está registrado en kqueue/epoll.
- `EINTR` en `kevent`/`epoll_wait` ya no es un error: se vuelve a esperar. Aquí sí se puede mirar `errno`, porque no viene de un read/write.
- Cada evento va dentro de un `try/catch`. Si algo falla (por ejemplo `std::bad_alloc`), **solo se cierra ese cliente**.
- Antes de atender un evento se comprueba que el fd sigue en `clients_`.
- Los handlers usan `clients_.find(fd)` en vez de `clients_[fd]`.
- `listen(fd, SOMAXCONN)` en vez de `listen(fd, 10)`, para aguantar mejor la prueba de estrés.

### 1.5 Timeouts: ninguna petición se queda colgada

**Problema.** `kevent`/`epoll_wait` esperaban sin límite (`NULL` / `-1`), y no había ningún control de tiempo. Un cliente que envía media petición y se calla se quedaba conectado para siempre.

**Por qué importa.** *"A request to your server should never hang indefinitely."*

**Qué se ha hecho:**

- `ClientSession` tiene un campo nuevo, **`last_activity`**: la hora del último `recv`/`send` ([inc/Server.hpp](inc/Server.hpp)).
- `kevent`/`epoll_wait` esperan **como máximo 1 segundo**, así el bucle se despierta aunque no haya eventos.
- Al final de cada vuelta, **`closeIdleClients`** cierra los clientes que llevan más de **`CLIENT_TIMEOUT` (60 s)** sin actividad.

### 1.6 Límites antes de acumular datos (400 / 413)

**Problema.** El tamaño del body solo se comprobaba **cuando ya había llegado entero**. Un cliente podía anunciar `Content-Length: 10000000000`, y el servidor iba guardando todo en memoria hasta agotarla. Lo mismo pasaba con unas cabeceras que nunca terminan (sin `\r\n\r\n`).

**Por qué importa.** *"must not crash under any circumstances (even if it runs out of memory)"*.

**Qué se ha hecho** ([src/RequestChecks.cpp](src/RequestChecks.cpp), función `earlyErrorCode`):

```
POST /upload HTTP/1.1          ┐
Host: localhost                │  MAX_HEADER_SIZE (16 KB, fijo en el código)
Content-Length: 5000           │
                               ┘
aaaaaaaaaa...                  ←  client_max_body_size (de la config)
```

- Si llegan **más de 16 KB sin que terminen las cabeceras**, se responde **400** y se cierra.
- En cuanto terminan las cabeceras, si el **`Content-Length` supera `client_max_body_size`**, se responde **413** sin esperar al body.
- Hacen falta los dos límites porque `client_max_body_size` solo se puede comprobar cuando se conoce el `Content-Length`, y eso solo ocurre cuando han terminado las cabeceras.
- `MAX_HEADER_SIZE` está definido en [inc/HttpRequest.hpp](inc/HttpRequest.hpp).

### 1.7 Path traversal

**Problema.** `GET /../../etc/passwd` (por ejemplo con `curl --path-as-is`) construía la ruta `www/../../etc/passwd` y servía ficheros de fuera del root.

**Qué se ha hecho** ([src/parseRequest.cpp](src/parseRequest.cpp)):

- La ruta se **decodifica** (`%20` → espacio, `%C3%B3` → `ó`…) en el parser. Así los ficheros con espacios o tildes, como los que se suben a `uploads/`, se pueden pedir sin problema.
- **Después** de decodificar, `isSafePath` rechaza con **400** las rutas que no empiezan por `/`, que contienen un byte nulo o que tienen un segmento `..`. Se hace después para que `/%2e%2e/` tampoco se cuele.
- `urlDecode` se ha movido desde [src/showUploadedFiles.cpp](src/showUploadedFiles.cpp) al parser, porque ahora la ruta llega ya decodificada a todos los handlers.

### 1.8 Keep-alive, pipelining y `Connection: close`

**Problema.**

- Si un cliente enviaba dos peticiones seguidas en el mismo paquete (*pipelining*), `request_buffer.clear()` **borraba la segunda**.
- Las respuestas de error incluían `Connection: close`, pero la conexión nunca se cerraba.
- En kqueue la lectura seguía activa mientras se enviaba una respuesta, así que una petición nueva podía **sobrescribir una respuesta a medio enviar**.

**Qué se ha hecho** ([src/switchMode.cpp](src/switchMode.cpp)):

- Del buffer solo se borran los bytes de **esa** petición (`erase(0, request_size)`). Lo que sobra es la siguiente y se procesa al terminar de enviar la respuesta.
- La conexión se cierra después de enviar si:
  - la respuesta lleva `Connection: close` (los errores, por ejemplo);
  - el cliente pidió `Connection: close`;
  - la petición es HTTP/1.0 sin `keep-alive`.

  Esto se guarda en un campo nuevo, `close_after_send`.
- **Mientras se envía una respuesta, la lectura está desactivada** (`EV_DISABLE` en kqueue, `EPOLLOUT` en epoll). Al terminar se vuelve a modo lectura. Ya no se usa `EV_ONESHOT`.

### 1.9 Makefile

- **El ejecutable se llama `./webserv`** y está en la raíz del repo. Antes era `bin/webserv`, y el subject dice *"Program name: webserv"* y *"./webserv [configuration file]"*.
- **Dependencias de headers** (`-MMD -MP` y `-include $(DEPS)`): si cambias un `.hpp`, se recompilan los `.cpp` que lo incluyen. Antes no ocurría, y se podían generar binarios inconsistentes, por ejemplo al hacer una modificación durante la evaluación.
- Sigue sin hacer relink: un segundo `make` dice `Nothing to be done`.
- `fclean` borra también `./webserv`, y `/webserv` está en el `.gitignore`.

---

## Parte 2 — CGI no bloqueante (sin commitear)

### El problema

`CgiHandler::execute()` hacía todo de forma **síncrona**:

1. `fork()` del script;
2. `write()` del body completo al pipe de stdin;
3. un **`poll()` propio** en bucle para leer la salida, con un máximo de 5 segundos.

Esto tenía tres fallos graves:

| Fallo | Consecuencia |
|---|---|
| Un **segundo `poll()`** | El subject exige *"only 1 poll() (or equivalent) for all the I/O operations"*. Puede dar un **0**. |
| `write()` del body **sin pasar por kqueue/epoll** | Igual que antes: lectura/escritura sin aviso previo. Además, si el body era mayor que el buffer del pipe (~64 KB), el `write()` se bloqueaba **para siempre**. |
| Bucle de espera de hasta 5 s | Mientras se ejecutaba un script, **todo el servidor se quedaba parado**: ningún otro cliente recibía respuesta. |

Además, `www/cgi/example.py` **estaba vacío**: el CGI nunca se había probado de verdad.

### El diseño nuevo

```
cliente ──petición──▶ processRequestBuffer ──▶ RequestHandler: "esto es CGI"
                                                 │
                    CgiHandler::start(): fork + pipes no bloqueantes
                    (no lee ni escribe nada)
                                                 │
  ┌──── el MISMO kqueue/epoll que los sockets ───┤
  │ stdin del script  (escritura) → un write() del body por evento
  │                                 → al acabar, close() = EOF para el script
  │ stdout del script (lectura)   → un read() por evento
  │                                 → read() == 0 (EOF): el script ha terminado
  └──────────────────────────────────────────────┘
                                                 │
                    finishCgi(): salida → respuesta HTTP → cliente en modo escritura

Timeout (CGI_TIMEOUT = 10 s)        → kill del script + respuesta 504
El cliente se desconecta antes      → kill del script + cierre de los pipes
```

### Qué hace cada pieza

**[inc/CgiHandler.hpp](inc/CgiHandler.hpp) — `struct CgiProcess`.** Es el estado de un script en marcha y se guarda en la `ClientSession` del cliente que lo pidió:

| Campo | Para qué |
|---|---|
| `pid` | Proceso hijo (`-1` si no hay CGI) |
| `stdin_fd` / `stdout_fd` | Nuestros extremos de los pipes (`-1` cuando están cerrados) |
| `body`, `body_sent` | Body pendiente de enviar al script y cuánto se ha enviado ya |
| `output` | Todo lo que el script ha impreso hasta ahora |
| `start_time` | Para el timeout |

**[src/CgiHandler.cpp](src/CgiHandler.cpp) — `CgiHandler::start()`** (sustituye a `execute()`):

- Comprueba que el script existe (si no, **404**) y que se puede leer (si no, **403**).
- Crea los dos pipes y pone **nuestros extremos** en `O_NONBLOCK`. Los del hijo se quedan bloqueantes, que es lo que espera un script normal.
- `fork()`. En el hijo:
  - `dup2` de los pipes a stdin/stdout;
  - **cierra todos los demás fds heredados** (sockets de escucha y de otros clientes). Si no, el script mantendría vivas conexiones que no son suyas y nunca veríamos el EOF de su stdout;
  - `chdir()` al directorio del script, para que funcionen las rutas relativas (lo pide el subject);
  - `execve()` con `python3` si el fichero es `.py`; si no, se ejecuta directamente.
- **No lee ni escribe nada**: solo rellena el `CgiProcess` y devuelve `0` (o el código de error).
- Se ha quitado `realpath()`, porque **no está en la lista de funciones permitidas**.
- El stderr del script ya no se mezcla con la respuesta HTTP: sale por la terminal del servidor.

**[src/cgiEvents.cpp](src/cgiEvents.cpp)** (nuevo) — lo que hace el Server con los pipes:

| Función | Qué hace |
|---|---|
| `startCgi` | Llama a `CgiHandler::start` y registra los pipes en kqueue/epoll. Si el body está vacío, cierra el stdin enseguida (EOF). |
| `handleCgiEvent` | **Un `write()` o un `read()` por evento.** En stdin envía el siguiente trozo del body y, cuando ha terminado, cierra el pipe. En stdout añade lo leído a `output`; cuando `read()` devuelve 0, construye la respuesta (si no hubo salida, **502**). |
| `finishCgi` | Libera el CGI y deja la respuesta lista para enviar al cliente. |
| `cleanupCgi` | Cierra los pipes y, si hace falta, hace `kill(SIGKILL)` al script. |
| `reapChildren` | `waitpid(pid, NULL, WNOHANG)`: recoge a los hijos terminados **sin bloquear nunca**, para que no queden procesos zombie. |
| `watchFd` / `closeCgiPipe` | Registrar o quitar un fd del multiplexor. |

**[src/Server.cpp](src/Server.cpp)**:

- Nuevo mapa `cgi_pipes_`: **fd del pipe → fd del cliente** al que pertenece.
- El bucle de eventos manda los eventos de los pipes a `handleCgiEvent`. En los pipes no se miran los flags de EOF/HUP: lo que indica que el script ha terminado es que `read()` devuelva 0.
- `handleClientDisconnect` mata el CGI del cliente que se va.
- `closeIdleClients` mata los CGI que superan **`CGI_TIMEOUT` (10 s)** y responde **504**.

**[src/RequestHandler.cpp](src/RequestHandler.cpp)**:

- `handleCgiRequest` **ya no ejecuta el script**: solo apunta cuál es (`_cgi_script`). Después de `handle()`, el Server pregunta con `wantsCgi()` / `getCgiScript()` y lo lanza él.
- Nuevo `buildError(code, req_id)` público, que da la página de error de un código. Lo usa el Server para los 404/403/500/502/504 del CGI.

**[src/switchMode.cpp](src/switchMode.cpp)**:

- Si la petición es CGI, se lanza el script y **todavía no se responde**: la respuesta la construirá `finishCgi` cuando el script termine.
- Mientras un CGI está en marcha, la siguiente petición del mismo cliente espera en el buffer.

**[www/cgi/example.py](www/cgi/example.py)**: un CGI de ejemplo que muestra el método, la query string, el body recibido y el directorio de trabajo.

### Pruebas realizadas

| Prueba | Resultado |
|---|---|
| `GET /cgi/example.py?x=1&y=2` | 200; el script recibe el método y la query, y se ejecuta dentro de `www/cgi` |
| POST de 5 MB a un script | Le llegan los 5 000 000 bytes (antes se bloqueaba) |
| Script en bucle infinito | 504 a los ~11 s, y un `GET /` lanzado mientras tanto responde en 0,0006 s |
| El cliente se va con el script en marcha | El script muere y no quedan zombies |
| Script que falla sin generar salida | 502 |
| Script que no existe | 404 |
| CGI + GET en la misma conexión | 200 200 (keep-alive) |

> ⚠️ La rama de **epoll** (Linux) no se ha podido compilar en el Mac. Se ha revisado a mano y es simétrica a la de kqueue, pero **hay que compilarla en un ordenador de 42 antes de la evaluación**.

---

## Parte 3 — Mensajes por pantalla (sin commitear)

### Antes

```
Inicializando sockets...
Listening on socket 3 (Port: 8080)
Accepted new connection: 5 (from listen socket 3)
Sent response to socket 5
Client disconnected: 7
Client 5 requested: www/showUploads
```

No decía **qué** se pidió ni **qué** se respondió, mezclaba castellano e inglés y el color gris se quedaba pegado a las líneas siguientes.

### Ahora

```
  webserv  ·  etc/default.conf  ·  2 servers
  ────────────────────────────────────────────────────────────
  ● 0.0.0.0:8080   root www/   max body 976.6 KB
    error pages 400 404 500
    /               GET POST            autoindex
    /uploads        GET DELETE          autoindex
    /old            GET                 autoindex  redirect 301 -> /
  ────────────────────────────────────────────────────────────
  ▸ Listening on http://localhost:8080  (0.0.0.0:8080, fd 3)

  Ready  ·  press Ctrl+C to stop

01:50:55  :8080 #6   GET     /                            200  15.5 KB
01:50:55  :8080 #6   GET     /nope.html                   404  4.1 KB   no file at www/nope.html
01:50:55  :8080 #6   GET     /cgi/example.py              200  188 B    CGI pid 9133
01:50:55  :8080 #6   GET     /old                         301  0 B      -> /
01:50:55  :8080 #6   DELETE  /index.html                  405  4.5 KB   DELETE not allowed in location /
01:50:55  :8080 #6   ???     (rejected before completion) 413  4.5 KB   Content-Length > client_max_body_size (976.6 KB)
01:51:06  :8080 #6   GET     /cgi/zz_loop.py              504  4.3 KB   CGI killed after 10s

  Shutting down  ·  10 requests served  ·  1 connections closed  ·  1 CGI killed
```

En la terminal el código de estado sale en color: verde (2xx), cian (3xx), amarillo (4xx) o rojo (5xx).

### Qué se ha hecho

**1. Resumen de la configuración al arrancar** ([src/Configuration.cpp](src/Configuration.cpp), `Configuration::print`):

- Para cada servidor muestra la IP:puerto, el root, el tamaño máximo del body y las páginas de error. Para cada location, sus métodos y opciones (autoindex, index, alias, upload, redirect).
- Lo usa también el tester (`bin/test`).
- Antes usaba `inet_ntoa`, que **no está en las funciones permitidas**; ahora usa `logfmt::ipToString`.

**2. Una línea por petición** ([src/switchMode.cpp](src/switchMode.cpp), `Server::logResponse`):

- Muestra la hora, el puerto, el fd del cliente, el método, la ruta, el código, el tamaño de la respuesta y el **motivo**.
- Se imprime desde un solo sitio. Lo llaman las respuestas normales y `finishCgi` (las respuestas CGI salen con su código real cuando el script termina).

**3. El motivo de cada respuesta** (la "nota"):

- `HttpResponse` tiene un campo nuevo, **`_note`**, que **no se envía al cliente**: solo sale en el log.
- `HttpRequest` tiene **`_errorNote`** y una sobrecarga **`setErrorCode(code, note)`**. Cada error del parser explica qué ha fallado: `missing Host header`, `malformed request line`, `URI longer than 2048 characters`, `unsupported version HTTP/2.0`, `duplicate host header`, `both Content-Length and Transfer-Encoding`, `unsafe path`…
- `RequestHandler::withNote(respuesta, nota)` añade la nota sin ensuciar cada `return`.
- Otras notas: métodos no permitidos, fichero no encontrado (con la ruta), redirect (destino), DELETE, upload, CGI (pid, 404/403/502/504).

**4. Cierre limpio con Ctrl+C** ([app/main.cpp](app/main.cpp), [src/Server.cpp](src/Server.cpp)):

- `signal(SIGINT, ...)` y `signal(SIGTERM, ...)` solo ponen a 1 la variable global `g_shutdown_requested`, que es lo único seguro que puede hacer un handler de señal.
- El bucle es ahora `while (!g_shutdown_requested)`. La señal interrumpe `kevent`/`epoll_wait` (`EINTR`), el bucle comprueba la variable y sale.
- **`Server::shutdown`** mata los CGI en marcha, cierra todos los clientes y el multiplexor, e imprime el resumen. Los sockets de escucha los cierra el destructor.

**5. [inc/Log.hpp](inc/Log.hpp) / [src/Log.cpp](src/Log.cpp)** (nuevos). **No es una clase**: contiene los colores ANSI (antes estaban en `HttpRequest.hpp`) y un `namespace logfmt` con 4 funciones de formato que se usan en tres ficheros distintos:

| Función | Ejemplo |
|---|---|
| `humanSize(15880)` | `"15.5 KB"` |
| `clock()` | `"01:50:55"` |
| `ipToString(ip)` | `"127.0.0.1"` (sustituye a `inet_ntoa`) |
| `pad("GET", 7)` | `"GET    "` (alinea las columnas) |

**6. Mensajes comentados, no borrados.** Se han comentado `Accepted new connection`, `Client disconnected`, `Sent response`, `Send failed`, `CGI started`, `CGI script not found/readable`, `File deleted`, `Error deleting`, `Client X requested`, los dos `[ERROR]` de `HttpRequest.cpp` y los 12 mensajes de `handleUpload.cpp`. Toda esa información aparece ahora en la línea de la petición.

---

## Parte 4 — Códigos de estado estándar (sin commitear)

### El problema

La línea de estado HTTP llevaba el mensaje de chiste como *reason phrase*:

```
HTTP/1.1 405 That verb isn't on the guest list for this endpoint. Strictly VIP access only.
```

Además, en la página de error el mismo texto salía **dos veces**, como título y como mensaje. Los redirects respondían `301 Redirect` y el 409 reutilizaba el mensaje del 400.

### Qué se ha hecho

- Nueva función **`HttpResponse::reasonPhrase(code)`** ([src/HttpResponse.cpp](src/HttpResponse.cpp)) con los reason phrases estándar: `404 Not Found`, `405 Method Not Allowed`, `413 Content Too Large`, `504 Gateway Timeout`…
- **`handleError`** ([src/RequestHandler.cpp](src/RequestHandler.cpp)) usa:
  - el reason phrase estándar en la **línea de estado** y en el **título** de la página (`{{title}}`);
  - el chiste **solo** en el texto de la página (`{{message}}`).
- Los redirects envían `301 Moved Permanently`, `302 Found`, etc.
- Nuevo mensaje `ERROR_409` para el upload de un fichero que ya existe.
- El 404 de los ficheros estáticos usa su chiste (`ERROR_404`); antes era un simple "Not Found".

### Resultado

```
HTTP/1.1 405 Method Not Allowed
...
<div class="title">Method Not Allowed</div>
<p class="message">That verb isn't on the guest list for this endpoint. Strictly VIP access only.</p>
```

---

## Lista de ficheros tocados

### Parte 1 (commiteado)

| Fichero | Cambio |
|---|---|
| `Makefile` | `NAME = webserv`, dependencias de headers, `fclean` |
| `.gitignore` | `/webserv` |
| `app/main.cpp` | `signal(SIGPIPE, SIG_IGN)` |
| `inc/Server.hpp` | `last_activity`, `close_after_send`, `CLIENT_TIMEOUT`, `processRequestBuffer`, `closeIdleClients` |
| `src/Server.cpp` | `fcntl`, accept sin excepciones, `try/catch`, `EINTR`, timeouts, `SOMAXCONN` |
| `src/switchMode.cpp` | un recv/send por evento, `processRequestBuffer`, pipelining, keep-alive |
| `inc/HttpRequest.hpp`, `src/RequestChecks.cpp` | `MAX_HEADER_SIZE`, `earlyErrorCode` |
| `src/parseRequest.cpp`, `src/showUploadedFiles.cpp` | `urlDecode` en el parser, `isSafePath` |

### Partes 2, 3 y 4 (sin commitear)

| Fichero | Cambio |
|---|---|
| `inc/CgiHandler.hpp`, `src/CgiHandler.cpp` | `CgiProcess`, `start()` no bloqueante, sin `realpath` |
| `src/cgiEvents.cpp` | **nuevo**: el CGI dentro del bucle de eventos |
| `inc/Server.hpp`, `src/Server.cpp` | `cgi_pipes_`, timeout de CGI, `shutdown`, resumen de arranque |
| `src/switchMode.cpp` | lanzar el CGI, `logResponse` |
| `inc/RequestHandler.hpp`, `src/RequestHandler.cpp` | `wantsCgi`, `buildError`, `withNote`, reason phrases |
| `inc/HttpResponse.hpp`, `src/HttpResponse.cpp` | `_note`, `reasonPhrase`, `ERROR_409/502/504` |
| `inc/HttpRequest.hpp`, `src/HttpRequest.cpp`, `src/parseRequest.cpp` | `_errorNote`, `setErrorCode(code, note)` |
| `inc/Configuration.hpp`, `src/Configuration.cpp` | `print()` con colores, sin `inet_ntoa` |
| `inc/Log.hpp`, `src/Log.cpp` | **nuevos**: colores y funciones de formato |
| `app/main.cpp` | Ctrl+C, banner |
| `src/handleUpload.cpp`, `src/showUploadedFiles.cpp` | mensajes comentados, notas, `ERROR_409` |
| `www/cgi/example.py` | CGI de ejemplo funcional |
| `etc/basic.conf` | cambio vuestro (root a `www`), no lo he tocado |

### Commits sugeridos para lo pendiente

```
feat(cgi): run CGI scripts through the main kqueue/epoll loop
docs(cgi): add working example.py script
feat(log): coloured startup summary, per-request log line and graceful Ctrl+C shutdown
fix(response): send standard reason phrases; keep funny text only in the error page
```

Acuérdate de hacer `git add` de los ficheros nuevos: `src/cgiEvents.cpp`, `inc/Log.hpp` y `src/Log.cpp`.

---

## Preguntas probables en la evaluación

**¿Dónde está tu único `poll()`?**
En `Server::run()`: `kevent()` en macOS o `epoll_wait()` en Linux. Por ahí pasan los sockets de escucha, los clientes y los pipes del CGI. `poll()` no aparece en ninguna parte del código.

**¿Lees o escribes sin que el multiplexor avise?**
No. `handleClientRead`, `handleClientWrite` y `handleCgiEvent` hacen **un solo** `recv`/`send`/`read`/`write` por evento. Los ficheros regulares (los estáticos y los uploads) no pasan por el multiplexor, y el subject lo permite.

**¿Miras `errno` después de leer o escribir?**
No. Si `recv`/`send` devuelve `<= 0`, se cierra el cliente sin mirar `errno`. Solo se mira después de `kevent`/`epoll_wait` (para `EINTR`) y en errores de arranque, que no son operaciones de lectura o escritura.

**¿Qué pasa si un script CGI no termina nunca?**
`closeIdleClients` lo detecta a los `CGI_TIMEOUT` segundos (10), le hace `kill(SIGKILL)` y responde **504**. Mientras tanto, el resto de clientes siguen siendo atendidos.

**¿Y si el cliente se va mientras el CGI está en marcha?**
`handleClientDisconnect` llama a `cleanupCgi(..., true)`: mata el script, cierra los pipes y lo recoge con `waitpid(WNOHANG)`.

**¿Cómo evitas los zombies?**
Con `waitpid(pid, NULL, WNOHANG)` en `reapChildren`. Si el hijo todavía no ha terminado, se guarda en `pending_children_` y se vuelve a intentar en cada vuelta del bucle, sin bloquear nunca.

**¿Por qué cierras todos los fds en el hijo del CGI?**
Porque el hijo hereda los sockets de escucha y los de otros clientes. Si se quedara con una copia del extremo de escritura de su propio pipe de stdout, nunca veríamos el EOF y la respuesta no terminaría.

**¿Qué pasa si te mandan un body enorme?**
En cuanto llegan las cabeceras se compara el `Content-Length` con `client_max_body_size`, y si lo supera se responde 413 **sin leer el body**. Si las cabeceras pasan de 16 KB, se responde 400.

**¿Qué pasa si un cliente cierra a mitad de una respuesta?**
SIGPIPE está ignorado, así que `send()` devuelve -1 y se cierra ese cliente. El servidor sigue funcionando.

**¿Por qué fcntl solo con `F_SETFL`?**
Porque el subject solo permite `F_SETFL`, `O_NONBLOCK` y `FD_CLOEXEC`.

---

## Lo que falta por hacer

1. **Quitar `www` del `.gitignore`.** Ahora mismo, en el repo solo están `www/cgi/example.py` y `www/img/li.jpg`. Si el evaluador clona el repo, no tendrá `index.html` ni la plantilla de errores.
2. **Routing a partir de la configuración.** Ahora mismo `/upload`, `/uploads/`, `/cgi/` y `/showUploads` están fijados en el código. Falta:
   - `index` (fichero por defecto de un directorio);
   - `autoindex` (listado de directorios, que no existe todavía; además ahora está **activado por defecto** y debería estar desactivado);
   - `upload_enabled` / `upload_path`;
   - CGI por extensión (el `"www" + path` fijo);
   - DELETE en cualquier ruta permitida.
3. **`error_page`**: se lee de la config pero no se usa. Además, `default.conf` apunta a `400.html` y `500.html`, que no existen.
4. **`Transfer-Encoding: chunked`**: hay que des-chunkear el body, cosa que el subject pide para el CGI.
5. **Ampliar `default.conf`** para demostrar todo: varios puertos, DELETE, redirect, `error_page`, un 413 fácil de provocar.
6. **Correcciones menores**:
   - un fichero vacío o un directorio no deberían dar 404;
   - un DELETE de algo inexistente debería dar 404, no 500;
   - métodos desconocidos → 501;
   - el parser de la config lee fuera de rango con ficheros mal formados y usa `inet_addr`, que no está permitida;
   - `getMimeType` (`.js` coincide con `.json`; faltan `.svg`, `.ico`, `.avif`);
   - la página de error deja el recuadro de la imagen vacío en los códigos que no son 404.
7. **Compilar en Linux** (rama de epoll) antes de la evaluación.
