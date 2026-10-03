# Spec: Autonomous Pulse (Proactive Agent Mode) — issue #1

## 1. Resumen

Agregar un modo de proactividad autónoma con toggle ON/OFF. Cuando está ON, un item periódico del scheduler dispara al agente con un prompt de "pulse". El agente revisa el contexto, decide si hay una acción útil, la ejecuta solo si corresponde y registra la decisión.

No se crea un subsistema nuevo. Se compone con piezas existentes:

| Pieza | Rol |
|---|---|
| `cap_scheduler` (modo `INTERVAL`, id fijo `proactive_pulse`) | Publica el evento periódico y persiste ON/OFF e intervalo |
| Regla del router (`claw_event_router`, acción `run_agent`) | Envía el evento al agente con el prompt de pulse |
| `cap_scheduler_enable(id, on/off)` | Implementa el toggle |
| `cap_exec_state` (#3) | Evita repetir trabajo (`exec_state_check` / `exec_state_mark`) |
| `cap_activity_log` (#4) | Registra decisión y resultado de cada pulse |

**Dependencias:** #3 y #4. Si no están mergeados en la rama base, **no se reimplementan**. En ese caso se documenta el bloqueo en el PR y se deja el skill referenciando las capabilities por nombre.

**Fuera de alcance:** UI de configuración y cualquier cambio en `claw_core`.

## 2. Contexto técnico y restricciones

KrakBot Nano es firmware C sobre ESP-IDF/FreeRTOS (ESP32-S3, 16MB flash, PSRAM). No hay SQLite ni npm/pip en runtime.

Antes de implementar hay que leer `CLAUDE.md`, `.agents/design.md` y `.agents/gotchas.md`. También hay que leer los headers reales de `cap_scheduler.h`, `claw_memory.h`, `claw_event.h`, `claw_paths` y `cap_router_mgr` (incluido su `SKILL.md`). No se inventan funciones.

Reglas aplicables:
- No tocar el loop de `claw_core`. Se extiende vía capability, skill, regla del router o hooks existentes.
- Estilo C orientado a objetos:
  - handle opaco `xxx_handle_t`;
  - `xxx_create/delete/start/stop`;
  - retorno `esp_err_t`;
  - config por struct;
  - recursos liberados en `delete`;
  - estado compartido protegido con mutex.
- Sin números ni strings mágicos: constantes o Kconfig. Archivos < 1500 líneas.
- Rutas escribibles siempre con `claw_paths_join(CLAW_PATH_DATA, ...)`. Nunca hardcodear `/fatfs`.
- La partición DATA (~3MB) no se reflashea. Cualquier formato de datos debe ser retrocompatible y de tamaño acotado.
- Escrituras atómicas (temporal + rename), tolerantes a archivo ausente o corrupto (arrancar vacío, sin crash). Escrituras agrupadas, sin fsync por entrada.
- Configuración vía Kconfig/`settings`, sin variables de entorno.
- Sin SQLite ni dependencias externas nuevas.

## 3. Diseño

### 3.1 Componente `components/claw_capabilities/cap_proactive/`

Estructura propuesta:
```
cap_proactive/
  CMakeLists.txt
  Kconfig
  include/cap_proactive.h
  src/cap_proactive.c
  skills/proactive_pulse/SKILL.md
  test_apps/            (si existe el patrón en componentes vecinos)
```

Se registra en `components/common/app_claw/app_capabilities.c`, siguiendo el patrón de las capabilities existentes.

### 3.2 Capabilities expuestas

| Capability | Argumentos | Comportamiento |
|---|---|---|
| `proactive_enable` | — | Crea o actualiza de forma idempotente el item `proactive_pulse` y la regla `run_agent`, y luego lo habilita |
| `proactive_disable` | — | Deshabilita el item con `cap_scheduler_enable(proactive_pulse, off)`. No borra el item ni la regla |
| `proactive_set_interval` | `seconds` | Aplica clamp al mínimo Kconfig y actualiza el item. Si está ON, el cambio aplica sin perder el estado ON |
| `proactive_status` | — | Devuelve ON/OFF, intervalo efectivo, mínimo, próxima ejecución si el scheduler la expone, y si la regla existe |

Reglas de comportamiento:
- **Idempotencia:** llamar a `enable` N veces deja exactamente un item `proactive_pulse` y una regla. No se duplica ninguno. Si la regla ya existe con el mismo contenido, no se reescribe.
- **Por defecto OFF:** en el primer arranque, o sin estado previo, el modo está apagado.
- **Clamp:** `seconds < MIN` se eleva a `MIN`. La respuesta de `proactive_set_interval` informa el valor efectivamente aplicado. Hay que definir un tope máximo razonable (constante/Kconfig) y validar la entrada, por ejemplo rechazando `seconds <= 0` o valores no numéricos con `ESP_ERR_INVALID_ARG`.
- **Errores:** si el scheduler o el router no están disponibles, la capability devuelve un error claro (`esp_err_t` y mensaje) y no deja un estado a medias. Si falla la creación de la regla tras crear el item, se revierte o se reintenta en el siguiente `enable`.
- **Concurrencia:** las capabilities pueden invocarse desde distintos contextos. Cualquier estado interno del componente se protege con mutex.

### 3.3 Kconfig

| Símbolo (nombres tentativos) | Default | Descripción |
|---|---|---|
| `CAP_PROACTIVE_DEFAULT_INTERVAL_SEC` | 900 (15 min) | Intervalo inicial al habilitar sin intervalo previo |
| `CAP_PROACTIVE_MIN_INTERVAL_SEC` | 300 (5 min) | Mínimo; el clamp se aplica siempre |
| `CAP_PROACTIVE_MAX_INTERVAL_SEC` | a definir | Tope superior (propuesta: 86400) |

Se valida en compilación que `DEFAULT >= MIN` (por ejemplo con `_Static_assert` o un `range` de Kconfig). El id `proactive_pulse`, el id de la regla y el texto del prompt son constantes con nombre, no literales dispersos.

### 3.4 Persistencia

ON/OFF e intervalo **se persisten reutilizando la persistencia del scheduler**. `cap_proactive` no crea un archivo propio, salvo que el scheduler no pueda almacenar el item deshabilitado. En ese caso se documenta la razón y se aplican las reglas de la sección 2 (ruta con `claw_paths_join`, escritura atómica, tolerancia a corrupción, tamaño acotado).

Al arrancar:
- Si el scheduler restaura el item `proactive_pulse`, el estado y el intervalo se conservan.
- La regla del router también debe sobrevivir. Hay que verificar si el router persiste sus reglas. Si no lo hace, `cap_proactive` re-asegura la regla en su `start` solo cuando el item existe y está habilitado.
- Si el estado está ausente o corrupto, el modo queda OFF, sin crash.

### 3.5 Regla del router

Regla con acción `run_agent` asociada al evento publicado por el item `proactive_pulse`. Su prompt instruye al agente a ejecutar el skill `proactive_pulse`. El prompt es breve y está definido como constante. Los detalles del procedimiento viven en el `SKILL.md`, no en el prompt.

Los nombres exactos de evento, campos de regla y API para crearla se toman de `cap_router_mgr` y `claw_event.h`. Se confirman leyendo los headers y el `SKILL.md` del router antes de codificar.

### 3.6 Skill `proactive_pulse` (`SKILL.md`)

Procedimiento que el agente sigue en cada pulse:

1. **Recuperar contexto:** memoria y sesiones recientes, con las capabilities de memoria existentes (`claw_memory`). Mantener las consultas acotadas para controlar el gasto de tokens.
2. **Revisar novedades, pendientes y oportunidades:** reuniones próximas, conversaciones pendientes, información nueva relevante.
3. **Evitar duplicados:** para cada acción candidata, consultar `exec_state_check` con una clave estable y determinista (por ejemplo, tipo de acción + identificador del objeto). Si ya está completada, se descarta.
4. **Evaluar utilidad:** decidir si hay una acción útil. **Si no hay nada relevante, terminar el pulse sin ejecutar ni generar trabajo artificial.** Un pulse sin acción es un resultado válido.
5. **Ejecutar solo si corresponde:**
   - Se permiten acciones no destructivas (preparar briefing o ficha, redactar follow-up, actualizar documentación o reportes).
   - **Nunca** ejecutar acciones destructivas o irreversibles sin confirmación del usuario. En ese caso se deja propuesta y registro, y no se actúa.
   - Limitar el número de acciones por pulse (propuesta: máximo 1–3, valor explícito en el skill).
6. **Registrar siempre:**
   - `activity_log`: qué decidió (actuar / no actuar y por qué) y qué hizo, incluso cuando la decisión fue no hacer nada.
   - `exec_state_mark`: marcar las acciones completadas con la misma clave usada en el paso 3.
7. **Finalizar** con un resumen corto. No iniciar conversación con el usuario salvo que una acción lo requiera, como una confirmación.

Los nombres exactos de las capabilities de #3 y #4 se toman de sus implementaciones. Este documento usa `exec_state_check`, `exec_state_mark` y `activity_log` como figuran en el issue.

## 4. Archivos a crear o modificar

- **Crear:** `components/claw_capabilities/cap_proactive/**` (ver 3.1).
- **Modificar:** `components/common/app_claw/app_capabilities.c` (registro y dependencia en su CMake/`REQUIRES` si aplica).
- **No modificar:** `claw_core`.
- Si el skill requiere registro explícito (lista de skills o empaquetado en la partición), seguir el mecanismo de los demás componentes.

## 5. Criterios de aceptación y su cobertura

| Criterio | Cobertura |
|---|---|
| Se puede activar/desactivar | `proactive_enable` / `proactive_disable` y `cap_scheduler_enable` |
| Se ejecuta según un intervalo configurable | Item `INTERVAL` y `proactive_set_interval` con clamp |
| Decide autónomamente si actuar | Pasos 2–4 del skill |
| Puede terminar un pulse sin acción | Paso 4 del skill |
| No repite acciones completadas | `exec_state_check` y `exec_state_mark` (pasos 3 y 6) |
| Cada ejecución queda en el Activity Log | Paso 6 del skill, incluso en pulses sin acción |

Criterios adicionales de esta spec:
- OFF por defecto.
- El estado y el intervalo sobreviven a un reinicio.
- `enable` es idempotente.
- Un intervalo menor al mínimo se eleva al mínimo.

## 6. Plan de implementación

1. Verificar que #3 y #4 estén en la rama base. Si no, parar y avisar en el PR; no reimplementarlos.
2. Leer los headers de `cap_scheduler`, router, `claw_event`, `claw_memory` y `claw_paths`, y el `SKILL.md` de `cap_router_mgr`. Tomar un componente `cap_*` existente como plantilla de estructura.
3. Crear `cap_proactive` (CMake, Kconfig, header, fuente).
4. Implementar las cuatro capabilities con el handle opaco, mutex y constantes.
5. Implementar la creación/actualización idempotente del item y de la regla.
6. Escribir el `SKILL.md`.
7. Registrar en `app_capabilities.c`.
8. Agregar tests (ver 7).
9. Redactar la descripción del PR (ver 8).

## 7. Tests

Se agregan donde exista el patrón (`components/**/test_apps/`, o tests Lua junto al módulo). Es probable que no puedan correrse en el entorno de NightShift.

Casos a cubrir:
- Clamp: valor menor al mínimo, igual al mínimo, mayor al máximo, cero y negativo.
- Idempotencia de `enable`: llamarlo dos veces deja un solo item y una sola regla.
- Estado por defecto OFF sin estado previo.
- `disable` no borra el item ni la regla; `enable` posterior los reutiliza.
- `status` refleja los cambios.
- Estado ausente o corrupto: arranca OFF sin crash.

Si no hay patrón de test aplicable a la capa de capabilities, se documenta y se prioriza la lista de pruebas manuales.

## 8. Verificación y contenido del PR

En NightShift no hay toolchain ESP-IDF, por lo que **no se puede compilar ni ejecutar tests aquí**. El PR:
- Mantiene el cambio pequeño y revisable.
- **No declara "tests pasados"** ni build exitoso, porque no se ejecutaron.
- Incluye los comandos de build:
  ```
  idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build
  ```
- Incluye la lista de pruebas manuales en placa:
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
  12. Verificar que el uso de flash/DATA no crece sin tope tras muchos pulses.

## 9. Riesgos y puntos abiertos

- **Dependencias #3 y #4:** si no están mergeados, esta feature queda bloqueada.
- **Persistencia de la regla del router:** hay que confirmar si el router persiste sus reglas. Si no lo hace, se necesita el re-aseguramiento descrito en 3.4.
- **Soporte del scheduler:** hay que confirmar que `INTERVAL` admite cambiar el intervalo de un item existente, y que un item deshabilitado se persiste.
- **Costo de tokens y batería:** el mínimo de 5 min y el default de 15 min lo mitigan. El skill debe mantener el contexto recuperado acotado.
- **Pulses solapados:** si un pulse tarda más que el intervalo, hay que verificar cómo se comportan el router y el scheduler. Si no hay protección, se evalúa saltar el pulse mientras haya uno en curso, sin tocar `claw_core`.
- **Tope máximo de intervalo y límite de acciones por pulse:** valores por confirmar en la implementación.