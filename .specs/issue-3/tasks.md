# Tasks: `cap_exec_state` (issue #3)

> Lista de tareas derivada del plan de implementación. Las tareas marcadas con **[CONFIRMAR]** dependen de lo que se resuelva en la Fase 0. No se puede compilar ni ejecutar tests en NightShift porque no hay toolchain ESP-IDF. La validación de build y tests queda para quien revise el PR.

---

## Fase 0: Reconocimiento (bloqueante)

- [ ] **T0.1** Leer `cap_scheduler.h` y `cap_scheduler_store.c`. Anotar:
  - si usa handle o singleton;
  - las firmas de `create/start/stop/delete`;
  - cómo carga y guarda con cJSON;
  - el estilo de logging (`TAG`, `ESP_LOGW`);
  - si usa PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`).
- [ ] **T0.2** Leer `claw_memory.h/.c`. Anotar el patrón de mutex, si ya existe un helper de truncado UTF-8-safe y si ya existe escritura atómica (`tmp` + `rename`).
- [ ] **T0.3** Leer `claw_paths.h`. Anotar la firma de `claw_paths_join`, el valor de `CLAW_PATH_DATA`, si hay helper `mkdir -p` y el tipo de FS de la partición `storage` (LittleFS, SPIFFS o FAT).
- [ ] **T0.4** Leer `claw_cap.h` o su equivalente. Anotar el struct descriptor de capability, la firma del handler y la convención para devolver errores al modelo.
- [ ] **T0.5** Leer `claw_event.h`. Ver si hay una work queue o un event loop reutilizable para el flush diferido.
- [ ] **T0.6** Leer `components/common/app_claw/app_capabilities.c` y su `CMakeLists.txt`. Anotar el patrón de registro, los `#if CONFIG_...` y los `REQUIRES`.
- [ ] **T0.7** Revisar otra `cap_*` que tenga `skills/`. Anotar el formato del frontmatter de `SKILL.md` y cómo se descubre o embebe la skill.
- [ ] **T0.8** Revisar algún `cap_*/test_apps/`. Anotar la estructura Unity, `sdkconfig.defaults`, `partitions.csv` y cómo se monta el FS en los tests.
- [ ] **T0.9** Revisar el Kconfig de otra capability. Anotar si usa `Kconfig` o `Kconfig.projbuild`, el prefijo de símbolos y el menú padre.
- [ ] **T0.10** Buscar un helper de tiempo (`claw_time_now_ms()` o similar). Si no existe, usar `gettimeofday`.
- [ ] **T0.11** Escribir la sección "Decisiones de alineación" con lo resuelto: patrón de handle, FS y estrategia de `rename`, mecanismo de flush diferido y firma del handler.

---

## Fase 1: Esqueleto del componente

- [ ] **T1.1** Crear la estructura `components/claw_capabilities/cap_exec_state/` con `include/`, `src/`, `skills/exec_state/` y `test_apps/main/`.
- [ ] **T1.2** Escribir `CMakeLists.txt` con `idf_component_register`:
  - SRCS `src/cap_exec_state.c` y `src/cap_exec_state_caps.c`;
  - INCLUDE_DIRS `include`;
  - `REQUIRES` según T0.3 y T0.4;
  - `PRIV_REQUIRES json esp_timer freertos log vfs`.
- [ ] **T1.3** Embeber o registrar `skills/exec_state/SKILL.md` siguiendo el patrón de T0.7. **[CONFIRMAR]**
- [ ] **T1.4** Escribir el `Kconfig` (o `Kconfig.projbuild` según T0.9) con estas opciones:

  | Símbolo | Default | Rango |
  |---|---|---|
  | `CAP_EXEC_STATE_MAX_ENTRIES` | 200 | 10–500 |
  | `CAP_EXEC_STATE_MAX_FILE_BYTES` | 49152 | 4096–65535 |
  | `CAP_EXEC_STATE_FLUSH_DELAY_MS` | 2000 | 0–60000 |
  | `CAP_EXEC_STATE_LIST_DEFAULT_LIMIT` | 20 | 1–500 |

- [ ] **Commit:** `cap_exec_state: skeleton, Kconfig, header` (incluye la Fase 2).

---

## Fase 2: Header público

- [ ] **T2.1** Definir las constantes `KEY_MAX_LEN=64`, `KIND_MAX_LEN=24`, `SUMMARY_MAX_LEN=200` y `FORMAT_VERSION=1`.
- [ ] **T2.2** Definir `cap_exec_state_status_t` con los valores `COMPLETED`, `FAILED` y `SKIPPED`.
- [ ] **T2.3** Definir `cap_exec_state_entry_t` con los campos `key`, `kind`, `status`, `first_seen_ms`, `last_run_ms`, `run_count` y `summary`.
- [ ] **T2.4** Definir `cap_exec_state_config_t` y la macro `CAP_EXEC_STATE_CONFIG_DEFAULT()` (`path=NULL`, `max_entries=0`, `max_file_bytes=0`, `flush_delay_ms=-1`).
- [ ] **T2.5** Declarar el ciclo de vida `create/start/stop/delete` según T0.1 (singleton o handle). **[CONFIRMAR]**
- [ ] **T2.6** Declarar `check`, `mark` y `list_json` con las firmas fijas del spec.
- [ ] **T2.7** Declarar los helpers `status_to_str`, `status_from_str` y `flush`. Agregar `count()` solo si los tests lo necesitan.
- [ ] **T2.8** Escribir el comentario de cabecera: semántica, límites, tradeoff del flush y la aclaración de que no es ISR-safe.
- [ ] **T2.9** Crear el header privado `src/cap_exec_state_priv.h` con `entry_to_cjson()` y `cap_exec_state_mark_ex(..., uint32_t *out_run_count)`.

---

## Fase 3: Store (`src/cap_exec_state.c`)

### 3.1 Estado y helpers

- [ ] **T3.1** Definir el struct interno: paths (`path`, `tmp`, `bad`), límites, el array `entries`, `count`, `lock`, `flush_lock`, `dirty`, `started`, `flush_timer` y `flush_task` (esta última solo si hace falta).
- [ ] **T3.2** Alocar `entries` con `max_entries` slots en PSRAM, con fallback a RAM interna.
- [ ] **T3.3** Implementar `utf8_truncate_copy`, o reusar el de `claw_memory` si existe (T0.2).
- [ ] **T3.4** Implementar `find_index`, `move_to_tail`, `evict_head` y `now_ms`.

### 3.2 Operaciones en memoria

- [ ] **T3.5** Implementar `mark_ex` y hacer que `mark` la envuelva:
  - validar `key` (no NULL, no vacía, ≤64 bytes) y el rango de `status`; si fallan, devolver `ESP_ERR_INVALID_ARG`;
  - truncar `kind` y `summary` de forma UTF-8-safe;
  - si la key ya existe: actualizar campos, incrementar `run_count` saturando en `UINT32_MAX` y mover la entrada al final;
  - si es nueva: hacer evict del head si el array está lleno y agregarla al final con `run_count=1`;
  - poner `dirty = true` y disparar el flush (inmediato si el delay es 0; si no, armar el timer **sin reiniciarlo** si ya está armado).
- [ ] **T3.6** Implementar `check`: lectura bajo lock, sin reordenar. Devuelve `ESP_OK` o `ESP_ERR_NOT_FOUND`.
- [ ] **T3.7** Implementar `list_json`:
  - si `limit` es 0 o mayor que el máximo, usar el máximo;
  - copiar las `k` entradas a un buffer temporal bajo lock;
  - armar el JSON fuera del lock, de la más reciente a la más antigua;
  - imprimir con `cJSON_PrintPreallocated`;
  - si no entra, reintentar quitando las más antiguas y agregar `"truncated":true`;
  - si no entra ni vacío, devolver `ESP_ERR_NO_MEM`.
- [ ] **T3.8** Implementar `status_to_str` y `status_from_str`.
- [ ] **Commit:** `cap_exec_state: in-memory store, mark/check/list`.

### 3.3 Persistencia

- [ ] **T3.9** Implementar `load_locked`:
  - asegurar que existe el directorio `state/`;
  - si el archivo no existe, arrancar vacío;
  - si el tamaño excede el límite o el JSON es inválido (falta `entries`, `version` no es número o es mayor a la soportada), renombrar a `.bad` (un solo backup) y arrancar vacío;
  - descartar items sin `key` válida;
  - completar campos faltantes con defaults (`status` desconocido → `failed`, `run_count=1`, strings vacíos);
  - ignorar campos desconocidos; si hay keys duplicadas, gana la última;
  - si hay más de `max_entries` items, recortar a los últimos y marcar `dirty`;
  - borrar el `.tmp` huérfano.
- [ ] **T3.10** Implementar `flush_now`:
  - tomar los locks en orden `flush_lock` → `lock`;
  - serializar con `cJSON_PrintUnformatted`;
  - aplicar el presupuesto de bytes con evict estimado y reserialización, logueando con `ESP_LOGW`;
  - poner `dirty = false` y hacer el I/O fuera de `lock`;
  - escribir `fopen`/`fwrite`/`fflush`/`fclose` al `.tmp` verificando cada retorno, y después `rename`;
  - ante un error: `ESP_LOGE`, volver a `dirty = true` y re-agendar el timer (2000 ms si el delay es 0).
- [ ] **T3.11** Implementar la estrategia de `rename` según el FS (T0.3). **[CONFIRMAR]** En SPIFFS o FAT:
  - el flush hace `unlink(path)` y después `rename(tmp, path)`;
  - la carga promueve el `.tmp` solo si falta el archivo principal y el `.tmp` es parseable.
- [ ] **T3.12** Implementar el disparo del flush diferido: el callback del `esp_timer` encola `flush_now` en la work queue existente (T0.5) o hace `xTaskNotifyGive` a una task `exec_state_flush` (~4KB de stack, prioridad baja). Nunca se hace I/O ni cJSON en el contexto del `esp_timer`. **[CONFIRMAR]**
- [ ] **T3.13** Implementar `create`, `start`, `stop` (detiene el timer y hace flush síncrono), `delete` (libera todo y deja la instancia en cero) y el `flush()` público.
- [ ] **T3.14** Revisar la concurrencia: el orden de locks siempre es `flush_lock` → `lock`, y `check`, `mark` y `list` nunca toman `flush_lock`.
- [ ] **Commit:** `cap_exec_state: atomic persistence, tolerant load, deferred flush`.

---

## Fase 4: Capabilities del modelo (`src/cap_exec_state_caps.c`)

- [ ] **T4.1** Implementar `entry_to_cjson()` compartido entre `list_json` y `check`, para que ambos usen el mismo formato.
- [ ] **T4.2** Capability `exec_state_check`:
  - schema con `key` requerida;
  - salida `{"found":false,"already_processed":false}` o `{"found":true,"already_processed":<status==completed>,"entry":{...}}`.
- [ ] **T4.3** Capability `exec_state_mark`:
  - schema con `key`, `kind` y `status` (enum) requeridos, y `summary` opcional;
  - usa `mark_ex` para obtener `run_count` sin carrera;
  - salida `{"ok":true,"run_count":N}`.
- [ ] **T4.4** Capability `exec_state_list`:
  - schema con `limit` opcional; default `CONFIG_CAP_EXEC_STATE_LIST_DEFAULT_LIMIT`;
  - salida `{"count":k,"entries":[...],"truncated"?:true}`.
- [ ] **T4.5** Manejar errores de validación (key inválida, status inválido, JSON mal formado) devolviendo `{"ok":false,"error":"..."}` sin abortar el turno, según T0.4.
- [ ] **T4.6** Definir los descriptores de las 3 capabilities con el struct de T0.4. **[CONFIRMAR]**
- [ ] **Commit:** `cap_exec_state: model capabilities`.

---

## Fase 5: Registro en la app

- [ ] **T5.1** En `app_capabilities.c`: agregar `#include "cap_exec_state.h"`, llamar `create(CONFIG_DEFAULT)` y `start`, y registrar las 3 capabilities.
- [ ] **T5.2** Si `create` falla: `ESP_LOGE` y seguir sin registrar las caps, sin caer el boot.
- [ ] **T5.3** Envolver el registro en `#if CONFIG_...` solo si las otras capabilities lo hacen.
- [ ] **T5.4** Agregar `cap_exec_state` a `REQUIRES`/`PRIV_REQUIRES` de `app_claw`.
- [ ] **Commit:** `app_claw: register cap_exec_state`.

---

## Fase 6: Skill `skills/exec_state/SKILL.md`

- [ ] **T6.1** Escribir el frontmatter con el formato de T0.7 (`name`, `description`, capabilities).
- [ ] **T6.2** Sección "Cuándo usarla": pulses autónomos y tareas programadas que procesan recursos individuales.
- [ ] **T6.3** Sección "Claves estables": formato `<tipo>:<id_estable>`, ejemplos, prohibido usar títulos o fechas relativas, máximo 64 chars.
- [ ] **T6.4** Sección "Flujo obligatorio": `check`, omitir o procesar según el resultado, y `mark` con un summary breve.
- [ ] **T6.5** Tabla de reglas de dedup: `completed`, `failed`, `skipped` y sin entrada.
- [ ] **T6.6** Sección "Última ejecución de una scheduled task" con la convención `task:<id>`.
- [ ] **T6.7** Sección "Contexto de ciclos previos" con `exec_state_list(limit)`.
- [ ] **T6.8** Sección "Límites": eviction de entradas, reloj sin SNTP y posible pérdida del último `mark` ante un corte de energía.
- [ ] **Commit:** `cap_exec_state: exec_state skill`.

---

## Fase 7: Tests (`test_apps/`)

- [ ] **T7.1** Armar el test app con el patrón de T0.8: `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` si hace falta, montaje del FS y `cfg.path` en un directorio de test.
- [ ] **T7.2** Implementar los casos en `test_cap_exec_state.c`:

  | Grupo | Casos |
  |---|---|
  | Operaciones básicas | **T1** mark nuevo (`run_count=1`, `first_seen==last_run`); **T2** mark repetido (`run_count=2`, `first_seen` estable); **T3** `failed` → `completed`; **T4** check inexistente → `NOT_FOUND` |
  | Eviction | **T5** por cantidad (N+1 → `k0` evicted); **T6** re-mark salva `k0`; **T7** `check` no reordena; **T8** presupuesto de bytes (`st_size ≤ 8192`) |
  | Persistencia | **T9** sobrevive `delete` + `create`; **T10** archivo ausente; **T11** archivo corrupto → vacío + `.bad`; **T12** versión futura; **T13** campos faltantes o desconocidos; **T14** `.tmp` huérfano |
  | Validación | **T15** key `""`/NULL/65 chars y status fuera de rango → `INVALID_ARG`; **T16** truncado UTF-8 (summary 199+"é" → 199 bytes; kind de 30 → 24) |
  | `list_json` | **T17** orden y límite (`limit=3`, `limit=0`); **T18** buffer chico, con canario contra overflow |
  | Flush y saturación | **T19** flush agrupado (`delay=200`, 10 marks, archivo correcto tras 300 ms); **T20** `run_count` saturado en `UINT32_MAX` |

- [ ] **Commit:** `cap_exec_state: unity test app`.

---

## Fase 8: Documentación

- [ ] **T8.1** Escribir un `README.md` del componente (o la sección equivalente en la doc de capabilities) con:
  - los defaults de Kconfig;
  - el formato del archivo;
  - la ubicación `<DATA>/state/exec_state.json`;
  - el comportamiento ante corrupción;
  - la convención `task:<id>`.
- [ ] **Commit:** `docs: cap_exec_state defaults and semantics`.

---

## Fase 9: PR

- [ ] **T9.1** Incluir la sección "Decisiones de alineación" (resultado de T0.11).
- [ ] **T9.2** Aclarar de forma explícita que **no se compiló ni se ejecutaron tests en NightShift (sin toolchain ESP-IDF)**.
- [ ] **T9.3** Incluir el comando de build `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build` y el comando del test app.
- [ ] **T9.4** Copiar como checklist las 10 pruebas manuales en placa de la sección 9 del spec.
- [ ] **T9.5** Documentar los tradeoffs: pérdida del último mark en la ventana de flush, eviction de `completed` y reloj sin SNTP.
- [ ] **T9.6** Confirmar que no hay cambios en `claw_core`, scheduler ni router.
- [ ] **T9.7** Verificar que cada archivo tenga menos de 1500 líneas (estimado: store ~600, caps ~250, tests ~500).

---

## Riesgos a vigilar durante la implementación

| Riesgo | Mitigación (tarea) |
|---|---|
| `rename` no sobrescribe en SPIFFS o FAT | T3.11 |
| Stack overflow si el flush corre en el `esp_timer` | T3.12 |
| Una ráfaga continua posterga el flush | T3.5 (el timer no se reinicia) |
| Se baja `MAX_ENTRIES` entre versiones | T3.9 (recorte + `dirty`) |
| Un status ilegible en disco bloquea el reproceso | T3.9 (default `failed`) |
| Carrera entre `mark` y la lectura de `run_count` | T2.9 / T4.3 (`mark_ex`) |