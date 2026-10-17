# Testers de webserv

El subject pide: *"Do not test with only one program. Write your tests in a more suitable language, such as Python or Golang"* y *"Stress test your server to ensure it remains available at all times"*.

Hay dos testers en `tst/`, escritos en Python 3 con solo la biblioteca estándar (no hay que instalar nada):

| Fichero | Para qué |
|---|---|
| [tst/tester.py](../tst/tester.py) | **Tests funcionales**: 51 comprobaciones de comportamiento (códigos, parser, conexiones, upload, CGI, chunked, config…) |
| [tst/stress.py](../tst/stress.py) | **Prueba de estrés**: muchos clientes en paralelo; mide disponibilidad, req/s y latencias |
| [tst/httpclient.py](../tst/httpclient.py) | Cliente HTTP con sockets en bruto que usan los dos |

Usan sockets en bruto, sin `curl` ni `requests`, para poder enviar peticiones mal formadas, varias peticiones en un solo paquete, bytes de uno en uno, cerrar a mitad de la respuesta, etc.

---

## tester.py — tests funcionales

```sh
make
python3 tst/tester.py              # todos (~20 s)
python3 tst/tester.py -k cgi       # solo los que contienen "cgi" en el nombre o grupo
python3 tst/tester.py -v           # muestra también los detalles de los que pasan
python3 tst/tester.py --slow       # añade el test del timeout de inactividad (+60 s)
python3 tst/tester.py --port 19000 # otros puertos (usa 19000 y 19001)
python3 tst/tester.py --tester42   # simula el tester oficial de 42 (ver más abajo)
```

**No necesitas arrancar el servidor**: el tester lanza su propio `./webserv`, con una configuración temporal, en los puertos **18480 y 18481**, así que no choca con un servidor que tengas abierto. También crea scripts CGI y ficheros temporales en `www/` (todos empiezan por `_t<pid>`) y **los borra al terminar**.

### Resultados

| Estado | Significado |
|---|---|
| `PASS` | Funciona como debe |
| `FAIL` | Algo obligatorio está roto. El tester termina con código 1. |
| `PENDING` | Funcionalidad todavía no implementada. **No** cuenta como fallo. Cuando se implemente, saldrá `PASS` con el texto "implemented!". |
| `CRASH` | **El servidor ha muerto.** En la evaluación eso es un 0. Se muestra el final del log del servidor. |

Después de cada test se comprueba que el proceso del servidor sigue vivo.

### Qué se prueba

| Grupo | Tests |
|---|---|
| **basic** | `GET /` (cabeceras, `Content-Length` correcto, contenido igual al fichero), 404, reason phrases estándar, **dos puertos con distinta config**, redirect 301, 405, páginas de error por defecto |
| **parsing** | Request line basura → 400, falta `Host` → 400, HTTP/1.0 sin `Host` → 200 y cierre, HTTP/2.0 → 505, URI de 3000 caracteres → 414, path traversal (`/../`, `/%2e%2e/`) → 400, **413 sin esperar al body**, cabeceras de más de 16 KB → 400, `%XX` en la ruta |
| **connection** | Keep-alive, pipelining, `Connection: close`, cliente que envía byte a byte, **30 clientes que cierran a mitad de una descarga de 5 MB** (SIGPIPE), clientes que envían media petición y desaparecen, **200 conexiones simultáneas**, timeout de inactividad (`--slow`) |
| **upload** | Subida multipart → descarga → aparece en el listado → DELETE → 404; subida binaria (los 256 valores de byte) intacta; misma subida dos veces → 409 |
| **cgi** | Variables de entorno y directorio de trabajo, body por stdin, **POST de 2,5 MB**, cabecera `Status`, script inexistente → 404, script sin salida → 502, **bucle infinito → 504 sin bloquear al resto**, el script muere si el cliente se va, CGI + keep-alive, 10 CGIs a la vez |
| **chunked** | Body chunked → CGI (stdin + `CONTENT_LENGTH`), chunked troceado en muchos paquetes, upload chunked, chunked demasiado grande → 413, chunked mal formado → 400, otra codificación → 501, chunked + otra petición en la misma conexión |
| **config** | `error_page` de la config, autoindex, directiva `index`, fichero vacío → 200, DELETE inexistente → 404, método desconocido → 501 |
| **final** | No quedan procesos zombie |

### Resultado actual

```
51 passed  ·  0 failed  ·  0 pending
```

Todo lo obligatorio que comprueba el tester funciona. El estado `PENDING` queda disponible para marcar tests de funcionalidades que aún no estén hechas (por ejemplo, del bonus).

### Si un test falla

- El log completo del servidor está en la ruta que se imprime al empezar (`server log: /var/folders/.../server.log`).
- Si te dice que el puerto está ocupado, cierra lo que lo use o lanza el tester con `--port`.
- Para añadir un test, escribe una función con el decorador `@test("grupo")` y usa `expect(...)` / `expect_status(...)`. Si es una funcionalidad aún no hecha, usa `@test("grupo", pending=True)`.

---

## El tester oficial de 42 (`tst/tests_42/`)

42 da un tester (`tst/tests_42/tester`) y un CGI de prueba (`tst/tests_42/cgi_tester`). **Son binarios de Linux**: solo funcionan en los ordenadores de 42.

```sh
bash tst/tests_42/setup.sh                 # crea YoupiBanane/, tester.conf y da permisos
./webserv tst/tests_42/tester.conf         # en una terminal
./tst/tests_42/tester http://localhost:8080   # en otra
```

`setup.sh` deja preparado lo que piden las instrucciones del tester:

| Lo que pide | Cómo queda en `tester.conf` |
|---|---|
| `/` solo GET | `location / { allowed_methods GET; }` (un `HEAD /` da 405) |
| `/post_body` POST con maxBody 100 | `client_max_body_size 100;` **dentro de la location** + `upload_enabled on;` |
| `/directory/` → `YoupiBanane`, index `youpi.bad_extension` | `root .../YoupiBanane; index youpi.bad_extension;` (directorio sin index → 404) |
| `.bla` por POST → `cgi_tester` | `cgi .bla /ruta/absoluta/cgi_tester;` |

El tester también hace POST a `/directory/youpla.bla`, que no aparece en su lista de ficheros. `setup.sh` crea ese fichero para que el CGI se ejecute (sin él, el servidor responde 404).

### Simularlo en macOS: `--tester42`

```sh
python3 tst/tester.py --tester42          # bodies de 10 MB (rápido)
python3 tst/tester.py --tester42 --big    # bodies de 100 MB y 20 clientes, como el tester real
```

Reproduce las mismas 24 pruebas, con un `cgi_tester` falso en Python (devuelve el body en mayúsculas, como el original). Resultado de referencia en un Mac con `--big`: **24/24 en ~28 s**. El POST de 100 MB al CGI tarda ~0,3 s, y el servidor llega a ~4,5 GB de memoria con 20 POST de 100 MB a la vez.

---

## stress.py — prueba de estrés

Este necesita **el servidor ya arrancado**:

```sh
./webserv                                        # en una terminal
python3 tst/stress.py                            # en otra: 50 clientes, 10 s, GET /
python3 tst/stress.py -c 200 -t 30               # 200 clientes durante 30 s
python3 tst/stress.py --no-keepalive             # una conexión nueva por petición
python3 tst/stress.py --path /cgi/example.py     # contra el CGI
python3 tst/stress.py --port 8081
```

Ejemplo de salida:

```
webserv stress test  ·  100 clients  ·  8 s  ·  GET /  ·  keep-alive on

  requests       91279
  throughput     11346 req/s
  availability   100.00 %
  status codes   200: 91279
  latency        p50 8.7 ms  ·  p95 8.9 ms  ·  p99 9.1 ms  ·  max 183.2 ms

  Server still answering after the test (GET / -> 200)
```

**Lo importante es la disponibilidad**: tiene que ser **100 %** (todas las peticiones reciben una respuesta HTTP válida), y el servidor tiene que seguir respondiendo después. Las peticiones por segundo dependen del ordenador y no se evalúan.

Resultados de referencia en un Mac:

| Escenario | req/s | Disponibilidad |
|---|---|---|
| 100 clientes, keep-alive, `GET /` | ~11 300 | 100 % |
| 50 clientes, sin keep-alive | ~9 900 | 100 % |
| 20 clientes contra `/cgi/example.py` | ~180 | 100 % |

### siege

En la evaluación es habitual que usen **siege**:

```sh
brew install siege                     # macOS (en Linux: apt install siege)
siege -b -c 50 -t 30S http://localhost:8080/
```

Lo que miran es que `Availability` sea **100 %** y que el servidor no muera. `stress.py` mide lo mismo sin instalar nada.
