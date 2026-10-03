# Plan de implementación: Registro de actividad autónoma (`cap_activity_log`), issue #4

## 0. Resumen

Se agrega un componente nuevo, `cap_activity_log`, que guarda en la partición DATA un log JSONL acotado y rotado con lo que KrakBot Nano hace de forma autónoma. Expone:

- una API C (`append`, `list_json`);
- dos capabilities para el modelo (`activity_log_list`, `activity_log_append`);
- una skill (`activity_log`).

El scheduler registra cada disparo de forma automática, sin cambiar su semántica. Las escrituras se agrupan en un buffer en RAM, y una tarea propia del componente las vuelca a flash. `append` nunca hace I/O.

> **Precondición:** este plan se escribió sin acceso al repo ni al toolchain. Todo nombre de API externa marcado **(verificar)** es provisorio. La Fase 0 es obligatoria antes de escribir código.

---

## 1. Decisiones tomadas (cierran los puntos abiertos de la spec)

| Tema | Decisión | Motivo |
|---|---|---|
| Buffer lleno | **Se descarta la entrada más vieja del buffer** y se incrementa `dropped`. Además se notifica a la tarea de volcado. | `append` se llama con el lock del scheduler tomado y no puede hacer I/O. Un "volcado forzado" síncrono violaría §5.3. |
| Quién vuelca | Una **tarea FreeRTOS dedicada** (`activity_log_flush`) que espera una notificación con un timeout igual a `FLUSH_INTERVAL_MS`. | Saca el I/O de flash del contexto del llamador. No usa callbacks de `esp_timer`, que no deben bloquear. |
| Disparo anticipado | `append` notifica a la tarea (`xTaskNotifyGive`, no bloqueante) cuando el lote llega a `FLUSH_BATCH` o cuando `result == error`. | Cumple §5.3. Un error se vuelca "lo antes posible" sin hacer I/O en el llamador. Así se mitiga el riesgo de §11. |
| Orden de `list_json` | **Cronológico inverso** (la entrada más nueva primero). | Es el uso típico del agente: "¿qué hice recién?". |
| Resultado del disparo del scheduler | Publicación OK: `result=completed`, `detail="event published"`. Fallo: `result=error`, `detail="publish failed: <esp_err_to_name>"`. | Es el mapeo de §6.3. La semántica se documenta en el header del scheduler y en el README. |
| Límite de `detail` | **200 bytes**, truncados en un límite de carácter UTF-8. Así siempre quedan ≤ 200 caracteres. | Tamaño de buffer fijo y predecible. |
| Serialización | Escritor JSON propio (escape manual), sin cJSON. | Es determinista, sin allocs y con tamaño de línea acotado. |
| Parseo al leer | cJSON (incluido en ESP-IDF; **verificar** que el repo ya lo use) por línea. Las líneas inválidas se saltan. | Tolera corrupción e ignora campos desconocidos sin trabajo extra. |
| Instancia | **Singleton del módulo** con `create/delete/start/stop` explícitos. `append` y `list_json` operan sobre la instancia global. | Lo necesitan el scheduler y las capabilities sin pasarse handles. Hay que **ajustarlo** si los otros `cap_*` usan otro patrón (Fase 0). |
| Hora | `gettimeofday()`. Si el año es < 2020 (sin sincronizar), se usa `esp_timer_get_time()/1000` (ms desde el arranque). **Verificar** cómo lo resuelven otros componentes y usar su helper si existe. | Es el mejor tiempo disponible. Queda documentado como limitación. |

---

## 2. Fase 0: Relevamiento obligatorio (sin escribir código)

Leer y anotar en el PR, en una sección de "Supuestos verificados":

1. `CLAUDE.md`, `.agents/design.md` y `.agents/gotchas.md`. De ahí salen las convenciones de nombres, el patrón OO, el uso de PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` o un wrapper propio), el estilo de logging (`TAG`) y las reglas de locks.
2. Un `cap_*` existente como plantilla, el más parecido (con persistencia en DATA y con capabilities). De él se copian:
   - la estructura de `CMakeLists.txt` (`idf_component_register`, `REQUIRES`/`PRIV_REQUIRES`);
   - el formato de `Kconfig.projbuild`;
   - el descriptor de capability: nombre, descripción, schema de parámetros (JSON string o struct) y firma del callback;
   - cómo devuelve el resultado y los errores al modelo;
   - si usa un handle explícito o una instancia de módulo.
3. `components/common/app_claw/app_capabilities.c`: el orden de inicialización, cómo se registra un `cap_*` y dónde se crean e inician los componentes. Hay que confirmar que `cap_activity_log` puede iniciarse **antes** que `cap_scheduler`.
4. `cap_scheduler.h` y `cap_scheduler.c`:
   - firma y cuerpo de `cap_scheduler_publish_entry_locked`;
   - qué campos de la entrada hay disponibles (id, tipo de schedule);
   - qué mutex se tiene tomado;
   - qué contadores se tocan.
5. El header de `claw_paths`: la firma real de `claw_paths_join` y de `CLAW_PATH_DATA`, cómo se libera el resultado (buffer del llamador o malloc) y si hay helpers de `mkdir`.
6. `claw_memory.h` y `claw_event.h`: solo para alinear el formato de `ref` (ids de tarea y rutas) con lo que ya usa el sistema.
7. El mecanismo de skills: dónde viven las `SKILL.md`, el frontmatter requerido y cómo se descubren (glob en build, embebido o copia a DATA).
8. Los patrones de tests: si existe `components/**/test_apps/` (Unity) o tests Lua, se elige el más cercano como plantilla.
9. FATFS/VFS: cómo se comporta `rename` sobre un destino existente (con FATFS de ESP-IDF falla, `EEXIST`, a confirmar en `.agents/gotchas.md`), si existe `mkdir`, y el tamaño real de la partición `storage` en `partitions*.csv`.

**Salida de la Fase 0:** confirmar o ajustar las decisiones de la §1 y los nombres de la §3. Si algo contradice este plan, gana el repo, y se documenta en el PR.

---

## 3. Archivos

### Nuevos

```
components/claw_capabilities/cap_activity_log/
├── CMakeLists.txt
├── Kconfig.projbuild
├── README.md
├── include/
│   └── cap_activity_log.h
├── src/
│   ├── cap_activity_log.c          # ciclo de vida, append, buffer, tarea de flush
│   ├── cap_activity_log_store.c    # serialización, escape, volcado, rotación, lectura
│   ├── cap_activity_log_caps.c     # capabilities activity_log_list / activity_log_append
│   └── cap_activity_log_priv.h     # structs internos y prototipos compartidos
├── skills/
│   └── activity_log/SKILL.md       # ruta exacta según el mecanismo de descubrimiento (verificar)
└── test_apps/                      # solo si existe el patrón en el repo
    ├── CMakeLists.txt
    ├── main/CMakeLists.txt
    ├── main/test_cap_activity_log.c
    └── sdkconfig.defaults
```

Cada archivo debe quedar bien por debajo de 1500 líneas. Se estiman ~300, ~450 y ~250 líneas.

### Modificados

- `components/common/app_claw/app_capabilities.c`: crear e iniciar el log antes que el scheduler, registrar las capabilities y detenerlo en el teardown (si existe).
- `CMakeLists.txt` de `app_claw`: agregar `cap_activity_log` a `REQUIRES`/`PRIV_REQUIRES`.
- `components/claw_capabilities/cap_scheduler/CMakeLists.txt`: agregar `cap_activity_log` a `PRIV_REQUIRES`.
- `components/claw_capabilities/cap_scheduler/src/cap_scheduler.c` (ruta a verificar): instrumentar `cap_scheduler_publish_entry_locked`.
- `components/claw_capabilities/cap_scheduler/include/cap_scheduler.h`: comentario sobre la semántica de `ESP_OK` (evento publicado, no trabajo terminado).
- Documentación general (README del repo o `.agents/design.md`, según convención): mencionar el componente y el uso de DATA.

### No se tocan

- `claw_core`.
- `claw_event_router`.

---

## 4. Header público (`include/cap_activity_log.h`), propuesta

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Límites por campo (bytes, sin incluir '\0'). Se trunca en límite UTF-8. */
#define CAP_ACTIVITY_LOG_ACTION_MAX   96
#define CAP_ACTIVITY_LOG_REASON_MAX   128
#define CAP_ACTIVITY_LOG_REF_MAX      128
#define CAP_ACTIVITY_LOG_DETAIL_MAX   200
#define CAP_ACTIVITY_LOG_LIST_MAX     50     /* tope duro de 'limit' */
#define CAP_ACTIVITY_LOG_LIST_DEFAULT 20

typedef enum {
    CAP_ACTIVITY_ORIGIN_PROACTIVE_PULSE = 0,
    CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK,
    CAP_ACTIVITY_ORIGIN_MANUAL,
    CAP_ACTIVITY_ORIGIN_MAX,
} cap_activity_origin_t;

typedef enum {
    CAP_ACTIVITY_RESULT_COMPLETED = 0,
    CAP_ACTIVITY_RESULT_SKIPPED,
    CAP_ACTIVITY_RESULT_ERROR,
    CAP_ACTIVITY_RESULT_MAX,
} cap_activity_result_t;

typedef struct {
    const char *base_dir;   /* NULL => claw_paths_join(CLAW_PATH_DATA, "activity") */
    size_t      max_bytes;  /* 0 => CONFIG_CAP_ACTIVITY_LOG_MAX_BYTES */
} cap_activity_log_config_t;

typedef struct {
    uint32_t pending;       /* entradas en buffer sin volcar */
    uint32_t dropped;       /* descartadas por buffer lleno */
    uint32_t flush_errors;  /* fallos de I/O en volcado */
    uint32_t rotations;
} cap_activity_log_stats_t;

/* Ciclo de vida (singleton). */
esp_err_t cap_activity_log_create(const cap_activity_log_config_t *cfg);
esp_err_t cap_activity_log_start(void);
esp_err_t cap_activity_log_stop(void);    /* volcado final */
esp_err_t cap_activity_log_delete(void);  /* stop implícito + libera recursos */

/* No bloqueante, sin I/O. Seguro de llamar con otros locks tomados.
 * Devuelve ESP_ERR_INVALID_STATE si no está iniciado,
 * ESP_ERR_INVALID_ARG si enum inválido u obligatorio NULL/vacío. */
esp_err_t cap_activity_log_append(cap_activity_origin_t origin,
                                  const char *action,
                                  const char *reason,
                                  cap_activity_result_t result,
                                  const char *ref,      /* nullable */
                                  const char *detail);  /* nullable */

/* Últimas 'limit' entradas con ts >= since_ms (y origin si origin_filter >= 0),
 * más nueva primero. Escribe un array JSON en 'out' ('\0'-terminado).
 * ESP_ERR_INVALID_SIZE si 'out' no alcanza (se devuelven las que entren y se
 * reporta 'truncated' vía *out_count/flag; ver README). */
esp_err_t cap_activity_log_list_json(size_t limit, int64_t since_ms,
                                     int origin_filter /* -1 = todos */,
                                     char *out, size_t out_size,
                                     size_t *out_count);

esp_err_t cap_activity_log_get_stats(cap_activity_log_stats_t *stats);

/* Conversión enum <-> string. */
const char *cap_activity_origin_to_str(cap_activity_origin_t o);
esp_err_t   cap_activity_origin_from_str(const char *s, cap_activity_origin_t *out);
const char *cap_activity_result_to_str(cap_activity_result_t r);
esp_err_t   cap_activity_result_from_str(const char *s, cap_activity_result_t *out);

#ifdef __cplusplus
}
#endif
```

Notas:

- Respecto de la spec, se agregó un `origin_filter` a `list_json`, para que la capability filtre sin duplicar la lógica. El filtro se aplica **antes** de contar `limit`.
- La salida es un buffer del llamador, salvo que la Fase 0 muestre que la convención del repo es devolver `cJSON*` o un `char*` alocado. En ese caso se adapta.
- Los nombres (`cap_activity_log_*`, `_t`, `_handle_t`) se ajustan a la convención real.

---

## 5. Kconfig (`Kconfig.projbuild`)

```
menu "Activity log (cap_activity_log)"
    config CAP_ACTIVITY_LOG_MAX_BYTES
        int "Tamaño máximo del segmento activo (bytes)"
        default 262144
        range 16384 1048576
        help
            Peor caso en disco: 2 x este valor (activo + activity.1.jsonl).
    config CAP_ACTIVITY_LOG_FLUSH_INTERVAL_MS
        int "Intervalo de volcado a flash (ms)"
        default 60000
        range 5000 600000
    config CAP_ACTIVITY_LOG_FLUSH_BATCH
        int "Entradas que disparan volcado anticipado"
        default 16
        range 1 128
    config CAP_ACTIVITY_LOG_BUFFER_ENTRIES
        int "Capacidad del buffer en RAM (entradas)"
        default 32
        range 4 256
        help
            Debe ser >= FLUSH_BATCH. Al llenarse se descarta la más vieja (contador 'dropped').
    config CAP_ACTIVITY_LOG_FLUSH_ON_ERROR
        bool "Volcar lo antes posible cuando result=error"
        default y
    config CAP_ACTIVITY_LOG_TASK_STACK
        int "Stack de la tarea de volcado"
        default 4096
endmenu
```

- `create` valida `BUFFER_ENTRIES >= FLUSH_BATCH`. Si no se cumple, loguea un warning y usa `FLUSH_BATCH = BUFFER_ENTRIES`.
- Memoria del buffer: cada entrada ocupa ~ 8 + 1 + 1 + 97 + 129 + 129 + 201 ≈ **570 B**, así que con 32 entradas son ~18 KB. Se aloca en PSRAM si la convención del repo lo permite; si no, en RAM interna. Se decide en la Fase 0.

---

## 6. Diseño interno

### 6.1 Estado del módulo (`cap_activity_log_priv.h`)

```c
typedef struct {
    int64_t ts_ms;
    uint8_t origin, result;
    char action[CAP_ACTIVITY_LOG_ACTION_MAX + 1];
    char reason[CAP_ACTIVITY_LOG_REASON_MAX + 1];
    char ref[CAP_ACTIVITY_LOG_REF_MAX + 1];       /* "" = ausente */
    char detail[CAP_ACTIVITY_LOG_DETAIL_MAX + 1]; /* "" = ausente */
} activity_entry_t;

typedef struct {
    SemaphoreHandle_t buf_mutex;   /* protege ring + stats; secciones cortas, sin I/O */
    SemaphoreHandle_t file_mutex;  /* serializa volcado/rotación vs. lectura */
    activity_entry_t *ring; size_t cap, head, count;
    TaskHandle_t flush_task; volatile bool running;
    char dir[...], path_active[...], path_old[...];
    size_t active_size;            /* cacheado; se recalcula con stat() al start */
    size_t max_bytes;
    cap_activity_log_stats_t stats;
} activity_log_t;
```

### 6.2 Orden de locks (anti-deadlock)

- El orden global es: **lock del scheduler → `buf_mutex`**. `buf_mutex` es una hoja: con él tomado no se llama a nada externo y no se toma ningún otro lock.
- `file_mutex` nunca se toma junto con `buf_mutex`. El volcado copia el lote bajo `buf_mutex`, lo suelta y recién después toma `file_mutex` para hacer I/O.
- `list_json` toma `file_mutex` para leer los archivos. Después toma `buf_mutex` brevemente, aparte, para copiar las entradas pendientes.
- `cap_activity_log` no llama al scheduler ni al router, así que no puede haber un ciclo.
- `append` toma `buf_mutex` con `xSemaphoreTake(..., pdMS_TO_TICKS(5))`. Si hay timeout, incrementa `dropped` y devuelve `ESP_ERR_TIMEOUT`. Nunca bloquea indefinidamente con el lock del scheduler tomado.

### 6.3 `append` (sin I/O)

1. Si `!running`, devuelve `ESP_ERR_INVALID_STATE`.
2. Valida: `origin < MAX`, `result < MAX`, y `action`/`reason` no NULL ni `""`. Si algo falla, `ESP_ERR_INVALID_ARG`.
3. Arma la entrada en el stack **fuera del lock**:
   - `ts` con `activity_now_ms()`;
   - copia truncada con `utf8_safe_copy(dst, src, max)`, que retrocede mientras `(byte & 0xC0) == 0x80` para no dejar un multibyte incompleto.
4. Toma `buf_mutex`:
   - si `count == cap`, descarta la más vieja (`head++`) y hace `dropped++`;
   - copia la entrada al ring y actualiza `pending`;
   - calcula `need_flush = count >= FLUSH_BATCH || (FLUSH_ON_ERROR && result == ERROR)`.
5. Suelta el mutex. Si `need_flush`, hace `xTaskNotifyGive(flush_task)`.
6. Devuelve `ESP_OK`.

### 6.4 Tarea de volcado

```
loop:
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS))
  flush_pending()
  if (!running) break
```

`flush_pending()` hace lo siguiente:

1. Bajo `buf_mutex`, mueve hasta `FLUSH_BATCH` (o todas) las entradas a un array local o un scratch estático. Después suelta el mutex.
2. Serializa cada entrada en `line_buf` (scratch estático del módulo, de tamaño `LINE_MAX`, ver §6.6) y suma `batch_bytes`.
3. Bajo `file_mutex`:
   - Si `active_size + batch_bytes > max_bytes`, rota:
     1. `unlink(path_old)`, ignorando `ENOENT`;
     2. `rename(path_active, path_old)`;
     3. `active_size = 0` y `rotations++`.
   - Si la rotación falla, loguea y sigue agregando al activo. Ese segmento puede pasarse del tope una vez y se reintenta en el próximo volcado.
   - `fopen(path_active, "a")`, un `fwrite` por línea (o concatenado en el scratch) y `fclose`. **Sin `fsync`.**
   - Si hay un fallo de I/O, `flush_errors++`. Las entradas de ese lote **se pierden**: se documenta y no se reencolan, para no hacer bucles sobre una flash dañada.
4. Si quedan más pendientes, repite.

`stop`:

1. `running = false` y `xTaskNotifyGive`.
2. Espera a que la tarea termine (semáforo o event group de "done", con timeout).
3. Hace un `flush_pending()` final desde el contexto de `stop` para cubrir carreras.

`delete` llama a `stop` si hace falta y libera el ring, los mutex y los paths.

> **Atomicidad de la rotación:** puede haber un corte entre `unlink(old)` y `rename(active→old)`. Ese corte es inocuo: queda solo `activity.jsonl`. Un corte después de `rename` también lo es: queda solo `activity.1.jsonl` y el activo se recrea con `"a"`. Ninguna secuencia deja un archivo parcial por la rotación. Se documenta el comportamiento de FATFS verificado en la Fase 0.

### 6.5 `start`: inicialización tolerante

1. Resolver `dir` con `claw_paths_join(CLAW_PATH_DATA, "activity")` (**verificar** la firma) y `mkdir(dir, 0755)`, tolerando `EEXIST`.
2. `stat(path_active)`: si no existe, `active_size = 0`; si existe, toma su tamaño.
3. No se repara el archivo. Una última línea truncada se ignora al leer.
4. **Caso a cubrir:** si la última línea quedó sin `\n` por un corte, la próxima escritura la concatenaría con una línea válida. Para evitarlo, en `start` se lee el último byte del archivo (`fseek(-1, SEEK_END)`). Si no es `\n`, se marca `need_leading_nl = true` y el primer volcado antepone `\n`. Con eso la línea corrupta queda aislada y se descarta al listar.
5. Crear los mutex y la tarea de volcado, con prioridad baja y el stack de Kconfig.

### 6.6 Serialización (`cap_activity_log_store.c`)

- `json_escape_append(dst, cap, src)`:
  - escapa `"` como `\"`, `\` como `\\`, `\n`, `\r`, `\t`, `\b` y `\f`;
  - el resto de los caracteres `< 0x20` (y `0x7F`) van como `\u00XX`;
  - los bytes UTF-8 ≥ 0x80 se copian tal cual.
- **Garantía de una sola línea:** después del escape no queda ningún `\n` crudo.
- Orden fijo de las claves: `ts, origin, action, reason, result[, ref][, detail]`. `ref` y `detail` se omiten si están vacíos.
- `LINE_MAX` sale de una constante calculada: `64 (claves + ts + enums) + 6 × (ACTION_MAX + REASON_MAX + REF_MAX + DETAIL_MAX) + 2 ≈ 3.4 KB`. Es el peor caso teórico, con todos los caracteres escapados como `\u00XX`. Como el scratch es estático y único, porque solo lo usa la tarea de volcado, este tamaño es aceptable. Si se quiere ahorrar, se aloca en PSRAM.

### 6.7 Lectura y `list_json`

**Estrategia:** recorrido hacia adelante, línea por línea, conservando un ring con las últimas `limit` coincidencias. No se carga el archivo completo.

1. `limit` se acota: `0` pasa a `LIST_DEFAULT` y cualquier valor `> LIST_MAX` se recorta a `LIST_MAX`.
2. Se reserva un ring de resultados de `limit × sizeof(activity_entry_t)` (≤ 50 × 570 ≈ 28 KB), en PSRAM si está disponible.
3. Bajo `file_mutex`, se procesa en orden cronológico `activity.1.jsonl` y luego `activity.jsonl`. Cada uno se lee con `fgets` sobre `line_buf` de lectura (`LINE_MAX`):
   - si la línea no terminó en `\n` y no es EOF (línea demasiado larga), se descarta el resto hasta `\n` y se salta la línea;
   - si `cJSON_Parse` falla, falta un campo obligatorio, un enum no se reconoce o el tipo no corresponde, se salta la línea (contador de debug);
   - los campos desconocidos se ignoran;
   - si `ts >= since_ms` y el filtro de origen coincide, la entrada se empuja al ring de resultados, sobrescribiendo la más vieja.
   - **Optimización opcional:** si `activity.jsonl` por sí solo ya aporta `limit` coincidencias, no hace falta leer `.1`. Pero como se lee primero `.1` para mantener el orden cronológico, se deja la lectura completa. El costo está acotado a 512 KB y no es una ruta caliente.
4. Se suelta `file_mutex`. Bajo `buf_mutex`, se copian las pendientes del buffer, que son las más nuevas, aplicando el mismo filtro.
5. La salida es un array JSON en orden **inverso**, de la más nueva a la más vieja. Se reutiliza el mismo escritor de §6.6, que es mínimo, sin cJSON para la salida. Si `out_size` no alcanza, se cierra el array con las entradas que entraron y se devuelve `ESP_ERR_INVALID_SIZE` junto con `*out_count`.
6. Se libera el ring.

---

## 7. Capabilities (`cap_activity_log_caps.c`)

Siguen el descriptor real del repo (Fase 0). La lógica es la siguiente.

### `activity_log_list`

- Schema:
  - `limit`: integer, opcional, default 20, máximo 50;
  - `origin`: string enum `["proactive_pulse","scheduled_task","manual"]`, opcional;
  - `since_ms`: integer, opcional, default 0.
- Callback:
  1. Parsea y valida. Un `origin` inválido devuelve el error `"invalid origin: <v>; expected proactive_pulse|scheduled_task|manual"`.
  2. Llama a `cap_activity_log_list_json` con un buffer de salida de 8 KB (o el tope de respuesta que use el repo).
  3. Devuelve `{"entries":[...],"count":N,"truncated":bool,"dropped":D}`, o el formato que use el repo.
- Si el log no está iniciado, devuelve el error `"activity log not available"`.

### `activity_log_append`

- Schema:
  - obligatorios: `origin`, `action`, `reason`, `result` (enum `["completed","skipped","error"]`);
  - opcionales: `ref`, `detail`.
- La descripción para el modelo menciona el límite de 200 caracteres de `detail` y que no hay que incluir secretos.
- Callback:
  1. Valida con `*_from_str`.
  2. Llama a `cap_activity_log_append`.
  3. Mapea los errores a mensajes claros: `INVALID_ARG` incluye qué campo falló; `INVALID_STATE` es "log no disponible"; `TIMEOUT` es "log ocupado, reintentar".
  4. En el éxito devuelve `{"ok":true}` y aclara que la entrada queda pendiente de volcado (visible enseguida en `activity_log_list`).
- `cap_activity_log_register_capabilities(...)` (nombre y firma según el patrón) se invoca desde `app_capabilities.c`.

---

## 8. Instrumentación del scheduler

En `cap_scheduler_publish_entry_locked`, el código queda así (es ilustrativo, los nombres son a verificar):

```c
esp_err_t err = /* publicación existente, sin cambios */;

/* Registro del DISPARO (no del resultado del trabajo). Best-effort:
 * nunca altera 'err' ni contadores. append es no bloqueante y sin I/O,
 * seguro con el lock del scheduler tomado (buf_mutex es lock hoja). */
{
    char reason[64], detail[64];
    snprintf(reason, sizeof(reason), "schedule %s", <tipo_o_id_entry>);
    if (err == ESP_OK) {
        snprintf(detail, sizeof(detail), "event published");
    } else {
        snprintf(detail, sizeof(detail), "publish failed: %s", esp_err_to_name(err));
    }
    esp_err_t lerr = cap_activity_log_append(CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK,
                                             "scheduled task fired",
                                             reason,
                                             err == ESP_OK ? CAP_ACTIVITY_RESULT_COMPLETED
                                                           : CAP_ACTIVITY_RESULT_ERROR,
                                             <entry_id>, detail);
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "activity log append failed: %s", esp_err_to_name(lerr));
    }
}
return err; /* idéntico al original */
```

Reglas:

- Se inserta después de la publicación y de la actualización de contadores, y antes del `return`. No se reordena nada.
- Si la función tiene varios puntos de retorno, se reestructura con un único `goto out`/helper **solo** si no cambia el flujo. Si no, se llama en cada rama relevante.
- `ref` es el id de la entrada del scheduler, el mismo que usa la memoria y el estado (verificar qué id es estable).
- Se agrega el comentario en `cap_scheduler.h` sobre la función o la API pública: "`ESP_OK` = evento publicado, no trabajo terminado. El resultado final lo registra el agente vía `activity_log_append`."
- **Dependencia:** `cap_scheduler` pasa a `PRIV_REQUIRES cap_activity_log`. Hay que verificar que no se cree una dependencia circular, porque `cap_activity_log` no depende de `cap_scheduler`.
- **Orden de arranque:** el log se inicia antes que el scheduler. Igual, si no está iniciado, `INVALID_STATE` se ignora en silencio.

---

## 9. Skill `activity_log`

Va en `skills/activity_log/SKILL.md`, o donde indique el mecanismo de descubrimiento, con el frontmatter que usen las otras skills. Contenido:

1. **Propósito:** dejar un registro de lo hecho de forma autónoma: qué, cuándo, por qué y con qué resultado.
2. **Cuándo registrar** con `activity_log_append`:
   - al terminar toda ejecución de un pulso proactivo (`origin=proactive_pulse`);
   - al terminar el trabajo de una tarea programada (`origin=scheduled_task`, con `ref` = id de la tarea);
   - cuando se decide **no** actuar (`result=skipped`, con `reason` que explique por qué);
   - ante cualquier error (`result=error`, con un `detail` breve).
   - Las acciones pedidas explícitamente por el usuario que conviene trazar usan `origin=manual`.
3. **Aclaración:** el disparo del scheduler ya deja una entrada automática ("scheduled task fired"). El agente agrega **otra** entrada con el resultado final, usando el mismo `ref`.
4. **Cómo escribir cada campo:**
   - `action` y `reason` concisos, en una línea;
   - `result` honesto;
   - `ref` con el id de la tarea o la ruta del archivo generado;
   - `detail` de ≤ 200 caracteres.
5. **Cuándo consultar** con `activity_log_list`:
   - antes de repetir una acción, para ver si ya se hizo (filtrar por `origin` o por `since_ms`);
   - cuando el usuario pregunta qué hizo el bot.
6. **Qué no registrar:** secretos, tokens, credenciales, contenido de mensajes privados ni datos personales sensibles.
7. **Ejemplo:**
   > Detecté una reunión con Cliente X mañana → `origin=proactive_pulse`, `action="Preparar ficha y briefing de Cliente X"`, `reason="Reunión en calendario para mañana"`, `result=completed`, `ref="<ruta del archivo de briefing>"`.

   Ejemplo de `skipped`:
   > `action="Revisar correo"`, `reason="Sin mensajes nuevos desde el último pulso"`, `result=skipped`.

---

## 10. Tests

Solo se agregan si existe el patrón (`test_apps` con Unity o Lua). **No se ejecutan en NightShift.**

Como base FS se usa la misma que los `test_apps` existentes: FATFS en una partición de test o un directorio temporal. Los tests usan `cap_activity_log_config_t.base_dir` y un `max_bytes` chico (por ejemplo, 4 KB) para forzar la rotación.

| # | Test | Verifica |
|---|---|---|
| 1 | `escape_special_chars` | `"`, `\`, `\n`, `\r`, `\t`, `\x01`. La línea resultante no tiene `\n` interno y cJSON la parsea igual al original. |
| 2 | `detail_truncate_utf8` | Un `detail` de 250 bytes con multibyte (`ñ`, emoji) en el borde: queda ≤ 200 bytes y es UTF-8 válido. |
| 3 | `invalid_args` | Un enum fuera de rango y `action`/`reason` NULL o `""` devuelven `ESP_ERR_INVALID_ARG`. |
| 4 | `not_started` | `append` antes de `start` devuelve `ESP_ERR_INVALID_STATE` sin crash. |
| 5 | `enum_str_roundtrip` | `to_str` y `from_str` para todos los valores, y un string inválido devuelve error. |
| 6 | `missing_dir_and_file` | Sin directorio, `start` lo crea y `list` devuelve `[]`. |
| 7 | `corrupt_last_line` | Se escribe un archivo con 2 líneas válidas y una truncada. `list` devuelve 2. Tras un `append` y un volcado, la nueva línea es legible (prefijo `\n`). |
| 8 | `unknown_fields_ignored` | Una línea con un campo extra se lista correctamente. |
| 9 | `rotation` | Con appends hasta pasar `max_bytes`, existe `.1`, la suma de tamaños es ≤ 2 × `max_bytes` + `LINE_MAX` y una segunda rotación reemplaza `.1`. |
| 10 | `list_limit_since_origin` | Respeta `limit`, el tope `LIST_MAX`, `since_ms`, el filtro de origen y el orden inverso. |
| 11 | `list_includes_pending` | Una entrada sin volcar aparece en `list`. |
| 12 | `list_across_segments` | El resultado combina `.1` y el activo en el orden correcto. |
| 13 | `no_write_before_batch` | Con un intervalo largo y menos de `FLUSH_BATCH` entradas, el tamaño del archivo no cambia. Al llegar al lote, sí cambia. |
| 14 | `final_flush_on_stop` | Las entradas pendientes están en el archivo tras `stop`. |
| 15 | `buffer_overflow_drops_oldest` | Con la tarea pausada (o un intervalo largo y `BATCH = cap + 1`), se incrementa `dropped` y se conserva lo más nuevo. |
| 16 | `concurrent_appends` | N tareas × M appends: todas las líneas son válidas y `count + dropped == N × M`. |
| 17 | `scheduler_fire_logs_entry` | Si hay un test del scheduler: un disparo genera una entrada `scheduled_task` con `ref` = id, y el retorno y los contadores son iguales con el log iniciado y sin iniciar. |

---

## 11. Documentación

- `cap_activity_log/README.md`:
  - formato JSONL y tabla de campos;
  - política de retrocompatibilidad (campos nuevos opcionales y campos desconocidos ignorados);
  - Kconfig y defaults;
  - peor caso de disco (2 × `MAX_BYTES` = 512 KB con el default), con la cuenta contra la partición `storage` real;
  - estrategia de volcado y pérdida posible ante un corte (hasta `BUFFER_ENTRIES` entradas o `FLUSH_INTERVAL_MS`);
  - política de buffer lleno (se descarta la más vieja, con `dropped`);
  - orden de locks;
  - semántica de la entrada del scheduler;
  - limitación de cobertura de §7 de la spec;
  - manejo de `ts` sin hora sincronizada.
- Comentario en `cap_scheduler.h` (§8).
- Si `.agents/design.md` mantiene un inventario de componentes o de uso de DATA, se agrega una línea.

---

## 12. Orden de ejecución y commits sugeridos

1. **Fase 0:** relevamiento y ajuste de nombres y firmas (sin commit, o una nota en el PR).
2. `feat(cap_activity_log)`: esqueleto con `CMakeLists`, `Kconfig`, header, ciclo de vida, enums y conversiones.
3. `feat(cap_activity_log)`: buffer, `append`, tarea de volcado, serialización y rotación.
4. `feat(cap_activity_log)`: lectura tolerante y `list_json`.
5. `feat(cap_activity_log)`: capabilities y registro en `app_capabilities.c`.
6. `feat(cap_scheduler)`: registro del disparo y documentación de la semántica de `ESP_OK`.
7. `docs(cap_activity_log)`: `SKILL.md` y README.
8. `test(cap_activity_log)`: `test_apps` (si existe el patrón).

---

## 13. Descripción del PR (checklist)

- [ ] Resumen del cambio y enlace a #4.
- [ ] Sección "Supuestos verificados", con lo encontrado en la Fase 0 y cualquier desvío respecto de este plan.
- [ ] Comandos de build:
  ```
  idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build
  ```
- [ ] Aviso explícito: **no se compiló ni se ejecutaron tests en NightShift**, por falta del toolchain ESP-IDF.
- [ ] Pruebas manuales en placa:
  1. Arranque limpio sin `activity/`: se crea el directorio y `activity_log_list` devuelve vacío.
  2. Disparar una tarea programada: aparece una entrada `scheduled_task` tras el volcado (por lote o intervalo).
  3. `activity_log_append` con `completed`, `skipped` y `error`: las tres se listan correctamente.
  4. `activity_log_list` con filtro por `origin` y con `since_ms`.
  5. Generar volumen hasta superar `MAX_BYTES`: existe `activity.1.jsonl` y el espacio usado en DATA queda acotado.
  6. Reinicio normal: el historial persiste. Corte de energía durante la escritura (si es posible): arranca sin crash.
  7. Inspeccionar el espacio libre en DATA antes y después.
  8. Verificar que el comportamiento del scheduler no cambió (mismas tareas disparadas, mismos contadores).
- [ ] Limitación documentada: los pulsos proactivos y el resultado final dependen de que el agente llame a `activity_log_append`.
- [ ] Seguimiento propuesto (no implementado): un hook en `claw_event_router`, si se identificó uno trivial.

---

## 14. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Espacio en DATA | Peor caso: 2 × `MAX_BYTES` + 1 línea. Se verifica contra `partitions.csv` y el uso de los otros componentes. Si queda ajustado, se baja el default a 128 KB (es Kconfig). |
| Pérdida ante corte de energía | Es un trade-off aceptado y documentado. Los errores se vuelcan lo antes posible (`FLUSH_ON_ERROR`). |
| Deadlock o latencia en el scheduler | `buf_mutex` es un lock hoja con timeout de 5 ms. No hay I/O en `append` y `file_mutex` nunca se anida con `buf_mutex`. |
| `rename` en FATFS con destino existente | `unlink` previo, tolerando `ENOENT`. Los cortes intermedios son inocuos (§6.4). |
| Línea truncada concatenada con la siguiente | Detección del último byte en `start` y prefijo `\n` en el primer volcado. |
| `ts` no epoch sin SNTP | Fallback a ms desde el arranque, documentado. Opcionalmente, el campo `ts_synced:false` (retrocompatible) si el equipo lo quiere. |
| Consumo de RAM | El buffer (~18 KB) y el ring de lectura (≤ 28 KB, temporal) van en PSRAM según la convención. `LINE_MAX` es un scratch estático único. |
| Firmas de API distintas a las supuestas | Fase 0 obligatoria. Gana el repo y el desvío se documenta en el PR. |
| Cobertura incompleta de ejecuciones autónomas | Se documenta. La skill instruye al agente. Instrumentar el router queda como seguimiento. |