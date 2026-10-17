# Hoja de ruta para la evaluación de webserv

Guía para la defensa, en el **mismo orden que la hoja de corrección** (`doc/WebserEval.pdf`). En cada apartado:

- **Qué hace el evaluador**
- **Qué responder**
- **Qué enseñar** (comandos y código)
- **Cuidado con** (lo que puede salir mal)

Al final hay una lista de **lo que hay que saberse al 100 %** y un banco de preguntas con su respuesta.

> **Regla de oro de la hoja:** si en algún momento el servidor hace segfault o termina de forma inesperada, la nota es **0**. Y si algo del apartado "Check the code" no es correcto o no se sabe explicar, **la evaluación se para**.

---

## 0. Antes de que empiece la evaluación

**Preparación (el día antes):**

```sh
git clone <repo> evaluacion && cd evaluacion      # en una carpeta vacía, como hará el evaluador
make                                              # sin warnings
make                                              # "Nothing to be done": sin relink
bash tst/tests_42/setup.sh                        # crea YoupiBanane para el tester de 42
python3 tst/tester.py                             # 52/52
bash tst/check_single_poll.sh                     # 3 ✔
brew install siege                                # la hoja pide instalarlo
```

**Ten abierto:**

- una terminal con `./webserv` (el log en colores ayuda mucho a explicar);
- otra terminal para `curl`, `telnet` y `siege`;
- el navegador (Safari o Chrome) con las herramientas de desarrollador y la pestaña **Network**;
- el editor con `src/Server.cpp` (función `run`) y `src/switchMode.cpp`.

**Si alternáis entre Docker y el Mac:** ejecutad `make re` antes de nada. Los `.o` de una plataforma no sirven en la otra.

---

## 1. README.md (eliminatorio: si falta algo, nota 0)

**Qué hace el evaluador:** abre `README.md` y comprueba cuatro cosas.

| Requisito | Dónde está |
|---|---|
| Primera línea en cursiva, **exactamente**: `*This project has been created as part of the 42 curriculum by login1, login2.*` | Línea 1 |
| Sección **Description** | `## Description` |
| Sección **Instructions** (compilar, ejecutar) | `## Instructions` |
| Sección **Resources** + **cómo se usó la IA** (para qué tareas y en qué partes) | `## Resources` → `### How AI was used` |

**Cuidado con:**

- los logins: tienen que estar **todos** los del equipo, separados por comas y con punto final;
- la sección de IA: os pueden preguntar por ella. Tiene que coincidir con lo que contéis.

---

## 2. "Check the code and ask questions" (la parte más importante)

Si algo de aquí falla, **la evaluación se para**. Es lo que más hay que dominar.

### 2.1 "Explica lo básico de un servidor HTTP"

**En una frase:** un servidor HTTP es un programa que **espera conexiones**, **recibe peticiones de texto** de los clientes (navegadores) y **devuelve respuestas de texto** (una página, una imagen, un error…), siguiendo las reglas del protocolo **HTTP**.

**Qué se envía realmente.** Al abrir `http://localhost:8080/index.html`, el navegador abre una conexión TCP al puerto 8080 y envía:

```
GET /index.html HTTP/1.1          ← línea de petición: MÉTODO  RUTA  VERSIÓN
Host: localhost:8080              ← cabeceras: información extra
Accept: text/html
                                  ← línea vacía: fin de las cabeceras
```

Y el servidor contesta:

```
HTTP/1.1 200 OK                   ← línea de estado: VERSIÓN  CÓDIGO  TEXTO
Content-Type: text/html           ← tipo de contenido
Content-Length: 15880             ← tamaño del body en bytes
                                  ← línea vacía
<!doctype html><html>…</html>     ← body: el contenido
```

Para enseñarlo en directo: `curl -v http://127.0.0.1:8080/` muestra las dos partes.

**Las piezas:**

| Pieza | Qué es | Ejemplos |
|---|---|---|
| Método | Qué quiere hacer el cliente | `GET` (dame), `POST` (te envío), `DELETE` (borra) |
| Ruta | Qué recurso quiere | `/index.html`, `/uploads/foto.png` |
| Cabeceras | Información adicional | `Host` (obligatoria en HTTP/1.1), `Content-Length`, `Content-Type` |
| Body | Los datos | El fichero que se sube o la página que se devuelve |
| Código de estado | Cómo ha ido | `200` bien, `301` redirección, `404` no existe, `405` método no permitido, `413` demasiado grande, `500` error del servidor |

**Lo que hace nuestro servidor con cada petición:**

```
1. Espera conexiones                       → kqueue/epoll + accept()          (Server::run, handleNewConnection)
2. Lee lo que llega                        → recv()                            (handleClientRead)
3. Comprueba que esté completa y sea válida → parse                            (isCompleteRequest, HttpRequest::parse)
4. Decide qué hacer según la config         → fichero, upload, CGI, error…     (RequestHandler::handle)
5. Construye la respuesta                   → línea de estado + cabeceras + body (HttpResponse::serialize)
6. La envía                                 → send()                            (handleClientWrite)
7. Mantiene la conexión para otra petición  → keep-alive, o la cierra
```

**Respuesta corta para la defensa:**

> Un servidor HTTP escucha en un puerto TCP. El cliente abre una conexión y envía una petición de texto: una línea con método, ruta y versión (`GET /index.html HTTP/1.1`), unas cabeceras, una línea vacía y, a veces, un body. El servidor la interpreta, decide qué hacer según su configuración (servir un fichero, guardar un upload, ejecutar un CGI…) y devuelve una respuesta: línea de estado (`HTTP/1.1 200 OK`), cabeceras, línea vacía y el body. Lo difícil del proyecto no es HTTP en sí, sino atender a muchos clientes a la vez en un solo hilo sin bloquearse, y para eso usamos kqueue/epoll.

**Una analogía, por si ayuda:** un camarero (el servidor) atiende mesas (los clientes) que piden en un formato acordado (HTTP). Va a la cocina (el disco o un script) y vuelve con el plato o con un "no tenemos eso" (404). Un buen camarero atiende **muchas mesas a la vez** sin quedarse plantado en ninguna: eso es el kqueue/epoll.

### 2.2 "¿Qué mecanismo de eventos usáis?"

**Respuesta:** **kqueue** en macOS y **epoll** en Linux (no `poll()`). Se elige al compilar (`#if USE_KQUEUE … #else …`), y el Makefile activa kqueue automáticamente en macOS. El subject lo permite: *"you can use any equivalent function such as select(), kqueue(), or epoll()"*.

#### Dónde se ve en el código: kqueue en macOS, epoll en Linux

**Ojo:** en Linux se usa **epoll**, no `poll()`. `poll()` no aparece en ninguna parte del código. La elección se hace **al compilar**: cada binario contiene solo uno de los dos mecanismos. Por eso hay que hacer `make re` al pasar del Mac a Linux.

**1. El Makefile decide** ([Makefile](../Makefile), línea 26):

```make
ifeq ($(shell uname -s),Darwin)    # ¿estamos en macOS?
CXXFLAGS   += -DUSE_KQUEUE         # sí: se define la macro USE_KQUEUE
endif
```

En Linux, `uname -s` dice `Linux`, la macro no se define y se compila la rama de epoll. (La regla `make kqueue` fuerza kqueue a mano, aunque no hace falta.)

**2. En el código, cada sitio que habla con el multiplexor tiene las dos versiones**, separadas por `#if … #else … #endif`:

```cpp
#if defined(USE_KQUEUE) || defined(__APPLE__)
    // versión kqueue (macOS)
#else
    // versión epoll (Linux)
#endif
```

| Qué hace | Dónde | macOS (kqueue) | Linux (epoll) |
|---|---|---|---|
| Incluir la cabecera | [Server.cpp:23](../src/Server.cpp#L23), [switchMode.cpp:6](../src/switchMode.cpp#L6), [cgiEvents.cpp:8](../src/cgiEvents.cpp#L8) | `<sys/event.h>` | `<sys/epoll.h>` |
| **Crear el multiplexor** | `Server::run()`, [Server.cpp:298](../src/Server.cpp#L298) y [:383](../src/Server.cpp#L383) | `kqueue()` | `epoll_create1(0)` |
| Registrar los sockets de escucha | `Server::run()`, [Server.cpp:308](../src/Server.cpp#L308) y [:397](../src/Server.cpp#L397) | `EV_SET(... EVFILT_READ, EV_ADD ...)` + `kevent(kq, &ev, 1, NULL, 0, NULL)` | `epoll_ctl(EPOLL_CTL_ADD, ... EPOLLIN)` |
| **Esperar eventos (el único "poll")** | `Server::run()`, [Server.cpp:325](../src/Server.cpp#L325) y [:409](../src/Server.cpp#L409) | `kevent(kq, NULL, 0, events, N, &timeout)` | `epoll_wait(epoll_fd, events, N, 500)` |
| Registrar un cliente nuevo | `handleNewConnection`, [Server.cpp:139](../src/Server.cpp#L139) | `EV_SET(... EVFILT_READ, EV_ADD ...)` + `kevent` | `epoll_ctl(EPOLL_CTL_ADD, ... EPOLLIN \| EPOLLRDHUP)` |
| Quitar un cliente | `handleClientDisconnect`, [Server.cpp:181](../src/Server.cpp#L181) | Basta con `close()`: kqueue lo quita solo | `epoll_ctl(EPOLL_CTL_DEL)` + `close()` |
| Pasar a modo escritura | `_switchToWriteMode`, [switchMode.cpp:280](../src/switchMode.cpp#L280) | `EVFILT_READ` → `EV_DISABLE`, `EVFILT_WRITE` → `EV_ENABLE` | `epoll_ctl(EPOLL_CTL_MOD, EPOLLOUT)` |
| Volver a modo lectura | `_switchToReadMode`, [switchMode.cpp:297](../src/switchMode.cpp#L297) | `EVFILT_WRITE` → `EV_DISABLE`, `EVFILT_READ` → `EV_ENABLE` | `epoll_ctl(EPOLL_CTL_MOD, EPOLLIN)` |
| Registrar un pipe del CGI | `watchFd`, [cgiEvents.cpp:25](../src/cgiEvents.cpp#L25) | `EV_SET(... EVFILT_READ/WRITE ...)` + `kevent` | `epoll_ctl(EPOLL_CTL_ADD, EPOLLIN/EPOLLOUT)` |
| Quitar un pipe del CGI | `closeCgiPipe`, [cgiEvents.cpp:42](../src/cgiEvents.cpp#L42) | `close()` | `epoll_ctl(EPOLL_CTL_DEL)` + `close()` |

**3. El bucle de `Server::run()` está duplicado.** De la línea 297 a la 381 está la versión kqueue y de la 381 a la 466, la de epoll. La lógica es **idéntica**: esperar, recorrer los eventos y llamar al mismo handler (`handleNewConnection`, `handleCgiEvent`, `handleClientRead`, `handleClientWrite`). Solo cambia cómo se lee el tipo de evento:

| | kqueue | epoll |
|---|---|---|
| fd del evento | `events[n].ident` | `events[n].data.fd` |
| ¿Se puede leer? | `events[n].filter == EVFILT_READ` | `events[n].events & EPOLLIN` |
| ¿Se puede escribir? | `events[n].filter == EVFILT_WRITE` | `events[n].events & EPOLLOUT` |
| ¿El cliente cerró? | `events[n].flags & EV_EOF` | `events[n].events & (EPOLLERR \| EPOLLHUP \| EPOLLRDHUP)` |

**Todo lo demás** (handlers, parser, routing, CGI) **es común**: no sabe ni le importa qué multiplexor hay debajo.

**Cómo demostrarlo:**

```sh
grep -n "USE_KQUEUE" Makefile src/*.cpp        # todos los puntos donde se elige
bash tst/check_single_poll.sh                  # en macOS: 1 KQUEUE; en Linux: 1 eventpoll
```

**Si preguntan "¿por qué epoll y no poll en Linux?":** epoll, como kqueue, mantiene la lista de fds **dentro del kernel** y devuelve solo los que están listos. `poll()` recibe y recorre la lista entera en cada llamada. Con muchos clientes, epoll y kqueue son más eficientes. Los tres están permitidos por el subject.

### 2.3 "Explica cómo funciona poll() (o vuestro equivalente)"

**Respuesta:**

> Le damos al kernel la lista de file descriptors que nos interesan y para qué: leer, escribir o ambas cosas. Llamamos a `kevent()` (o `epoll_wait()`) y el proceso **espera** hasta que alguno esté listo, o hasta que pase el timeout (0,5 s). Nos devuelve **solo los que están listos** y para qué. Así, un único hilo atiende a cientos de clientes sin quedarse bloqueado en ninguno: solo lee o escribe en un socket cuando el kernel garantiza que la operación no va a esperar.
>
> En kqueue, la misma función sirve para dos cosas: `kevent(kq, &cambio, 1, NULL, 0, NULL)` **registra** un fd (como `epoll_ctl`), y `kevent(kq, NULL, 0, events, N, &timeout)` **espera** eventos (como `epoll_wait`).

### 2.4 "¿Usáis un solo poll()? ¿Cómo gestionáis accept() y la lectura/escritura de los clientes?"

**Respuesta:** un solo multiplexor y un único punto de espera, en `Server::run()` ([src/Server.cpp](../src/Server.cpp), función `run`: `kevent` en macOS, `epoll_wait` en Linux). Ahí se registran **todos** los fds: los sockets de escucha, los clientes y los pipes del CGI. Para cada evento:

```
kevent() devuelve los fds listos
 ├─ socket de escucha listo     → handleNewConnection → accept()      (un accept por evento)
 ├─ pipe de un CGI listo        → handleCgiEvent      → read()/write() (uno por evento)
 ├─ cliente con EOF             → handleClientDisconnect
 ├─ cliente listo para leer     → handleClientRead    → recv()        (uno por evento)
 └─ cliente listo para escribir → handleClientWrite   → send()        (uno por evento)
```

**Qué enseñar:**

```sh
bash tst/check_single_poll.sh
```

Demuestra en el código que no hay `poll()` ni `select()` y que solo hay un punto de espera. En ejecución, `lsof` muestra **1 KQUEUE** mientras se atienden clientes y 3 CGIs a la vez.

### 2.5 "El poll() debe estar en el bucle principal y vigilar lectura y escritura a la vez" (si no, nota 0)

**Qué enseñar:** el bucle `while (!g_shutdown_requested)` de `Server::run()`. El mismo kqueue tiene filtros `EVFILT_READ` y `EVFILT_WRITE`. En epoll, `EPOLLIN` y `EPOLLOUT`.

**Detalle a saber:** mientras se envía una respuesta, se desactiva la lectura de ese cliente (`_switchToWriteMode`), y al terminar se reactiva (`_switchToReadMode`), en [src/switchMode.cpp](../src/switchMode.cpp). Así una petición nueva no puede pisar una respuesta a medio enviar. El multiplexor sigue vigilando lectura y escritura a la vez para el conjunto de fds.

### 2.6 "Enseñad el camino desde poll() hasta la lectura/escritura del cliente"

Recorred el código **en este orden**:

1. `Server::run()` → `kevent(kq, NULL, 0, events, …)`. Es el único sitio donde se espera.
2. El bucle `for` sobre `events`, que según el tipo llama a `handleClientRead` o a `handleClientWrite`.
3. `Server::handleClientRead()` ([src/switchMode.cpp](../src/switchMode.cpp)): **un** `recv()`; si devuelve `<= 0`, se desconecta al cliente; si no, se añade lo leído al buffer y se llama a `processRequestBuffer()`.
4. `processRequestBuffer()`: ¿la petición está completa? Entonces se parsea, se genera la respuesta y `queueResponse()` → `_switchToWriteMode()`. **Aquí no se escribe nada**: solo se pide aviso de escritura.
5. Siguiente vuelta del bucle: `kevent` avisa de que se puede escribir → `handleClientWrite()` → **un** `send()`. Si no cabe todo, se espera al siguiente aviso.

> La hoja dice *"It is acceptable to read/write until EAGAIN, then return to the main loop"*. Nosotros somos más estrictos: **una sola operación por aviso**, sin `EAGAIN` y sin mirar `errno`.

### 2.7 "Buscad todos los read/recv/write/send y comprobad que, si hay error, se elimina al cliente"

| Llamada | Dónde | Qué pasa si falla |
|---|---|---|
| `recv()` | `handleClientRead` | `<= 0` → `handleClientDisconnect` (cliente eliminado) |
| `send()` | `handleClientWrite` | `<= 0` → `handleClientDisconnect` (cliente eliminado) |
| `write()` al stdin del CGI | `handleCgiEvent` ([src/cgiEvents.cpp](../src/cgiEvents.cpp)) | `<= 0` → se cierra ese pipe (el script dejó de leer); se sigue leyendo su salida |
| `read()` del stdout del CGI | `handleCgiEvent` | `> 0` → se acumula; `0` o `-1` → el script terminó: se construye la respuesta (502 si no escribió nada) |

Para buscarlas:

```sh
grep -nE '(^|[^a-zA-Z_:.])(read|write|recv|send)\s*\(' src/*.cpp
```

### 2.8 "Comprobad que se gestionan los dos valores de retorno (-1 y 0)"

**Respuesta:** en los cuatro casos se comprueba `<= 0` o `> 0`, que cubre a la vez el `-1` (error) y el `0` (cierre o EOF). No hay ningún caso en que solo se mire uno de los dos.

### 2.9 "Si se mira errno después de read/recv/write/send, nota 0"

**Respuesta:** nunca se mira. Si `recv`/`send` devuelve `<= 0`, se cierra el cliente sin consultar `errno`. `errno` solo aparece después de funciones que no son de lectura o escritura: `socket`, `bind`, `listen`, `kevent` (para `EINTR`, cuando llega Ctrl+C), `accept` (solo para el log) y `remove`/`opendir` (ficheros del disco).

```sh
grep -n errno src/*.cpp
```

### 2.10 "No leer ni escribir fds de eventos sin pasar por poll(). Los ficheros del disco están exentos"

**Respuesta:**

- sockets y pipes: siempre después de un aviso de kqueue/epoll;
- ficheros del disco (la config, las páginas estáticas, los uploads): se leen y escriben directamente con `std::ifstream`/`std::ofstream`, como permite el subject (*"Regular disk files are exempt"*).

### 2.11 "Comprobad que la E/S de ficheros del disco no bloquea el bucle"

**Respuesta:** leer un fichero normal del disco no se queda esperando a otro programa, como sí pasa con un socket. Se lee de una vez en `readFile()` ([src/otherUtilities.cpp](../src/otherUtilities.cpp)) y el envío al cliente sí pasa por kqueue, por trozos. Con la prueba de estrés lo podéis demostrar: miles de peticiones por segundo a la página principal.

### 2.12 "Compila sin relink"

```sh
make re && make     # el segundo make: "Nothing to be done for 'all'"
```

Si os preguntan por qué: el Makefile solo vuelve a compilar los `.o` cuyos `.cpp` o `.hpp` han cambiado (`-MMD -MP`) y solo enlaza si algún `.o` es más nuevo que `webserv`.

---

## 3. Configuración

**Qué hace el evaluador:** edita la config y prueba cada punto. Arrancad con `./webserv` (usa `etc/default.conf`) o `./webserv etc/default.conf`.

| Lo que pide | Cómo demostrarlo con `etc/default.conf` |
|---|---|
| **Códigos de estado correctos** | Todos usan el estándar (`HttpResponse::reasonPhrase`): 200, 301, 400, 403, 404, 405, 408, 409, 413, 414, 431, 500, 501, 502, 504, 505 |
| **Varios sitios en distintas interfaces y puertos** | `http://localhost:8080/` (`0.0.0.0:8080`, root `www/`) y `http://127.0.0.1:8081/` (otra interfaz, root `www/site2/`) |
| **Página de error por defecto** (*"try modifying the 404"*) | Sin nada configurado sale la plantilla `www/err/template.html`. Para cambiarla: crear un fichero (p. ej. `www/err/mi404.html`), descomentar `error_page 404 www/err/mi404.html;` en `default.conf` y reiniciar |
| **Límite de body** | El sitio del 8081 tiene un límite de 100 bytes. Corto: `curl -X POST -H "Content-Type: plain/text" --data "BODY IS HERE short" http://127.0.0.1:8081/` → **200**. Largo: `curl -X POST -H "Content-Type: plain/text" --data "$(printf 'x%.0s' $(seq 200))" http://127.0.0.1:8081/` → **413** |
| **Rutas a distintos directorios** | `/uploads/` → `www/uploads/`, `/cgi/` → `www/cgi/`, y el sitio del 8081 → `www/site2/` |
| **Fichero por defecto de un directorio** | `http://localhost:8080/` → `www/index.html`; `http://localhost:8080/showUploads` → `www/showUploads/index.html` (directiva `index`, por defecto `index.html`) |
| **Métodos aceptados por ruta** (*"try DELETE with and without permission"*) | Con permiso: subir un fichero y `curl -X DELETE http://localhost:8080/uploads/<fichero>` → **200**. Sin permiso: `curl -X DELETE http://localhost:8080/index.html` → **405**, con la cabecera `Allow: GET` |

**Saberse:**

- **Cómo se elige la location:** el prefijo más largo que coincide con la URL (`findMatchingLocation` en [src/obtainServerConfigToRequest.cpp](../src/obtainServerConfigToRequest.cpp)).
- **`root` en una location:** el prefijo de la location se sustituye por el root, como en el ejemplo `/kapouet` del subject: `/uploads/a.png` con `root www/uploads` → `www/uploads/a.png`.
- **`client_max_body_size`** puede ponerse en el server o en una location. El 413 se da **en cuanto llegan las cabeceras**, sin esperar al body (`earlyErrorCode`).
- **Errores de la config**: el programa se niega a arrancar con un mensaje claro (puerto inválido, IP inválida, `;` que falta, directiva desconocida, dos `server` en el mismo `interfaz:puerto`…).

---

## 4. Basic checks (telnet, curl)

```sh
# GET, POST y DELETE
curl -i http://localhost:8080/                                   # 200
echo hola > prueba.txt
curl -i -F file=@prueba.txt http://localhost:8080/upload         # 200 (multipart, como el formulario)
curl http://localhost:8080/uploads/prueba.txt                     # "hola"
curl -i -X DELETE http://localhost:8080/uploads/prueba.txt        # 200
curl -i -X DELETE http://localhost:8080/uploads/prueba.txt        # 404 (ya no existe)

# UNKNOWN: sin crash
curl -i -X FOO http://localhost:8080/                             # 501 Not Implemented

# Con telnet (escribir la petición a mano y pulsar Enter dos veces)
telnet localhost 8080
GET / HTTP/1.1
Host: localhost

```

**Cuidado con:** con telnet, si no escribes la cabecera `Host:` en HTTP/1.1, la respuesta es **400**, y es correcto (el `Host` es obligatorio en HTTP/1.1). Con `GET / HTTP/1.0` no hace falta.

**Upload de varios ficheros:** el formulario de `http://localhost:8080/` envía todos los ficheros en una sola petición, y se guardan todos.

---

## 5. CGI

**Qué hace el evaluador:** prueba el CGI con GET y POST, y con scripts que fallan o no terminan.

```sh
curl 'http://localhost:8080/cgi/example.py?nombre=paula'          # GET: muestra método y query
curl -d 'nombre=paula' http://localhost:8080/cgi/example.py       # POST: el body llega al script
```

**Scripts con errores** (créalos durante la evaluación):

```sh
printf 'while True:\n    pass\n' > www/cgi/loop.py
printf 'import sys\nsys.exit(1)\n' > www/cgi/error.py
printf 'esto no es python(\n' > www/cgi/sintaxis.py

curl -i http://localhost:8080/cgi/loop.py       # 504 a los ~10 s; MIENTRAS TANTO el navegador sigue funcionando
curl -i http://localhost:8080/cgi/error.py      # 502 Bad Gateway (terminó sin escribir nada)
curl -i http://localhost:8080/cgi/sintaxis.py   # 502 (el error de Python sale en la terminal del servidor)
curl -i http://localhost:8080/cgi/noexiste.py   # 404
```

**Saberse:** [doc/cgi.md](cgi.md) lo explica paso a paso.

- **Cómo se decide que es CGI:** por la extensión, con la directiva `cgi .py /usr/bin/python3;` de la location.
- **El directorio correcto:** el hijo hace `chdir()` al directorio del script antes del `execve()`.
- **No bloquea al servidor:**
  - `CgiHandler::start` solo hace `fork` y crea pipes no bloqueantes;
  - los dos pipes se registran en el **mismo** kqueue/epoll;
  - `handleCgiEvent` escribe el body y lee la salida, una operación por aviso.
- **Cómo se sabe que terminó:** `read()` del stdout devuelve `0` (EOF).
- **Script infinito:** si pasa más de `CGI_TIMEOUT` (10 s) sin leer ni escribir, se mata con `kill(SIGKILL)` y se responde 504.
- **Zombies:** `waitpid(pid, NULL, WNOHANG)` en `reapChildren`, que nunca bloquea.
- **Body chunked:** se des-chunkea antes de pasarlo al script, que recibe `CONTENT_LENGTH` y EOF al final.
- **Variables de entorno:** `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SCRIPT_NAME`, `PATH_INFO`, `SERVER_PORT`… y cada cabecera como `HTTP_*`.
- **`fork` solo se usa para el CGI**: hay un único `fork()`, en `CgiHandler::start`.

---

## 6. Con el navegador

**Qué hace el evaluador:** abre las herramientas de desarrollador, pestaña **Network**, y navega.

| Prueba | URL | Qué se ve |
|---|---|---|
| Cabeceras de petición y respuesta | `http://localhost:8080/` | Respuesta: `HTTP/1.1 200 OK`, `Content-Type: text/html; charset=UTF-8`, `Content-Length` |
| Sitio estático completo | `http://localhost:8080/` | La página, el icono SVG y la imagen |
| URL incorrecta | `http://localhost:8080/noexiste` | Página de error 404 (la plantilla) |
| Listar un directorio | `http://localhost:8080/uploads/` | "Index of /uploads/" con enlaces |
| URL con redirect | `http://localhost:8080/old` | 301 → vuelve a `/` |
| Lo que quieran | `/showUploads`, subir y borrar ficheros, `/cgi/example.py`, `http://127.0.0.1:8081/` | — |

**Cuidado con:**

- Safari y Chrome piden `/favicon.ico` y `/apple-touch-icon.png` por su cuenta. Los **404** del log son normales: esos ficheros no existen.
- `/uploads` (sin barra) redirige a `/uploads/` porque el listado usa enlaces relativos. Los directorios con index (`/showUploads`) se sirven directamente.

---

## 7. Port issues

| Prueba | Qué pasa | Por qué |
|---|---|---|
| Varias interfaces y puertos con sitios distintos | `:8080` sirve `www/`; `127.0.0.1:8081` sirve `www/site2/` | Un socket de escucha por cada `interfaz:puerto` |
| **Varios sitios en el mismo `interfaz:puerto`** | **Error al arrancar**: `webserv: duplicate listen 127.0.0.1:8080 (server blocks 1 and 2): virtual hosts are not supported` | No implementamos virtual hosts (`server_name`), así que no habría forma de elegir entre los dos. La hoja acepta las dos opciones: dar error o implementar virtual hosts |
| **Dos `webserv` a la vez, con configs distintas y el mismo puerto** | El segundo sale con `webserv: Address already in use`; el primero sigue funcionando | El sistema no deja que dos procesos escuchen en el mismo puerto. `bind()` falla en el segundo, que termina de forma controlada |

Para probar el segundo caso:

```sh
printf 'server { listen 127.0.0.1:9000; root www/; }\nserver { listen 127.0.0.1:9000; root www/site2/; }\n' > /tmp/dup.conf
./webserv /tmp/dup.conf
```

---

## 8. Siege y prueba de estrés

```sh
touch www/empty.html                                   # "a simple GET request on an empty page"
./webserv
siege -b -c 50 -t 30S http://127.0.0.1:8080/empty.html  # en otra terminal
```

> **Usad `127.0.0.1`, no `localhost`.** En macOS, `localhost` se resuelve primero como IPv6 (`::1`) y el servidor escucha en IPv4, así que siege da `socket: unable to connect ... Connection refused`. curl y el navegador prueban la siguiente dirección y por eso funcionan con `localhost`.

**Qué mira el evaluador:**

- **Availability > 99,5 %.** La nuestra da 100 %.
- **La memoria no crece sin parar.** Vigiladla con `top -pid $(pgrep -x webserv)` o `ps -o rss= -p $(pgrep -x webserv)`. En nuestras pruebas se queda fija (~2,7 MB) tras 350 000 peticiones.
- **No hay conexiones colgadas.** Los clientes inactivos se cierran a los 60 s, y un body a medias recibe 408 a los 2 s.
- **siege indefinido sin reiniciar el servidor.**

**Acordad con el evaluador los parámetros** (`-c` clientes, `-d` espera entre peticiones, `-r` repeticiones): la hoja lo pide. En macOS, un `-c` muy alto puede agotar los puertos del propio cliente. Eso es un límite del sistema, no del servidor.

Alternativa sin instalar nada: `python3 tst/stress.py -c 100 -t 30` (con el servidor arrancado).

---

## 9. Fugas de memoria

```sh
leaks --atExit -- ./webserv          # macOS: usarlo un rato y pulsar Ctrl+C → "0 leaks for 0 total leaked bytes"
valgrind --leak-check=full ./webserv # Linux
```

**Saberse:** con Ctrl+C el servidor cierra de forma ordenada (`Server::shutdown`): mata los CGI que estén en marcha, cierra todos los clientes y el kqueue, y el destructor cierra los sockets de escucha. Por eso `leaks` puede medir al salir.

---

## 10. Bonus (solo si todo lo obligatorio salió perfecto)

| Bonus | Estado |
|---|---|
| Cookies y sesiones | **No implementado**: decid que no lo hicisteis |
| Varios CGI | El código lo soporta (varias líneas `cgi .ext intérprete` en una location, p. ej. `cgi .sh /bin/sh;`), pero no hay un ejemplo preparado. Si queréis optar a este punto, preparad un `.sh` de ejemplo y su config |

---

## 11. Lo que hay que saberse al 100 %

Si alguna de estas cosas no se sabe explicar, puede pararse la evaluación o marcarse "Can't support / explain code".

1. **El bucle de eventos.**
   - Dónde está: `Server::run`.
   - Qué hace `kevent` (registrar frente a esperar).
   - Por qué hay un timeout de 0,5 s: para aplicar los timeouts aunque no lleguen eventos.
   - Cómo se reparte cada evento.
2. **Una operación por evento y sin `errno`.**
   - Enseñar `handleClientRead` y `handleClientWrite`.
   - Por qué `<= 0` cierra al cliente.
3. **Cómo se detecta que una petición está completa.**
   - `\r\n\r\n` + `Content-Length`, o el chunk final `0` (`isCompleteRequest`).
   - Qué pasa con el pipelining: solo se quita del buffer la petición actual.
4. **El envío parcial.**
   - Si `send` no envía todo, `bytes_sent` guarda por dónde va y el resto sale en el siguiente aviso.
5. **El CGI de principio a fin.** Ver [doc/cgi.md](cgi.md):
   - `fork`, pipes no bloqueantes y `dup2`;
   - por qué el hijo cierra todos los demás fds (si no, no llega el EOF);
   - el `chdir` y el `execve`;
   - los pipes en el kqueue;
   - el EOF, el timeout y `waitpid(WNOHANG)`.
6. **El routing por configuración.**
   - El orden de `RequestHandler::handle`:
     - error de parseo → 501 → 404 sin location;
     - 405 → redirect → CGI por extensión;
     - GET (fichero, index, autoindex) / POST (upload) / DELETE.
7. **Códigos de estado.**
   - Qué significa cada uno y cuándo lo devolvéis: 400, 403, 404, 405, 408, 413, 414, 431, 501, 502, 504, 505.
8. **Timeouts**: por qué ninguna petición se queda colgada.
   - `CLIENT_TIMEOUT` 60 s;
   - `BODY_TIMEOUT` 2 s (408);
   - `CGI_TIMEOUT` 10 s (504).
9. **Por qué no se cae.**
   - SIGPIPE ignorado: si un cliente se va a mitad de un `send`, no mata el proceso.
   - `try/catch` en cada evento.
   - Límites de tamaño antes de guardar datos en memoria (413, 431).
   - Validación de la config al arrancar.
10. **Funciones permitidas.**
    - Todo está en la lista del subject.
    - `inet_addr` y `realpath` se sustituyeron por código propio.
    - `fcntl` solo con `F_SETFL` + `O_NONBLOCK`.

---

## 12. Banco de preguntas y respuestas

**¿Qué pasa si un cliente se desconecta a mitad de una respuesta?**
`send()` devuelve `-1` (SIGPIPE está ignorado, así que no mata el proceso) y se llama a `handleClientDisconnect`, que cierra el fd, borra la sesión y, si tenía un CGI en marcha, lo mata.

**¿Qué pasa si un cliente envía media petición y se calla?**
Si aún no han llegado las cabeceras, se cierra a los 60 s. Si ya llegaron y falta el body, se responde **408** a los 2 s.

**¿Qué pasa si te mandan un body de 10 GB?**
En cuanto llegan las cabeceras se compara `Content-Length` con `client_max_body_size` y se responde **413** sin leer el body. Con chunked, se cuentan los bytes a medida que llegan y se corta al superar el límite.

**¿Qué pasa si las cabeceras son enormes?**
Una línea de más de 8 KB, o cabeceras de más de 16 KB sin terminar → **431**.

**¿Por qué kqueue y no poll?**
Es el equivalente nativo de macOS, y el subject lo permite. A diferencia de `poll()`, no hay que pasar toda la lista de fds en cada llamada: el kernel la mantiene y solo devuelve los que están listos.

**¿Cómo funciona `Transfer-Encoding: chunked`?**
El body llega en trozos: `<tamaño en hex>\r\n<datos>\r\n`, y termina con `0\r\n\r\n`. Mientras llega, `scanChunked` sabe de forma incremental si ya está completo. Al parsear, `decodeChunked` junta los trozos, y el CGI recibe el body limpio.

**¿Qué es keep-alive y cómo lo gestionáis?**
En HTTP/1.1 la conexión se reutiliza para varias peticiones. Tras enviar una respuesta se vuelve a modo lectura. Se cierra si el cliente pide `Connection: close`, si es HTTP/1.0 sin `keep-alive`, o si la respuesta es un error.

**¿Qué hace el servidor con Ctrl+C?**
El handler de SIGINT solo pone a 1 la variable `g_shutdown_requested`. El bucle termina, y `Server::shutdown` mata los CGI, cierra los clientes y el kqueue e imprime un resumen.

**¿Por qué un directorio sin barra final a veces redirige?**
Si tiene index, se sirve directamente. Si va a mostrar un listado (autoindex), primero se redirige a la URL con `/`, porque los enlaces del listado son relativos y sin la barra apuntarían al directorio padre.

**¿Cómo se guarda un upload?**
El formulario envía `multipart/form-data` con una parte por fichero. `parseMultipartFormData` las separa por el boundary y se guardan todas en el `upload_path` de la location. Si algún nombre ya existe → 409, sin guardar ninguno. Un POST en crudo a `/uploads/foto.png` se guarda como `foto.png` y sobrescribe, como un PUT.

**¿Qué testers habéis usado?**
- `tst/tester.py`: 52 pruebas funcionales.
- `--tester42`: simula el tester oficial.
- `tst/stress.py`: prueba de estrés.
- `tst/check_single_poll.sh`: demuestra que hay un solo multiplexor.
- El tester oficial de 42 y `full_test_suite.py`: 95/100; el único fallo es un error del propio tester, que espera rechazar dos cabeceras válidas.

---

## 12b. Preguntas específicas que os pueden hacer

Agrupadas por tema. Las respuestas están basadas en vuestro código.

### HTTP

**¿Diferencia entre GET y POST?**
GET pide un recurso y no lleva body. POST envía datos en el body (un formulario, un fichero). GET es *idempotente*: repetirlo no cambia nada en el servidor. POST puede crear algo nuevo cada vez.

**¿Qué significan los códigos 2xx, 3xx, 4xx y 5xx?**
- 2xx: éxito (`200 OK`).
- 3xx: redirección (`301 Moved Permanently`, con la cabecera `Location`).
- 4xx: error del **cliente** (`400` petición mal formada, `404` no existe, `405` método no permitido, `413` body demasiado grande).
- 5xx: error del **servidor** (`500`, `502` el CGI falló, `504` el CGI tardó demasiado).

**¿Diferencia entre 404 y 405? ¿Y entre 405 y 501?**
- `404`: el recurso no existe.
- `405`: existe, pero ese método no está permitido en esa ruta. Va con la cabecera `Allow` (p. ej. `Allow: GET`).
- `501`: el servidor no implementa ese método en absoluto (`FOO`, `PUT`, `OPTIONS`…).

**¿Diferencias entre HTTP/1.0 y HTTP/1.1?**
- En 1.1 la cabecera `Host` es obligatoria (sin ella → 400).
- La conexión se mantiene abierta por defecto (keep-alive); en 1.0 se cierra salvo que se pida `Connection: keep-alive`.
- 1.1 añade `Transfer-Encoding: chunked`.

**¿Cómo sabe el servidor dónde termina una petición?**
- Las cabeceras terminan en `\r\n\r\n`.
- El body mide lo que dice `Content-Length`, o, si es chunked, termina con el chunk `0\r\n\r\n`.
- Sin ninguna de las dos, no hay body.
- Está en `HttpRequest::isCompleteRequest` ([src/RequestChecks.cpp](../src/RequestChecks.cpp)).

**¿Qué es `Content-Type` y cómo lo decidís?**
Dice qué tipo de contenido es el body (`text/html`, `image/png`…). Se elige por la extensión del fichero en `RequestHandler::getMimeType` ([src/otherUtilities.cpp](../src/otherUtilities.cpp)). Si la extensión no se conoce, `application/octet-stream`.

**¿Las cabeceras distinguen mayúsculas?**
El nombre no: `Host`, `host` y `HOST` son la misma cabecera. Por eso se guardan en minúsculas (`normalizeHeaderName`). El valor sí puede distinguirlas.

**¿Qué pasa si llegan dos `Content-Length` o dos `Host`?**
400. Si se aceptaran, servidores distintos podrían interpretar la misma petición de forma diferente (*request smuggling*). Lo mismo si llegan a la vez `Content-Length` y `Transfer-Encoding`.

### Sockets y red

**¿Qué es un socket? ¿Y un file descriptor?**
Un socket es el extremo de una conexión de red. Para el programa es un **file descriptor**: un número entero que identifica algo abierto (fichero, socket, pipe), sobre el que se hace `read`/`write`/`close`.

**¿Qué hacen `socket`, `bind`, `listen` y `accept`?**
- `socket()`: crea el socket.
- `bind()`: lo asocia a una IP y un puerto.
- `listen()`: lo pone a esperar conexiones (con una cola de pendientes).
- `accept()`: saca una conexión de la cola y devuelve **un fd nuevo** para ese cliente.

Está en `setupListeningSockets` y `handleNewConnection` ([src/Server.cpp](../src/Server.cpp)).

**¿Para qué sirve `SO_REUSEADDR`?**
Para poder volver a arrancar el servidor enseguida en el mismo puerto. Sin él, tras cerrar el servidor el puerto queda unos segundos ocupado (estado `TIME_WAIT`) y `bind()` falla con "Address already in use".

**¿Qué es el backlog de `listen()`?**
El tamaño de la cola de conexiones que esperan a que se haga `accept()`. Usáis `SOMAXCONN`, el máximo que permite el sistema.

**¿Por qué los sockets son no bloqueantes si ya usáis kqueue/epoll?**
Por seguridad: aunque kqueue diga que un fd está listo, una operación que intente más de lo disponible podría bloquear. En modo no bloqueante, en el peor caso devuelve un error en vez de congelar el servidor. Se activa con `fcntl(fd, F_SETFL, O_NONBLOCK)`, el único uso permitido de `fcntl` en macOS.

**¿Qué es `INADDR_ANY` / `0.0.0.0`?**
"Todas las interfaces". `listen 8080;` escucha en todas; `listen 127.0.0.1:8081;` solo en la interfaz local.

**¿Diferencia entre TCP y HTTP?**
TCP es el transporte: lleva bytes de forma fiable y ordenada entre dos máquinas. HTTP es el protocolo de aplicación que va **encima**: define qué significan esos bytes (petición, cabeceras, respuesta).

### Multiplexación (kqueue/epoll)

**¿Diferencia entre `select`, `poll`, `epoll` y `kqueue`?**
- `select`: limitado a 1024 fds y hay que reconstruir los conjuntos en cada llamada.
- `poll`: recibe la lista entera de fds en cada llamada y la recorre.
- `epoll` (Linux) y `kqueue` (BSD/macOS): mantienen la lista **dentro del kernel** y solo devuelven los fds listos. Son más eficientes con muchos clientes.

**¿Usáis level-triggered o edge-triggered?**
**Level-triggered**, que es el modo por defecto (no usáis `EV_CLEAR` ni `EPOLLET`): mientras un fd tenga datos pendientes, el kernel lo sigue avisando en cada llamada. Por eso basta con **una lectura por aviso**: si queda algo, avisará otra vez.

**¿Para qué es el timeout de 0,5 s de `kevent`/`epoll_wait`?**
Para que el bucle se despierte aunque no haya eventos y pueda aplicar los timeouts: clientes inactivos (60 s), body a medias (2 s → 408) y CGI colgado (10 s → 504).

**¿Qué pasa si `send()` no envía toda la respuesta?**
Envía lo que cabe en el buffer del kernel y devuelve cuánto ha enviado. Se guarda por dónde va (`bytes_sent`) y el resto se envía en el siguiente aviso de escritura (`handleClientWrite`).

**¿Por qué no usáis `EAGAIN`?**
Porque nunca intentamos una segunda operación sin aviso: una sola por evento. El subject prohíbe mirar `errno` después de leer o escribir, así que este diseño lo evita por completo.

### CGI

**¿Qué es un CGI?**
Un programa externo que genera la respuesta. El servidor lo ejecuta, le pasa los datos de la petición por **variables de entorno** y el body por **stdin**, y lo que el script escribe en **stdout** es la respuesta.

**¿Qué funciones usáis y para qué?**
- `pipe()`: dos tuberías, una para el stdin del script y otra para su stdout.
- `fork()`: crea el proceso hijo.
- `dup2()`: conecta los pipes al stdin y stdout del hijo.
- `chdir()`: cambia al directorio del script.
- `execve()`: sustituye el hijo por el script (o por su intérprete).
- `waitpid(WNOHANG)`: recoge al hijo sin bloquear.
- `kill()`: lo mata si se cuelga.

**¿Por qué hay que cerrar los extremos de los pipes que no se usan?**
Un pipe solo da EOF cuando **todos** sus extremos de escritura están cerrados. Si el hijo conservara una copia del extremo de escritura de su stdout, el servidor nunca vería el final de la salida. Por eso el hijo cierra todos los fds excepto stdin/stdout/stderr.

**¿Qué es un proceso zombie y cómo lo evitáis?**
Un hijo que ha terminado y cuyo padre todavía no ha recogido su estado. Se evita con `waitpid(pid, NULL, WNOHANG)` en `reapChildren`, que no bloquea: si aún no terminó, se reintenta en la siguiente vuelta del bucle.

**¿Qué variables de entorno le pasáis?**
`REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SCRIPT_NAME`, `PATH_INFO`, `SERVER_PROTOCOL`, `SERVER_PORT`, `REDIRECT_STATUS` (la necesita php-cgi) y cada cabecera como `HTTP_*`. Se preparan en `CgiHandler::setupEnvironment`.

**¿Cómo sabe el servidor qué código HTTP devolver con un CGI?**
Lee la salida del script: si incluye una cabecera `Status: 201 Created`, la usa; si no, `200 OK`. Lo hace `CgiHandler::parseOutput`.

### Configuración

**¿Cómo se elige la location de una URL?**
Por el **prefijo más largo** que coincide (`findMatchingLocation`). Con `/` y `/uploads`, la URL `/uploads/a.png` usa `/uploads`.

**¿Qué hace `root` en una location?**
Sustituye el prefijo de la location por ese directorio, como en el ejemplo `/kapouet` del subject: `location /uploads { root www/uploads; }` → `/uploads/a.png` = `www/uploads/a.png`.

**¿Qué pasa si la config está mal?**
El servidor no arranca y dice qué falla: `;` que falta, directiva desconocida, puerto fuera de 1–65535, IP inválida, dos `server` en el mismo `interfaz:puerto`… Lo hace el parser en [src/Configuration.cpp](../src/Configuration.cpp).

### Robustez y C++

**¿Cómo os aseguráis de que no se cae nunca?**
- SIGPIPE está ignorado: un cliente que se va a mitad de un `send` no mata el proceso.
- Hay un `try/catch` en cada evento: un fallo solo cierra a ese cliente.
- Los tamaños se limitan antes de guardar datos (413, 431).
- Los timeouts impiden conexiones colgadas.
- La config se valida al arrancar.

**¿Por qué C++98?**
Lo exige el subject (`-std=c++98`). No hay `auto`, `nullptr`, `std::move` ni lambdas. Por ejemplo, para no copiar bodies grandes se usa `std::string::swap()` en lugar de `std::move`.

**¿Qué hace `-MMD -MP` en el Makefile?**
Genera ficheros `.d` con las cabeceras que incluye cada `.cpp`, de modo que si cambia un `.hpp` se recompilan los `.cpp` que lo usan. No provoca relink innecesario.

---

## 12c. Modificaciones en directo que os pueden pedir

El subject avisa: *"a brief modification of the project may be requested… a minor behaviour change, a few lines of code"*. Estas son las más probables y dónde se hacen. **Practicadlas antes.**

| Os piden… | Dónde | Qué cambiar |
|---|---|---|
| Cambiar un timeout | [inc/Server.hpp](../inc/Server.hpp) / [inc/CgiHandler.hpp](../inc/CgiHandler.hpp) | `CLIENT_TIMEOUT`, `BODY_TIMEOUT`, `CGI_TIMEOUT`. También sin tocar código: `make re FLAGS=-DCGI_TIMEOUT=5` |
| Añadir un tipo MIME (p. ej. `.mp4`) | [src/otherUtilities.cpp](../src/otherUtilities.cpp), `getMimeType` | Añadir `{"ext", "tipo/mime"}` a la tabla `types` |
| Añadir un código de estado | [src/HttpResponse.cpp](../src/HttpResponse.cpp), `reasonPhrase`; [inc/HttpResponse.hpp](../inc/HttpResponse.hpp); [src/RequestHandler.cpp](../src/RequestHandler.cpp), `buildError` | Un `case` en `reasonPhrase`, un `ERROR_xxx` con su mensaje y un `case` en `buildError` |
| Aceptar un método nuevo (p. ej. PUT) | [src/RequestHandler.cpp](../src/RequestHandler.cpp), `handle` | Añadirlo a la comprobación del paso 2 (si no, da 501) y decidir qué hace en el paso 7 |
| Añadir una cabecera a todas las respuestas (p. ej. `Server: webserv`) | [src/switchMode.cpp](../src/switchMode.cpp), `queueResponse` | `response.setHeader("Server", "webserv");` antes de `serialize()` |
| Una directiva nueva en la config | [src/Configuration.cpp](../src/Configuration.cpp), `parseLocationBlock` o `parseServerBlock`; [inc/Configuration.hpp](../inc/Configuration.hpp) | Un campo nuevo en `LocationConfig`/`ServerConfig`, su getter y un `else if (directive == "...")` que lo lea con `nextValue` |
| Cambiar la config por defecto | [app/main.cpp](../app/main.cpp) | `#define DEFAULT_CONFIG_PATH` |
| Cambiar el límite de cabeceras | [inc/HttpRequest.hpp](../inc/HttpRequest.hpp) | `MAX_HEADER_SIZE`, `MAX_HEADER_LINE` |
| Cambiar qué se imprime en el log | [src/switchMode.cpp](../src/switchMode.cpp), `logResponse` | El `std::cout` de esa función |
| Otro tipo de CGI (p. ej. shell) | Solo config | `cgi .sh /bin/sh;` en la location y un script `.sh` |
| Cambiar la página de error de un código | Solo config | `error_page 404 www/err/mi404.html;` |

**Cómo probar una modificación:** `make` (solo recompila lo que ha cambiado) → `./webserv` → `curl -i …`. Y, si da tiempo, `python3 tst/tester.py` para comprobar que no se ha roto nada.

---

## 13. Chuleta rápida (para tenerla a mano)

**Todas las demostraciones con curl, de una vez:** `bash tst/curl_tester.sh`. Imprime cada comando antes de ejecutarlo y marca ✔/✘ (32 pruebas en el orden de la hoja; `--fast` omite la del CGI infinito).

**Las pruebas "con telnet", de una vez:** `python3 tst/telnet_tester.py`. Muestra cada línea que se escribiría en telnet (`>`) y la respuesta (`<`): keep-alive, HTTP/1.0, sin `Host`, petición mal escrita, método desconocido, POST a mano, pipelining, letra a letra, **un cliente a medias que no bloquea a los demás**, 408, 431 y el segundo sitio (20 pruebas).

Para repetirlas a mano: `telnet 127.0.0.1 8080` (en macOS: `brew install telnet`) o `nc -c 127.0.0.1 8080` (el `-c` hace que Enter envíe `\r\n`, como pide HTTP).

```sh
make re && ./webserv                                   # arrancar (etc/default.conf)
bash tst/check_single_poll.sh                          # un solo multiplexor
curl -i -X FOO http://localhost:8080/                  # 501
curl -i -X DELETE http://localhost:8080/index.html     # 405 + Allow
curl -i -X POST --data "$(printf 'x%.0s' $(seq 200))" http://127.0.0.1:8081/   # 413
curl -i http://localhost:8080/old                      # 301
curl -i http://localhost:8080/uploads/                 # autoindex
curl -i 'http://localhost:8080/cgi/example.py?a=1'     # CGI GET
curl -i -d a=1 http://localhost:8080/cgi/example.py    # CGI POST
siege -b -c 50 -t 30S http://127.0.0.1:8080/empty.html # estrés
leaks --atExit -- ./webserv                            # fugas (salir con Ctrl+C)
```

---

## 14. Flujo completo paso a paso (dónde está cada cosa)

El camino de una petición, desde `./webserv` hasta que la respuesta sale por el socket. Los números de línea son aproximados: si cambian, busca la función con `grep -n "nombreFuncion" src/*.cpp`.

### A. Arranque (una sola vez)

| # | Qué pasa | Dónde |
|---|---|---|
| 1 | Se ignora SIGPIPE; Ctrl+C/SIGTERM solo activan un flag | `app/main.cpp:28-30`, `onShutdownSignal` (l.11) |
| 2 | Se elige el config: el argumento o `etc/default.conf` | `app/main.cpp` (`DEFAULT_CONFIG_PATH`) |
| 3 | Se parsea el config: tokens → bloques `server` → bloques `location` | `Configuration::parse` (`src/Configuration.cpp:312`) → `tokenize` (152), `parseServerBlock` (279), `parseLocationBlock` (210) |
| 4 | Se valida (puertos, tamaños, CGI con ruta absoluta, `listen` duplicado → error) | dentro de `parse` y los helpers `parseIPv4`, `parsePort`, `parseSize` |
| 5 | Resumen con colores de lo cargado | `Configuration::print` (393) |
| 6 | Un socket por cada `listen`: `socket` → `SO_REUSEADDR` → `bind` → `listen` → no bloqueante | `Server::setupListeningSockets` (`src/Server.cpp:62`; socket 75, setsockopt 80, bind 93, listen 100) |

### B. El bucle de eventos (el único sitio donde se espera)

| # | Qué pasa | Dónde |
|---|---|---|
| 7 | Se crea el multiplexor y se registran los sockets de escucha | `Server::run` (`src/Server.cpp:295`): kqueue l.297-381 (macOS), epoll l.381-466 (Linux) |
| 8 | Espera de eventos con 0,5 s de timeout | `kevent` (l.325) / `epoll_wait` (l.409) |
| 9 | Reparto de cada evento: socket de escucha → accept; pipe de CGI → `handleCgiEvent`; cliente legible → `handleClientRead`; cliente escribible → `handleClientWrite`; error/EOF → `handleClientDisconnect` | el `for` de eventos dentro de `run` |
| 10 | Tras cada vuelta: timeouts y recogida de hijos | `closeIdleClients` (l.378 / 463) |
| 11 | Si llegó Ctrl+C: se sale del bucle y se cierra todo | `Server::shutdown` (`src/Server.cpp:271`) |

### C. Una petición, de principio a fin

| # | Qué pasa | Dónde |
|---|---|---|
| 12 | `accept()` del nuevo cliente, no bloqueante, se registra para lectura y se crea su `ClientSession` | `Server::handleNewConnection` (`src/Server.cpp:123`); `ClientSession` en `inc/Server.hpp` |
| 13 | **Un solo** `recv()`; `<= 0` → se cierra el cliente; si no, se añade a `request_buffer` | `Server::handleClientRead` (`src/switchMode.cpp:23`, recv l.31) |
| 14 | Límite de body que toca (el de la location o el del server) | `maxBodySizeFor` (`src/obtainServerConfigToRequest.cpp:92`) |
| 15 | ¿Ha llegado la petición entera? (`\r\n\r\n` + Content-Length, o el chunk `0`) | `HttpRequest::isCompleteRequest` (`src/RequestChecks.cpp:233`); chunked incremental: `scanChunked` (172) |
| 16 | Si aún no: ¿hay que cortar ya? cabeceras > 16 KB → 431, Content-Length inválido → 400, body > límite → 413 (y se cierra la conexión) | `HttpRequest::earlyErrorCode` (`src/RequestChecks.cpp:286`), llamado desde `processRequestBuffer` |
| 17 | Si está completa: se separa del buffer (pipelining: lo que sobra se queda) | `Server::processRequestBuffer` (`src/switchMode.cpp:47`) |
| 18 | Parseo: línea de petición (método, URI, versión; 414, 400, 505, path traversal) | `parse_request_line` (`src/parseRequest.cpp:66`), `urlDecode`, `isSafePath` |
| 19 | Parseo: cabeceras (nombre token, valor sin caracteres de control, líneas > 8 KB → 431, Host obligatorio en 1.1) y body (decodifica chunked) | `HttpRequest::parse` (`src/HttpRequest.cpp:337`) → `parseMessage` (280) → `parseHeaderLine` (219); `decodeChunked` (`src/RequestChecks.cpp:109`) |
| 20 | Routing: location con el prefijo más largo; root sustituye el prefijo; carpeta de uploads | `applyRequestConfiguration` (`src/obtainServerConfigToRequest.cpp:61`) → `findMatchingLocation` (3); `resolveUploadDirectory` (34) |
| 21 | Decisión, **en este orden**: error de parseo → método no implementado (501) → sin location (404) → método no permitido (405 + `Allow`) → redirect → CGI por extensión → GET/HEAD → DELETE → POST con upload → si no, 405 | `RequestHandler::handle` (`src/RequestHandler.cpp:46`) |

### D. Las ramas de `handle`

| Rama | Qué hace | Dónde |
|---|---|---|
| GET/HEAD | ¿fichero o directorio? | `handleGet` (`src/RequestHandler.cpp:269`) |
| Directorio | `index` si existe → si no, `autoindex` (301 si falta la `/`) → si no, 404 | `handleDirectory` (291); `autoindexHtml` (397), `autoindexJson` (435, con `Accept: application/json`) |
| Fichero | lee del disco con `ifstream`, pone `Content-Type` | `serveFile` (339); `readFile`, `getMimeType` (`src/otherUtilities.cpp:82`, 48) |
| DELETE | solo ficheros normales: 200 / 404 / 403 | `handleDelete` (`src/RequestHandler.cpp:118`) |
| POST upload | multipart (todos los ficheros) o body crudo; 409 si ya existe | `handleUploadRequest` (`src/handleUpload.cpp:228`) → `handleFileUpload` (163) → `parseMultipartFormData`, `saveFileToDisk` |
| Redirect | código + cabecera `Location` | dentro de `handle` (paso 5) |
| Errores | `error_page` del config → si no, plantilla `www/err/template.html` → si no, HTML mínimo | `buildError` (172), `handleError` (201) |

### E. CGI (sin bloquear el servidor)

| # | Qué pasa | Dónde |
|---|---|---|
| 22 | `handle` solo marca "esto es CGI" (script + intérprete del config) | `RequestHandler.cpp`, paso 6 de `handle`; `wantsCgi` (157) |
| 23 | El Server lo lanza y registra los dos pipes en el mismo kqueue/epoll | `Server::startCgi` (`src/cgiEvents.cpp:55`) → `watchFd` (23) |
| 24 | Comprueba el script (404/403), crea pipes no bloqueantes, `fork`; el hijo hace `dup2`, cierra fds, `chdir` al directorio del script y `execve` del intérprete | `CgiHandler::start` (`src/CgiHandler.cpp:218`; fork 260, dup2 270, chdir 289, execve 299); variables de entorno: `setupEnvironment` (41) |
| 25 | Cada evento del pipe: **un** `write` del body al stdin del script o **un** `read` de su salida | `Server::handleCgiEvent` (`src/cgiEvents.cpp:91`; write 102, read 120) |
| 26 | `read() == 0` = el script terminó: se parsean sus cabeceras (`Status`, `Content-Type`) y se arma la respuesta; sin salida → 502 | `finishCgi` (151) → `CgiHandler::parseOutput` (`src/CgiHandler.cpp:139`) |
| 27 | Limpieza y hijos: `waitpid(WNOHANG)`; más de 10 s sin I/O → kill + 504 | `cleanupCgi` (164), `reapChildren` (181); timeout en `closeIdleClients` |

### F. Envío de la respuesta

| # | Qué pasa | Dónde |
|---|---|---|
| 28 | Se serializa la respuesta (en HEAD sin body), se pone `Connection: close` si toca y se cambia el socket a escritura | `Server::queueResponse` (`src/switchMode.cpp:166`) → `HttpResponse::serialize` (`src/HttpResponse.cpp:136`); `_switchToWriteMode` (279) |
| 29 | **Un solo** `send()` por evento, desde el offset `bytes_sent`; `<= 0` → se cierra el cliente | `Server::handleClientWrite` (`src/switchMode.cpp:223`, send l.237) |
| 30 | Terminado: si `close_after_send` → cerrar; si no → volver a lectura y procesar lo que quedó en el buffer (pipelining) | final de `handleClientWrite`; `_switchToReadMode` (296) |
| 31 | Línea de log con colores (método, ruta, código, motivo del error) | `logResponse` (`src/switchMode.cpp:186`) |

### G. Cierres y timeouts

| Caso | Dónde |
|---|---|
| Cliente cierra o error en el socket (mata también su CGI) | `Server::handleClientDisconnect` (`src/Server.cpp:171`) |
| 60 s sin actividad → se cierra; body a medias 2 s → 408; CGI 10 s → 504 | `Server::closeIdleClients` (`src/Server.cpp:199`); constantes en `inc/Server.hpp` y `inc/CgiHandler.hpp` |
| Ctrl+C → cierre ordenado de clientes, CGIs y sockets | `Server::shutdown` (`src/Server.cpp:271`) |

### H. "Si me piden X, voy a…"

| Me piden… | Voy a… |
|---|---|
| El poll / kqueue / epoll | `Server::run` (`src/Server.cpp:295`) |
| Todos los read/recv/write/send | `grep -n "recv(\|send(\|read(\|write(" src/*.cpp` → `switchMode.cpp:31`, `switchMode.cpp:237`, `cgiEvents.cpp:102`, `cgiEvents.cpp:120` |
| Dónde se cierra un cliente por error de I/O | `handleClientRead` / `handleClientWrite` → `handleClientDisconnect` |
| accept | `handleNewConnection` (`src/Server.cpp:123`) |
| Cambiar un código de error o su página | `buildError` / `handleError` (`src/RequestHandler.cpp:172`, 201) |
| Añadir/quitar un método | `RequestHandler::handle`, pasos 2 y 7 |
| Body demasiado grande (413) | `earlyErrorCode` + `maxBodySizeFor` |
| Chunked | `scanChunked` / `decodeChunked` (`src/RequestChecks.cpp`) |
| Cabeceras, Host, versión | `parseHeaderLine`, `parse` (`src/HttpRequest.cpp`); `parse_request_line` (`src/parseRequest.cpp`) |
| Qué location se elige | `findMatchingLocation` (`src/obtainServerConfigToRequest.cpp:3`) |
| index / autoindex | `handleDirectory` (`src/RequestHandler.cpp:291`) |
| Uploads | `src/handleUpload.cpp` |
| DELETE | `handleDelete` (`src/RequestHandler.cpp:118`) |
| fork / execve / variables del CGI | `CgiHandler::start` / `setupEnvironment` (`src/CgiHandler.cpp`) |
| Que el CGI no bloquea | `startCgi` + `handleCgiEvent` (`src/cgiEvents.cpp`) |
| Timeouts | `closeIdleClients`; constantes `CLIENT_TIMEOUT`, `BODY_TIMEOUT`, `CGI_TIMEOUT` |
| Una directiva nueva del config | `parseServerBlock` / `parseLocationBlock` (`src/Configuration.cpp`) + su getter en `inc/Configuration.hpp` |
| keep-alive / Connection: close | `processRequestBuffer` (cálculo de `close_connection`) y final de `handleClientWrite` |
| MIME types | `getMimeType` (`src/otherUtilities.cpp:48`) |
