# Spec: Estado persistente de ejecución (`cap_exec_state`): issue #3

## 1. Resumen

Se agrega un store JSON mínimo, acotado y persistente sobre la partición DATA. Responde "¿ya procesé el recurso X?" para ejecuciones autónomas y recurrentes (Autonomous Pulse, Scheduled Tasks). Se expone como componente de capabilities (`cap_exec_state`) más una skill (`exec_state`).

El agente marca el recurso como procesado él mismo, al terminar, vía capability. No se toca `claw_core` ni el router, ni se observa el cierre del trabajo. Esto evita acoplar el core y evita interpretar el `ESP_OK` del scheduler, que solo significa "evento publicado".

## 2. Contexto y estado actual

- `claw_memory` guarda memoria de largo plazo (items de 256 chars con tags/keywords), perfil y resúmenes de sesión. Sirve para recordar contexto, no para dedup por recurso.
- `cap_scheduler_store.c` persiste definiciones (JSON) y contadores (NVS), no un historial de recursos procesados.
- Firmware C sobre ESP-IDF/FreeRTOS (ESP32-S3, PSRAM). No hay SQLite ni dependencias externas nuevas.
- La partición `storage` (DATA, ~3MB) no se reflashea con `idf.py flash`. Por eso el formato debe ser retrocompatible y de tamaño acotado.

**Pre-requisito de implementación:** antes de codear hay que leer los headers reales y no inventar funciones. Son `cap_scheduler.h`, `claw_memory.h`, `claw_event.h`, `claw_paths.h`, `claw_cap.h` (o equivalente de registro de capabilities) y `app_capabilities.c`. También hay que tomar como referencia de estilo `cap_scheduler_store.c` y `claw_memory`. Los nombres de API que aparecen abajo salvo `claw_paths_join`/`CLAW_PATH_DATA` y los de este issue son ilustrativos y se deben alinear con lo que exista.

## 3. Alcance

1. Nuevo componente `components/claw_capabilities/cap_exec_state/`, registrado en `components/common/app_claw/app_capabilities.c`.
2. Archivo de estado: `claw_paths_join(CLAW_PATH_DATA, "state/exec_state.json")`. El directorio `state/` se crea si no existe.
3. API C: `cap_exec_state_check`, `cap_exec_state_mark`, `cap_exec_state_list_json`.
4. Tres capabilities para el modelo: `exec_state_check`, `exec_state_mark`, `exec_state_list`.
5. Skill `exec_state` (`SKILL.md`) en el `skills/` del componente.
6. Opciones Kconfig, tests y documentación de defaults.

### Fuera de alcance

- Integración con scheduler/router (la hacen los issues #1/#2 vía skill).
- Cualquier cambio en `claw_core`.
- `commitProcessed` o mecanismos opt-in.
- SQLite.
- Retención por tiempo (TTL).

## 4. Diseño

### 4.1 Modelo de datos

Entrada:

| Campo | Tipo | Límite / semántica |
|---|---|---|
| `key` | string | Clave estable armada por el agente (p. ej. `lead:123`). Máx. `CAP_EXEC_STATE_KEY_MAX_LEN` (64). Identidad única. |
| `kind` | string | Categoría libre (`lead`, `meeting`, `conversation`, `deliverable`, ...). Máx. `CAP_EXEC_STATE_KIND_MAX_LEN` (24). |
| `status` | enum | `completed` \| `failed` \| `skipped` |
| `first_seen_ms` | int64 | Se fija en el primer `mark` y no cambia. |
| `last_run_ms` | int64 | Se actualiza en cada `mark`. |
| `run_count` | uint32 | Se incrementa en cada `mark` (saturando). |
| `summary` | string | Máx. `CAP_EXEC_STATE_SUMMARY_MAX_LEN` (200). Se trunca sin romper UTF-8. |

Los límites son constantes en el header, sin números mágicos en el `.c`.

Formato en disco:

```json
{
  "version": 1,
  "entries": [
    {"key":"lead:123","kind":"lead","status":"completed",
     "first_seen_ms":1730000000000,"last_run_ms":1730000500000,
     "run_count":1,"summary":"..."}
  ]
}
```

- `version` permite evolución retrocompatible. El lector ignora campos desconocidos y tolera campos faltantes (defaults seguros).
- El orden del array es de menos a más reciente. Es el criterio de eviction y no depende de que el reloj de pared sea válido (por ejemplo, sin SNTP).

### 4.2 Semántica

- **`mark(key, kind, status, summary)`**
  - Si la clave no existe, crea la entrada con `first_seen_ms = last_run_ms = now` y `run_count = 1`.
  - Si existe, actualiza `status`, `kind`, `summary` y `last_run_ms`, incrementa `run_count` y conserva `first_seen_ms`. La entrada pasa al final del array (más reciente).
  - Si se supera el tope (ver 4.3), evicta desde el principio (menos reciente).
- **`check(key, out_entry)`**
  - Devuelve `ESP_OK` con la entrada si existe y `ESP_ERR_NOT_FOUND` si no.
  - No modifica el orden de recencia, para que consultar no "revalide" entradas.
- **Regla de dedup (documentada en la skill):**
  - `completed` → no reprocesar.
  - `failed` → se permite reintento. El `mark` posterior incrementa `run_count`.
  - `skipped` → se considera ya evaluado y no se reprocesa salvo que el agente tenga razón explícita (por ejemplo, el recurso cambió).
  - Sin entrada → procesar.
- **Deduplicación:** indefinida mientras la entrada no sea evicted. No hay retención por tiempo.

### 4.3 Acotado

- `CONFIG_CAP_EXEC_STATE_MAX_ENTRIES` (default 200): máximo de entradas.
- **Presupuesto en bytes:** con los límites de campo, una entrada peor-caso ronda ~400 B, así que 200 entradas podrían superar 64KB. Por eso se agrega `CAP_EXEC_STATE_MAX_FILE_BYTES` (default 48KB, por debajo de 64KB). Al serializar, si el JSON excede el presupuesto, se evictan entradas más antiguas hasta que entre. Así el archivo queda siempre por debajo de 64KB aunque `MAX_ENTRIES` se suba.
- La eviction aplica en memoria. El archivo siempre refleja el estado acotado.

### 4.4 Persistencia y desgaste de flash

- Cache en memoria (PSRAM si el patrón del repo lo usa) cargada en `create`/`start`.
- **Carga tolerante:**
  - Archivo ausente → arranca vacío.
  - JSON corrupto o `version` desconocida → log de warning, arranca vacío y no crashea.
  - Opcionalmente renombra el corrupto a `exec_state.json.bad` para diagnóstico (un solo backup, sin crecimiento).
- **Escritura atómica:** serializa a `exec_state.json.tmp` y hace `rename` sobre el destino. Si el rename falla, loguea y mantiene el estado en memoria con la marca dirty para reintentar.
- **Escrituras agrupadas:** `mark` marca dirty y agenda un flush diferido (`CONFIG_CAP_EXEC_STATE_FLUSH_DELAY_MS`, default 2000; `0` = flush inmediato). Varios `mark` seguidos producen una sola escritura. No se hace `fsync` por entrada. El flush también corre en `stop`/`delete`.
- Tradeoff documentado: ante un corte de energía dentro de la ventana de flush se puede perder el último `mark`, lo que implica a lo sumo un reproceso. Es aceptable y se documenta.

### 4.5 API C (estilo del repo)

Handle opaco y ciclo de vida:

- `cap_exec_state_handle_t`, `cap_exec_state_config_t` (path override opcional para tests, `max_entries`, `max_file_bytes`, `flush_delay_ms`; defaults desde Kconfig).
- `cap_exec_state_create(const cap_exec_state_config_t *cfg, cap_exec_state_handle_t *out)`, `_start`, `_stop`, `_delete` (libera cache, timer y mutex; hace flush final).

Operaciones, todas `esp_err_t`, con mutex interno:

- `cap_exec_state_check(const char *key, cap_exec_state_entry_t *out_entry)`
- `cap_exec_state_mark(const char *key, const char *kind, cap_exec_state_status_t status, const char *summary)`
- `cap_exec_state_list_json(size_t limit, char *out, size_t out_size)`: devuelve JSON con las últimas `limit` entradas (más reciente primero). `limit = 0` o mayor al máximo se acota al máximo. Trunca de forma segura si `out` es chico y devuelve `ESP_ERR_NO_MEM` o indica truncamiento, siguiendo el patrón de otras capabilities.

La forma exacta de las firmas (instancia vs. singleton) se alinea con el patrón de `cap_scheduler`/`claw_memory` una vez leídos los headers. Si el patrón del repo es un singleton interno, las funciones públicas listadas arriba se mantienen tal cual.

Validaciones: `key` y `status` obligatorios y no vacíos. `key` mayor que el máximo devuelve `ESP_ERR_INVALID_ARG` (no se trunca, para no colisionar claves). `kind` y `summary` se truncan y `summary` puede ser NULL.

### 4.6 Capabilities para el modelo

Se registran en `app_capabilities.c` siguiendo el patrón existente de capabilities (descriptor, schema de entrada JSON, handler que escribe salida JSON).

| Capability | Entrada | Salida |
|---|---|---|
| `exec_state_check` | `{key}` | `{found, already_processed, entry?}`, donde `already_processed = (status == completed)`. |
| `exec_state_mark` | `{key, kind, status, summary?}` | `{ok, run_count}` o error legible. |
| `exec_state_list` | `{limit?}` | `{count, entries:[...]}`. Default `CONFIG_CAP_EXEC_STATE_LIST_DEFAULT_LIMIT` (20). |

Los errores de validación (status inválido, key vacía) se devuelven como mensaje claro al modelo, sin abortar.

### 4.7 Skill `exec_state`

`components/claw_capabilities/cap_exec_state/skills/exec_state/SKILL.md`, con el mismo formato (frontmatter y registro) que las skills existentes. Contenido:

- Cuándo usarla: ejecuciones recurrentes o autónomas que procesan recursos individuales.
- Construir `key` estable con prefijo de tipo (`lead:123`, `meeting:<id>`, `conv:<id>`).
- Flujo obligatorio: `exec_state_check` antes de procesar → si `already_processed`, omitir → si no, procesar → `exec_state_mark` al terminar con `completed`/`failed`/`skipped` y un `summary` breve.
- `failed` permite reintento y `completed` no. `skipped` evita reevaluar.
- Usar `exec_state_list` para ver qué se hizo en ciclos anteriores y para que una scheduled task conozca su última ejecución (clave sugerida: `task:<id>`).
- Aviso de límites: la entrada puede ser evicted y entonces el recurso podría reprocesarse.

### 4.8 Concurrencia

Un mutex protege cache y flags dirty. Nunca se invoca I/O de archivos ni el timer de flush mientras se retiene el mutex más tiempo del necesario: se serializa a un buffer bajo lock y se escribe fuera del lock, o se usa un lock dedicado de flush. No se bloquea desde ISR. El callback del timer de flush solo notifica o agenda según el patrón del repo, y no hace I/O pesado en el contexto del timer service si el repo evita eso.

### 4.9 Kconfig

`components/claw_capabilities/cap_exec_state/Kconfig` (o `Kconfig.projbuild` según patrón del repo):

- `CAP_EXEC_STATE_MAX_ENTRIES` (int, default 200, rango 10–500).
- `CAP_EXEC_STATE_MAX_FILE_BYTES` (int, default 49152).
- `CAP_EXEC_STATE_FLUSH_DELAY_MS` (int, default 2000).
- `CAP_EXEC_STATE_LIST_DEFAULT_LIMIT` (int, default 20).

## 5. Archivos a crear/modificar

- Nuevo: `cap_exec_state/CMakeLists.txt`, `Kconfig`, `include/cap_exec_state.h`, `src/cap_exec_state.c` (store) y `src/cap_exec_state_caps.c` (capabilities, si el patrón las separa), `skills/exec_state/SKILL.md`.
- Nuevo: `cap_exec_state/test_apps/` (patrón existente de test apps Unity).
- Modificar: `components/common/app_claw/app_capabilities.c` (registro). Si el patrón lo exige, también el listado de skills/`CMakeLists` del app.
- Sin cambios en `claw_core`, scheduler ni router.

Cada archivo debe quedar por debajo de 1500 líneas.

## 6. Tests

Aunque no se puedan ejecutar en el entorno de NightShift, se agregan en `test_apps/` siguiendo el patrón del repo:

- mark nuevo → check devuelve entrada con `run_count = 1`.
- mark repetido → `run_count` incrementa y `first_seen_ms` se conserva.
- `failed` seguido de `completed` actualiza el status.
- Eviction: con `max_entries = N`, insertar N+1 evicta la menos reciente. Un `mark` sobre una entrada vieja la hace más reciente y la salva.
- Presupuesto en bytes: entradas de tamaño máximo no producen un archivo mayor que `max_file_bytes`.
- Persistencia: `delete` + `create` recarga el estado.
- Archivo ausente → vacío. Archivo corrupto/truncado → vacío sin crash.
- Tmp huérfano (`.tmp` de un corte previo) no afecta la carga.
- Validación: key vacía/larga, status inválido, `summary` truncado a 200 sin romper UTF-8.
- `list_json`: orden, `limit` y buffer chico.

## 7. Criterios de aceptación (mapeo al issue)

- El estado sobrevive reinicios → persistencia atómica en DATA y recarga en `start`.
- Un nuevo pulse puede consultar ejecuciones anteriores → `exec_state_check` y `exec_state_list`.
- Las scheduled tasks conocen su última ejecución → convención de clave `task:<id>` en la skill, con `last_run_ms`, `status` y `run_count`.
- Se pueden detectar y evitar duplicados → regla de dedup de 4.2 y flujo de la skill.
- Usable por Pulse y Scheduled Tasks → capa agnóstica del origen, expuesta vía capabilities y skill.

Se cumple de forma pragmática el "estado mínimo" del issue: identificador (`key`/`kind`), fecha/hora (`first_seen_ms`/`last_run_ms`), acción y resultado (`summary`), estado (`status`) y referencias de dedup (`key`).

## 8. Riesgos y notas

- **Pérdida del último mark ante corte de energía:** se mitiga con el flush diferido corto. El costo es un posible reproceso.
- **Reloj sin sincronizar:** el orden de eviction no depende del reloj. Los timestamps pueden ser poco confiables sin SNTP y se documenta.
- **Claves inestables del agente:** los duplicados dependen de la calidad de la clave, y la skill lo enfatiza.
- **Evicción de entradas `completed`:** puede reprocesar recursos muy antiguos. Es el comportamiento acordado (sin TTL).
- **Compatibilidad:** `version: 1` y un lector tolerante permiten extender el formato sin migraciones destructivas.

## 9. Verificación

El entorno de NightShift no tiene toolchain ESP-IDF, así que no se compila ni se ejecutan tests. La descripción del PR debe incluir lo siguiente y no declarar "tests pasados".

**Build (a ejecutar por quien revise):**

```
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build
```

**Pruebas manuales en placa:**

1. Marcar `lead:1` como `completed`, hacer `exec_state_check` y verificar `already_processed = true`.
2. Reiniciar la placa y repetir el check para confirmar persistencia.
3. Marcar `failed` y luego reintentar. Verificar que `run_count` sube y que `completed` cierra el ciclo.
4. Insertar más de `MAX_ENTRIES` claves y verificar eviction de las más antiguas con `exec_state_list`.
5. Corromper `exec_state.json` (truncarlo) y reiniciar. Debe arrancar vacío sin crash y con warning en el log.
6. Borrar el archivo y verificar arranque limpio.
7. Con `summary` de 200+ chars y claves en el límite, verificar que el archivo queda por debajo de 64KB.
8. Con el agente, ejecutar un flujo recurrente con la skill y verificar que no reprocesa lo `completed`.
9. Cortar la alimentación justo después de un `mark` para observar el comportamiento de la ventana de flush.
10. Revisar el desgaste: varios `mark` en ráfaga deben producir una sola escritura.