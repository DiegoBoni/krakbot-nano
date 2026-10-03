# Tasks: Autonomous Pulse (Proactive Agent Mode), issue #1

> Repo: `DiegoBoni/krakbot-nano` · Spec: issue #1 · Plan: Autonomous Pulse
> Los nombres de APIs de `cap_scheduler`, `cap_router_mgr`, `claw_event`, `claw_memory` y `claw_paths` son **tentativos** hasta cerrar la Fase 0. No se inventa ninguna API. `claw_core` no se modifica.

---

## Fase 0: Verificaciones previas (bloqueantes)

### T0.1 Verificar las dependencias #3 y #4
- [ ] Buscar `cap_exec_state` y `cap_activity_log` en `components/claw_capabilities/` de la rama base.
- [ ] Confirmar que están registradas en `components/common/app_claw/app_capabilities.c`.
- [ ] Si están: anotar los nombres reales de las capabilities (`exec_state_check`, `exec_state_mark`, `activity_log`) y sus argumentos exactos (clave, estado, formato de entrada del log).
- [ ] Si faltan: **no reimplementarlas**. Anotar el bloqueo para incluirlo en el PR en la sección "Bloqueado por #3 / #4".

**Criterio de aceptación:** queda registrado si #3 y #4 están presentes (con nombres y argumentos reales) o si el PR va a salir bloqueado.

### T0.2 Leer convenciones del repo
- [ ] Leer `CLAUDE.md`.
- [ ] Leer `.agents/design.md`.
- [ ] Leer `.agents/gotchas.md` (FATFS, PSRAM, tareas).

**Criterio de aceptación:** convenciones de capability, estilo C orientado a objetos, manejo de errores y gotchas identificados.

### T0.3 Leer la API de `cap_scheduler`
- [ ] Leer `cap_scheduler.h` y su `.c`.
- [ ] Identificar la API de alta, actualización, borrado y lookup de items.
- [ ] Confirmar cómo se usa el modo `INTERVAL` y la firma de `cap_scheduler_enable`.
- [ ] Ver si persiste los items **deshabilitados**.
- [ ] Ver si permite **actualizar el intervalo** de un item existente.
- [ ] Ver si expone `next_run`.
- [ ] Identificar el evento que publica al disparar y su payload (en particular, el campo con el id del item).

**Criterio de aceptación:** firmas reales anotadas y las preguntas de la tabla 1.3 sobre el scheduler respondidas.

### T0.4 Leer la API de `cap_router_mgr`
- [ ] Leer el header y el `SKILL.md` de `cap_router_mgr`.
- [ ] Identificar la estructura de una regla: id, match de evento, acción `run_agent` y campo de prompt.
- [ ] Identificar la API de alta, actualización, borrado y consulta de reglas.
- [ ] Ver si las reglas se **persisten**.
- [ ] Ver si serializa ejecuciones de `run_agent` o si existe un hook de "fin de ejecución" (para el riesgo de pulses solapados).

**Criterio de aceptación:** firmas reales anotadas y las preguntas de la tabla 1.3 sobre el router respondidas.

### T0.5 Leer `claw_event`, `claw_memory` y `claw_paths`
- [ ] `claw_event.h`: tipo y fuente del evento del scheduler, y cómo hacer match por id de item.
- [ ] `claw_memory.h`: nombres reales de las capabilities de memoria y sesiones que va a usar el skill.
- [ ] `claw_paths`: uso de `claw_paths_join(CLAW_PATH_DATA, …)` (solo para el fallback).

**Criterio de aceptación:** match de evento definido y capabilities de memoria y sesiones identificadas por su nombre real.

### T0.6 Estudiar un `cap_*` vecino como plantilla
- [ ] Elegir `cap_scheduler` o un componente chico similar.
- [ ] Extraer la plantilla de `CMakeLists.txt` y `Kconfig`.
- [ ] Extraer el mecanismo de registro de capabilities (tabla o descriptor).
- [ ] Ver dónde se ubican los `skills/` y cómo se empaquetan o registran.
- [ ] Ver si existe `test_apps/` (Unity) o tests Lua junto al módulo.
- [ ] Leer `app_capabilities.c` y el `CMakeLists.txt` de `app_claw` (patrón de registro y `REQUIRES`).
- [ ] Anotar la convención del timeout de mutex.

**Criterio de aceptación:** plantilla de componente, registro, skills y tests definida.

### T0.7 Registrar las decisiones de diseño
- [ ] Completar la tabla de decisiones para el PR:

| Pregunta | Respuesta | Consecuencia |
|---|---|---|
| ¿El scheduler persiste items deshabilitados? | | Sin archivo propio / fallback 3.4.b |
| ¿El scheduler permite actualizar el intervalo? | | Update / delete + create con mutex |
| ¿El router persiste reglas? | | Solo verificar / re-asegurar en `start` |
| ¿Hay protección contra pulses solapados? | | Documentar / Opción A / Opción B |
| ¿El scheduler expone `next_run`? | | Incluir en `status` / omitir y documentar |

**Criterio de aceptación:** las cinco decisiones están tomadas y anotadas antes de escribir código.

---

## Fase 1: Esqueleto del componente (commit 1)

### T1.1 Crear la estructura de directorios
- [ ] `components/claw_capabilities/cap_proactive/`
- [ ] `include/cap_proactive.h`
- [ ] `src/cap_proactive.c`
- [ ] `skills/proactive_pulse/` (vacío por ahora)

### T1.2 `CMakeLists.txt`
- [ ] Registrar el componente siguiendo la plantilla vecina.
- [ ] `REQUIRES` / `PRIV_REQUIRES` solo de los componentes reales: scheduler, router, event, paths (si hay fallback), FreeRTOS.
- [ ] No agregar dependencias nuevas.

### T1.3 `Kconfig`
- [ ] Menú "Proactive Pulse" con:
  - [ ] `CAP_PROACTIVE_MIN_INTERVAL_SEC` (60–86400, default 300).
  - [ ] `CAP_PROACTIVE_MAX_INTERVAL_SEC` (60–604800, default 86400).
  - [ ] `CAP_PROACTIVE_DEFAULT_INTERVAL_SEC` (60–604800, default 900).

### T1.4 Validación en compilación
- [ ] `_Static_assert` DEFAULT >= MIN.
- [ ] `_Static_assert` DEFAULT <= MAX.
- [ ] `_Static_assert` MIN <= MAX.

### T1.5 Constantes con nombre
- [ ] `CAP_PROACTIVE_ITEM_ID` = `"proactive_pulse"`
- [ ] `CAP_PROACTIVE_RULE_ID` = `"proactive_pulse_rule"`
- [ ] `CAP_PROACTIVE_SKILL_NAME` = `"proactive_pulse"`
- [ ] `CAP_PROACTIVE_PULSE_PROMPT` = `"Autonomous pulse: run skill proactive_pulse."`
- [ ] `CAP_PROACTIVE_MUTEX_TIMEOUT_MS`, según la convención del repo.
- [ ] Verificar que no haya literales repetidos fuera de este bloque.

### T1.6 Header público `cap_proactive.h`
- [ ] `cap_proactive_handle_t` (struct opaco).
- [ ] `cap_proactive_config_t`, con los handles o dependencias según cómo los reciben las demás capabilities.
- [ ] `cap_proactive_status_t` (`enabled`, `interval_sec`, `min_interval_sec`, `max_interval_sec`, `has_next_run`, `next_run`, `rule_present`, `item_present`).
- [ ] Prototipos de `create`, `start`, `stop`, `delete`, `enable`, `disable`, `set_interval`, `get_status` y `clamp_interval`.
- [ ] Ajustar los nombres finales a la plantilla vecina.

### T1.7 Ciclo de vida
- [ ] `struct cap_proactive` interno: `SemaphoreHandle_t` y referencias a scheduler y router. `pulse_in_flight` solo si se elige la Opción B.
- [ ] **No** cachear ON/OFF ni el intervalo: la fuente de verdad es el scheduler.
- [ ] `cap_proactive_create`: validar argumentos, reservar memoria, crear el mutex y limpiar si algo falla.
- [ ] `cap_proactive_start` / `cap_proactive_stop`: stubs por ahora.
- [ ] `cap_proactive_delete`: liberar el mutex y la memoria, **sin** borrar el item ni la regla.

**Criterio de aceptación:** el componente queda estructuralmente completo, con el ciclo de vida y sin lógica de negocio.

---

## Fase 2: Clamp y validación del intervalo (commit 2)

### T2.1 Helper puro `cap_proactive_clamp_interval`
- [ ] `requested <= 0` devuelve `ESP_ERR_INVALID_ARG`.
- [ ] `requested < MIN` aplica MIN y lo marca como recortado.
- [ ] `requested > MAX` aplica MAX y lo marca como recortado (decisión: **clamp**, no rechazo).
- [ ] Dentro del rango, se aplica tal cual, sin marcar recorte.
- [ ] Exponer el flag `clamped` (parámetro de salida extra o comparando `requested` con `applied`; elegir uno y documentarlo).

### T2.2 Parseo del argumento en el handler
- [ ] Argumento ausente devuelve `ESP_ERR_INVALID_ARG` con un mensaje claro.
- [ ] Valor no numérico devuelve `ESP_ERR_INVALID_ARG`.
- [ ] Desborde de `int32_t` devuelve `ESP_ERR_INVALID_ARG`.
- [ ] Usar un parseo seguro (`strtol` con chequeo de `errno` y `endptr`, o el helper del repo).

### T2.3 Tests del helper (si hay infraestructura Unity)
- [ ] T1: `MIN-1` da MIN con `clamped=true`.
- [ ] T2: `MIN` da MIN con `clamped=false`.
- [ ] T3: `MAX+1` da MAX con `clamped=true`.
- [ ] T4: `0` y los negativos dan `ESP_ERR_INVALID_ARG`.

**Criterio de aceptación:** helper sin dependencias del scheduler y casos T1–T4 escritos.

---

## Fase 3: `enable` / `disable` (commit 3)

### T3.1 `ensure_rule()` (privada)
- [ ] Buscar la regla por `CAP_PROACTIVE_RULE_ID`.
- [ ] Si no existe, crearla con el match del evento del scheduler para el item `proactive_pulse`, la acción `run_agent` y el prompt `CAP_PROACTIVE_PULSE_PROMPT`.
- [ ] Si existe con el mismo contenido (comparación campo por campo: match, acción, prompt), no hacer nada.
- [ ] Si existe con otro contenido, actualizarla, o hacer delete + create si no hay update.
- [ ] Devolver si la regla fue creada, actualizada o ya existía.

### T3.2 `cap_proactive_enable`
- [ ] Tomar el mutex con timeout y liberarlo en todas las salidas (`goto cleanup` o el patrón del repo).
- [ ] Sin scheduler o router, devolver `ESP_ERR_INVALID_STATE` con el mensaje "scheduler/router no disponible".
- [ ] Buscar el item `proactive_pulse`:
  - [ ] Si no existe, crearlo en modo `INTERVAL` con `DEFAULT_INTERVAL_SEC` y **deshabilitado**, y anotar que se creó en esta llamada.
  - [ ] Si existe, conservar su intervalo.
- [ ] Llamar a `ensure_rule()`.
- [ ] Si `ensure_rule` falla:
  - [ ] Si el item se creó en esta llamada, borrarlo (rollback).
  - [ ] Si ya existía, dejarlo en su estado previo.
  - [ ] Devolver el error.
- [ ] Recién después de asegurar la regla, llamar a `cap_scheduler_enable(ITEM_ID, true)`.
- [ ] Responder con el estado, el intervalo efectivo y si la regla fue creada o ya existía.

### T3.3 `cap_proactive_disable`
- [ ] Mutex en todas las salidas.
- [ ] Si el item no existe, devolver OK con el mensaje "ya está OFF".
- [ ] Llamar a `cap_scheduler_enable(ITEM_ID, false)`.
- [ ] **No** borrar el item ni la regla.

### T3.4 Handlers de capability
- [ ] Handler `proactive_enable` que delega en `cap_proactive_enable`.
- [ ] Handler `proactive_disable` que delega en `cap_proactive_disable`.
- [ ] Formato de respuesta (JSON o texto) igual al de las demás capabilities.

**Criterio de aceptación:** `enable` es idempotente, nunca hay un item habilitado sin regla y `disable` es idempotente y no destructivo.

---

## Fase 4: `set_interval` y `status` (commit 4)

### T4.1 `cap_proactive_set_interval`
- [ ] Mutex en todas las salidas.
- [ ] Validar y recortar con `cap_proactive_clamp_interval`.
- [ ] Si el item no existe, crearlo **deshabilitado** con el intervalo aplicado, sin crear la regla.
- [ ] Si el item existe:
  - [ ] Si el scheduler tiene update, actualizar el intervalo.
  - [ ] Si no lo tiene, leer el flag habilitado, borrar el item y recrearlo con el mismo id, el nuevo intervalo y el mismo flag, todo con el mutex tomado.
- [ ] Responder `{requested, applied, clamped, enabled}`.

### T4.2 `cap_proactive_get_status`
- [ ] Mutex en todas las salidas.
- [ ] Leer el item: existencia, habilitado, intervalo y `next_run` si el scheduler lo expone.
- [ ] Leer la regla: existencia.
- [ ] Si el item no existe: `enabled=false`, `item_present=false`, intervalo = DEFAULT marcado como "default, no configurado".
- [ ] Incluir siempre MIN y MAX.
- [ ] Si no hay `next_run`, usar `has_next_run=false` y omitir el campo en la respuesta.

### T4.3 Handlers de capability
- [ ] Handler `proactive_set_interval(seconds)` con el parseo de T2.2.
- [ ] Handler `proactive_status`.
- [ ] Formato de respuesta igual al de las demás capabilities.

**Criterio de aceptación:** `set_interval` conserva el estado ON/OFF e informa el recorte, y `status` refleja el estado real del scheduler y del router.

---

## Fase 5: Arranque y persistencia (commit 5)

### T5.1 `cap_proactive_start`
- [ ] Consultar si existe el item `proactive_pulse`.
- [ ] Si no existe, el modo queda OFF y no se crea nada.
- [ ] Si existe y está habilitado, y el router no persiste reglas o la regla falta, llamar a `ensure_rule()` (idempotente).
- [ ] Si existe y está deshabilitado, no hacer nada.
- [ ] Ante cualquier error de lectura, modo OFF con log de warning, sin abort.

### T5.2 Fallback de persistencia (solo si T0.7 determinó que el scheduler no persiste items deshabilitados)
- [ ] Ruta: `claw_paths_join(CLAW_PATH_DATA, "proactive.json")`.
- [ ] Formato acotado: `{"v":1,"enabled":bool,"interval":uint}`.
- [ ] Escritura atómica: `proactive.json.tmp` y después `rename`.
- [ ] Escribir solo cuando cambia el estado (`enable`, `disable`, `set_interval`), sin fsync por entrada.
- [ ] Lectura tolerante: si el archivo está ausente, no se puede parsear o tiene una versión desconocida, arrancar OFF con el intervalo default y loguear un warning.
- [ ] En `start`, reconstruir el item desde el archivo si hace falta.
- [ ] Documentar en el PR por qué fue necesario.

### T5.3 Pulses solapados (según T0.7)
- [ ] Opción A (preferida): documentar que el mínimo de 300 s y la idempotencia vía `exec_state` mitigan el riesgo.
- [ ] Opción B (solo si existe un hook de "fin de ejecución"): `pulse_in_flight` protegido por mutex, que se setea al despachar y se limpia al terminar.
- [ ] Si la Opción B no es viable, descartarla y documentarlo.

**Criterio de aceptación:** OFF por defecto, estado ausente o corrupto arranca OFF sin crash y la regla queda asegurada cuando el modo está ON.

---

## Fase 6: Skill `proactive_pulse` (commit 6)

### T6.1 Crear `skills/proactive_pulse/SKILL.md`
- [ ] Front-matter y formato iguales a los demás skills del repo.
- [ ] **Propósito:** pulse autónomo y periódico. No hacer nada es un resultado válido.

### T6.2 Límites explícitos
- [ ] `MAX_ACTIONS_PER_PULSE = 2`, escrito literalmente.
- [ ] Contexto acotado: últimas N=10 entradas de memoria y M=3 sesiones recientes, con las capabilities reales de `claw_memory` (T0.5).

### T6.3 Procedimiento
- [ ] 1. Recuperar el contexto acotado.
- [ ] 2. Revisar novedades, pendientes y oportunidades (reuniones próximas, conversaciones pendientes, información nueva).
- [ ] 3. Por cada candidata, armar la clave determinista `<tipo_accion>:<id_objeto>` y llamar a `exec_state_check`. Si ya está completada, descartarla.
- [ ] 4. Evaluar la utilidad. Si no queda nada relevante, ir al paso 6 con la decisión "no actuar".
- [ ] 5. Ejecutar solo acciones **no destructivas** (briefing, ficha, borrador de follow-up, actualizar documentación o reportes), hasta `MAX_ACTIONS_PER_PULSE`. Las acciones destructivas o irreversibles no se ejecutan: se registran como propuesta y se pide confirmación.
- [ ] 6. **Registrar siempre** en `activity_log` la decisión, el motivo y las acciones hechas o propuestas. Por cada acción completada, llamar a `exec_state_mark` con la misma clave.
- [ ] 7. Terminar con un resumen corto, sin abrir conversación salvo que haga falta una confirmación.

### T6.4 Nota de dependencias
- [ ] Si `exec_state_*` o `activity_log` no están disponibles en runtime, registrarlo en el log del sistema y **no actuar**.
- [ ] Usar los nombres y argumentos reales relevados en T0.1.

### T6.5 Empaquetado del skill
- [ ] Registrar o empaquetar el skill con el mismo mecanismo que los demás componentes (lista o partición), solo si existe.

**Criterio de aceptación:** el skill es autocontenido, acotado, idempotente y seguro ante acciones destructivas y ante dependencias faltantes.

---

## Fase 7: Registro en `app_claw` (commit 7)

### T7.1 `app_capabilities.c`
- [ ] Crear y arrancar `cap_proactive` **después** de `cap_scheduler` y del router (y de `cap_exec_state` y `cap_activity_log`, si existen).
- [ ] Pasar los handles o dependencias por `cap_proactive_config_t`.
- [ ] Registrar las cuatro capabilities (`proactive_enable`, `proactive_disable`, `proactive_set_interval`, `proactive_status`) con el mecanismo de los vecinos.

### T7.2 CMake de `app_claw`
- [ ] Agregar `cap_proactive` a `REQUIRES` si el patrón lo pide.

**Criterio de aceptación:** las capabilities quedan disponibles al arrancar, respetando el orden de dependencias, y `claw_core` sigue sin cambios.

---

## Fase 8: `test_apps` (commit 8, solo si existe el patrón)

### T8.1 Estructura
- [ ] `test_apps/CMakeLists.txt`
- [ ] `test_apps/sdkconfig.defaults`
- [ ] `test_apps/main/test_cap_proactive.c`

### T8.2 Casos
- [ ] T1–T4: helper de clamp (si no se hicieron en la Fase 2).
- [ ] T5: argumento no numérico o ausente da `ESP_ERR_INVALID_ARG`.
- [ ] T6: sin estado previo, `status` devuelve `enabled=false` e `item_present=false`.
- [ ] T7: `enable` dos veces deja un solo item y una sola regla.
- [ ] T8: `disable` conserva el item y la regla, y un `enable` posterior los reutiliza.
- [ ] T9: `set_interval` con ON conserva ON y cambia el intervalo.
- [ ] T10: `status` refleja `enable`, `disable` y `set_interval`.
- [ ] T11: si falla la creación de la regla, se revierte el item creado en esa llamada (si se pueden inyectar fallas).
- [ ] T12: estado ausente o corrupto arranca OFF sin crash.

### T8.3 Sin patrón de test
- [ ] Cubrir al menos T1–T4 si hay Unity disponible.
- [ ] Documentar en el PR que el resto queda para pruebas manuales.

**Criterio de aceptación:** tests escritos según el patrón del repo. **No se ejecutan en NightShift.**

---

## Fase 9: Pull Request

### T9.1 Descripción del PR
- [ ] **Resumen:** qué agrega `cap_proactive` y cómo se compone con scheduler, router, `exec_state` y `activity_log`.
- [ ] **Estado de dependencias:** #3 y #4 presentes, o la sección "Bloqueado por #3 / #4".
- [ ] **Decisiones:** la tabla de T0.7 completa.
- [ ] **Valores abiertos:** MAX = 86400 s con clamp y `MAX_ACTIONS_PER_PULSE = 2`, modificables en revisión.
- [ ] **Archivos tocados:** lista corta, aclarando que `claw_core` no se modificó.
- [ ] **Verificación:** aclarar que no se compiló ni se ejecutaron tests porque NightShift no tiene toolchain ESP-IDF. No declarar build ni tests exitosos.
- [ ] **Comando de build:** `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`

### T9.2 Pruebas manuales en placa (listadas en el PR)
- [ ] 1. Primer arranque: `proactive_status` indica OFF.
- [ ] 2. `proactive_enable` crea el item y la regla, y el estado pasa a ON.
- [ ] 3. `proactive_enable` repetido no genera duplicados.
- [ ] 4. `proactive_set_interval` por debajo del mínimo queda en el mínimo y lo informa.
- [ ] 5. Un pulse ejecuta el agente y genera una entrada en `activity_log`.
- [ ] 6. Un pulse sin contexto relevante termina sin acción y deja registro.
- [ ] 7. Una acción ya completada no se repite (`exec_state_check`).
- [ ] 8. Una acción destructiva no se ejecuta sin confirmación.
- [ ] 9. Después de `proactive_disable` dejan de llegar pulses.
- [ ] 10. Un reinicio con ON y con OFF conserva el estado y el intervalo.
- [ ] 11. Un reinicio con estado dañado o ausente arranca OFF sin crash.
- [ ] 12. Después de muchos pulses, el uso de flash/DATA no crece sin tope.

---

## Checklist de cierre

- [ ] #3 y #4 verificados (presentes o bloqueo documentado).
- [ ] APIs reales confirmadas, sin funciones inventadas.
- [ ] Sin literales mágicos: ids, prompt y límites como constantes o Kconfig.
- [ ] `_Static_assert` de DEFAULT, MIN y MAX.
- [ ] Mutex en todas las operaciones públicas y liberado en `delete`.
- [ ] `enable` idempotente, con rollback si falla la regla.
- [ ] OFF por defecto. Estado ausente o corrupto deja el modo OFF sin crash.
- [ ] Rutas solo vía `claw_paths_join(CLAW_PATH_DATA, …)` (si hay fallback).
- [ ] `claw_core` sin cambios.
- [ ] Todos los archivos con menos de 1500 líneas (`cap_proactive.c` estimado en 400–600).
- [ ] El PR no declara build ni tests exitosos e incluye el comando de build y la lista de pruebas manuales.