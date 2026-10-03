# Tasks: Estado persistente para ejecuciones autónomas y recurrentes

## Estado vigente después del descubrimiento (2026-10-03)

**Bloqueado en fase 0; implementación no iniciada y tests no ejecutados.**
La evidencia vigente está en [discovery.md](discovery.md). El runtime es C con
ESP-IDF/FreeRTOS y Lua, con dependencias CMake/IDF Component Manager; Node y
Python no son el runtime de la aplicación. No hay un comando único de tests
para este issue y el entorno local no dispone de ESP-IDF para ejecutar el build.

**El contenido original debajo es una propuesta histórica obsoleta, no una
instrucción de implementación aprobada.** Esto incluye las rutas `src/state`,
las extensiones TS/Python, los bindings SQLite host, las variables de entorno,
el helper `runTracked`, las fases 1–6, el fallback JSON y el orden de PRs.
Sus criterios de aceptación siguen siendo objetivos pendientes, no resultados.
No debe ejecutarse ninguna fase posterior hasta resolver los bloqueos y
reemplazar el diseño y las tareas por una versión compatible con el firmware.
Marcar el documento como obsoleto no completa T0.8.

| Tarea | Evidencia y pendiente real |
|---|---|
| T0.1 | Stack y comandos de build documentados localmente; comentario en el issue pendiente. No hay tests del store ni resultados de CI. |
| T0.2 | Scheduler identificado en `components/claw_capabilities/cap_scheduler/src/cap_scheduler.c` y `cap_scheduler_store.c`; Autonomous Pulse no encontrado. Falta definir su entrypoint y cómo observar el resultado final del trabajo asíncrono. |
| T0.3 | Configuración en `application/edge_agent/components/app_config/` y NVS. DATA se resuelve con `claw_paths`; no se leen variables `KRAKBOT_STATE_*`. Equivalencia settings/Kconfig pendiente. |
| T0.4 | Bootstrap en `application/edge_agent/main/main.c` y `components/common/app_claw/app_claw.c`, después de `build_storage_paths()` y antes de los consumidores. Ownership, inyección y shutdown del store pendientes. |
| T0.5 | Backend sin aprobar. Validar SQLite para ESP-IDF/FATFS en CI y target, o aprobar fallback JSON en C; no instalar bindings Node/Python. |
| T0.6 | Defaults propuestos: retención apagada, deduplicación indefinida, resumen JSON de 16 KB y una instancia por archivo. No implementados; registro externo pendiente. |
| T0.7 | `commitProcessed` sin aprobación; mantenerlo fuera de la implementación hasta decidir. |
| T0.8 | Replanificación pendiente después de resolver los bloqueos. `components/claw_modules/claw_state/` es solo una propuesta, con handle opaco, API `esp_err_t` y rutas mediante `claw_paths_join(CLAW_PATH_DATA, ...)`. |

El scheduler publica eventos: `ESP_OK` indica aceptación de la publicación,
no éxito de la acción. La integración futura debe registrar el cierre real antes
de marcar recursos procesados. Ningún criterio funcional del issue se declara
cumplido por esta documentación. No corresponde cerrar el issue por este cambio.

---

## Propuesta original archivada (obsoleta)

**Repo:** DiegoBoni/krakbot-nano · **Issue:** #3 · **Plan de referencia:** "Plan de implementación: Estado persistente para ejecuciones autónomas y recurrentes"

> **Nota:** las rutas y los nombres de archivo son propuestas del plan. La extensión `(ts|py)` y las ubicaciones exactas se confirman en la Fase 0. Ninguna tarea de las Fases 1 a 6 debería empezar sin completar las tareas bloqueantes de la Fase 0 (T0.1–T0.5).

**Convenciones:**
- `[ ]` tarea pendiente · **Depende de:** tareas que tienen que estar terminadas antes · **Hecho cuando:** criterio de aceptación verificable.
- Las tareas marcadas con 🔒 son bloqueantes para la fase siguiente.

---

## Fase 0 — Descubrimiento y decisiones bloqueantes

- [ ] **T0.1** 🔒 Identificar el stack del repo: lenguaje, runtime, gestor de paquetes y framework de tests.
  - **Hecho cuando:** está documentado en el issue (comentario) el stack confirmado y el comando para correr los tests.
- [ ] **T0.2** 🔒 Ubicar los entrypoints de Autonomous Pulse (loop del pulse) y de Scheduled Tasks (scheduler y definición de tasks).
  - **Hecho cuando:** hay una lista de archivos concretos a modificar en la Fase 4.
- [ ] **T0.3** Ubicar el mecanismo de configuración (env vars / archivo de config) y el directorio de datos de la app.
  - **Hecho cuando:** está definido dónde se lee `KRAKBOT_STATE_PATH` y cuál es el `<dataDir>` por defecto.
- [ ] **T0.4** Ubicar el bootstrap de la app (arranque y shutdown) y el mecanismo de inyección de dependencias que se usa (constructor, contenedor, contexto o singleton).
  - **Hecho cuando:** está identificado el punto exacto donde se abre/cierra el store y se llama a `recoverInterrupted()`.
- [ ] **T0.5** 🔒 Confirmar el backend con el owner: SQLite en modo WAL (`better-sqlite3` en Node o `sqlite3` de la stdlib en Python).
  - Validar que la dependencia nativa compila en CI y en el target de deploy.
  - **Hecho cuando:** el backend está aprobado. Si se rechaza SQLite, se reemplazan las Fases 2.x por las tareas del Anexo A.
- [ ] **T0.6** Acordar con el owner los defaults de las preguntas abiertas 3–5. Si no hay respuesta, se aplican estos:
  - Retención de `dedup_key` indefinida; `KRAKBOT_STATE_DEDUP_RETENTION_DAYS` existe pero está deshabilitado por defecto.
  - `result` guarda solo un resumen/referencia en JSON, con un límite de 16 KB (trunca y loguea un warning).
  - Multi-instancia fuera de alcance: una instancia por archivo de estado.
  - **Hecho cuando:** las decisiones quedan registradas en el issue.
- [ ] **T0.7** Validar con el owner la desviación sobre éxito parcial (`ctx.commitProcessed` opt-in, ver Fase 3).
  - **Hecho cuando:** está aprobada o descartada (si se descarta, se omite T3.5).
- [ ] **T0.8** Ajustar las rutas propuestas de este documento según los hallazgos de T0.1–T0.4.

---

## Fase 1 — Modelo de datos y contrato (`StateStore`)

**Depende de:** T0.1, T0.5, T0.8

- [ ] **T1.1** Crear `src/state/types.(ts|py)` con:
  - `ExecutionKind`: `pulse` | `scheduled_task`.
  - `ExecutionStatus`: `running` | `completed` | `failed` | `skipped` (documentar que `running` es transitorio e interno).
  - `ResourceRef`: `type`, `external_id`, `fingerprint?`.
  - `ExecutionRecord`: `id`, `kind`, `source_id`, `action`, `started_at`, `finished_at`, `status`, `result`, `error`, `skip_reason`, `resources`.
  - `ListExecutionsFilter`: `kind?`, `sourceId?`, `status?`, `since?`, `limit?`.
- [ ] **T1.2** Crear `src/state/StateStore.(ts|py)` con la interfaz:
  - `open()`, `close()`, `recoverInterrupted() → number`.
  - `startExecution(kind, sourceId, action) → ExecutionRecord`.
  - `finishExecution(id, status, { result?, error?, skipReason? })`.
  - `recordProcessed(executionId, resources[]) → { recorded, duplicates }`.
  - `hasProcessed(action, resourceRef) → boolean`.
  - `filterUnprocessed(action, resources[]) → resources[]`.
  - `getLastExecution(kind, sourceId) → ExecutionRecord | null`.
  - `listExecutions(filter) → ExecutionRecord[]`.
  - `purgeOlderThan(days)` (opcional).
- [ ] **T1.3** Definir los errores del módulo: `InvalidTransitionError`, `ValidationError` y `UnsupportedSchemaVersionError`.
- [ ] **T1.4** Crear `src/state/dedupKey.(ts|py)` con `computeDedupKey(action, ref)`:
  - `sha256(action + "\x1f" + type + "\x1f" + external_id + "\x1f" + (fingerprint ?? ""))`.
  - Normalizar `action` y `type` (trim + minúsculas); **no** normalizar `external_id`.
- [ ] **T1.5** Implementar las validaciones de transición, reutilizables por cualquier backend:
  - `status` final ∈ {`completed`, `failed`, `skipped`}.
  - `error` obligatorio si `status = failed`.
  - `skip_reason` obligatorio si `status = skipped`.
- [ ] **T1.6** Tests unitarios de `computeDedupKey`:
  - Misma entrada → misma clave (determinismo).
  - `fingerprint` distinto → clave distinta.
  - `fingerprint` ausente vs. vacío → misma clave (documentado).
  - Mayúsculas/espacios en `action` o `type` → misma clave; diferencia en `external_id` → clave distinta.
  - Sin colisión por concatenación (p. ej. `("ab","c")` vs. `("a","bc")`).

**Hecho cuando:** los tipos y la interfaz compilan/importan sin errores y los tests de T1.6 pasan.

---

## Fase 2 — Backend SQLite (`SqliteStateStore`)

**Depende de:** Fase 1

### 2.1 Configuración y esquema
- [ ] **T2.1** Crear `src/state/config.(ts|py)`:
  - Resolver `KRAKBOT_STATE_PATH` con default `<dataDir>/state/krakbot-state.db`.
  - Resolver `KRAKBOT_STATE_RETENTION_DAYS` y `KRAKBOT_STATE_DEDUP_RETENTION_DAYS` (ambos deshabilitados por defecto).
- [ ] **T2.2** Agregar la dependencia de SQLite elegida en T0.5 al gestor de paquetes.
- [ ] **T2.3** Crear `src/state/sqlite/migrations.(ts|py)` con la migración v1:
  - Tablas `schema_meta`, `executions`, `execution_resources` y `processed_resources`, según el esquema del plan.
  - Índices `idx_exec_kind_source_started`, `idx_exec_status` e `idx_processed_lookup`.
  - `execution_resources.execution_id` con `ON DELETE CASCADE`.
  - `processed_resources.execution_id` **nullable** con `ON DELETE SET NULL`.
- [ ] **T2.4** Implementar el runner de migraciones:
  - Leer `schema_meta.schema_version` (ausente = 0).
  - Aplicar las migraciones pendientes dentro de una transacción.
  - Si la versión en disco es mayor que la soportada, lanzar `UnsupportedSchemaVersionError` con un mensaje claro.

### 2.2 Apertura y ciclo de vida
- [ ] **T2.5** Implementar `open()` en `src/state/sqlite/SqliteStateStore.(ts|py)`:
  - Crear el directorio padre si no existe.
  - Ejecutar `PRAGMA journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON` y `busy_timeout=5000`.
  - Correr las migraciones.
  - Loguear a nivel info la ruta y la versión de esquema.
- [ ] **T2.6** Implementar `close()` (idempotente).
- [ ] **T2.7** Implementar un helper de transacción de escritura con `BEGIN IMMEDIATE`. Si el binding es asíncrono, agregar una cola o mutex en proceso.
- [ ] **T2.8** Implementar una factory `createStateStore(config)` que devuelva la implementación según el backend configurado.

### 2.3 Métodos
- [ ] **T2.9** `startExecution`: `INSERT` con UUID v4, `status='running'` y `started_at` en ISO 8601 UTC.
- [ ] **T2.10** `finishExecution`:
  - Validar con T1.5.
  - `UPDATE ... WHERE id=? AND status='running'`; si `changes=0`, lanzar `InvalidTransitionError`.
  - Serializar `result` a JSON; si supera el límite (16 KB por defecto), truncar y loguear un warning.
- [ ] **T2.11** `recordProcessed`, en una sola transacción, por cada recurso:
  - `INSERT OR IGNORE INTO processed_resources`; si `changes=0`, va a `duplicates`, si no, a `recorded`.
  - Upsert en `execution_resources` con `outcome='processed'` o `'skipped_duplicate'`.
- [ ] **T2.12** `hasProcessed`: `SELECT 1 FROM processed_resources WHERE dedup_key=?`.
- [ ] **T2.13** `filterUnprocessed`: calcular las claves, consultar en chunks de ≤500 y devolver los no procesados preservando el orden de entrada.
- [ ] **T2.14** `getLastExecution`: la última ejecución con `status != 'running'` para `(kind, source_id)`, ordenada por `started_at DESC`; `null` si no hay. Documentar la exclusión de `running` (agregar `includeRunning?` solo si la Fase 0 lo justifica).
- [ ] **T2.15** `listExecutions`:
  - `WHERE` dinámico por `kind`, `sourceId`, `status` y `since`.
  - Orden `started_at DESC`; `limit` por defecto 100, máximo 1000.
  - Cargar los `resources` con un segundo query batched sobre `execution_resources`.
- [ ] **T2.16** `recoverInterrupted`: marcar todas las `running` como `failed`, con `error='interrupted'` y `finished_at=now()`; devolver la cantidad de filas afectadas.
- [ ] **T2.17** (Opcional, PR 4) `purgeOlderThan(days)`: borrar las ejecuciones cerradas con `finished_at < now - days` (cascada a `execution_resources`), sin tocar `processed_resources`.

### 2.4 Tests del store
Todos los tests usan archivos temporales, nunca la ruta real.

- [ ] **T2.18** Tests unitarios (5.1):
  - Ciclo start → finish con cada estado final.
  - Un `finishExecution` doble lanza `InvalidTransitionError`.
  - `failed` sin `error` y `skipped` sin `skip_reason` lanzan `ValidationError`.
  - `hasProcessed` es `false` antes y `true` después de `recordProcessed`.
  - `filterUnprocessed` preserva el orden y funciona con >500 elementos (chunking).
  - Un `fingerprint` distinto no se considera duplicado.
  - `getLastExecution` devuelve `null` si nunca corrió y la última cerrada si sí (ignora las `running`).
  - `listExecutions` respeta filtros, orden, `limit` por defecto y tope de 1000.
  - `result` mayor al límite se trunca y emite un warning.
- [ ] **T2.19** Test de persistencia (5.2): escribir, `close()`, reabrir sobre el mismo archivo y verificar que todos los campos son iguales (incluidos `result` JSON y `resources`).
- [ ] **T2.20** Test de interrupción (5.3): `startExecution` sin cierre, reabrir y llamar a `recoverInterrupted()`. Verificar `failed` + `error='interrupted'` + `finished_at` seteado, y que sus recursos no están en `processed_resources`.
- [ ] **T2.21** Test de concurrencia (5.4): dos stores sobre el mismo archivo llaman a `recordProcessed` en paralelo con la misma `dedup_key`. Queda exactamente una fila en `processed_resources` y uno de los dos la recibe en `duplicates`.
- [ ] **T2.22** Tests de migraciones (5.6): una base vacía queda en v1; una base con versión mayor da `UnsupportedSchemaVersionError`.
- [ ] **T2.23** Integrar los tests de la Fase 1 y la Fase 2 al pipeline de CI existente.

**Hecho cuando:** T2.18–T2.22 pasan localmente y en CI. → **PR 1** (Fases 1 + 2, sin cambios de comportamiento en la app).

---

## Fase 3 — Helper de integración común (`runTracked`)

**Depende de:** Fase 2 (PR 1 mergeado o en la misma rama)

- [ ] **T3.1** Crear `src/state/runTracked.(ts|py)` con la firma `runTracked(store, { kind, sourceId, action }, callback) → ExecutionRecord`.
- [ ] **T3.2** Construir el contexto `ctx`:
  - `executionId`.
  - `lastExecution`, precargado con `getLastExecution(kind, sourceId)`.
  - `filterUnprocessed(resources, { action? })`: usa el `action` de la ejecución por defecto o el que se pase.
  - `markProcessed(resources)`: acumula en memoria.
  - `markSkippedDuplicate(resources)`: deja traza con `outcome='skipped_duplicate'`.
  - `setResult(summary)`.
  - `skip(reason)`: permite que la tarea cierre como `skipped` con un motivo propio (p. ej. `not_due`).
- [ ] **T3.3** Implementar el flujo de éxito:
  - `startExecution` → callback → `recordProcessed(acumulados)`.
  - Si no hubo recursos nuevos y sí duplicados: cerrar `skipped` con `skip_reason='duplicate'`.
  - Si la tarea llamó a `ctx.skip(reason)`: cerrar `skipped` con ese motivo.
  - Si no: cerrar `completed` con el `result`.
- [ ] **T3.4** Implementar el flujo de fallo:
  - `finishExecution(id, 'failed', { error })` **sin** llamar a `recordProcessed` con los acumulados.
  - Relanzar o loguear el error según la convención del repo (definida en la Fase 0).
- [ ] **T3.5** (Si se aprueba en T0.7) Implementar `ctx.commitProcessed(resources)`: persiste inmediatamente y es opt-in para acciones no idempotentes. Documentarlo como desviación consciente de la spec.
- [ ] **T3.6** Verificar que `src/state` no importa ni referencia nada de Pulse ni de Scheduled Tasks (criterio de aceptación 5). Agregar un chequeo de lint o un test si el stack lo permite.
- [ ] **T3.7** Tests de `runTracked`:
  - Éxito con recursos nuevos → `completed` y recursos en `processed_resources`.
  - Excepción en el callback → `failed`, ningún recurso marcado, error propagado.
  - Todos los recursos duplicados → `skipped` / `duplicate`.
  - `ctx.skip('not_due')` → `skipped` / `not_due`.
  - `commitProcessed` antes de un fallo → solo esos recursos quedan marcados (si aplica T3.5).
  - Ejecución sin recursos → igual queda registrada como `completed`.
  - `ctx.lastExecution` refleja la corrida anterior.

**Hecho cuando:** T3.7 pasa en CI. → **PR 2**.

---

## Fase 4 — Integración con Autonomous Pulse y Scheduled Tasks

**Depende de:** Fase 3, T0.2, T0.4

### 4.1 Bootstrap
- [ ] **T4.1** En el punto de arranque identificado en T0.4, en este orden:
  1. `createStateStore(config)`.
  2. `open()`.
  3. `recoverInterrupted()` + log a nivel info con la cantidad recuperada.
- [ ] **T4.2** Inyectar el store en Pulse y en el Scheduler con el mecanismo existente del repo (evitar un singleton global si ya se usa inyección).
- [ ] **T4.3** Llamar a `close()` en el shutdown (incluir señales de terminación si el repo ya las maneja).

### 4.2 Scheduled Tasks
- [ ] **T4.4** Envolver la ejecución de cada task con `runTracked(store, { kind: 'scheduled_task', sourceId: task.id ?? task.name, action: task.action ?? task.name }, ...)`.
- [ ] **T4.5** Exponer `ctx.lastExecution` a las tasks; si una task decide no correr, cerrar con `ctx.skip('not_due')`.
- [ ] **T4.6** En las tasks que iteran recursos (leads, reuniones, etc.), agregar `ctx.filterUnprocessed` antes de actuar y `ctx.markProcessed` después, sin cambiar la lógica de negocio.

### 4.3 Autonomous Pulse
- [ ] **T4.7** Definir el catálogo de `action` por tipo de trabajo del pulse (granularidad de la `dedup_key`) y documentarlo.
- [ ] **T4.8** Envolver cada ciclo de pulse con `runTracked(store, { kind: 'pulse', sourceId: pulseId, action }, ...)`.
- [ ] **T4.9** Antes de decidir, consultar `store.listExecutions({ kind: 'pulse', since: <ventana>, limit })` y pasar ese contexto al pulse. Definir la ventana por defecto.
- [ ] **T4.10** Antes de actuar sobre conversaciones, leads, reuniones o entregables, usar `ctx.filterUnprocessed(resources, { action })` con el `action` específico de cada sub-acción.

### 4.4 Tests de integración
- [ ] **T4.11** Test de integración (5.5) con un archivo temporal:
  - Un flujo de pulse y uno de scheduled task contra el mismo store.
  - El segundo ciclo sobre los mismos recursos cierra como `skipped` / `duplicate`.
  - Un recurso con `fingerprint` nuevo se reprocesa.
  - `listExecutions({ kind: 'pulse' })` desde un pulse nuevo ve los anteriores.
  - Una scheduled task ve su última ejecución en `ctx.lastExecution` después de reiniciar el store.
- [ ] **T4.12** Corrida manual de la app: verificar con `sqlite3` que el archivo de estado tiene registros de pulse y de scheduled tasks.

**Hecho cuando:** T4.11 pasa en CI y la corrida manual de T4.12 muestra los registros esperados.

---

## Fase 5 — Configuración, documentación y cierre

**Depende de:** Fase 4 (va en el mismo PR 3)

- [ ] **T5.1** Crear `docs/state.md` (o una sección en el README) con:
  - Las variables `KRAKBOT_STATE_PATH`, `KRAKBOT_STATE_RETENTION_DAYS` y `KRAKBOT_STATE_DEDUP_RETENTION_DAYS`, con sus defaults.
  - El supuesto de una sola instancia por archivo de estado.
  - La recomendación de no guardar contenido sensible (conversaciones completas, datos de leads) en `result` ni en `resources`.
  - La semántica de estados y de `skip_reason` (`duplicate`, `not_due`, `interrupted`).
  - La política de éxito parcial y `commitProcessed` (si aplica).
  - Cómo inspeccionar el archivo con `sqlite3` (queries de ejemplo).
- [ ] **T5.2** Agregar las variables a `.env.example`, si existe.
- [ ] **T5.3** Agregar el archivo de estado por defecto (y sus archivos `-wal`/`-shm`) al `.gitignore`.
- [ ] **T5.4** Revisar los logs: info al abrir el store y al recuperar interrumpidas; debug por ejecución iniciada o cerrada; warning al truncar `result`.
- [ ] **T5.5** Cerrar el issue #3 con el mapeo de criterios de aceptación (ver abajo) y links a los PRs.

**Hecho cuando:** la documentación está mergeada. → **PR 3** (Fases 4 + 5).

---

## Fase 6 — Retención (opcional)

**Depende de:** Fase 2 y resolución de la pregunta abierta 3 (T0.6)

- [ ] **T6.1** Completar `purgeOlderThan(days)` (T2.17) si no se hizo en el PR 1.
- [ ] **T6.2** Decidir el disparador: al arrancar, en una scheduled task propia o manual. Respetar que esté apagado por defecto.
- [ ] **T6.3** (Solo si `KRAKBOT_STATE_DEDUP_RETENTION_DAYS` está habilitado) Implementar la purga separada de `processed_resources`.
- [ ] **T6.4** Test de retención (5.7): `purgeOlderThan` borra las ejecuciones viejas y sus `execution_resources`, mantiene `processed_resources` (con `execution_id` en `NULL`) y no toca las `running`.

**Hecho cuando:** T6.4 pasa en CI. → **PR 4**.

---

## Anexo A — Fallback JSON (solo si en T0.5 se rechaza SQLite)

Estas tareas reemplazan a T2.2–T2.7 y T2.9–T2.17.

- [ ] **TA.1** Implementar `JsonStateStore` con la forma `{ schema_version, executions: {id: record}, processed: {dedup_key: entry} }`.
- [ ] **TA.2** Escritura atómica: escribir a `*.tmp`, hacer `fsync` y luego `rename`, después de cada mutación.
- [ ] **TA.3** Mutex en proceso para todas las mutaciones, más un lockfile `*.lock` al abrir para detectar una segunda instancia.
- [ ] **TA.4** Chequeo e inserción en `processed` dentro del mutex para garantizar unicidad.
- [ ] **TA.5** Parametrizar la suite de tests de la Fase 2 por backend y correrla contra `JsonStateStore`.
- [ ] **TA.6** Pasar la retención (Fase 6) de opcional a recomendada y documentar el costo O(n) por escritura.

---

## Mapeo a criterios de aceptación

| Criterio | Tareas |
|---|---|
| El estado sobrevive reinicios | T2.3–T2.5, T2.19, T2.20 |
| Un pulse nuevo consulta ejecuciones anteriores | T2.15, T4.9, T4.11 |
| Las scheduled tasks conocen su última ejecución | T2.14, T3.2, T4.5, T2.18, T4.11 |
| Se detectan y evitan duplicados | T1.4, T2.11, T2.13, T3.3, T4.6, T4.10, T1.6, T2.18, T2.21, T4.11 |
| Interfaz común para Pulse y Scheduled Tasks | T1.2, T3.1–T3.6, T4.4, T4.8 |

---

## Orden de PRs

1. **PR 1:** Fases 1 y 2 (T1.1–T2.23, sin T2.17). Es el store aislado y no cambia comportamiento.
2. **PR 2:** Fase 3 (T3.1–T3.7).
3. **PR 3:** Fases 4 y 5 (T4.1–T5.5).
4. **PR 4 (opcional):** Fase 6 (T6.1–T6.4).