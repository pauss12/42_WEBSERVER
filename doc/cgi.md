# Cómo funciona el CGI en webserv

Este documento explica, paso a paso, qué pasa desde que llega una petición a un script CGI hasta que el cliente recibe la respuesta. Cada paso enlaza al código que lo hace.

---

## Índice

1. [Qué es un CGI](#1-qué-es-un-cgi)
2. [La regla del subject que lo condiciona todo](#2-la-regla-del-subject-que-lo-condiciona-todo)
3. [Visión general](#3-visión-general)
4. [Paso a paso](#4-paso-a-paso)
5. [Variables de entorno](#5-variables-de-entorno)
6. [Formato de la salida del script](#6-formato-de-la-salida-del-script)
7. [Errores y códigos de respuesta](#7-errores-y-códigos-de-respuesta)
8. [Casos especiales](#8-casos-especiales)
9. [Cómo escribir un script CGI](#9-cómo-escribir-un-script-cgi)
10. [Cómo probarlo](#10-cómo-probarlo)
11. [Limitaciones actuales](#11-limitaciones-actuales)
12. [Preguntas de evaluación](#12-preguntas-de-evaluación)

---

## 1. Qué es un CGI

**CGI** (*Common Gateway Interface*, RFC 3875) es la forma más antigua de generar páginas dinámicas. En vez de leer un fichero y enviarlo, el servidor **ejecuta un programa** y le envía al cliente lo que ese programa escribe.

El servidor y el script se comunican con tres mecanismos estándar de Unix:

| Canal | Dirección | Qué viaja |
|---|---|---|
| **Variables de entorno** | servidor → script | Los datos de la petición: método, query string, cabeceras, tamaño del body… |
| **stdin** del script | servidor → script | El **body** de la petición (por ejemplo, los datos de un formulario POST) |
| **stdout** del script | script → servidor | La **respuesta**: cabeceras CGI, una línea vacía y el body |

El script puede estar escrito en cualquier lenguaje (Python, PHP, bash, C…), porque solo usa el entorno, stdin y stdout.

---

## 2. La regla del subject que lo condiciona todo

> *It must be non-blocking and use only 1 poll() (or equivalent) for all the I/O operations between the clients and the server.*
>
> *I/O that can wait for data (sockets, **pipes**/FIFOs, etc.) must be non-blocking and driven by a single poll() (or equivalent). Calling read/recv or write/send on these descriptors without prior readiness will result in a grade of 0.*

Para comunicarnos con el script usamos **pipes**, así que:

- los pipes tienen que estar en **modo no bloqueante**;
- tienen que registrarse en **el mismo** kqueue/epoll que los sockets, no en un `poll()` aparte;
- solo podemos hacer `read()`/`write()` sobre ellos **cuando kqueue/epoll dice que están listos**, y **uno por evento**.

Además, mientras el script se ejecuta, **el servidor tiene que seguir atendiendo a los demás clientes**. Por eso no se puede esperar a que el script termine: lanzamos el script, volvemos al bucle de eventos y vamos atendiendo sus pipes cuando avisan, igual que hacemos con un cliente.

---

## 3. Visión general

```
                         ┌──────────────────────────────────────────┐
  Cliente                │                webserv                   │            Script
                         │                                          │
  POST /cgi/x.py ───────▶│ processRequestBuffer                     │
  (socket)               │   └─ RequestHandler: "esto es CGI"       │
                         │   └─ startCgi ─ CgiHandler::start ──────────fork()──▶ execve(python3 x.py)
                         │                                          │
                         │      kqueue / epoll (UNO SOLO)           │
                         │   ┌──────────────────────────────────┐   │
                         │   │ socket del cliente               │   │
                         │   │ stdin_fd  (escritura) ───────────┼───┼──body──▶ stdin
                         │   │ stdout_fd (lectura)   ◀──────────┼───┼─salida─── stdout
                         │   └──────────────────────────────────┘   │
                         │                                          │
                         │ read() == 0 (EOF) → finishCgi            │
  ◀──── HTTP/1.1 200 ────│   └─ parseOutput → respuesta HTTP        │
                         └──────────────────────────────────────────┘
```

**Quién hace qué:**

| Pieza | Fichero | Responsabilidad |
|---|---|---|
| `RequestHandler` | [src/RequestHandler.cpp](../src/RequestHandler.cpp) | Decide que la petición es CGI y qué script hay que ejecutar. **No ejecuta nada.** |
| `CgiHandler` | [src/CgiHandler.cpp](../src/CgiHandler.cpp) | Prepara el entorno, crea los pipes, hace `fork` + `execve`, y convierte la salida del script en un `HttpResponse`. **Nunca lee ni escribe en los pipes.** |
| `Server` (CGI) | [src/cgiEvents.cpp](../src/cgiEvents.cpp) | Registra los pipes en kqueue/epoll, hace los `read`/`write` cuando avisan y libera todo al final. |
| `CgiProcess` | [inc/CgiHandler.hpp](../inc/CgiHandler.hpp) | El estado de un script en marcha. Vive dentro de la `ClientSession` del cliente. |

---

## 4. Paso a paso

### Paso 1 — Llega la petición y se detecta que es CGI

[switchMode.cpp: `processRequestBuffer`](../src/switchMode.cpp) parsea la petición y llama a `RequestHandler::handle()`.

**Qué es CGI lo decide la configuración, por extensión** (el subject: *"Execution of CGI, based on file extension"*). En la location se indica qué extensiones son scripts y con qué intérprete se ejecutan:

```nginx
location /cgi {
    root www/cgi;
    cgi .py /usr/bin/python3;     # cgi <extensión> <intérprete>
    cgi .sh /bin/sh;              # se pueden poner varias (varios tipos de CGI = bonus)
    allowed_methods GET POST;
}
```

En [`RequestHandler::handle`](../src/RequestHandler.cpp), antes de llegar al CGI, se hacen las comprobaciones comunes:

1. errores de parseo (400, 413, 414, 505…);
2. método no implementado (501);
3. ninguna location coincide (404);
4. método permitido en la location (si no, 405);
5. redirect de la location.

Después se calcula la ruta real en disco (`root` de la location + resto de la URL: `/cgi/example.py` → `www/cgi/example.py`) y, si su extensión está en la directiva `cgi` de la location, [`handle`](../src/RequestHandler.cpp#L94) **solo apunta el script y su intérprete**:

```cpp
std::map<std::string, std::string>::const_iterator cgi = location->getCgi().find(extensionOf(fs_path));
if (cgi != location->getCgi().end()) {
    _cgi_script = fs_path;            // "www/cgi/example.py"
    _cgi_interpreter = cgi->second;   // "/usr/bin/python3"
    return HttpResponse();            // respuesta vacía: no se usa
}
```

`handle()` no puede ejecutar el script porque tiene que devolver una respuesta **ya**, y la respuesta del CGI tardará en llegar. Así que solo deja apuntado qué script hay que lanzar, y el Server lo consulta justo después:

```cpp
response = handler.handle(request);
if (handler.wantsCgi()) {
    int error_code = startCgi(client_fd, multiplexer_fd, request,
                              handler.getCgiScript(), handler.getCgiInterpreter());
    if (error_code == 0) {
        session.close_after_send = close_connection;
        return;             // ← NO respondemos todavía
    }
    response = handler.buildError(error_code, ...);   // 404 / 403 / 500
}
```

### Paso 2 — `CgiHandler::start`: fork y pipes

[`CgiHandler::start`](../src/CgiHandler.cpp#L216) hace lo siguiente, en orden:

**2.1 Comprobar el script.**
```cpp
if (access(script_path.c_str(), F_OK) != 0) return 404;                       // no existe
if (access(script_path.c_str(), R_OK) != 0) return 403;                       // no se puede leer
if (interpreter.empty() && access(script_path.c_str(), X_OK) != 0) return 403; // sin intérprete: debe ser ejecutable
if (!interpreter.empty() && access(interpreter.c_str(), X_OK) != 0) return 500; // el intérprete de la config no existe
```

**2.2 Crear dos pipes.** Un pipe tiene un extremo de lectura (`[0]`) y otro de escritura (`[1]`):

```
stdin_pipe:   webserv escribe en [1]  ──────▶  [0] el script lee (su stdin)
stdout_pipe:  webserv lee de   [0]  ◀──────  [1] el script escribe (su stdout)
```

**2.3 Poner en no bloqueante solo NUESTROS extremos.**
```cpp
fcntl(stdin_pipe[1],  F_SETFL, O_NONBLOCK);
fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
```
Los extremos del script se quedan **bloqueantes**, porque un script normal espera que `sys.stdin.read()` se bloquee hasta tener datos. `O_NONBLOCK` es una propiedad de cada extremo del pipe, así que cambiar el nuestro no afecta al del script.

**2.4 Preparar el entorno** con `setupEnvironment` (ver [sección 5](#5-variables-de-entorno)) y convertirlo a `char**` con `createEnvArray`, que es lo que espera `execve`.

**2.5 `std::cout.flush()` antes del `fork`.** Si quedara texto en el buffer de `std::cout`, el hijo heredaría una copia y lo imprimiría también.

**2.6 `fork()`.** Ahora hay dos procesos ejecutando el mismo código. `fork()` devuelve `0` en el hijo y el pid del hijo en el padre.

**2.7 En el HIJO:**

```cpp
dup2(stdin_pipe[0], STDIN_FILENO);     // su stdin  = extremo de lectura del pipe 1
dup2(stdout_pipe[1], STDOUT_FILENO);   // su stdout = extremo de escritura del pipe 2

for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd)
    close(fd);                         // cierra TODO lo demás
```

El hijo hereda **todos** los fds del servidor: los sockets de escucha, los de otros clientes y los cuatro extremos de los pipes. Cerrarlos es importante por dos motivos:

- **EOF**: un pipe solo da EOF al lector cuando **todos** los extremos de escritura están cerrados. Si el hijo conservara una copia de `stdout_pipe[1]` además de su stdout, o una copia de `stdin_pipe[1]`, nunca veríamos el final.
- **Conexiones ajenas**: si el hijo tuviera abierto el socket de otro cliente, ese socket no se cerraría de verdad cuando el servidor lo cerrase.

Después:

```cpp
chdir(dir);          // "www/cgi": el subject pide ejecutarlo en su directorio
execve(interpreter, {interpreter, "example.py", NULL}, envp);   // con intérprete (de la config)
execve("./script", {"./script", NULL}, envp);                    // sin intérprete
std::exit(1);        // solo se llega aquí si execve falla
```

Si `execve` funciona, el proceso **se convierte** en el script y el código de webserv desaparece de ese proceso. Si falla, el hijo sale sin escribir nada, y el padre lo detectará como "sin salida" (**502**).

**2.8 En el PADRE:**

```cpp
close(stdin_pipe[0]);    // el extremo de lectura de stdin es del hijo
close(stdout_pipe[1]);   // el extremo de escritura de stdout es del hijo
freeEnvArray(envp);

cgi.pid       = pid;
cgi.stdin_fd  = stdin_pipe[1];    // aquí escribiremos el body
cgi.stdout_fd = stdout_pipe[0];   // de aquí leeremos la salida
cgi.last_io   = time(NULL);
return 0;
```

Justo después, [`startCgi`](../src/cgiEvents.cpp) pasa el body de la petición a `cgi.body` con `request.takeBody(cgi.body)`, un `swap` **sin copiar** (puede ser de 100 MB). Cuando se ha enviado entero al script, se libera esa memoria.

`start()` vuelve enseguida: **no ha leído ni escrito nada en los pipes**.

### Paso 3 — `startCgi`: registrar los pipes en kqueue/epoll

[`Server::startCgi`](../src/cgiEvents.cpp#L55):

```cpp
cgi_pipes_[cgi.stdout_fd] = client_fd;          // "este pipe es de este cliente"
watchFd(cgi.stdout_fd, multiplexer_fd, false);  // avísame cuando haya algo que leer

if (cgi.body.empty())
    closeCgiPipe(cgi.stdin_fd, ...);            // GET: no hay body → EOF inmediato
else {
    cgi_pipes_[cgi.stdin_fd] = client_fd;
    watchFd(cgi.stdin_fd, multiplexer_fd, true);  // avísame cuando pueda escribir
}
```

El mapa **`cgi_pipes_`** (fd del pipe → fd del cliente) sirve para que, cuando kqueue/epoll avise de un fd, sepamos que es un pipe de CGI y a qué cliente pertenece.

[`watchFd`](../src/cgiEvents.cpp#L23) es `EV_SET(... EVFILT_READ/EVFILT_WRITE, EV_ADD ...)` en kqueue o `epoll_ctl(EPOLL_CTL_ADD, EPOLLIN/EPOLLOUT)` en epoll.

Después `processRequestBuffer` hace `return` y el servidor **vuelve al bucle de eventos** a atender a otros clientes.

### Paso 4 — El bucle de eventos reparte los avisos

En [`Server::run`](../src/Server.cpp#L314) se añade un caso al reparto de eventos:

```cpp
if (listen_sockets_.find(fd) != end)        handleNewConnection(...);
else if (cgi_pipes_.find(fd) != end)        handleCgiEvent(fd, ...);   // ← pipe de CGI
else if (clients_.find(fd) == end)          continue;                  // ya cerrado
else if (EOF/HUP)                           handleClientDisconnect(...);
else if (lectura)                           handleClientRead(...);
else if (escritura)                         handleClientWrite(...);
```

En los pipes **no se miran los flags de EOF/HUP**. Cuando el script termina, kqueue/epoll avisan con un evento de lectura, y lo que de verdad indica el final es que `read()` devuelva `0`. Si cerrásemos al ver el flag, podríamos perder los últimos datos que el script dejó en el pipe.

### Paso 5 — `handleCgiEvent`: un write o un read por evento

[`Server::handleCgiEvent`](../src/cgiEvents.cpp#L90):

**Si el pipe que avisa es el stdin del script**, se envía el siguiente trozo del body:

```cpp
ssize_t written = write(pipe_fd, cgi.body.data() + cgi.body_sent,
                        cgi.body.size() - cgi.body_sent);
if (written <= 0) {                    // el script dejó de leer (o murió)
    closeCgiPipe(cgi.stdin_fd, ...);   // dejamos de enviar, pero seguimos leyendo su salida
    return;
}
cgi.body_sent += written;
if (cgi.body_sent >= cgi.body.size())
    closeCgiPipe(cgi.stdin_fd, ...);   // todo enviado → close() = EOF para el script
```

Un pipe tiene un buffer de unos 64 KB. Con un body de 2,5 MB, `write()` acepta solo lo que cabe, y kqueue/epoll vuelve a avisar cuando el script ha leído y hay sitio. Por eso un POST grande se envía en muchos eventos sin bloquear nunca.

**Cerrar el stdin es imprescindible.** Un script que hace `sys.stdin.read()` lee hasta EOF, y EOF solo llega cuando cerramos nuestro extremo de escritura.

**Si el pipe que avisa es el stdout del script**, se lee lo que haya:

```cpp
ssize_t count = read(pipe_fd, buffer, sizeof(buffer));
if (count > 0) {
    cgi.output.append(buffer, count);   // acumular y esperar al siguiente aviso
    return;
}
// count == 0: EOF → el script ha cerrado su stdout (normalmente porque ha terminado)
if (cgi.output.empty())
    finishCgi(client_fd, ..., error 502);                   // no escribió nada
else
    finishCgi(client_fd, ..., CgiHandler::parseOutput(cgi.output));
```

**Fíjate:** no se mira `errno` en ningún momento. `<= 0` en el write significa "dejar de enviar", y `0` o `-1` en el read significa "terminado".

Es normal que el stdin y el stdout estén registrados **a la vez**: el script puede ir escribiendo salida mientras todavía le llega el body.

### Paso 6 — `finishCgi`: la respuesta al cliente

[`Server::finishCgi`](../src/cgiEvents.cpp#L145):

1. `cleanupCgi(session, ..., false)` cierra los pipes que queden abiertos y deja el pid pendiente de recoger, sin matarlo.
2. Si el cliente pidió `Connection: close` (o la respuesta lo lleva), marca `close_after_send`.
3. `session.response = res.serialize()`.
4. `logResponse(...)` imprime la línea del log, por ejemplo `GET /cgi/example.py -> 200 CGI pid 9133`.
5. `_switchToWriteMode(client_fd, ...)`: a partir de aquí, la respuesta se envía exactamente igual que cualquier otra (`handleClientWrite`).

### Paso 7 — `cleanupCgi` y `reapChildren`: sin zombies

Cuando un proceso hijo termina, el sistema guarda su código de salida hasta que el padre lo recoge con `waitpid`. Mientras tanto es un **zombie**. Si no se recogen, se acumulan.

[`cleanupCgi`](../src/cgiEvents.cpp#L164) cierra los pipes, hace `kill(pid, SIGKILL)` si se lo piden y añade el pid a `pending_children_`. [`reapChildren`](../src/cgiEvents.cpp#L181) recorre esa lista:

```cpp
if (waitpid(pending_children_[i], NULL, WNOHANG) != 0)   // >0 recogido, -1 ya no existe
    pending_children_.erase(...);
// 0 → todavía vive: se reintenta en la siguiente vuelta del bucle (cada ≤ 1 s)
```

**`WNOHANG`** es la clave: `waitpid` nunca bloquea. Si el hijo aún no ha terminado, devuelve `0` enseguida y lo intentamos más tarde desde `closeIdleClients`, que se ejecuta en cada vuelta del bucle.

### Resumen temporal de una petición POST

```
t=0     cliente envía POST /cgi/echo.py (body 2,5 MB)
        processRequestBuffer → handle → wantsCgi → startCgi → fork
        registrar stdin_fd (W) y stdout_fd (R) → volver al bucle
t=0+    kqueue: stdin_fd escribible → write 64 KB
        kqueue: otro cliente pide GET / → se le responde (no esperamos al CGI)
        kqueue: stdin_fd escribible → write 64 KB      (×40 veces)
        ...     último trozo → close(stdin_fd) → el script ve EOF
        kqueue: stdout_fd legible → read 4 KB → output += ...
        ...
        kqueue: stdout_fd legible → read == 0 → finishCgi → parseOutput
        cliente pasa a modo escritura → handleClientWrite → send()
        reapChildren → waitpid(WNOHANG) → hijo recogido
```

---

## 5. Variables de entorno

Las prepara [`CgiHandler::setupEnvironment`](../src/CgiHandler.cpp#L41). El subject dice: *"The full request and arguments provided by the client must be available to the CGI."*

| Variable | Valor | Ejemplo |
|---|---|---|
| `GATEWAY_INTERFACE` | Siempre `CGI/1.1` | `CGI/1.1` |
| `SERVER_PROTOCOL` | Siempre `HTTP/1.1` | `HTTP/1.1` |
| `SERVER_SOFTWARE` | Siempre `webserv/1.0` | `webserv/1.0` |
| `REQUEST_METHOD` | Método de la petición | `POST` |
| `QUERY_STRING` | Lo que va tras `?`, sin decodificar | `name=paula&x=1` |
| `REQUEST_URI` | Ruta + `?` + query | `/cgi/env.py?name=paula` |
| `SCRIPT_NAME` | Ruta de la URL | `/cgi/env.py` |
| `PATH_INFO` | Ruta de la URL (igual que `SCRIPT_NAME`) | `/cgi/env.py` |
| `SCRIPT_FILENAME` | Nombre del fichero. El script se ejecuta en su directorio, así que basta el nombre. | `env.py` |
| `PATH_TRANSLATED` | Igual que `SCRIPT_FILENAME` | `env.py` |
| `CONTENT_LENGTH` | Tamaño del body (solo en POST o si hay body) | `5` |
| `CONTENT_TYPE` | Cabecera `Content-Type` (si existe) | `text/plain` |
| `SERVER_PORT` | Puerto del server block que atendió la petición | `8080` |
| `SERVER_NAME` | Cabecera `Host` sin el puerto | `localhost` |
| `REMOTE_ADDR` | Fijo a `127.0.0.1` (ver limitaciones) | `127.0.0.1` |
| `REDIRECT_STATUS` | `200`. `php-cgi` se niega a ejecutarse sin ella. | `200` |
| `PATH` | El `PATH` del servidor, para que el script encuentre programas | `/usr/bin:/bin:…` |
| `HTTP_*` | **Cada cabecera de la petición**: en mayúsculas, con `-` → `_` y el prefijo `HTTP_` | `X-Custom: hi` → `HTTP_X_CUSTOM=hi` |

`Content-Type` y `Content-Length` no se repiten como `HTTP_CONTENT_TYPE`, porque ya tienen su propia variable (así lo indica la RFC 3875).

---

### Body chunked

El subject dice: *"for chunked requests, your server needs to un-chunk them, the CGI will expect EOF as the end of the body"*.

Con `Transfer-Encoding: chunked` el body llega troceado (`5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n`). [`HttpRequest::decodeChunked`](../src/RequestChecks.cpp) lo procesa en dos momentos:

1. **Mientras llega** (`isCompleteRequest` / `earlyErrorCode`): solo cuenta, sin copiar, si ya llegó el chunk final `0` y cuántos bytes lleva el body. Si supera `client_max_body_size` → 413 sin esperar al resto.
2. **Al parsear** (`HttpRequest::parse`): decodifica el body. El script recibe el body ya des-chunkeado por stdin, `CONTENT_LENGTH` con su tamaño real y EOF al final.

Un chunked mal formado da 400; otra codificación (`Transfer-Encoding: gzip`) da 501.

---

## 6. Formato de la salida del script

El script debe escribir en su stdout **cabeceras CGI, una línea vacía y el body**:

```
Content-Type: text/html          ← cabeceras CGI
Status: 201 Created              ← opcional: código de respuesta
X-Lo-Que-Sea: valor              ← cualquier otra cabecera pasa tal cual al cliente
                                 ← línea vacía (\r\n o \n)
<html>...</html>                 ← body
```

[`CgiHandler::parseOutput`](../src/CgiHandler.cpp#L139) lo convierte en una respuesta HTTP:

| Situación | Qué hace |
|---|---|
| Separador `\r\n\r\n` o `\n\n` | Lo de antes son cabeceras y lo de después, el body |
| Sin separador | Toda la salida es el body: `200`, `Content-Type: text/html` |
| Cabecera `Status: 201 Created` | La respuesta será `HTTP/1.1 201 Created`. La cabecera `Status` no se envía al cliente. |
| Sin cabecera `Status` | `200 OK` |
| Resto de cabeceras | Se copian tal cual a la respuesta |
| `Content-Length` | **Siempre se recalcula** a partir del body real |

El subject dice: *"If no content_length is returned from the CGI, EOF will mark the end of the returned data."* Eso es justo lo que hacemos: leemos hasta EOF (`read() == 0`) y calculamos el `Content-Length` nosotros.

---

## 7. Errores y códigos de respuesta

| Código | Cuándo | Dónde se decide |
|---|---|---|
| **404** Not Found | El script no existe | `CgiHandler::start` → `access(F_OK)` |
| **403** Forbidden | El script existe pero no se puede leer | `CgiHandler::start` → `access(R_OK)` |
| **405** Method Not Allowed | El método no está permitido en la location | `RequestHandler::handle`, antes de llegar al CGI |
| **413** Content Too Large | Body mayor que `client_max_body_size` | Antes de recibir el body (`earlyErrorCode`) |
| **500** Internal Server Error | Falla `pipe()`, `fcntl()`, `fork()` o el registro en kqueue/epoll | `CgiHandler::start` / `startCgi` |
| **502** Bad Gateway | El script termina **sin escribir nada** (error de Python, `execve` fallido, `exit(1)`…) | `handleCgiEvent`, al llegar EOF con `output` vacío |
| **504** Gateway Timeout | El script pasa más de **`CGI_TIMEOUT` (10 s)** sin leer ni escribir en sus pipes | `closeIdleClients` |

Todos estos casos salen en el log del servidor con su motivo:

```
GET     /cgi/missing.py   404  4.4 KB   CGI script not found: www/cgi/missing.py
GET     /cgi/silent.py    502  4.3 KB   CGI exited without output
GET     /cgi/loop.py      504  4.3 KB   CGI killed after 10s
GET     /cgi/example.py   200  188 B    CGI pid 9133
```

---

## 8. Casos especiales

### El script no termina nunca (bucle infinito)

[`closeIdleClients`](../src/Server.cpp#L202) se ejecuta en cada vuelta del bucle, como mínimo una vez por segundo gracias al timeout de 1 s de `kevent`/`epoll_wait`:

```cpp
if (it->second.cgi.active()) {
    if (now - it->second.cgi.last_io > CGI_TIMEOUT)
        cgi_expired.push_back(it->first);
}
...
cleanupCgi(session, mux, true);           // kill(SIGKILL) + cerrar pipes
finishCgi(client_fd, mux, error 504);     // el cliente recibe 504
```

`last_io` se actualiza cada vez que se lee o escribe en los pipes del script. El timeout cuenta **desde la última actividad, no desde el arranque**: un script en bucle infinito que no escribe nada muere a los 10 s, pero uno que está procesando un body de 100 MB (y va leyendo y escribiendo) no se mata a mitad.

Mientras tanto, el resto de clientes se atienden con normalidad. El tester lo comprueba: con un script en bucle infinito, un `GET /` responde en 0,0006 s.

`CGI_TIMEOUT` se define en [inc/CgiHandler.hpp](../inc/CgiHandler.hpp) y se puede cambiar al compilar: `make FLAGS=-DCGI_TIMEOUT=5`.

Los clientes con un CGI en marcha **no** cuentan para el timeout de inactividad (`CLIENT_TIMEOUT`). Se les aplica solo el del CGI.

### El cliente se desconecta mientras el script se ejecuta

[`handleClientDisconnect`](../src/Server.cpp#L174):

```cpp
if (it->second.cgi.active())
    cleanupCgi(it->second, multiplexer_fd, true);   // nadie va a leer su salida: matarlo
```

### El script deja de leer su stdin antes de recibir todo el body

El siguiente `write()` falla (`EPIPE`; como SIGPIPE está ignorado, no nos mata) y devuelve `-1`. Cerramos el stdin y **seguimos leyendo su salida**: la respuesta puede ser válida aunque el script no haya querido leerlo todo.

### Peticiones en cola del mismo cliente

Mientras `session.cgi.active()`, `processRequestBuffer` no procesa nuevas peticiones de ese cliente: se quedan en `request_buffer`. Cuando se ha enviado la respuesta del CGI, `handleClientWrite` las procesa.

### Ctrl+C con scripts en marcha

[`Server::shutdown`](../src/Server.cpp) llama a `cleanupCgi(..., true)` para cada CGI activo antes de cerrar.

### El stderr del script

No se redirige: los errores de Python (tracebacks) salen por la terminal del servidor, no en la página que recibe el cliente. Si el script solo escribe en stderr y nada en stdout, el resultado es **502**.

---

## 9. Cómo escribir un script CGI

Ejemplo mínimo en Python ([www/cgi/example.py](../www/cgi/example.py)):

```python
#!/usr/bin/env python3
import os
import sys

method = os.environ.get("REQUEST_METHOD", "")
query  = os.environ.get("QUERY_STRING", "")
body   = sys.stdin.read()                      # lee el body hasta EOF

print("Content-Type: text/html")               # cabeceras
print()                                        # línea vacía: obligatoria
print("<h1>Hello from CGI</h1>")               # body
print("<p>Method: %s, query: %s, body: %s</p>" % (method, query, body))
```

Reglas:

- **Siempre** cabeceras + línea vacía antes del body. Si no, toda la salida se toma como body.
- Para cambiar el código de respuesta: `print("Status: 404 Not Found")`.
- Lee el body con `sys.stdin.read()`. Llega EOF cuando se ha enviado entero.
- Las rutas relativas son relativas a `www/cgi/`, porque el script se ejecuta en su directorio.
- La extensión tiene que estar en la directiva `cgi` de su location (`cgi .py /usr/bin/python3;`). Con intérprete, el script no necesita permiso de ejecución.
- Si se configura una extensión sin intérprete válido, el servidor responde 500. Un script ejecutado directamente (sin intérprete) necesita `chmod +x` y una línea `#!` válida.

---

## 10. Cómo probarlo

### A mano con curl

```sh
./webserv                     # etc/default.conf, puerto 8080

curl -i 'http://localhost:8080/cgi/example.py?name=paula'          # GET + query
curl -i -d 'nombre=paula' http://localhost:8080/cgi/example.py     # POST con body
curl -i http://localhost:8080/cgi/no-existe.py                     # 404
```

### Con el tester

```sh
python3 tst/tester.py -k cgi
```

Crea scripts temporales (`env.py`, `echo.py`, `len.py`, `status.py`, `silent.py`, `loop.py`) y comprueba:

| Test | Qué comprueba |
|---|---|
| `cgi_get_environment` | `REQUEST_METHOD`, `QUERY_STRING`, `GATEWAY_INTERFACE`, `SERVER_PORT`, `HTTP_X_CUSTOM` y que el directorio de trabajo es `www/cgi` |
| `cgi_post_body` | El body llega por stdin; `CONTENT_LENGTH` y `CONTENT_TYPE` |
| `cgi_big_post` | Un POST de 2,5 MB (mucho mayor que el buffer del pipe) llega entero |
| `cgi_status_header` | `Status: 201 Created` y las cabeceras propias del script |
| `cgi_not_found` / `cgi_no_output` | 404 / 502 |
| `cgi_timeout_does_not_block` | Bucle infinito → 504, y otro cliente responde mientras tanto en menos de 1 s |
| `cgi_client_leaves` | Si el cliente se va, el proceso del script muere |
| `cgi_keep_alive` | CGI y después estático por la misma conexión |
| `cgi_concurrent` | 10 CGIs a la vez, cada uno recibe su respuesta |
| `no_zombie_processes` | No quedan procesos zombie |

### Bajo carga

```sh
python3 tst/stress.py --path /cgi/example.py -c 20 -t 10
```

Resultado de referencia en un Mac: ~180 peticiones por segundo, 100 % de disponibilidad. Cada petición hace un `fork` + arrancar Python, que es lo que más cuesta.

---

## 11. Limitaciones actuales

| Limitación | Detalle |
|---|---|
| **`REMOTE_ADDR` fijo** | Siempre `127.0.0.1`. Para ponerlo bien habría que guardar la dirección que da `accept()`. |
| **`PATH_INFO` = `SCRIPT_NAME`** | No se separa la información extra de la ruta (`/cgi/x.py/extra`). |
| **Fds cerrados hasta 1024** | El hijo cierra los fds 3–1023. Si el servidor llegara a usar fds más altos, el hijo los heredaría. |

---

## 12. Preguntas de evaluación

**¿Usas un `poll()` aparte para el CGI?**
No. Los dos pipes se registran en el mismo kqueue/epoll que los sockets ([`watchFd`](../src/cgiEvents.cpp#L23)). `poll()` no aparece en ninguna parte del código.

**¿Haces `read`/`write` en los pipes sin que te avise el multiplexor?**
No. `CgiHandler::start` no toca los pipes. Todos los `read`/`write` están en `handleCgiEvent`, que solo se llama cuando kqueue/epoll avisa de ese fd, y hace uno por evento.

**¿Por qué los pipes del lado del servidor son no bloqueantes?**
Para que un `write()` de un body grande nunca se quede esperando a que el script lea: escribe lo que quepa y devuelve. El resto se envía en el siguiente aviso.

**¿Cómo sabes que el script ha terminado?**
Porque `read()` del stdout devuelve `0` (EOF). Eso ocurre cuando todos los extremos de escritura de ese pipe están cerrados, es decir, cuando el script ha cerrado su stdout o ha terminado.

**¿Por qué cierras todos los fds en el hijo?**
Si el hijo conservara una copia de un extremo de escritura de sus propios pipes, el EOF no llegaría nunca. Además, heredaría los sockets de otros clientes y los mantendría abiertos.

**¿Cómo le dices al script que el body ha terminado?**
Cerrando nuestro extremo de escritura del stdin (`closeCgiPipe(cgi.stdin_fd)`). El script recibe EOF al leer.

**¿Qué pasa con un script infinito? ¿Se bloquea el servidor?**
No se bloquea, porque nunca esperamos al script. A los 10 s, `closeIdleClients` lo mata con `SIGKILL` y responde 504. Mientras tanto, los demás clientes se atienden con normalidad.

**¿Cómo evitas los zombies?**
`waitpid(pid, NULL, WNOHANG)` en `reapChildren`. Si el hijo aún vive, se reintenta en la siguiente vuelta del bucle. `WNOHANG` hace que nunca bloquee.

**¿Por qué haces `chdir`?**
El subject dice: *"The CGI should be run in the correct directory for relative path file access"*. Así, si el script abre `"datos.txt"`, lo busca en `www/cgi/`.

**¿`fork` se usa para algo más?**
No. El subject dice: *"You can't use fork for anything other than CGI"*. El único `fork()` está en `CgiHandler::start`.

**¿Qué funciones del sistema usa el CGI y están todas permitidas?**
`pipe`, `fcntl` (solo `F_SETFL` + `O_NONBLOCK`), `fork`, `dup2`, `close`, `chdir`, `execve`, `access`, `read`, `write`, `kill` y `waitpid`. Todas están en la lista del subject. Se quitó `realpath()` porque no lo está.
