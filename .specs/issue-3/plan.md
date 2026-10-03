# Plan de implementación: Estado persistente para ejecuciones autónomas y recurrentes

**Repo:** DiegoBoni/krakbot-nano · **Issue:** #3 · **Spec de referencia:** "Estado persistente para ejecuciones autónomas y recurrentes"

> **Aviso:** el plan se armó sin acceso al código del repo, porque el workspace del agente estaba vacío. Las rutas, los nombres de módulos y las dependencias son propuestas. La **Fase 0** sirve para validarlos y ajustar el plan antes de escribir código.

---

## 0. Resumen

Vamos a agregar un módulo `state` con una interfaz `StateStore` y una implementación sobre SQLite (`SqliteStateStore`). Autonomous Pulse y Scheduled Tasks lo van a consumir a través de un helper común (`runTracked`) que maneja el ciclo de vida de cada ejecución: inicio, deduplicación, cierre y registro de recursos procesados.

La deduplicación se apoya en una tabla `processed_resources` con restricción `UNIQUE(dedup_key)`. Así la verificación y el registro son atómicos aunque haya ejecuciones concurrentes.

---

## Fase 0 — Descubrimiento y decisiones bloqueantes

**Objetivo:** confirmar el stack y resolver las preguntas abiertas que cambian el diseño.

| # | Tarea | Resultado esperado |
|---|---|---|
| 0.1 | Identificar el lenguaje, el runtime, el gestor de paquetes y el framework de tests del repo. | Stack confirmado (p. ej. TypeScript/Node + Vitest/Jest, o Python + pytest). |
| 0.2 | Ubicar dónde viven Autonomous Pulse y Scheduled Tasks: entrypoints, scheduler y loop del pulse. | Lista de archivos a tocar en la Fase 4. |
| 0.3 | Ubicar el mecanismo de configuración (env vars, archivo de config) y el "directorio de datos" de la app. | Dónde se lee `KRAKBOT_STATE_PATH` y cuál es su default. |
| 0.4 | Ver el bootstrap de la app (arranque y shutdown) para enganchar la apertura del store y la recuperación de ejecuciones interrumpidas. | Punto de inicialización definido. |
| 0.5 | Decidir el backend (pregunta abierta 1). **Recomendación: SQLite en modo WAL**, porque resuelve la atomicidad, las transacciones y la unicidad sin código propio. Hay que confirmar que una dependencia nativa es aceptable: `better-sqlite3` en Node o `sqlite3` de la stdlib en Python. | Backend elegido. Si se rechazan las dependencias nativas, ver el Anexo A. |
| 0.6 | Fijar defaults para las preguntas abiertas 3–5 (ver la sección "Decisiones por defecto"). | Acordado con el owner del repo. |

**Decisiones por defecto, si no hay respuesta del owner:**
- **Retención de `dedup_key`:** indefinida. La limpieza por antigüedad solo borra `executions` cerradas y **nunca** `processed_resources`, salvo con un flag explícito separado (`KRAKBOT_STATE_DEDUP_RETENTION_DAYS`, sin habilitar por defecto).
- **`result`:** se guarda un resumen o referencia en JSON. Se documenta que no se debe guardar contenido completo de conversaciones ni de leads. Se aplica un límite de tamaño (p. ej. 16 KB) que trunca y registra un warning.
- **Multi-instancia:** fuera de alcance. Se documenta el supuesto de una instancia por archivo de estado.

---

## Fase 1 — Modelo de datos y contrato (`StateStore`)

**Archivos propuestos** (ajustar según la Fase 0):
- `src/state/types.(ts|py)`: `ExecutionKind`, `ExecutionStatus`, `ResourceRef`, `ExecutionRecord` y `ListExecutionsFilter`.
- `src/state/StateStore.(ts|py)`: la interfaz.
- `src/state/dedupKey.(ts|py)`: la derivación de `dedup_key`.

**Tareas:**
1. Definir los tipos según la sección 4 de la spec.
   - `kind`: `pulse` | `scheduled_task`.
   - `status`: `running` (solo interno, mientras `finished_at` es `null`) | `completed` | `failed` | `skipped`.
     - Nota: la spec lista tres estados finales. `running` se agrega como estado transitorio para representar "en curso" sin depender solo de `finished_at`.
2. Definir la interfaz `StateStore` con los métodos de la sección 6:
   - `startExecution(kind, sourceId, action) → ExecutionRecord`
   - `finishExecution(id, status, { result?, error?, skipReason? })`
   - `recordProcessed(executionId, resources[]) → { recorded: ResourceRef[], duplicates: ResourceRef[] }`. Se extiende el contrato para devolver qué se pudo registrar, que es lo que hace útil la restricción de unicidad.
   - `hasProcessed(action, resourceRef) → boolean`
   - `filterUnprocessed(action, resources[]) → resources[]`
   - `getLastExecution(kind, sourceId) → ExecutionRecord | null`
   - `listExecutions(filter) → ExecutionRecord[]`
   - Ciclo de vida: `open()`, `close()` y `recoverInterrupted() → number`.
3. Implementar `computeDedupKey(action, ref)`:
   - Forma canónica: `sha256(action + "\x1f" + type + "\x1f" + external_id + "\x1f" + (fingerprint ?? ""))`.
   - El separador `\x1f` evita colisiones por concatenación.
   - Normalizar `type` y `action` (trim y minúsculas). `external_id` no se normaliza.
4. Validar las transiciones de estado:
   - Solo se puede llamar a `finishExecution` sobre un registro `running`.
   - `error` es obligatorio si `status = failed`.
   - `skip_reason` es obligatorio si `status = skipped`.

**Criterio de salida:** los tipos y la interfaz compilan, y los tests unitarios de `computeDedupKey` pasan (determinismo; un `fingerprint` distinto da una clave distinta).

---

## Fase 2 — Backend SQLite (`SqliteStateStore`)

**Archivos propuestos:**
- `src/state/sqlite/SqliteStateStore.(ts|py)`
- `src/state/sqlite/migrations.(ts|py)`
- `src/state/config.(ts|py)`: resuelve `KRAKBOT_STATE_PATH` y `KRAKBOT_STATE_RETENTION_DAYS`.

### 2.1 Esquema (migración v1)

```sql
CREATE TABLE schema_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
); -- ('schema_version', '1')

CREATE TABLE executions (
  id           TEXT PRIMARY KEY,           -- UUID v4
  kind         TEXT NOT NULL CHECK (kind IN ('pulse','scheduled_task')),
  source_id    TEXT NOT NULL,
  action       TEXT NOT NULL,
  started_at   TEXT NOT NULL,              -- ISO 8601 UTC
  finished_at  TEXT,
  status       TEXT NOT NULL CHECK (status IN ('running','completed','failed','skipped')),
  result       TEXT,                       -- JSON serializado
  error        TEXT,
  skip_reason  TEXT
);
CREATE INDEX idx_exec_kind_source_started ON executions(kind, source_id, started_at DESC);
CREATE INDEX idx_exec_status ON executions(status);

CREATE TABLE execution_resources (       -- recursos "vistos" por la ejecución (todos)
  execution_id TEXT NOT NULL REFERENCES executions(id) ON DELETE CASCADE,
  type         TEXT NOT NULL,
  external_id  TEXT NOT NULL,
  fingerprint  TEXT,
  outcome      TEXT NOT NULL CHECK (outcome IN ('processed','skipped_duplicate','failed')),
  PRIMARY KEY (execution_id, type, external_id)
);

CREATE TABLE processed_resources (       -- índice de deduplicación
  dedup_key    TEXT PRIMARY KEY,           -- UNIQUE implícito
  action       TEXT NOT NULL,
  type         TEXT NOT NULL,
  external_id  TEXT NOT NULL,
  fingerprint  TEXT,
  execution_id TEXT NOT NULL REFERENCES executions(id),
  processed_at TEXT NOT NULL
);
CREATE INDEX idx_processed_lookup ON processed_resources(action, type, external_id);
```

**Decisiones:**
- Se separan `execution_resources` (para el historial: qué tocó cada ejecución) y `processed_resources` (para deduplicar). Así la retención puede tratar el historial y la deduplicación de forma independiente (sección 9 de la spec).
- `processed_resources.execution_id` **no** tiene `ON DELETE CASCADE`, para que borrar historial viejo no borre las claves de deduplicación. Si la limpieza borra una ejecución referenciada, la FK se resuelve dejando `execution_id` "huérfano": se desactiva la FK en esa columna o se usa `ON DELETE SET NULL` con columna nullable. **Se elige `SET NULL`.**

### 2.2 Apertura y configuración
1. Resolver la ruta: `KRAKBOT_STATE_PATH` o el default `<dataDir>/state/krakbot-state.db`. Crear el directorio si no existe.
2. Al abrir, ejecutar:
   - `PRAGMA journal_mode=WAL;`
   - `PRAGMA synchronous=NORMAL;`
   - `PRAGMA foreign_keys=ON;`
   - `PRAGMA busy_timeout=5000;`
3. Correr las migraciones pendientes dentro de una transacción, según `schema_meta.schema_version`. Si la versión en disco es mayor que la soportada, fallar con un error claro (protege contra downgrades).

### 2.3 Implementación de métodos
- **`startExecution`:** `INSERT` con `status='running'`, `started_at=now()` y un UUID nuevo.
- **`finishExecution`:** `UPDATE ... WHERE id=? AND status='running'`. Si `changes=0`, lanzar `InvalidTransitionError`. Serializar `result` como JSON y truncarlo si supera el límite.
- **`recordProcessed`:** dentro de una transacción, para cada recurso:
  - `INSERT OR IGNORE INTO processed_resources`. Si `changes=0`, la clave ya estaba y el recurso va a `duplicates`.
  - Upsert en `execution_resources` con el `outcome` correspondiente.
- **`hasProcessed`:** `SELECT 1 FROM processed_resources WHERE dedup_key=?`.
- **`filterUnprocessed`:** calcular las claves y hacer un `SELECT dedup_key ... WHERE dedup_key IN (...)` en chunks de ≤500. Devuelve los recursos cuya clave no aparece, preservando el orden de entrada.
- **`getLastExecution`:** `SELECT ... WHERE kind=? AND source_id=? AND status!='running' ORDER BY started_at DESC LIMIT 1`.
  - Se excluyen las `running`, porque "última ejecución" tiene que reflejar una corrida terminada.
  - Hay que documentarlo y exponer `includeRunning?` si la Fase 0 muestra que hace falta.
- **`listExecutions`:**
  - Construye un `WHERE` dinámico con `kind`, `sourceId`, `status` y `since` (`started_at >= ?`).
  - Ordena por `started_at DESC`. `limit` vale 100 por defecto, con un máximo de 1000.
  - Incluye los `resources` de cada ejecución con un segundo query batched sobre `execution_resources`.
- **`recoverInterrupted`:**
  - `UPDATE executions SET status='failed', error='interrupted', finished_at=now() WHERE status='running'`.
  - Devuelve el número de filas afectadas.
  - Sus recursos nunca entraron en `processed_resources`, porque `recordProcessed` solo se llama para éxitos. Por eso no cuentan como procesados.
- **`purgeOlderThan(days)`** (opcional, apagado por defecto): borra de `executions` las ejecuciones cerradas con `finished_at < now - days`, más las filas de `execution_resources` por cascada. No toca `processed_resources`.

### 2.4 Serialización de escrituras
- SQLite ya serializa a los escritores. Además, todas las operaciones de escritura pasan por una transacción `BEGIN IMMEDIATE`, para tomar el lock de escritura al principio y evitar `SQLITE_BUSY` a mitad de transacción.
- Si el binding es asíncrono (p. ej. `sqlite3` de Node), envolverlo con una cola o mutex en proceso. Con `better-sqlite3`, que es síncrono, no hace falta.

**Criterio de salida:** los tests unitarios y de persistencia de la Fase 5 (5.1–5.3) pasan contra `SqliteStateStore`.

---

## Fase 3 — Helper de integración común (`runTracked`)

**Objetivo:** que Pulse y Scheduled Tasks usen el mismo flujo sin duplicar lógica, y que `StateStore` no conozca a sus consumidores (criterio de aceptación 5).

**Archivo propuesto:** `src/state/runTracked.(ts|py)`

**Firma conceptual:**
```
runTracked(store, { kind, sourceId, action }, async (ctx) => { ... }) → ExecutionRecord
```

**Contexto `ctx` que recibe la tarea:**
- `ctx.executionId`
- `ctx.filterUnprocessed(resources)`: delega en el store con el `action` ya fijado.
- `ctx.markProcessed(resources)`: acumula los recursos procesados con éxito; se persisten al cerrar.
- `ctx.markSkippedDuplicate(resources)`: deja traza en `execution_resources` con `outcome='skipped_duplicate'`.
- `ctx.setResult(summary)`.
- `ctx.lastExecution`: precargado con `getLastExecution(kind, sourceId)`.

**Flujo:**
1. `startExecution`.
2. Ejecutar el callback.
3. Si termina OK:
   - `recordProcessed(acumulados)`.
   - Si no hubo recursos nuevos y sí duplicados, se cierra con `status='skipped'` y `skip_reason='duplicate'`. Si no, se cierra con `completed`.
4. Si lanza una excepción: `finishExecution(id, 'failed', { error: mensaje })`. **No** se llama a `recordProcessed`, así que los recursos se reintentan en el próximo ciclo. Después se relanza el error, o se loguea, según la convención del repo.

**Política sobre éxito parcial (decisión de plan):**
- Si la tarea procesó con éxito algunos recursos antes de fallar, ¿se marcan? La spec dice que un recurso en una ejecución `failed` no se marca como procesado. **Se respeta literalmente:** ninguno se marca.
- Para evitar reprocesar acciones con efectos externos ya hechas, se expone `ctx.commitProcessed(resources)`. Persiste inmediatamente y es opt-in, para tareas donde la acción no es idempotente (p. ej. enviar un mensaje).
- Esto se documenta como desviación consciente y se valida con el owner.

**Criterio de salida:** tests de `runTracked` para los casos de éxito, fallo, todo duplicado y `commitProcessed`.

---

## Fase 4 — Integración con Autonomous Pulse y Scheduled Tasks

### 4.1 Bootstrap
- En el arranque de la app (punto identificado en 0.4), en este orden:
  1. Crear la instancia única de `StateStore` con `createStateStore(config)`.
  2. Llamar a `open()`.
  3. Llamar a `recoverInterrupted()` y loguear cuántas ejecuciones se marcaron como `interrupted`.
- Inyectar el store en Pulse y Scheduler: por constructor o por el contenedor/contexto existente. Evitar un singleton global si el repo ya usa inyección.
- En el shutdown, llamar a `close()`.

### 4.2 Scheduled Tasks
- Envolver la ejecución de cada task con `runTracked(store, { kind: 'scheduled_task', sourceId: task.id ?? task.name, action: task.action ?? task.name }, ...)`.
- Exponer `ctx.lastExecution` a la task para que decida si le corresponde correr (sección 7.6 de la spec).
  - Si decide no correr, cierra con `skipped` y `skip_reason='not_due'`. Así queda registro y no se confunde con `duplicate`.
- Las tasks que iteran recursos (leads, reuniones, etc.) usan `ctx.filterUnprocessed` antes de actuar y `ctx.markProcessed` después.

### 4.3 Autonomous Pulse
- Envolver cada ciclo de pulse con `runTracked(store, { kind: 'pulse', sourceId: pulseId, action: <acción del pulse> }, ...)`.
- Antes de decidir, el pulse puede consultar `store.listExecutions({ kind: 'pulse', since: <ventana>, limit })` para tener contexto de pulses anteriores (sección 7.7 de la spec).
- Antes de actuar sobre conversaciones, leads, reuniones o entregables, usar `ctx.filterUnprocessed`.
- Si el pulse tiene varias acciones distintas en un mismo ciclo, usar el `action` específico en cada llamada de deduplicación: `ctx.filterUnprocessed(resources, { action })`.

### 4.4 Restricciones
- No modificar la lógica de negocio de las tareas: solo agregar consultas y registro de estado (sección 3 de la spec).
- Si una task o pulse no procesa recursos identificables, igual queda registrada su ejecución (inicio, fin y estado), sin deduplicación.

**Criterio de salida:** los tests de integración de la Fase 5 (5.5) pasan, y una corrida manual muestra los registros en el archivo de estado.

---

## Fase 5 — Pruebas

| # | Tipo | Casos |
|---|---|---|
| 5.1 | Unitarias del store | Ciclo start→finish; `finishExecution` doble lanza error; validaciones de `error` y `skip_reason`; `hasProcessed` antes y después de `recordProcessed`; `filterUnprocessed` preserva el orden y filtra en lote (>500 elementos, para probar el chunking); un `fingerprint` distinto no es duplicado; `getLastExecution` devuelve `null` si nunca corrió y la última cerrada si sí. |
| 5.2 | Persistencia | Escribir, `close()`, reabrir sobre el mismo archivo y leer: todos los campos son iguales (incluido `result` JSON y `resources`). |
| 5.3 | Interrupción | `startExecution` sin cierre, reabrir y `recoverInterrupted()`: el registro queda `failed` con `error='interrupted'` y `finished_at` seteado, y sus recursos **no** están en `processed_resources`. |
| 5.4 | Concurrencia | Dos conexiones o stores sobre el mismo archivo hacen `recordProcessed` con la misma `dedup_key` en paralelo: queda exactamente una fila en `processed_resources`, y uno de los dos la recibe en `duplicates`. |
| 5.5 | Integración | Un flujo de pulse y uno de scheduled task contra el mismo store (archivo temporal): el segundo ciclo sobre los mismos recursos cierra como `skipped/duplicate`; un recurso con `fingerprint` nuevo se reprocesa; un `listExecutions({kind:'pulse'})` desde un pulse nuevo ve los anteriores. |
| 5.6 | Migraciones | Una base vacía queda en v1; una base con versión mayor a la soportada da error claro. |
| 5.7 | Retención (si se implementa) | `purgeOlderThan` borra ejecuciones viejas y mantiene `processed_resources`. |

- Todos los tests usan archivos temporales, nunca el path real.
- Hay que agregarlos al pipeline de CI existente.

---

## Fase 6 — Configuración, documentación y cierre

1. Documentar en el README o en `docs/state.md`:
   - Las variables `KRAKBOT_STATE_PATH` y `KRAKBOT_STATE_RETENTION_DAYS`.
   - El supuesto de una sola instancia.
   - La recomendación de no guardar contenido sensible en `result` ni en `resources`.
   - Cómo inspeccionar el archivo con `sqlite3`.
2. Agregar las variables a `.env.example`, si existe.
3. Agregar el archivo de estado por defecto al `.gitignore`.
4. Logging: un log a nivel info al abrir el store (ruta y versión de esquema) y al recuperar ejecuciones interrumpidas; un log a nivel debug por ejecución iniciada o cerrada.

---

## Mapeo a criterios de aceptación

| Criterio | Cubierto por |
|---|---|
| El estado sobrevive reinicios | Fase 2 (SQLite + WAL), test 5.2 |
| Un pulse nuevo consulta ejecuciones anteriores | Fase 4.3 (`listExecutions`), test 5.5 |
| Las scheduled tasks conocen su última ejecución | Fase 2.3 (`getLastExecution`), Fase 4.2 (`ctx.lastExecution`), test 5.1 |
| Se detectan y evitan duplicados | Fase 1.3 (`dedup_key`), Fase 2.3 (`INSERT OR IGNORE` + UNIQUE), Fase 3 (`skipped/duplicate`), tests 5.1, 5.4 y 5.5 |
| Interfaz común para Pulse y Scheduled Tasks | Fase 1 (`StateStore`), Fase 3 (`runTracked`), sin referencias a consumidores dentro de `src/state` |

---

## Orden sugerido de PRs

1. **PR 1:** Fases 1 y 2, con los tests 5.1–5.4 y 5.6. Es el store aislado y no cambia comportamiento.
2. **PR 2:** Fase 3 (`runTracked`) con sus tests.
3. **PR 3:** Fase 4 (bootstrap + Scheduled Tasks + Pulse), con el test 5.5 y la Fase 6.
4. **PR 4 (opcional):** retención (`purgeOlderThan` + test 5.7), una vez resuelta la pregunta abierta 3.

---

## Riesgos del plan

- **Estructura del repo desconocida:** las rutas y la forma de inyección pueden cambiar. Se mitiga con la Fase 0 obligatoria antes del PR 1.
- **Dependencia nativa de SQLite:** puede romper builds en algunos entornos. Se mitiga validando en CI y en el target de deploy, con el Anexo A como fallback.
- **Éxito parcial en ejecuciones fallidas:** puede repetir efectos externos. Se mitiga con `commitProcessed` opt-in, pendiente de validar con el owner.
- **Semántica de `action` en Pulse:** si el pulse no tiene acciones discretas, la `dedup_key` puede quedar demasiado gruesa o demasiado fina. Hay que definir los `action` por tipo de trabajo durante la Fase 4.3.

---

## Anexo A — Fallback: backend JSON con escritura atómica

Solo aplica si en la Fase 0 se rechaza SQLite.

- Un archivo `krakbot-state.json` con la forma `{ schema_version, executions: {id: record}, processed: {dedup_key: entry} }`.
- **Escritura atómica:** escribir a `*.tmp`, hacer `fsync` y luego `rename`. Mantener el estado en memoria y persistir después de cada mutación.
- **Serialización:** un mutex en proceso para todas las mutaciones, más un lockfile (`*.lock`) al abrir para detectar una segunda instancia.
- **Unicidad:** chequear e insertar en `processed` dentro del mutex.
- **Limitaciones:** el costo de escritura es O(n) por mutación y no escala a historiales grandes. Por eso la retención pasa a ser recomendable, no opcional.
- La misma interfaz `StateStore` y la misma suite de tests (parametrizada por backend) garantizan que sea intercambiable.