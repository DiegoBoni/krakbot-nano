# Tasks: Registro de actividad autónoma (`cap_activity_log`), issue #4

> Lista de tareas derivada del plan de implementación. Cada tarea indica los archivos que toca, de qué tareas depende y su criterio de aceptación.
> **Regla general:** si algo del repo contradice el plan, gana el repo. El desvío se anota en la sección "Supuestos verificados" del PR.
> **Aviso:** en NightShift no hay toolchain ESP-IDF. No se compila ni se ejecutan tests. La verificación se hace en placa (T-10.3).

---

## Fase 0: Relevamiento obligatorio (sin escribir código)

### T-0.1 Leer las convenciones del repo
- [ ] Leer `CLAUDE.md`, `.agents/design.md` y `.agents/gotchas.md`.
- [ ] Anotar:
  - convención de nombres;
  - patrón OO (handle explícito o singleton);
  - uso de PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` o un wrapper propio);
  - estilo de logging (`TAG`);
  - reglas de locks.
- **Aceptación:** las notas quedan en el borrador del PR, en "Supuestos verificados".

### T-0.2 Elegir un `cap_*` existente como plantilla
- [ ] Identificar el `cap_*` más parecido: uno que persista en DATA y exponga capabilities.
- [ ] Anotar:
  - la estructura de `CMakeLists.txt` (`idf_component_register`, `REQUIRES`/`PRIV_REQUIRES`);
  - el formato de `Kconfig.projbuild`;
  - el descriptor de capability: nombre, descripción, schema y firma del callback;
  - cómo devuelve el resultado y los errores al modelo;
  - si usa un handle explícito o una instancia de módulo;
  - el tope de tamaño de las respuestas.
- **Aceptación:** la plantilla queda elegida y las firmas reales quedan documentadas.

### T-0.3 Relevar `app_capabilities.c`
- [ ] Revisar `components/common/app_claw/app_capabilities.c`: orden de inicialización, registro de los `cap_*` y teardown (si existe).
- [ ] Confirmar que `cap_activity_log` puede crearse e iniciarse **antes** que `cap_scheduler`.
- **Aceptación:** está identificado el punto exacto de inserción.

### T-0.4 Relevar `cap_scheduler`
- [ ] Leer `cap_scheduler.h` y `cap_scheduler.c` (confirmar la ruta).
- [ ] Anotar sobre `cap_scheduler_publish_entry_locked`:
  - su firma y sus puntos de retorno;
  - qué mutex está tomado;
  - qué contadores se tocan;
  - qué campos de la entrada hay disponibles (id estable, tipo de schedule).
- **Aceptación:** están definidos el id que se usa como `ref` y el texto que va en `reason`.

### T-0.5 Relevar `claw_paths`, `claw_memory` y `claw_event`
- [ ] Revisar `claw_paths`:
  - firma real de `claw_paths_join` y de `CLAW_PATH_DATA`;
  - si el resultado va en un buffer del llamador o se aloca;
  - si hay helpers de `mkdir`.
- [ ] Revisar `claw_memory.h` y `claw_event.h` para alinear el formato de `ref` (ids de tarea y rutas).
- **Aceptación:** las firmas reales quedan anotadas.

### T-0.6 Relevar el mecanismo de skills
- [ ] Averiguar:
  - dónde viven las `SKILL.md`;
  - qué frontmatter requieren;
  - cómo se descubren (glob en build, embebido o copia a DATA).
- **Aceptación:** está definida la ruta final de `skills/activity_log/SKILL.md`.

### T-0.7 Relevar los patrones de tests
- [ ] Buscar `components/**/test_apps/` (Unity) o tests Lua.
- [ ] Elegir la plantilla más cercana y cómo arma su FS (partición FATFS de test o directorio temporal).
- **Aceptación:** está decidido si se agregan tests y con qué plantilla.

### T-0.8 Relevar FATFS/VFS y la partición
- [ ] Confirmar cómo se comporta `rename` con un destino existente (se espera `EEXIST`).
- [ ] Confirmar que existe `mkdir`.
- [ ] Revisar `.agents/gotchas.md` por otras particularidades del FS.
- [ ] Leer el tamaño real de la partición `storage` en `partitions*.csv` y el uso de DATA de los otros componentes.
- [ ] Verificar si cJSON ya se usa en el repo.
- [ ] Verificar si ya hay un helper de hora o de detección de "hora sincronizada".
- **Aceptación:** queda decidido si se mantiene el default `MAX_BYTES` = 256 KB o se baja a 128 KB.

### T-0.9 Cerrar los ajustes de diseño
- [ ] Confirmar o ajustar las decisiones de la §1 del plan:
  - singleton o handle;
  - buffer del llamador o salida alocada;
  - PSRAM;
  - helper de hora;
  - cJSON.
- [ ] Ajustar los nombres de la API pública a la convención real.
- **Depende de:** T-0.1 a T-0.8.
- **Aceptación:** la sección "Supuestos verificados" del PR está completa.

---

## Fase 1: Esqueleto del componente
Commit: `feat(cap_activity_log): esqueleto`

### T-1.1 `CMakeLists.txt` del componente
- [ ] Crear `components/claw_capabilities/cap_activity_log/CMakeLists.txt` según la plantilla:
  - SRCS: `cap_activity_log.c`, `cap_activity_log_store.c` y `cap_activity_log_caps.c`;
  - INCLUDE_DIRS: `include`;
  - REQUIRES/PRIV_REQUIRES mínimos: `esp_timer`, `freertos`, `json` (cJSON), `claw_paths`, `vfs`/`fatfs` y lo que pida el descriptor de capabilities.
- [ ] Verificar que **no** dependa de `cap_scheduler`.
- **Depende de:** T-0.9.

### T-1.2 `Kconfig.projbuild`
- [ ] Crear el menú "Activity log (cap_activity_log)" con estas opciones:

| Opción | Default | Rango |
|---|---|---|
| `CAP_ACTIVITY_LOG_MAX_BYTES` | 262144 (o 131072 según T-0.8) | 16384–1048576 |
| `CAP_ACTIVITY_LOG_FLUSH_INTERVAL_MS` | 60000 | 5000–600000 |
| `CAP_ACTIVITY_LOG_FLUSH_BATCH` | 16 | 1–128 |
| `CAP_ACTIVITY_LOG_BUFFER_ENTRIES` | 32 | 4–256 |
| `CAP_ACTIVITY_LOG_FLUSH_ON_ERROR` | y | bool |
| `CAP_ACTIVITY_LOG_TASK_STACK` | 4096 | — |

- [ ] Incluir textos de ayuda:
  - en `MAX_BYTES`, el peor caso en disco: 2 × el valor;
  - en `BUFFER_ENTRIES`, que debe ser ≥ `FLUSH_BATCH` y que al llenarse se descarta la entrada más vieja.

### T-1.3 Header público `include/cap_activity_log.h`
- [ ] Definir:
  - las constantes `ACTION_MAX` (96), `REASON_MAX` (128), `REF_MAX` (128), `DETAIL_MAX` (200), `LIST_MAX` (50) y `LIST_DEFAULT` (20);
  - los enums `cap_activity_origin_t` (`proactive_pulse`, `scheduled_task`, `manual`, `MAX`) y `cap_activity_result_t` (`completed`, `skipped`, `error`, `MAX`);
  - los structs `cap_activity_log_config_t` (`base_dir`, `max_bytes`) y `cap_activity_log_stats_t` (`pending`, `dropped`, `flush_errors`, `rotations`).
- [ ] Declarar los prototipos:
  - ciclo de vida: `create`, `start`, `stop`, `delete`;
  - operaciones: `append`, `list_json` (con `origin_filter`) y `get_stats`;
  - conversiones enum ↔ string.
- [ ] Documentar en cada función los contratos de error y la nota "no bloqueante, sin I/O" de `append`.
- [ ] Agregar `extern "C"` y `#pragma once`.

### T-1.4 Header privado `src/cap_activity_log_priv.h`
- [ ] Definir `activity_entry_t`: `ts_ms`, `origin`, `result` y los buffers de tamaño fijo `+1`. Un `ref` o `detail` vacío (`""`) significa "ausente".
- [ ] Definir `activity_log_t`:
  - `buf_mutex`, `file_mutex`;
  - el ring (`ring`, `cap`, `head`, `count`);
  - `flush_task`, `running` y un semáforo o event group de "done";
  - `dir`, `path_active`, `path_old`;
  - `active_size`, `max_bytes`, `need_leading_nl`, `stats`, `flush_batch`.
- [ ] Declarar los prototipos compartidos entre los `.c`:
  - store: `utf8_safe_copy`, `json_escape_append`, `activity_serialize_line`, `activity_store_flush`, `activity_store_rotate`, `activity_store_read`;
  - otros: `activity_now_ms`, `LINE_MAX`.

### T-1.5 Ciclo de vida y conversiones de enums (`cap_activity_log.c`)
- [ ] Implementar `create`:
  - singleton: `ESP_ERR_INVALID_STATE` si ya existe;
  - validar `BUFFER_ENTRIES >= FLUSH_BATCH`; si no se cumple, warning y `flush_batch = BUFFER_ENTRIES`;
  - alocar el ring (en PSRAM según la convención);
  - crear los mutex;
  - resolver los paths: `base_dir` o `claw_paths_join(CLAW_PATH_DATA, "activity")`, que dan `activity.jsonl` y `activity.1.jsonl`.
- [ ] Implementar las conversiones `*_to_str` / `*_from_str`: strings inválidos o NULL devuelven `ESP_ERR_INVALID_ARG`.
- [ ] Implementar `get_stats` bajo `buf_mutex`.
- [ ] Implementar `delete`: llama a `stop` si está corriendo y libera el ring, los mutex y los paths.
- **Aceptación:** `create`/`delete` repetidos no pierden memoria. Llamar a `append` antes de `start` devuelve `ESP_ERR_INVALID_STATE`.

---

## Fase 2: Buffer, `append`, volcado y rotación
Commit: `feat(cap_activity_log): buffer, append, volcado y rotación`

### T-2.1 Utilidades de texto (`cap_activity_log_store.c`)
- [ ] Implementar `utf8_safe_copy(dst, src, max)`: trunca en `max` bytes y retrocede mientras `(b & 0xC0) == 0x80`, para no dejar un multibyte incompleto. Siempre termina en `'\0'`.
- [ ] Implementar `json_escape_append(dst, cap, src)`:
  - `"` va como `\"` y `\` como `\\`;
  - `\n`, `\r`, `\t`, `\b` y `\f` se escapan;
  - el resto de `< 0x20` y `0x7F` va como `\u00XX`;
  - los bytes ≥ 0x80 se copian tal cual.
- [ ] Definir `LINE_MAX` = `64 + 6 × (ACTION_MAX + REASON_MAX + REF_MAX + DETAIL_MAX) + 2` (≈ 3.4 KB).
- **Aceptación:** la salida escapada nunca contiene un `\n` crudo.

### T-2.2 Serialización de la línea JSONL
- [ ] Implementar `activity_serialize_line(entry, buf, cap)`:
  - claves en orden fijo: `ts, origin, action, reason, result[, ref][, detail]`;
  - `ref` y `detail` se omiten si están vacíos;
  - la línea termina en `\n`;
  - devuelve la longitud.
- [ ] El escritor es propio, sin cJSON ni allocs.

### T-2.3 Hora (`activity_now_ms`)
- [ ] Usar `gettimeofday()`. Si el año es < 2020, caer a `esp_timer_get_time()/1000`.
- [ ] Si T-0.8 encontró un helper existente, usarlo en lugar de esta lógica.

### T-2.4 `cap_activity_log_append` (sin I/O)
- [ ] Implementar la lógica:
  1. Si `!running`, devolver `ESP_ERR_INVALID_STATE`.
  2. Validar los enums y que `action`/`reason` no sean NULL ni `""`. Si algo falla, `ESP_ERR_INVALID_ARG`.
  3. Armar la entrada en el stack **fuera del lock**, con `ts` y las copias truncadas en UTF-8.
  4. Tomar `buf_mutex` con un timeout de 5 ms. Si hay timeout, `dropped++` y devolver `ESP_ERR_TIMEOUT`.
  5. Si el ring está lleno, descartar la más vieja y hacer `dropped++`. Insertar la entrada y actualizar `pending`.
  6. Calcular `need_flush = count >= flush_batch || (FLUSH_ON_ERROR && result == ERROR)`.
  7. Soltar el mutex. Si `need_flush`, hacer `xTaskNotifyGive(flush_task)`.
- [ ] Documentar en el código que `buf_mutex` es un lock hoja: con él tomado no se llama a nada externo.
- **Aceptación:** `append` no hace I/O, no aloca memoria y no bloquea más de 5 ms.

### T-2.5 Volcado y rotación (`activity_store_flush`)
- [ ] Hacer el volcado en pasos, con los dos mutex nunca anidados:
  1. Bajo `buf_mutex`, mover hasta `flush_batch` entradas a un lote local o estático y soltar el mutex.
  2. Serializar en el scratch estático `line_buf` y sumar `batch_bytes`.
  3. Bajo `file_mutex`, rotar si `active_size + batch_bytes > max_bytes`:
     - `unlink(path_old)`, ignorando `ENOENT`;
     - `rename(path_active, path_old)`;
     - `active_size = 0` y `rotations++`;
     - si la rotación falla, loguear y seguir escribiendo en el activo.
  4. Escribir:
     - `fopen(path_active, "a")`;
     - si `need_leading_nl`, escribir `\n` primero y limpiar el flag;
     - `fwrite` de las líneas y `fclose`, **sin `fsync`**;
     - actualizar `active_size`.
  5. Si hay un error de I/O, `flush_errors++`. Las entradas del lote se pierden, no se reencolan.
  6. Repetir mientras queden pendientes.
- **Aceptación:** `file_mutex` y `buf_mutex` nunca están tomados a la vez.

### T-2.6 Tarea de volcado y `start`/`stop`
- [ ] Implementar `start`:
  1. Crear el directorio con `mkdir(dir, 0755)`, tolerando `EEXIST`.
  2. Hacer `stat(path_active)` para tomar `active_size` (0 si no existe).
  3. Si el archivo no está vacío, leer el último byte. Si no es `\n`, marcar `need_leading_nl = true`.
  4. Crear la tarea `activity_log_flush` con prioridad baja y el stack de Kconfig.
  5. Poner `running = true`.
- [ ] Implementar el loop de la tarea:
  1. `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS))`;
  2. `flush_pending()`;
  3. si `!running`, salir del loop;
  4. al salir, señalar "done" y `vTaskDelete(NULL)`.
- [ ] Implementar `stop`:
  1. `running = false` y `xTaskNotifyGive`;
  2. esperar el "done" con timeout;
  3. hacer un `flush_pending()` final desde el contexto de `stop`.
- **Aceptación:** con un intervalo largo y menos de `flush_batch` entradas, el archivo no cambia. Tras `stop`, todas las entradas pendientes están en disco.

---

## Fase 3: Lectura tolerante y `list_json`
Commit: `feat(cap_activity_log): lectura y list_json`

### T-3.1 Parseo de línea
- [ ] Implementar `activity_parse_line(line, entry)` con cJSON. Devuelve error si:
  - el JSON es inválido;
  - falta `ts`, `origin`, `action`, `reason` o `result`;
  - un tipo no corresponde;
  - un enum no se reconoce.
- [ ] Ignorar los campos desconocidos.
- [ ] Copiar los strings con `utf8_safe_copy`.
- [ ] Llevar un contador de líneas saltadas (log de debug).

### T-3.2 Lectura de los segmentos (`activity_store_read`)
- [ ] Bajo `file_mutex`, leer en orden cronológico: primero `activity.1.jsonl` y después `activity.jsonl`, con `fgets` sobre un buffer de lectura de `LINE_MAX`.
- [ ] Manejar las líneas demasiado largas (sin `\n` y sin EOF): descartar el resto hasta `\n` y saltar la línea.
- [ ] Tratar un archivo inexistente como vacío.
- [ ] Si la entrada cumple `ts >= since_ms` y el filtro de origen, empujarla al ring de resultados de tamaño `limit`, sobrescribiendo la más vieja.

### T-3.3 `cap_activity_log_list_json`
- [ ] Acotar `limit`: `0` pasa a `LIST_DEFAULT` y cualquier valor `> LIST_MAX` se recorta a `LIST_MAX`.
- [ ] Alocar el ring de resultados en PSRAM si está disponible. Es temporal, ≤ 28 KB.
- [ ] Leer los segmentos (T-3.2) y soltar `file_mutex`.
- [ ] Bajo `buf_mutex`, por separado, agregar las entradas pendientes con el mismo filtro.
- [ ] Escribir en `out` un array JSON de la más nueva a la más vieja, con el escritor de T-2.2.
- [ ] Si `out_size` no alcanza, cerrar el array con las entradas que entraron y devolver `ESP_ERR_INVALID_SIZE` junto con `*out_count`.
- [ ] Liberar el ring.
- [ ] Si el log no está iniciado, devolver `ESP_ERR_INVALID_STATE`.
- **Aceptación:** una línea corrupta o truncada no rompe el listado. Las entradas sin volcar aparecen en la salida.

---

## Fase 4: Capabilities y registro
Commit: `feat(cap_activity_log): capabilities y registro`

### T-4.1 Capability `activity_log_list` (`cap_activity_log_caps.c`)
- [ ] Schema:
  - `limit`: integer, opcional, default 20, máximo 50;
  - `origin`: enum `proactive_pulse | scheduled_task | manual`, opcional;
  - `since_ms`: integer, opcional, default 0.
- [ ] Callback:
  1. Validar. Un `origin` inválido devuelve el error `"invalid origin: <v>; expected proactive_pulse|scheduled_task|manual"`.
  2. Llamar a `list_json` con un buffer de 8 KB (o el tope del repo).
  3. Responder `{"entries":[...],"count":N,"truncated":bool,"dropped":D}`, o el formato del repo.
  4. Si el log no está iniciado, devolver `"activity log not available"`.

### T-4.2 Capability `activity_log_append`
- [ ] Schema:
  - obligatorios: `origin`, `action`, `reason`, `result` (enum `completed | skipped | error`);
  - opcionales: `ref`, `detail`.
- [ ] La descripción para el modelo menciona que `detail` tiene un máximo de 200 caracteres y que no hay que incluir secretos.
- [ ] Callback:
  1. Validar con `*_from_str` y llamar a `append`.
  2. Mapear los errores:
     - `INVALID_ARG` indica qué campo falló;
     - `INVALID_STATE` es "log no disponible";
     - `TIMEOUT` es "log ocupado, reintentar".
  3. En el éxito devolver `{"ok":true}` y aclarar que la entrada queda pendiente de volcado pero ya es visible en `activity_log_list`.

### T-4.3 Función de registro
- [ ] Implementar `cap_activity_log_register_capabilities(...)` con el nombre y la firma del patrón real.

### T-4.4 Integración en `app_claw`
- [ ] Agregar `cap_activity_log` a `REQUIRES`/`PRIV_REQUIRES` en el `CMakeLists.txt` de `app_claw`.
- [ ] En `app_capabilities.c`:
  - `create` + `start` del log **antes** que el scheduler;
  - registrar las capabilities;
  - `stop`/`delete` en el teardown, si existe.
- [ ] Si el log falla al iniciar, loguear el error y seguir, sin abortar el arranque. Confirmar que esto respeta la convención del repo.
- **Depende de:** T-0.3, T-4.3.

---

## Fase 5: Instrumentación del scheduler
Commit: `feat(cap_scheduler): registro del disparo`

### T-5.1 Dependencia
- [ ] Agregar `cap_activity_log` a `PRIV_REQUIRES` en `components/claw_capabilities/cap_scheduler/CMakeLists.txt`.
- [ ] Verificar que no haya una dependencia circular.

### T-5.2 Instrumentar `cap_scheduler_publish_entry_locked`
- [ ] Insertar el bloque de registro **después** de la publicación y de la actualización de contadores, y **antes** del `return`:
  - `origin = SCHEDULED_TASK`;
  - `action = "scheduled task fired"`;
  - `reason = "schedule <tipo/id>"`;
  - `ref = <id estable de la entrada>`;
  - si `err == ESP_OK`: `result = completed`, `detail = "event published"`;
  - si no: `result = error`, `detail = "publish failed: <esp_err_to_name>"`.
- [ ] Ignorar `INVALID_STATE` en silencio. Para cualquier otro error, solo `ESP_LOGD`.
- [ ] No modificar `err`, los contadores ni el orden existente.
- [ ] Si hay varios puntos de retorno, usar un `goto out` o un helper **solo** si no cambia el flujo. Si no, llamar en cada rama relevante.
- **Aceptación:** el valor de retorno y los contadores son idénticos con el log iniciado y sin iniciar.

### T-5.3 Documentar la semántica de `ESP_OK`
- [ ] Agregar un comentario en `cap_scheduler.h`: "`ESP_OK` = evento publicado, no trabajo terminado. El resultado final lo registra el agente vía `activity_log_append`."

---

## Fase 6: Skill y documentación
Commit: `docs(cap_activity_log)`

### T-6.1 `skills/activity_log/SKILL.md`
- [ ] Usar la ruta y el frontmatter que salgan de T-0.6.
- [ ] Contenido:
  1. **Propósito** del registro.
  2. **Cuándo registrar:**
     - al terminar un pulso proactivo;
     - al terminar una tarea programada (con `ref` = id de la tarea);
     - cuando se decide no actuar (`skipped`, con el motivo);
     - ante cualquier error (`error`, con un `detail` breve);
     - las acciones del usuario que conviene trazar usan `manual`.
  3. **Aclaración:** el scheduler ya registra el disparo. El agente agrega otra entrada con el resultado final, usando el mismo `ref`.
  4. **Cómo escribir cada campo:** `action` y `reason` concisos, `result` honesto, `ref` con un id o una ruta, `detail` de ≤ 200 caracteres.
  5. **Cuándo consultar:** antes de repetir una acción y cuando el usuario pregunta qué hizo el bot.
  6. **Qué no registrar:** secretos, tokens, credenciales, mensajes privados ni datos personales sensibles.
  7. **Ejemplos:** uno `completed` (briefing de Cliente X) y uno `skipped` (revisar correo sin mensajes nuevos).

### T-6.2 `cap_activity_log/README.md`
- [ ] Documentar:
  - formato JSONL y tabla de campos;
  - política de retrocompatibilidad (campos nuevos opcionales y campos desconocidos ignorados);
  - Kconfig y defaults;
  - peor caso de disco (2 × `MAX_BYTES`), con la cuenta contra la partición `storage` real;
  - estrategia de volcado y pérdida posible ante un corte de energía;
  - política de buffer lleno (`dropped`);
  - orden de locks;
  - semántica de la entrada del scheduler;
  - limitación de cobertura;
  - `ts` sin hora sincronizada;
  - comportamiento de `rename` en FATFS y atomicidad de la rotación.

### T-6.3 Documentación general
- [ ] Si `.agents/design.md` o el README del repo mantienen un inventario de componentes o de uso de DATA, agregar una línea.

---

## Fase 7: Tests (solo si existe el patrón, según T-0.7)
Commit: `test(cap_activity_log)`

### T-7.1 Estructura de `test_apps/`
- [ ] Crear `CMakeLists.txt`, `main/CMakeLists.txt`, `main/test_cap_activity_log.c` y `sdkconfig.defaults` según la plantilla.
- [ ] Usar `base_dir` de test, `max_bytes` de 4 KB y un intervalo de volcado largo.

### T-7.2 Tests unitarios
- [ ] `escape_special_chars`
- [ ] `detail_truncate_utf8`
- [ ] `invalid_args`
- [ ] `not_started`
- [ ] `enum_str_roundtrip`

### T-7.3 Tests de persistencia
- [ ] `missing_dir_and_file`
- [ ] `corrupt_last_line`
- [ ] `unknown_fields_ignored`
- [ ] `rotation`: verifica que `.1` existe, que el tamaño total es ≤ 2 × `max_bytes` + `LINE_MAX` y que una segunda rotación reemplaza `.1`.

### T-7.4 Tests de listado
- [ ] `list_limit_since_origin`
- [ ] `list_includes_pending`
- [ ] `list_across_segments`

### T-7.5 Tests de volcado y concurrencia
- [ ] `no_write_before_batch`
- [ ] `final_flush_on_stop`
- [ ] `buffer_overflow_drops_oldest`
- [ ] `concurrent_appends`: verifica que `count + dropped == N × M` y que todas las líneas son válidas.

### T-7.6 Test del scheduler
- [ ] `scheduler_fire_logs_entry` (si existe un test del scheduler): se genera una entrada `scheduled_task` con `ref` = id, y el retorno y los contadores son iguales con y sin el log iniciado.

---

## Fase 8: Revisión final y PR

### T-8.1 Autorrevisión
- [ ] Cada archivo queda en menos de 1500 líneas (estimado: ~300, ~450 y ~250).
- [ ] No hay I/O ni allocs en `append`.
- [ ] `buf_mutex` y `file_mutex` nunca están anidados.
- [ ] No se tocó `claw_core` ni `claw_event_router`.
- [ ] Los nombres y el estilo coinciden con la plantilla.

### T-8.2 Descripción del PR
- [ ] Resumen y enlace a #4.
- [ ] Sección "Supuestos verificados", con lo de la Fase 0 y los desvíos respecto del plan.
- [ ] Comandos de build: `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`.
- [ ] Aviso explícito: **no se compiló ni se ejecutaron tests en NightShift**.
- [ ] Checklist de pruebas manuales en placa (T-8.3).
- [ ] Limitación: los pulsos proactivos y el resultado final dependen de que el agente llame a `activity_log_append`.
- [ ] Seguimiento propuesto (no implementado): un hook en `claw_event_router`.

### T-8.3 Pruebas manuales en placa (las ejecuta el revisor)
- [ ] Arranque limpio sin `activity/`: se crea el directorio y la lista devuelve vacío.
- [ ] Disparo de una tarea programada: aparece una entrada `scheduled_task` tras el volcado.
- [ ] `append` con `completed`, `skipped` y `error`: las tres se listan.
- [ ] Filtros `origin` y `since_ms`.
- [ ] Volumen por encima de `MAX_BYTES`: existe `activity.1.jsonl` y el espacio queda acotado.
- [ ] Reinicio: el historial persiste. Corte de energía durante la escritura: arranca sin crash.
- [ ] Espacio libre en DATA antes y después.
- [ ] El comportamiento del scheduler no cambió.

---

## Dependencias entre fases

```
Fase 0 ──► Fase 1 ──► Fase 2 ──► Fase 3 ──► Fase 4 ──► Fase 5
                                              │
                                              └──► Fase 6 ──► Fase 7 ──► Fase 8
```

- La Fase 5 requiere que `append` esté implementado (T-2.4) y que el arranque esté integrado (T-4.4).
- La Fase 7 se omite completa si T-0.7 no encuentra un patrón de tests.