# Spec: Registro de actividad autónoma (`cap_activity_log`) — Issue #4

## 1. Objetivo

Dejar un registro consultable de lo que KrakBot Nano hace de forma autónoma: **qué hizo, cuándo, por qué y con qué resultado**. El historial lo puede leer el agente (vía capability) y queda en la partición DATA con tamaño acotado.

## 2. Estado actual y premisas

- No existe log de actividad. El scheduler (`components/claw_capabilities/cap_scheduler/`) y el router (`components/claw_modules/claw_event_router/`) disparan trabajo autónomo, pero solo dejan contadores en el scheduler.
- Es firmware C sobre ESP-IDF/FreeRTOS (ESP32-S3). No hay SQLite, npm ni pip, y no se agregan dependencias externas.
- La partición `storage` (DATA) tiene ~3 MB y no se reflashea con `idf.py flash`. El formato debe ser retrocompatible, de tamaño acotado y cuidar el desgaste de flash.
- **Este entorno no tiene el repo ni el toolchain ESP-IDF.** Esta spec no pudo verificar firmas reales. El implementador debe leer antes `CLAUDE.md`, `.agents/design.md`, `.agents/gotchas.md`, `cap_scheduler.h`, `claw_memory.h`, `claw_event.h` y el header de `claw_paths`. No debe inventar funciones. Todo nombre de API externa marcado "(verificar)" es provisorio.

## 3. Alcance

1. Componente nuevo `components/claw_capabilities/cap_activity_log/`, registrado en `components/common/app_claw/app_capabilities.c`.
2. Almacenamiento en JSONL, en `claw_paths_join(CLAW_PATH_DATA, "activity/activity.jsonl")`.
3. API C y dos capabilities para el modelo.
4. Instrumentación mínima del disparo del scheduler.
5. Skill `activity_log` (`SKILL.md`).
6. Tests donde exista patrón y documentación.

### Fuera de alcance

- UI web del log, export y consulta remota.
- Cambios en `claw_core`.
- Instrumentar el router, salvo que ya haya un hook existente que lo permita sin tocar su semántica (ver §6.2).

## 4. Formato de datos

Una línea JSON por entrada (JSONL, UTF-8, `\n` como separador):

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `ts` | int (epoch ms) | sí | Momento del registro. Si no hay hora sincronizada, usar el mejor tiempo disponible (verificar cómo lo resuelven otros componentes). |
| `origin` | string | sí | `proactive_pulse` \| `scheduled_task` \| `manual` |
| `action` | string | sí | Qué se hizo o decidió. |
| `reason` | string | sí | Motivo o trigger. |
| `result` | string | sí | `completed` \| `skipped` \| `error` |
| `ref` | string | no | Id de tarea o ruta de archivo/artefacto relacionado. Permite relacionar la entrada con la memoria o el estado de ejecución. |
| `detail` | string | no | Texto corto, **≤ 200 caracteres**. Se trunca de forma segura, sin cortar a mitad de un carácter UTF-8. |

Reglas:
- Los enums se definen como constantes o `enum` en el header, sin strings mágicos dispersos. Hay una función de conversión enum↔string.
- Los strings se escapan correctamente al serializar (comillas, `\`, caracteres de control, saltos de línea). Una entrada nunca puede ocupar más de una línea.
- Se fija un largo máximo por campo (constantes) y se trunca. Con eso el tamaño máximo de una línea queda acotado.
- **Retrocompatibilidad:** al leer se ignoran campos desconocidos. Los campos nuevos futuros deben ser opcionales.

## 5. Tamaño acotado, rotación y desgaste de flash

### 5.1 Kconfig (`Kconfig.projbuild` del componente)

- `CONFIG_CAP_ACTIVITY_LOG_MAX_BYTES` (default `262144`, 256 KB): tope del segmento activo.
- `CONFIG_CAP_ACTIVITY_LOG_FLUSH_INTERVAL_MS`: intervalo de volcado del buffer.
- `CONFIG_CAP_ACTIVITY_LOG_FLUSH_BATCH`: cantidad de entradas que dispara un volcado anticipado.
- `CONFIG_CAP_ACTIVITY_LOG_BUFFER_ENTRIES` (o bytes): capacidad del buffer en RAM.

Los defaults deben ser razonables para el desgaste de flash (volcado en el orden de decenas de segundos o más).

### 5.2 Rotación

- Cuando el segmento activo más el lote a volcar superaría `MAX_BYTES`, se rota: `activity.jsonl` pasa a `activity.1.jsonl`, reemplazando el anterior, y se empieza un `activity.jsonl` nuevo.
- Peor caso en disco: `2 × MAX_BYTES` (512 KB con el default). Debe documentarse y revisarse contra los ~3 MB de DATA, porque otros componentes también escriben ahí.
- La rotación usa `rename` (verificar el comportamiento de `rename` sobre un destino existente en FATFS; si hace falta, `unlink` previo, tolerando que el destino no exista).

### 5.3 Escrituras agrupadas

- `append` solo encola la entrada en un buffer en RAM, protegido por mutex.
- El volcado a flash ocurre cuando se cumple lo primero de: el lote alcanza `FLUSH_BATCH`, vence `FLUSH_INTERVAL_MS`, o se llama a `stop`/`delete` (volcado final).
- El volcado agrega al archivo con `fopen("a")`, `fwrite` y `fclose`. **No hay `fsync` por entrada.**
- Si el buffer se llena antes del volcado, se fuerza un volcado o se descarta la entrada más vieja del buffer, con un contador `dropped` expuesto en el estado. Hay que elegir una de las dos y documentarla.
- `append` nunca bloquea mucho tiempo ni hace I/O de flash en el contexto del llamador. Esto importa porque se llama desde el scheduler con su lock tomado (ver §6.1).

### 5.4 Atomicidad y tolerancia a corrupción

- El log es de solo agregar, así que no usa temp+rename para cada append.
- La rotación sí debe ser atómica por `rename`.
- Al arrancar:
  - Si el directorio `activity/` no existe, se crea.
  - Si el archivo falta, se arranca vacío.
  - Si la última línea está truncada o es JSON inválido (corte de energía), se ignora al listar y no se crashea.
- `list_json` salta las líneas inválidas.

## 6. Diseño del componente

### 6.1 Estilo y API C

Se sigue el estilo OO del repo: handle opaco, `create/delete/start/stop`, config por struct, `esp_err_t`, recursos liberados en `delete`, estado compartido bajo mutex. Archivos < 1500 líneas.

```c
typedef struct cap_activity_log *cap_activity_log_handle_t;   // nombre exacto: verificar convención

typedef enum { CAP_ACTIVITY_ORIGIN_PROACTIVE_PULSE, CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK, CAP_ACTIVITY_ORIGIN_MANUAL } cap_activity_origin_t;
typedef enum { CAP_ACTIVITY_RESULT_COMPLETED, CAP_ACTIVITY_RESULT_SKIPPED, CAP_ACTIVITY_RESULT_ERROR } cap_activity_result_t;

esp_err_t cap_activity_log_append(origin, action, reason, result, ref /*nullable*/, detail /*nullable*/);
esp_err_t cap_activity_log_list_json(size_t limit, int64_t since_ms, <out buffer/size o cJSON, ver convención del repo>);
```

Detalles:
- Si el log es un singleton accesible por las capabilities, `append` y `list_json` pueden operar sobre una instancia global del componente. Hay que seguir cómo lo resuelven otros `cap_*`: handle explícito o instancia del módulo.
- `append` valida los enums y los strings obligatorios no vacíos, y devuelve `ESP_ERR_INVALID_ARG` si no cumplen.
- Si el log no está inicializado, `append` devuelve error sin crashear. El scheduler debe tolerarlo (§6.3).
- `list_json(limit, since_ms, out)`:
  - Devuelve las últimas `limit` entradas con `ts >= since_ms`, en orden cronológico o inverso (a definir y documentar).
  - Lee el buffer pendiente más `activity.jsonl` y, si hace falta, `activity.1.jsonl`.
  - Aplica un `limit` máximo (constante) y acota el tamaño de salida.
  - Lee por stream o por líneas, sin cargar 256 KB completos en RAM. Si se necesita memoria grande, usar PSRAM según las convenciones del repo.

### 6.2 Capabilities para el modelo

Registradas en `app_capabilities.c`, siguiendo el patrón de los otros `cap_*` (verificar la estructura de descriptor, schema de parámetros y callback).

- **`activity_log_list`**
  - Parámetros: `limit` (opcional, con default y máximo), `origin` (opcional, filtro), `since_ms` (opcional).
  - Devuelve las últimas N entradas en JSON, aplicando el filtro por origen.
- **`activity_log_append`**
  - Parámetros: `origin`, `action`, `reason`, `result`, `ref` (opcional), `detail` (opcional).
  - Sirve para que el agente registre su propia decisión, incluido cuando decide **no** actuar (`result=skipped`).
  - Valida los enums y devuelve un error claro al modelo si son inválidos.

### 6.3 Instrumentación del scheduler

- En `cap_scheduler_publish_entry_locked`, tras el intento de publicar el evento, se llama a `cap_activity_log_append` con `origin=scheduled_task`, `action` = disparo de la tarea, `reason` = trigger (tipo de schedule o id de la entrada) y `ref` = id de la tarea.
- **No cambia la semántica del scheduler:**
  - El valor de retorno de la función y los contadores quedan idénticos.
  - Un fallo del log (error, no inicializado) se ignora o se loguea con `ESP_LOG`, sin propagarse.
  - Se llama con el lock del scheduler tomado, así que `append` debe ser no bloqueante y no hacer I/O de flash (§5.3). Hay que verificar que no haya inversión de locks ni riesgo de deadlock con el mutex del log.
- **Semántica a documentar** (en el header del scheduler o en el README del componente): en este punto, `ESP_OK` significa "evento publicado", no "trabajo terminado". Por eso la entrada del scheduler registra el **disparo**:
  - Si la publicación fue OK, `result=completed` con `detail` indicando "evento publicado" (o un `result` equivalente, a decidir y documentar).
  - Si falló, `result=error` con el código en `detail`.
  - El resultado final del trabajo lo registra el agente después, con `activity_log_append`.
- El router (`claw_event_router`) no se instrumenta en este issue, para mantener el cambio chico. Si el implementador identifica un hook existente y trivial, puede proponerlo en el PR como seguimiento, no implementarlo.
- No se toca `claw_core`.

### 6.4 Skill `activity_log`

- `SKILL.md` en el directorio `skills/` del componente, siguiendo el formato y el mecanismo de descubrimiento de las otras skills (verificar).
- Contenido:
  - Cuándo registrar: tras toda ejecución autónoma (pulso proactivo o tarea programada), incluida la decisión de no actuar (`skipped`) y los errores (`error`).
  - Cómo registrar: elegir `origin` correcto, `action` y `reason` concisos, `result` honesto, `ref` con el id de tarea o archivo generado, `detail` ≤ 200 caracteres.
  - Que el disparo del scheduler ya deja una entrada automática y que el agente agrega el **resultado final**.
  - Cuándo consultar con `activity_log_list` (por ejemplo, para no repetir una acción ya hecha).
  - Qué no registrar: secretos, credenciales ni contenido sensible en `detail`.
  - Ejemplo breve: "Detecté una reunión con Cliente X mañana" → `origin=proactive_pulse`, `action="Preparar ficha y briefing de Cliente X"`, `result=completed`, `ref=<ruta del archivo>`.

## 7. Criterios de aceptación (mapeo)

| Criterio del issue | Cómo se cumple |
|---|---|
| Toda ejecución automática genera una entrada | Entrada automática por cada disparo del scheduler. Entrada del agente con `activity_log_append` para el resultado y para los pulsos proactivos, indicado por la skill. |
| Se distingue el origen | Campo `origin` (enum). |
| Se registra cuando el agente decide no actuar | `result=skipped`, con `activity_log_append` y la skill. |
| Los errores quedan registrados | `result=error`, tanto en el disparo del scheduler como por el agente. |
| El historial puede consultarse | `cap_activity_log_list_json` y la capability `activity_log_list`. |
| Relacionable con estado/memoria de ejecución | Campo `ref` (id de tarea o ruta). La skill indica usar el mismo id que usan el scheduler y la memoria. |

**Limitación a documentar:** los pulsos proactivos y el resultado final de las tareas dependen de que el agente invoque `activity_log_append`. Sin cambios en `claw_core` ni en el router, el firmware no puede garantizarlo. Solo el disparo del scheduler queda registrado de forma automática.

## 8. Plan de implementación

1. Leer `CLAUDE.md`, `.agents/design.md`, `.agents/gotchas.md` y los headers reales (`cap_scheduler.h`, `claw_memory.h`, `claw_event.h`, `claw_paths`). Tomar como plantilla un `cap_*` existente (estructura de `CMakeLists.txt`, `Kconfig`, registro y descriptor de capability).
2. Crear `cap_activity_log/` con `CMakeLists.txt`, `Kconfig.projbuild`, `include/cap_activity_log.h` y `src/cap_activity_log.c`. Si pasa de ~500 líneas, separar en `..._store.c` (buffer, volcado, rotación) y `..._caps.c` (capabilities).
3. Implementar el buffer, el volcado, la rotación y la lectura tolerante a corrupción.
4. Implementar `append` y `list_json`.
5. Registrar las capabilities `activity_log_list` y `activity_log_append` en `app_capabilities.c`, y agregar el componente a las dependencias del build donde corresponda.
6. Instrumentar `cap_scheduler_publish_entry_locked` y documentar la semántica de `ESP_OK`.
7. Escribir `SKILL.md`.
8. Agregar tests y documentación.

## 9. Tests

Se agregan donde exista patrón (`components/**/test_apps/` con Unity, o tests Lua junto al módulo). Hay que seguir la estructura de un `test_apps` existente. **No se pueden ejecutar en el entorno de NightShift**, y el PR no debe afirmar que pasaron.

- Serialización: escape de comillas, `\` y saltos de línea; truncado de `detail` a 200 caracteres sin cortar UTF-8; enums inválidos rechazados.
- Rotación: al superar `MAX_BYTES` se genera `activity.1.jsonl` y el tamaño total queda acotado.
- Archivo ausente: arranca vacío. Última línea truncada o corrupta: se ignora sin crash.
- `list_json`: respeta `limit` y `since_ms`, incluye entradas del buffer aún sin volcar y cruza el límite entre segmentos.
- Volcado: no escribe antes del lote o intervalo, y el volcado final ocurre en `stop`/`delete`.
- Concurrencia básica: appends desde varias tareas no corrompen el buffer.
- Scheduler: el disparo genera una entrada con `origin=scheduled_task` y el retorno y los contadores no cambian, incluso si el log falla.

## 10. Descripción del PR (requisitos)

- Debe incluir los comandos de build:
  ```
  idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build
  ```
- Debe indicar explícitamente que **no se compiló ni se ejecutaron tests** en NightShift por falta de toolchain.
- Debe incluir esta lista de pruebas manuales en placa:
  1. Arranque limpio sin `activity/`: se crea el directorio y `activity_log_list` devuelve vacío.
  2. Disparar una tarea programada: aparece una entrada `scheduled_task` tras el volcado (por lote o intervalo).
  3. `activity_log_append` con `completed`, `skipped` y `error`: las tres se listan correctamente.
  4. `activity_log_list` con filtro por `origin` y con `since_ms`.
  5. Generar volumen hasta superar `MAX_BYTES`: existe `activity.1.jsonl` y el espacio usado en DATA queda acotado.
  6. Reinicio normal: el historial persiste. Corte de energía durante la escritura (si es posible): arranca sin crash.
  7. Inspeccionar el espacio libre en DATA antes y después.
  8. Verificar que el comportamiento del scheduler no cambió (mismas tareas disparadas, mismos contadores).

## 11. Riesgos y decisiones abiertas

- **Espacio en DATA:** el peor caso es `2 × MAX_BYTES`. Confirmar con el espacio real disponible; si es ajustado, bajar el default.
- **Pérdida de entradas ante corte de energía:** el buffer en RAM puede perder las últimas entradas no volcadas. Es un trade-off aceptado a cambio de menos desgaste de flash. Documentarlo y valorar un volcado inmediato para `result=error`.
- **Hora del sistema:** sin sincronización, `ts` puede no ser epoch real. Verificar cómo lo maneja el resto del firmware.
- **Locks:** `append` se llama con el lock del scheduler tomado. Debe cumplirse el orden de locks y que `append` no haga I/O bloqueante.
- **Sin garantía de cobertura total:** ver la limitación en §7.