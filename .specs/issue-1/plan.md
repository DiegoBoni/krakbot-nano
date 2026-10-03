# Plan de implementación: Autonomous Pulse (Proactive Agent Mode), issue #1

> Repo: `DiegoBoni/krakbot-nano` · Spec: issue #1
> Nota: el plan se escribió sin acceso al código fuente. Todos los nombres de funciones, structs, eventos y campos de `cap_scheduler`, `cap_router_mgr`, `claw_event`, `claw_memory` y `claw_paths` que aparecen acá son **tentativos**. Hay que confirmarlos en el paso 1 antes de escribir código. No se inventa ninguna API.

---

## 0. Objetivo

Agregar el componente `cap_proactive`. Expone cuatro capabilities (`proactive_enable`, `proactive_disable`, `proactive_set_interval`, `proactive_status`) que arman, con piezas que ya existen, un "pulse" periódico del agente:

- **Disparo:** item `INTERVAL` de `cap_scheduler` con id fijo `proactive_pulse`.
- **Ruteo:** regla del router con acción `run_agent` y un prompt corto que manda ejecutar el skill `proactive_pulse`.
- **Toggle:** `cap_scheduler_enable(id, on/off)`.
- **Anti-duplicados y auditoría:** `exec_state_check` / `exec_state_mark` (#3) y `activity_log` (#4), usados desde el `SKILL.md`.

No se toca `claw_core`. No se agregan dependencias ni archivos de datos propios, salvo que sea estrictamente necesario (ver 3.4).

---

## 1. Fase 0: verificaciones previas (bloqueantes)

### 1.1 Dependencias #3 y #4
- [ ] Buscar en la rama base `cap_exec_state` y `cap_activity_log`: directorios en `components/claw_capabilities/`, registro en `app_capabilities.c` y nombres reales de las capabilities.
- [ ] **Si faltan:** no reimplementarlas. Seguir con el componente (scheduler, router y toggle no dependen de ellas). El `SKILL.md` las referencia por nombre (`exec_state_check`, `exec_state_mark`, `activity_log`) y el PR lleva una sección **"Bloqueado por #3 / #4"** que explica que el skill no funciona de punta a punta hasta que se mergeen.
- [ ] **Si están:** anotar los nombres y argumentos exactos (clave, estado, formato de entrada del log) para usarlos tal cual en el skill.

### 1.2 Lectura obligatoria
| Archivo | Qué extraer |
|---|---|
| `CLAUDE.md`, `.agents/design.md`, `.agents/gotchas.md` | Convenciones, patrón de capability, gotchas de FATFS, PSRAM y tareas |
| `cap_scheduler.h` (y su `.c`) | API de alta, actualización, borrado y lookup de items; modo `INTERVAL`; `cap_scheduler_enable`; si persiste items deshabilitados; si permite cambiar el intervalo de un item existente; si expone `next_run`; qué evento publica y con qué payload |
| `cap_router_mgr` (header y `SKILL.md`) | Estructura de una regla (id, match de evento, acción `run_agent`, campo de prompt), API de alta, actualización y consulta, y si las reglas se persisten |
| `claw_event.h` | Tipo y fuente del evento que publica el scheduler, y cómo hacer match por id de item |
| `claw_memory.h` | Capabilities de memoria y sesiones que puede usar el skill (nombres reales) |
| `claw_paths` | `claw_paths_join(CLAW_PATH_DATA, …)`, solo si llega a hacer falta un archivo propio |
| Un `cap_*` vecino (de preferencia `cap_scheduler` o uno chico similar) | Plantilla de `CMakeLists.txt`, `Kconfig`, registro de capabilities, ubicación y empaquetado de `skills/`, y existencia de `test_apps/` |
| `components/common/app_claw/app_capabilities.c` y su CMake | Patrón de registro y `REQUIRES` |

### 1.3 Decisiones que salen de la lectura (registrar en el PR)
| Pregunta | Si la respuesta es "sí" | Si la respuesta es "no" |
|---|---|---|
| ¿El scheduler persiste un item **deshabilitado**? | No hace falta archivo propio | Archivo mínimo propio (ver 3.4.b) |
| ¿El scheduler permite **actualizar el intervalo** de un item existente? | Usar update | Hacer delete + create conservando el estado ON/OFF, con el mutex tomado |
| ¿El router **persiste reglas**? | Solo verificar en `start` | Re-asegurar la regla en `start` cuando el item existe y está ON |
| ¿El router y el scheduler evitan **pulses solapados**? | Documentar | Guard en `cap_proactive` (ver 6.3), sin tocar `claw_core` |
| ¿El scheduler expone `next_run`? | Incluirlo en `status` | Omitir el campo y documentarlo |

---

## 2. Estructura de archivos

```
components/claw_capabilities/cap_proactive/
  CMakeLists.txt
  Kconfig
  include/cap_proactive.h
  src/cap_proactive.c
  skills/proactive_pulse/SKILL.md
  test_apps/                      (solo si el patrón existe en componentes vecinos)
    main/test_cap_proactive.c
    CMakeLists.txt
    sdkconfig.defaults
```

Modificaciones:
- `components/common/app_claw/app_capabilities.c`: registro de `cap_proactive`.
- `components/common/app_claw/CMakeLists.txt`: `REQUIRES cap_proactive`, si el patrón lo pide.
- Mecanismo de empaquetado y registro de skills (lista o partición), solo si los demás componentes lo usan.

**No se modifica:** `claw_core`.

---

## 3. Diseño detallado

### 3.1 Kconfig (`cap_proactive/Kconfig`)

```
menu "Proactive Pulse"
    config CAP_PROACTIVE_MIN_INTERVAL_SEC
        int "Minimum pulse interval (seconds)"
        range 60 86400
        default 300
    config CAP_PROACTIVE_MAX_INTERVAL_SEC
        int "Maximum pulse interval (seconds)"
        range 60 604800
        default 86400
    config CAP_PROACTIVE_DEFAULT_INTERVAL_SEC
        int "Default pulse interval (seconds)"
        range 60 604800
        default 900
endmenu
```

Validación en compilación, en `cap_proactive.c`:
```c
_Static_assert(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC >= CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC, "...");
_Static_assert(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC <= CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, "...");
_Static_assert(CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC <= CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, "...");
```

### 3.2 Constantes con nombre (en `cap_proactive.c` o en un header privado)

```c
#define CAP_PROACTIVE_ITEM_ID        "proactive_pulse"
#define CAP_PROACTIVE_RULE_ID        "proactive_pulse_rule"
#define CAP_PROACTIVE_SKILL_NAME     "proactive_pulse"
#define CAP_PROACTIVE_PULSE_PROMPT   "Autonomous pulse: run skill proactive_pulse."
#define CAP_PROACTIVE_MUTEX_TIMEOUT_MS  /* constante, según convención del repo */
```

No hay literales repetidos fuera de este bloque.

### 3.3 API pública (`include/cap_proactive.h`)

Sigue el estilo C orientado a objetos del repo. Los nombres finales se ajustan a la plantilla del `cap_*` vecino.

```c
typedef struct cap_proactive *cap_proactive_handle_t;

typedef struct {
    /* handles/deps según cómo los reciban otras capabilities (scheduler, router) */
} cap_proactive_config_t;

esp_err_t cap_proactive_create(const cap_proactive_config_t *cfg, cap_proactive_handle_t *out);
esp_err_t cap_proactive_start(cap_proactive_handle_t h);   /* re-asegura la regla si hace falta */
esp_err_t cap_proactive_stop(cap_proactive_handle_t h);
esp_err_t cap_proactive_delete(cap_proactive_handle_t h);  /* libera mutex y memoria; NO borra item ni regla */

/* Operaciones (las usan los handlers de capability y los tests) */
esp_err_t cap_proactive_enable(cap_proactive_handle_t h);
esp_err_t cap_proactive_disable(cap_proactive_handle_t h);
esp_err_t cap_proactive_set_interval(cap_proactive_handle_t h, int32_t seconds, uint32_t *applied);
esp_err_t cap_proactive_get_status(cap_proactive_handle_t h, cap_proactive_status_t *out);

/* Helper puro, testeable sin scheduler */
esp_err_t cap_proactive_clamp_interval(int32_t requested, uint32_t *applied);

typedef struct {
    bool     enabled;
    uint32_t interval_sec;
    uint32_t min_interval_sec;
    uint32_t max_interval_sec;
    bool     has_next_run;
    int64_t  next_run;          /* si el scheduler lo expone */
    bool     rule_present;
    bool     item_present;
} cap_proactive_status_t;
```

Estado interno (`struct cap_proactive`): mutex FreeRTOS (`SemaphoreHandle_t`), referencias a scheduler y router y, opcionalmente, `bool pulse_in_flight` (ver 6.3). **No** se cachea ON/OFF ni el intervalo: la fuente de verdad es el scheduler.

### 3.4 Persistencia

**a) Camino esperado: sin archivo propio.**
El item `proactive_pulse` vive en la persistencia del scheduler, que guarda el modo, el intervalo y el flag habilitado. La regla vive en la persistencia del router, si existe.

**b) Fallback, solo si el scheduler no persiste items deshabilitados:**
- Archivo `proactive.json` en `claw_paths_join(CLAW_PATH_DATA, "proactive.json")`, de tamaño fijo y acotado: `{"v":1,"enabled":bool,"interval":uint}`.
- Escritura atómica: se escribe `proactive.json.tmp` y después `rename`. Solo se escribe cuando cambia el estado, sin fsync por entrada.
- Lectura tolerante: si el archivo está ausente, no se puede parsear o tiene una versión desconocida, se arranca OFF con el intervalo default y se loguea un warning, sin crash.
- Retrocompatible: el campo `v` permite migrar más adelante.
- En el PR se documenta por qué fue necesario.

**Arranque (`cap_proactive_start`):**
1. Consultar en el scheduler si existe el item `proactive_pulse`.
2. Si no existe, el modo queda OFF y no se crea nada (OFF por defecto).
3. Si existe y está habilitado, y el router no persiste reglas o la regla falta, se re-asegura la regla de forma idempotente.
4. Si existe y está deshabilitado, no se hace nada.
5. Cualquier error de lectura deja el modo OFF, con log y sin abort.

### 3.5 Lógica de las capabilities

Todas las operaciones toman el mutex al principio y lo liberan en todas las salidas, con un único `goto cleanup` o el patrón del repo.

**`proactive_enable`**
1. Si no hay scheduler o router, devolver `ESP_ERR_INVALID_STATE` con el mensaje "scheduler/router no disponible".
2. Buscar el item. Si no existe, crearlo en modo `INTERVAL` con `DEFAULT_INTERVAL_SEC` y **deshabilitado**. Si existe, conservar su intervalo.
3. `ensure_rule()`: buscar la regla por `CAP_PROACTIVE_RULE_ID`.
   - Si no existe, crearla.
   - Si existe con el mismo contenido (evento, acción `run_agent`, prompt), no hacer nada.
   - Si existe con otro contenido, actualizarla, o hacer delete + create si no hay update.
4. Si `ensure_rule` falla y el item se creó en esta misma llamada, borrarlo (rollback) y devolver el error. Si el item ya existía, dejarlo como estaba (deshabilitado o en su estado previo) y devolver el error; el próximo `enable` reintenta.
5. `cap_scheduler_enable(ITEM_ID, true)`.
6. Responder con el estado, el intervalo efectivo y si la regla fue creada o ya existía.

> Para que no quede un estado a medias, el item se crea deshabilitado y solo se habilita después de asegurar la regla. Así nunca hay un pulse sin una regla que lo rutee.

**`proactive_disable`**
1. Si el item no existe, devolver OK con el mensaje "ya está OFF". Es idempotente.
2. `cap_scheduler_enable(ITEM_ID, false)`. No borra el item ni la regla.

**`proactive_set_interval(seconds)`**
1. Parsear el argumento. Si falta, no es numérico, es `<= 0` o desborda, devolver `ESP_ERR_INVALID_ARG` con un mensaje claro.
2. `cap_proactive_clamp_interval`: si es menor que MIN se lleva a MIN, si es mayor que MAX se lleva a MAX, y se informa en la respuesta.
3. Si el item no existe, crearlo **deshabilitado** con el intervalo aplicado, para que un `enable` posterior lo use. No se crea la regla todavía.
4. Si el item existe, actualizar el intervalo conservando el estado ON/OFF. Si el scheduler requiere recrear el item, se recrea con el mismo id y el mismo flag habilitado.
5. Responder `{requested, applied, clamped: bool, enabled}`.

> Decisión sobre MAX: la spec deja abierto si un valor mayor que MAX se rechaza o se recorta. Se propone **recortar** (clamp) por coherencia con MIN, y documentarlo. El valor 0, los negativos y los no numéricos sí se rechazan.

**`proactive_status`**
- Lee el item (existencia, habilitado, intervalo, next_run si está disponible) y la regla (existencia).
- Si el item no existe, devuelve `enabled=false`, intervalo = DEFAULT (marcado como "default, no configurado"), MIN y MAX.
- Formato de respuesta: el mismo que usen las demás capabilities (JSON o texto).

### 3.6 Regla del router

- Id: `CAP_PROACTIVE_RULE_ID`.
- Match: el evento que publica el scheduler para el item `proactive_pulse`. El tipo, la fuente y el campo de id salen de `claw_event.h` y del `SKILL.md` de `cap_router_mgr`.
- Acción: `run_agent`.
- Prompt: `CAP_PROACTIVE_PULSE_PROMPT`, corto. Todo el procedimiento vive en el `SKILL.md`.
- La comparación de "mismo contenido" se hace campo por campo (match, acción, prompt) para no reescribir la regla sin necesidad.

### 3.7 Skill `skills/proactive_pulse/SKILL.md`

Con front-matter y formato iguales a los demás skills del repo. Contenido:

1. **Propósito:** pulse autónomo y periódico. No hacer nada es un resultado válido.
2. **Límites explícitos:**
   - `MAX_ACTIONS_PER_PULSE = 2` (valor propuesto dentro del rango 1–3 de la spec, escrito literalmente en el skill).
   - Contexto acotado: últimas N entradas de memoria y M sesiones recientes (valores explícitos, por ejemplo N=10 y M=3), usando las capabilities reales de `claw_memory`.
3. **Procedimiento:**
   1. Recuperar el contexto acotado.
   2. Revisar novedades, pendientes y oportunidades (reuniones próximas, conversaciones pendientes, información nueva).
   3. Por cada candidata, armar una clave determinista `<tipo_accion>:<id_objeto>` (por ejemplo `briefing:meeting_<id>`) y llamar a `exec_state_check`. Si ya está completada, se descarta.
   4. Evaluar la utilidad. Si no queda nada relevante, ir directo al paso 6 con la decisión "no actuar".
   5. Ejecutar solo acciones **no destructivas** (briefing, ficha, borrador de follow-up, actualizar documentación o reportes), sin pasar de `MAX_ACTIONS_PER_PULSE`. Ante una acción destructiva o irreversible: **no ejecutar**, dejar la propuesta registrada y pedir confirmación al usuario.
   6. **Registrar siempre** en `activity_log`: la decisión (actuar o no, y por qué) y las acciones hechas o propuestas. Por cada acción completada, llamar a `exec_state_mark` con la misma clave del paso 3.
   7. Terminar con un resumen corto. No abrir conversación con el usuario salvo que haga falta una confirmación.
4. **Nota de dependencias:** si `exec_state_*` o `activity_log` no están disponibles en runtime, el skill lo registra en el log del sistema y **no actúa**, por seguridad.

### 3.8 Registro en `app_capabilities.c`

- Agregar la creación y el arranque de `cap_proactive` **después** de `cap_scheduler` y del router, para respetar el orden de dependencias. Si existen `cap_exec_state` y `cap_activity_log`, también después de ellos.
- Registrar las cuatro capabilities con el mismo mecanismo (tabla o descriptor) que usan los vecinos.
- Agregar `cap_proactive` a `REQUIRES` en el CMake de `app_claw` si aplica.

---

## 4. Orden de implementación (commits sugeridos)

1. `cap_proactive`: esqueleto. CMake, Kconfig con `_Static_assert`, header y `create/delete/start/stop` con mutex, sin lógica.
2. `cap_proactive`: clamp y validación de intervalo (helper puro) y sus tests.
3. `cap_proactive`: `enable`/`disable` idempotentes, con `ensure_rule` y rollback.
4. `cap_proactive`: `set_interval` y `status`.
5. `cap_proactive`: re-aseguramiento de la regla en `start` (si el router no persiste) y fallback de persistencia (solo si hace falta).
6. `cap_proactive`: skill `proactive_pulse` (`SKILL.md`).
7. `app_claw`: registro de `cap_proactive`.
8. `cap_proactive`: `test_apps` (si existe el patrón).

Mantener `cap_proactive.c` muy por debajo de 1500 líneas (estimado: 400–600).

---

## 5. Tests

Donde exista el patrón (`components/**/test_apps/` con Unity, o tests Lua junto al módulo). **No se pueden ejecutar en NightShift.**

| # | Caso | Nivel |
|---|---|---|
| T1 | Clamp: `MIN-1` da MIN y `clamped=true` | helper puro |
| T2 | Clamp: `MIN` da MIN y `clamped=false` | helper puro |
| T3 | Clamp: `MAX+1` da MAX y `clamped=true` | helper puro |
| T4 | `0` y negativos dan `ESP_ERR_INVALID_ARG` | helper puro |
| T5 | Argumento no numérico o ausente da `ESP_ERR_INVALID_ARG` | handler |
| T6 | Sin estado previo, `status` devuelve `enabled=false` e `item_present=false` | integración con scheduler |
| T7 | `enable` dos veces deja un item y una regla | integración |
| T8 | `disable` deja el item y la regla presentes; `enable` posterior los reutiliza sin duplicar | integración |
| T9 | `set_interval` con ON conserva ON y cambia el intervalo | integración |
| T10 | `status` refleja cada cambio (enable, disable, set_interval) | integración |
| T11 | Falla simulada al crear la regla: el item creado en esa llamada se revierte | integración (si se pueden inyectar fallas) |
| T12 | Estado ausente o corrupto (scheduler o fallback): arranca OFF sin crash | integración |

Si no hay patrón de test para la capa de capabilities, se cubre al menos T1–T4 sobre el helper puro (si existe infraestructura Unity) y se documenta que el resto queda en pruebas manuales.

---

## 6. Riesgos y mitigaciones

### 6.1 Dependencias #3 y #4 sin mergear
Se entrega el componente, y el skill las referencia por nombre y "no actúa" si faltan. El PR queda marcado como bloqueado para la validación de punta a punta.

### 6.2 Capacidades del scheduler y del router
Las cubre la tabla de decisiones de 1.3. Cada alternativa está diseñada y no hace falta tocar `claw_core`.

### 6.3 Pulses solapados
Si la lectura muestra que el router o `run_agent` no serializa:
- Opción A (preferida): documentar que el mínimo de 300 s hace improbable el solapamiento y que el skill es idempotente gracias a `exec_state`.
- Opción B: si el router permite condiciones o hooks, un flag `pulse_in_flight` en `cap_proactive` protegido por mutex, que se setea al despachar y se limpia al terminar. Solo es viable si existe un hook de "fin de ejecución"; si no existe, se descarta y se documenta.

### 6.4 Tokens y batería
MIN=300 s, DEFAULT=900 s, contexto acotado y límite de acciones explícitos en el skill.

### 6.5 Crecimiento de DATA
`cap_proactive` no escribe en cada pulse (como mucho escribe el fallback de tamaño fijo cuando cambia el estado). El crecimiento por pulse depende de `activity_log` (#4), que debe tener su propio tope. Se verifica en la prueba manual 12.

### 6.6 Valores abiertos
- MAX = 86400 s, recortado con clamp en lugar de rechazado.
- `MAX_ACTIONS_PER_PULSE = 2`.

Se pueden cambiar en revisión.

---

## 7. Descripción del PR (plantilla)

- **Resumen:** qué agrega y cómo se compone con scheduler, router, exec_state y activity_log.
- **Estado de dependencias:** si #3 y #4 están presentes o el PR está bloqueado.
- **Decisiones tomadas en la lectura de headers:** tabla de 1.3 completa (persistencia de items deshabilitados, update de intervalo, persistencia de reglas, solapamiento, next_run).
- **Archivos tocados:** lista corta. Se aclara que no se modificó `claw_core`.
- **Verificación:** *no se compiló ni se ejecutaron tests; NightShift no tiene toolchain ESP-IDF.* No se declara build ni tests exitosos.
- **Comandos de build:**
  ```
  idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build
  ```
- **Pruebas manuales en placa:**
  1. Primer arranque: `proactive_status` indica OFF.
  2. `proactive_enable`: se crean el item `proactive_pulse` y la regla, y el estado pasa a ON.
  3. `proactive_enable` repetido: sin duplicados.
  4. `proactive_set_interval` con valor menor al mínimo: queda en el mínimo y lo informa.
  5. Esperar un pulse: el agente se ejecuta y aparece una entrada en `activity_log`.
  6. Pulse sin contexto relevante: termina sin acción y deja registro.
  7. Pulse con acción candidata ya completada: no se repite (`exec_state_check`).
  8. Acción destructiva candidata: no se ejecuta sin confirmación.
  9. `proactive_disable`: dejan de llegar pulses.
  10. Reinicio con ON y con OFF: se conservan el estado y el intervalo.
  11. Reinicio con estado dañado o ausente: arranca OFF sin crash.
  12. Después de muchos pulses, el uso de flash/DATA no crece sin tope.

---

## 8. Checklist de cierre

- [ ] #3 y #4 verificados (presentes o bloqueo documentado).
- [ ] APIs reales confirmadas: ninguna función inventada.
- [ ] Sin literales mágicos: ids, prompt y límites como constantes o Kconfig.
- [ ] `_Static_assert` de DEFAULT, MIN y MAX.
- [ ] Mutex en todas las operaciones públicas y liberación en `delete`.
- [ ] `enable` idempotente, con rollback si falla la regla.
- [ ] OFF por defecto. Estado ausente o corrupto deja el modo OFF sin crash.
- [ ] Rutas solo vía `claw_paths_join(CLAW_PATH_DATA, …)` (si hay fallback).
- [ ] `claw_core` sin cambios.
- [ ] Archivos de menos de 1500 líneas.
- [ ] El PR no declara build ni tests exitosos e incluye los comandos y la lista de pruebas manuales.