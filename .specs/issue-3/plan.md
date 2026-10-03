# Plan de implementación: `cap_exec_state` (issue #3)

> **Nota de contexto:** este plan se armó sin acceso al código del repo, porque el workspace de NightShift no tiene checkout. Los nombres de API internos que no fija el spec (registro de capabilities, helpers de paths, patrón de skills, test apps) se tratan como **a confirmar en la Fase 0**. Las decisiones que dependen de eso están marcadas con **[CONFIRMAR]**.

---

## Fase 0: Reconocimiento obligatorio (antes de escribir código)

Objetivo: alinear firmas, estilo y registro con lo que ya existe. No inventar funciones.

| # | Archivo a leer | Qué extraer |
|---|---|---|
| 0.1 | `cap_scheduler.h`, `cap_scheduler_store.c` | Patrón de handle (instancia o singleton), forma de `create/start/stop/delete`, cómo carga y guarda JSON, uso de cJSON, manejo de errores y logging (`TAG`, `ESP_LOGW`), si usa PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`). |
| 0.2 | `claw_memory.h` / `.c` | Patrón de mutex (`SemaphoreHandle_t` o wrapper), cómo trunca strings (¿ya hay helper UTF-8-safe?), escritura atómica (¿ya hay `tmp` + `rename`?). |
| 0.3 | `claw_paths.h` | Firma exacta de `claw_paths_join`, valor de `CLAW_PATH_DATA`, si existe un helper de `mkdir -p`, y tipo de FS de la partición `storage` (LittleFS, SPIFFS o FAT). Esto decide la estrategia de `rename` (ver 3.4). |
| 0.4 | `claw_cap.h` (o equivalente) | Struct descriptor de capability (nombre, descripción, schema JSON, handler), firma del handler (`const char *input_json` → `char *out, size_t out_size`, o `cJSON*`), convención de errores al modelo. |
| 0.5 | `claw_event.h` | Si hay una work queue o un event loop genérico reusable para el flush diferido (y así evitar una task propia). |
| 0.6 | `components/common/app_claw/app_capabilities.c` | Cómo se registran otras capabilities (orden, `#if CONFIG_...`, `REQUIRES` del CMake del app). |
| 0.7 | Otra `cap_*` con `skills/` | Formato del frontmatter de `SKILL.md` y cómo se descubren o embeben las skills (¿`EMBED_FILES`? ¿copia al FS? ¿listado en el CMake del app?). |
| 0.8 | Algún `cap_*/test_apps/` | Estructura Unity (`main/`, `sdkconfig.defaults`, `partitions.csv`, montaje de FS en tests). |
| 0.9 | Kconfig de otra capability | Si se usa `Kconfig` o `Kconfig.projbuild`, el prefijo de símbolos y el menú padre. |
| 0.10 | Helper de tiempo | Si existe un `claw_time_now_ms()` o similar. Si no hay, usar `gettimeofday` → ms. |

**Salida de la Fase 0:** una lista corta de decisiones fijadas que se incluye como sección "Decisiones de alineación" en la descripción del PR.

---

## Fase 1: Esqueleto del componente

### 1.1 Estructura

```
components/claw_capabilities/cap_exec_state/
├── CMakeLists.txt
├── Kconfig                      (o Kconfig.projbuild [CONFIRMAR 0.9])
├── include/cap_exec_state.h
├── src/cap_exec_state.c         (store: cache, eviction, persistencia)
├── src/cap_exec_state_caps.c    (capabilities para el modelo)
├── skills/exec_state/SKILL.md
└── test_apps/
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    ├── partitions.csv           (si el patrón lo requiere)
    └── main/
        ├── CMakeLists.txt
        └── test_cap_exec_state.c
```

### 1.2 `CMakeLists.txt`

- `idf_component_register(SRCS "src/cap_exec_state.c" "src/cap_exec_state_caps.c" INCLUDE_DIRS "include" REQUIRES <claw_paths/claw_cap comp> PRIV_REQUIRES json esp_timer freertos log vfs)`.
- Embedding o registro de `skills/exec_state/SKILL.md` según 0.7.

### 1.3 `Kconfig`

```
menu "Exec State capability"
    config CAP_EXEC_STATE_MAX_ENTRIES
        int "Max entries" ; default 200 ; range 10 500
    config CAP_EXEC_STATE_MAX_FILE_BYTES
        int "Max serialized file size (bytes)" ; default 49152 ; range 4096 65535
    config CAP_EXEC_STATE_FLUSH_DELAY_MS
        int "Deferred flush delay (ms, 0 = immediate)" ; default 2000 ; range 0 60000
    config CAP_EXEC_STATE_LIST_DEFAULT_LIMIT
        int "Default limit for exec_state_list" ; default 20 ; range 1 500
endmenu
```

`range ... 65535` en `MAX_FILE_BYTES` garantiza que el archivo quede por debajo de 64KB aunque se reconfigure.

---

## Fase 2: Header público (`include/cap_exec_state.h`)

```c
#define CAP_EXEC_STATE_KEY_MAX_LEN      64
#define CAP_EXEC_STATE_KIND_MAX_LEN     24
#define CAP_EXEC_STATE_SUMMARY_MAX_LEN  200
#define CAP_EXEC_STATE_FORMAT_VERSION   1

typedef enum {
    CAP_EXEC_STATE_STATUS_COMPLETED = 0,
    CAP_EXEC_STATE_STATUS_FAILED,
    CAP_EXEC_STATE_STATUS_SKIPPED,
} cap_exec_state_status_t;

typedef struct {
    char key[CAP_EXEC_STATE_KEY_MAX_LEN + 1];
    char kind[CAP_EXEC_STATE_KIND_MAX_LEN + 1];
    cap_exec_state_status_t status;
    int64_t first_seen_ms;
    int64_t last_run_ms;
    uint32_t run_count;
    char summary[CAP_EXEC_STATE_SUMMARY_MAX_LEN + 1];
} cap_exec_state_entry_t;

typedef struct {
    const char *path;        /* NULL → claw_paths_join(CLAW_PATH_DATA, "state/exec_state.json") */
    size_t max_entries;      /* 0 → CONFIG_CAP_EXEC_STATE_MAX_ENTRIES */
    size_t max_file_bytes;   /* 0 → CONFIG_CAP_EXEC_STATE_MAX_FILE_BYTES */
    int32_t flush_delay_ms;  /* <0 → CONFIG_CAP_EXEC_STATE_FLUSH_DELAY_MS; 0 = inmediato */
} cap_exec_state_config_t;

#define CAP_EXEC_STATE_CONFIG_DEFAULT() { .path = NULL, .max_entries = 0, .max_file_bytes = 0, .flush_delay_ms = -1 }
```

**Ciclo de vida [CONFIRMAR 0.1]:**

- Si el repo usa handle: `typedef struct cap_exec_state *cap_exec_state_handle_t;` más `create/start/stop/delete`.
- Si el repo usa singleton (lo más probable para una capability): `create/start/stop/delete` operan sobre una instancia interna estática y `check/mark/list_json` mantienen exactamente las firmas del spec (sin handle). En ese caso, para los tests se expone `create` con `cfg->path` override y `delete` limpia la instancia, lo que permite ciclos `delete`+`create` en el mismo test.

**Operaciones** (firmas fijas del spec):

```c
esp_err_t cap_exec_state_check(const char *key, cap_exec_state_entry_t *out_entry);
esp_err_t cap_exec_state_mark(const char *key, const char *kind,
                              cap_exec_state_status_t status, const char *summary);
esp_err_t cap_exec_state_list_json(size_t limit, char *out, size_t out_size);

/* helpers públicos usados por caps y tests */
const char *cap_exec_state_status_to_str(cap_exec_state_status_t s);
esp_err_t   cap_exec_state_status_from_str(const char *s, cap_exec_state_status_t *out);
esp_err_t   cap_exec_state_flush(void);   /* flush síncrono: tests y stop/delete */
```

Opcional, solo si hace falta para tests de eviction o run_count sin depender de `check`: `cap_exec_state_count(void)`.

---

## Fase 3: Store (`src/cap_exec_state.c`)

### 3.1 Estado interno

```c
typedef struct {
    char path[...];  char tmp_path[...];  char bad_path[...];
    size_t max_entries, max_file_bytes;  uint32_t flush_delay_ms;
    cap_exec_state_entry_t *entries;  /* array contiguo en PSRAM, [0]=menos reciente */
    size_t count;
    SemaphoreHandle_t lock;           /* protege entries/count/dirty */
    SemaphoreHandle_t flush_lock;     /* serializa I/O de archivo */
    bool dirty;  bool started;
    esp_timer_handle_t flush_timer;   /* one-shot */
    TaskHandle_t flush_task;          /* solo si no hay work queue reusable [0.5] */
} cap_exec_state_t;
```

- `entries` se aloca una vez con `max_entries` slots (`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` con fallback a interna, según el patrón 0.1). Con 200 slots de ~300 B son ~60KB en PSRAM, aceptable.
- El array ordenado por recencia hace que "mover al final" sea un `memmove` de O(n), trivial para n ≤ 500.

### 3.2 Helpers privados

- `utf8_truncate_copy(dst, dst_size, src, max_bytes)`: copia hasta `max_bytes` y retrocede mientras el byte de corte sea de continuación (`(b & 0xC0) == 0x80`) para no partir un codepoint. Si `claw_memory` ya tiene uno, se reusa [0.2].
- `find_index(key)`: búsqueda lineal con `strcmp`.
- `move_to_tail(idx)`: `memmove` del bloque `[idx+1..count)` una posición hacia atrás y copia de la entrada al final.
- `evict_head(n)`: `memmove` de `[n..count)` a `[0..)` y `count -= n`.
- `now_ms()`: helper del repo [0.10] o `gettimeofday`.

### 3.3 Operaciones

**`mark`**

1. Validar: `key` no NULL y no vacía, y `strlen(key) <= KEY_MAX_LEN`; si no, `ESP_ERR_INVALID_ARG` (la key no se trunca). Validar el rango de `status`. `kind` puede ser NULL o vacío (se guarda `""`) y se trunca UTF-8-safe a 24. `summary` puede ser NULL y se trunca a 200.
2. Tomar `lock`.
3. Si existe: actualizar `kind/status/summary/last_run_ms`, hacer `run_count = (run_count == UINT32_MAX) ? run_count : run_count + 1` y `move_to_tail`.
   Si no existe: si `count == max_entries`, `evict_head(1)`. Después se agrega al final con `first_seen_ms = last_run_ms = now` y `run_count = 1`.
4. `dirty = true` y soltar `lock`.
5. Si `flush_delay_ms == 0`, flush síncrono. Si no, `esp_timer_stop` + `esp_timer_start_once(delay)`: el reinicio agrupa ráfagas. **Alternativa:** no reiniciar si ya está armado, para acotar la latencia máxima. **Recomendado:** no reiniciar, así una ráfaga continua no posterga el flush indefinidamente. Se documenta en el código.

**`check`**

Tomar `lock`, hacer `find_index`, copiar a `out_entry` (si no es NULL) y soltar. Devuelve `ESP_OK` o `ESP_ERR_NOT_FOUND`. No reordena.

**`list_json`**

1. `limit == 0 || limit > max_entries` → `limit = max_entries`. Tomar `lock`.
2. Construir con cJSON `{"count":k,"entries":[...]}` iterando desde `count-1` hacia atrás (lo más reciente primero) y soltar `lock`.
3. Imprimir con `cJSON_PrintPreallocated(root, out, out_size, false)`.
4. Si falla por buffer chico: reintentar quitando entradas del final del array (las más antiguas de la lista) hasta que entre. Si entra con menos, se agrega `"truncated":true` y se devuelve `ESP_OK`. Si no entra ni con 0 entradas, `ESP_ERR_NO_MEM` [alinear con el patrón 0.4].

Para no retener el lock durante el armado del JSON, se puede copiar primero `k` entradas a un buffer temporal en PSRAM bajo lock y armar el JSON fuera. **Recomendado:** sí, coherente con 4.8.

### 3.4 Persistencia

**Carga (`load_locked`, en `create` o `start` según el patrón 0.1):**

1. Asegurar el directorio `state/` con `mkdir` y tolerar `EEXIST` (helper del repo si existe [0.3]).
2. Si `exec_state.json` no existe → `count = 0` (el `.tmp` huérfano se ignora, ver nota de rename). Si existe → leer completo, cuidando que el tamaño sea ≤ `max(max_file_bytes, 64KB)`; si es mayor, se trata como corrupto.
3. `cJSON_Parse`. Si es NULL, o falta `entries`, o `version` no es número, o `version > CAP_EXEC_STATE_FORMAT_VERSION` → `ESP_LOGW`, `rename(path, bad_path)` (borrando antes un `.bad` previo: un solo backup) y arranque vacío.
4. Por cada item: requerir `key` string válida (no vacía y ≤ max) o se descarta el item. El resto con defaults: `kind=""`, `status=completed` si el string es desconocido o falta (**[DECISIÓN]**: elegir `failed` sería más conservador porque permite reintento; **recomendado `failed`** para no bloquear trabajo por un dato dañado), `first_seen_ms = last_run_ms` o 0, `run_count=1`, `summary=""`. Campos desconocidos se ignoran. Si hay keys duplicadas, gana la última.
5. Si el archivo trae más de `max_entries` items (porque se bajó el Kconfig), se conservan los últimos `max_entries` y se marca `dirty`.
6. Al final, `unlink(tmp_path)` silencioso para limpiar huérfanos.

**Flush (`flush_now`):**

1. Tomar `flush_lock` y luego `lock`. Si `!dirty`, soltar todo y salir.
2. Serializar con cJSON (`version`, `entries` en orden de menos a más reciente) usando `cJSON_PrintUnformatted`.
3. **Presupuesto en bytes:** si `len > max_file_bytes`, estimar `drop = ceil((len - max_file_bytes) / (len / count)) + 1`, hacer `evict_head(drop)` **en memoria** y reserializar. El loop está acotado porque siempre avanza. Se loguea en `ESP_LOGW` la cantidad evicted por bytes.
4. `dirty = false` y soltar `lock`. Desde acá el I/O corre fuera del lock de datos.
5. `fopen(tmp, "w")`, `fwrite`, `fflush`, `fclose` y verificar cada retorno. Después `rename(tmp, path)`.
   - **[CONFIRMAR 0.3] Tipo de FS:** LittleFS sobrescribe atómicamente con `rename`. En SPIFFS o FAT, `rename` falla si el destino existe. En ese caso el fallback es `unlink(path)` + `rename(tmp, path)`, y la carga agrega la regla "si falta `path` y existe `tmp` parseable, promover `tmp`". Esa regla solo aplica cuando falta el principal, así que un `.tmp` huérfano con principal presente sigue sin afectar (cumple el test del spec).
6. Ante cualquier fallo de I/O: `ESP_LOGE`, retomar `lock`, `dirty = true` y re-agendar el timer (sin loop caliente: se usa el mismo `flush_delay_ms`, o 2000 ms si es 0). Soltar `flush_lock`.
7. `free` del buffer cJSON.

**Disparo del flush diferido [CONFIRMAR 0.5]:**

- **Preferido:** si el repo tiene una work queue o un loop de eventos para trabajo diferido, el callback del `esp_timer` solo encola `flush_now`.
- **Si no existe:** una task chica `exec_state_flush` (stack ~4KB, prioridad baja) bloqueada en `ulTaskNotifyTake`. El callback del timer hace `xTaskNotifyGive`. No se hace I/O ni cJSON en el contexto del `esp_timer` task, porque su stack es chico.

**`stop` / `delete`:**

- `stop`: detener el timer y llamar `flush_now()` síncrono.
- `delete`: `stop` si estaba started, eliminar el timer y la task, liberar `entries` y los semáforos, y dejar la instancia en NULL o cero.

### 3.5 Concurrencia (resumen)

- Orden de locks siempre `flush_lock` → `lock`, nunca al revés.
- `check`, `mark` y `list` nunca toman `flush_lock`, así que no se bloquean por I/O.
- No hay llamadas desde ISR. Ninguna API pública es ISR-safe y se documenta en el header.

---

## Fase 4: Capabilities (`src/cap_exec_state_caps.c`)

Siguiendo el descriptor y la firma de handler de 0.4:

| Capability | Schema de entrada | Lógica | Salida |
|---|---|---|---|
| `exec_state_check` | `{"type":"object","properties":{"key":{"type":"string"}},"required":["key"]}` | `cap_exec_state_check` | `{"found":false,"already_processed":false}` o `{"found":true,"already_processed":<status==completed>,"entry":{...}}` |
| `exec_state_mark` | `key` (req), `kind` (req), `status` enum `["completed","failed","skipped"]` (req), `summary` (opt) | `status_from_str` y después `cap_exec_state_mark`, más un `check` para leer `run_count` | `{"ok":true,"run_count":N}` o `{"ok":false,"error":"status must be one of completed|failed|skipped"}` |
| `exec_state_list` | `limit` (integer, opt) | Default `CONFIG_CAP_EXEC_STATE_LIST_DEFAULT_LIMIT` y `cap_exec_state_list_json` | `{"count":k,"entries":[...],"truncated"?:true}` |

- Errores de validación (`key` vacía o larga, `status` inválido, JSON de entrada mal formado) → salida `{"ok":false,"error":"..."}` legible. El handler devuelve éxito (o el código que el patrón use para "error de tool, no fatal") para no abortar el turno del agente [0.4].
- Serialización de `entry`: un helper compartido `entry_to_cjson()`, que vive en el store y se expone de forma interna vía un header privado `src/cap_exec_state_priv.h` para que `list_json` y `check` usen exactamente el mismo formato.
- **`mark` + `run_count`:** para evitar una carrera entre `mark` y `check`, se agrega una variante interna `cap_exec_state_mark_ex(..., uint32_t *out_run_count)` en el header privado. `cap_exec_state_mark` la envuelve con NULL.

### Registro en `app_capabilities.c`

- `#include "cap_exec_state.h"`.
- En la secuencia de init: `cap_exec_state_create(&cfg_default)` + `start`, y registro de las 3 capabilities con la función existente [0.6]. Si `create` falla, `ESP_LOGE` y se sigue sin registrar las caps (no se cae el boot).
- Si el patrón lo usa, el registro va envuelto en `#if CONFIG_...`. No se agrega un toggle nuevo si las otras caps no lo tienen.
- Se agrega `cap_exec_state` a `REQUIRES`/`PRIV_REQUIRES` del componente `app_claw`.

---

## Fase 5: Skill `skills/exec_state/SKILL.md`

Frontmatter con el formato exacto de 0.7 (p. ej. `name`, `description`, `capabilities: [exec_state_check, exec_state_mark, exec_state_list]`). Secciones:

1. **Cuándo usarla:** pulses autónomos o tareas programadas que procesan recursos individuales (leads, reuniones, conversaciones, entregables).
2. **Claves estables:** `<tipo>:<id_estable>` (`lead:123`, `meeting:<calendar_event_id>`, `conv:<thread_id>`, `deliverable:<id>`, `task:<task_id>`). Nunca usar títulos, fechas relativas ni textos que cambien. Máximo 64 chars.
3. **Flujo obligatorio:**
   1. `exec_state_check(key)`.
   2. Si `already_processed` → omitir.
   3. Si `found` con `status=skipped` → omitir salvo cambio explícito del recurso.
   4. Si `found` con `status=failed` o no `found` → procesar.
   5. Al terminar: `exec_state_mark(key, kind, status, summary)` con un `summary` breve (≤200, qué se hizo y el resultado).
4. **Tabla de reglas de dedup:** `completed` / `failed` / `skipped` / sin entrada.
5. **Última ejecución de una scheduled task:** al inicio, `exec_state_check("task:<id>")` para leer `last_run_ms`, `status` y `run_count`. Al final, `exec_state_mark("task:<id>", "task", ...)`.
6. **Contexto de ciclos previos:** `exec_state_list(limit)`.
7. **Límites:** tope de entradas y de bytes. Las entradas antiguas pueden ser evicted y el recurso reprocesarse. Los timestamps pueden no ser confiables si el reloj no se sincronizó. Ante un corte de energía puede perderse el último `mark`.

---

## Fase 6: Tests (`test_apps/`)

Patrón Unity del repo [0.8]. FS montado en el test app (misma tecnología que `storage`) y `cfg.path` apuntando a un directorio de test. Se usa `flush_delay_ms = 0` salvo en el test de agrupamiento.

| # | Caso | Verificación |
|---|---|---|
| T1 | mark nuevo | `check` → `ESP_OK`, `run_count==1`, `first_seen_ms==last_run_ms` |
| T2 | mark repetido | `run_count==2`, `first_seen_ms` igual, `last_run_ms` ≥ anterior |
| T3 | `failed` → `completed` | status final `completed` y `run_count==2` |
| T4 | check inexistente | `ESP_ERR_NOT_FOUND` |
| T5 | Eviction por cantidad | `max_entries=N`, insertar N+1 → `k0` no existe y `k1..kN` sí |
| T6 | Re-mark salva | Insertar N, re-mark `k0`, insertar 1 más → `k0` existe y `k1` fue evicted |
| T7 | check no reordena | Insertar N, `check(k0)`, insertar 1 → `k0` evicted |
| T8 | Presupuesto de bytes | `max_entries=500`, `max_file_bytes=8192`, entradas con key de 64 y summary de 200 → `stat(path).st_size <= 8192` y `count` reducido |
| T9 | Persistencia | mark varias → `delete` → `create`+`start` → todas presentes con campos iguales |
| T10 | Archivo ausente | `create` sobre un path inexistente → vacío y `ESP_OK` |
| T11 | Corrupto o truncado | Escribir `{"version":1,"entr` → `create` OK, vacío, existe `.bad` |
| T12 | Versión futura | `{"version":99,...}` → vacío sin crash |
| T13 | Campos faltantes o desconocidos | Item sin `run_count` y con un campo extra → se carga con defaults |
| T14 | Tmp huérfano | `path` válido + `.tmp` basura → se carga `path` y desaparece el `.tmp` |
| T15 | Validación | key `""` / NULL / 65 chars → `ESP_ERR_INVALID_ARG`, status fuera de rango → `INVALID_ARG` |
| T16 | Truncado UTF-8 | summary de 199 ASCII + "é" (2 bytes) → longitud 199 y UTF-8 válido. kind de 30 chars → 24 |
| T17 | `list_json` orden y límite | 5 marks, `limit=3` → los 3 más recientes, el más reciente primero. `limit=0` → todos |
| T18 | `list_json` buffer chico | `out_size` reducido → `truncated:true` o `ESP_ERR_NO_MEM` según el contrato, sin overflow (canario al final del buffer) |
| T19 | Flush agrupado | `flush_delay_ms=200`, 10 marks seguidos → el archivo no cambia de inmediato y después de 300 ms existe con las 10 entradas (mtime y una sola escritura, observado vía contador de debug si existe) |
| T20 | run_count saturado | Cargar una entrada con `run_count=4294967295` y hacer mark → sigue en el máximo |

---

## Fase 7: Documentación

- Comentario de cabecera en `cap_exec_state.h` con la semántica, los límites, el tradeoff de flush y la nota de que no es ISR-safe.
- `README.md` corto en el componente, o la sección equivalente en la doc de capabilities del repo si existe: defaults de Kconfig, formato del archivo, ubicación (`<DATA>/state/exec_state.json`), comportamiento ante corrupción y la convención `task:<id>`.

---

## Orden de commits sugerido

1. `cap_exec_state: skeleton, Kconfig, header`
2. `cap_exec_state: in-memory store, mark/check/list`
3. `cap_exec_state: atomic persistence, tolerant load, deferred flush`
4. `cap_exec_state: model capabilities`
5. `app_claw: register cap_exec_state`
6. `cap_exec_state: exec_state skill`
7. `cap_exec_state: unity test app`
8. `docs: cap_exec_state defaults and semantics`

Cada archivo queda por debajo de 1500 líneas. Estimación: store ~600, caps ~250, tests ~500.

---

## Checklist de la descripción del PR

- [ ] Sección "Decisiones de alineación" (resultado de la Fase 0: patrón de handle, FS y estrategia de rename, mecanismo de flush diferido, firma del handler de capabilities).
- [ ] Aclarar de forma explícita: **no se compiló ni se ejecutaron tests en NightShift (sin toolchain ESP-IDF)**.
- [ ] Comando de build: `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`.
- [ ] Comando de test app (según el patrón del repo).
- [ ] Las 10 pruebas manuales en placa de la sección 9 del spec, copiadas como checklist.
- [ ] Tradeoffs documentados: pérdida del último mark en la ventana de flush, eviction de `completed`, reloj sin SNTP.
- [ ] Confirmación de que no hay cambios en `claw_core`, scheduler ni router.

---

## Riesgos de implementación (además de los del spec)

| Riesgo | Mitigación |
|---|---|
| `rename` no sobrescribe en SPIFFS o FAT | Fallback `unlink`+`rename` y promoción del `.tmp` en la carga solo si falta el principal (Fase 3.4). |
| Stack overflow si el flush corre en el `esp_timer` task | El flush siempre corre en una work queue o en una task dedicada (Fase 3.4). |
| Una ráfaga continua posterga el flush indefinidamente | El timer no se reinicia si ya está armado, así la latencia queda acotada a `flush_delay_ms`. |
| Bajar `MAX_ENTRIES` entre versiones | La carga recorta a los últimos N y marca dirty. |
| Un status ilegible en disco bloquea el reproceso | El default al cargar es `failed` (permite reintento). |
| Carrera entre `mark` y lectura de `run_count` en la capability | `mark_ex` interno devuelve `run_count` bajo el mismo lock. |