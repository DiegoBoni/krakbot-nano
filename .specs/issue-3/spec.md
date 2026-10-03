# Spec: Estado persistente para ejecuciones autónomas y recurrentes

**Repo:** DiegoBoni/krakbot-nano · **Issue:** #3

## 1. Objetivo

Agregar una capa de estado persistente para que las ejecuciones autónomas y recurrentes (Autonomous Pulse y Scheduled Tasks) sepan qué hizo KrakBot antes y puedan evitar trabajo duplicado.

## 2. Problema

Un agente que corre periódicamente sin memoria puede reprocesar en cada ciclo las mismas conversaciones, leads, reuniones o entregables. Eso genera acciones repetidas, ruido para el usuario y gasto innecesario de recursos.

## 3. Alcance

**Incluye**
- Un modelo de datos para registrar ejecuciones.
- Un almacenamiento persistente que sobreviva reinicios del proceso.
- Una API interna para consultar el historial, consultar la última ejecución y detectar duplicados.
- Integración con Autonomous Pulse y Scheduled Tasks mediante una interfaz común.

**No incluye**
- Una UI para visualizar el historial.
- Sincronización entre múltiples instancias de KrakBot.
- Cambios en la lógica de negocio de cada tarea, más allá de consultar y registrar estado.

> Nota: esta spec no se basó en una lectura del código existente. Los nombres de módulos y rutas son propuestas y hay que validarlos contra la estructura real del repo antes de implementar.

## 4. Modelo de datos

Cada ejecución se guarda como un **registro de ejecución** (`ExecutionRecord`):

| Campo | Tipo | Descripción |
|---|---|---|
| `id` | string (UUID) | Identificador único de la ejecución. |
| `kind` | enum: `pulse` \| `scheduled_task` | Tipo de ejecución. |
| `source_id` | string | Identificador de la tarea programada o del pulse que originó la ejecución (p. ej. nombre o id de la task). |
| `started_at` | ISO 8601 (UTC) | Fecha y hora de inicio. |
| `finished_at` | ISO 8601 (UTC), nullable | Fecha y hora de fin. |
| `action` | string | Acción o tarea ejecutada. |
| `resources` | lista de `ResourceRef` | Recursos o contexto procesados. |
| `result` | JSON / string, nullable | Resultado generado, o un resumen y referencia a él. |
| `status` | enum: `completed` \| `failed` \| `skipped` | Estado final. |
| `error` | string, nullable | Detalle del error si `status = failed`. |
| `skip_reason` | string, nullable | Motivo si `status = skipped` (p. ej. `duplicate`). |

**`ResourceRef`**: referencia mínima para detectar trabajo ya procesado.

| Campo | Descripción |
|---|---|
| `type` | Tipo de recurso: `conversation`, `lead`, `meeting`, `deliverable`, etc. |
| `external_id` | Identificador del recurso en su sistema de origen. |
| `fingerprint` | Hash opcional del contenido o versión, para detectar cambios en un recurso ya visto. |

**Clave de deduplicación (`dedup_key`)**
- Se deriva de `action` + `type` + `external_id` (+ `fingerprint` si aplica).
- Se persiste junto a cada recurso procesado con éxito.
- Si el recurso cambia (el `fingerprint` es distinto), no se considera duplicado.

## 5. Almacenamiento

- Persistencia local en disco, sin servicios externos adicionales. Se propone **SQLite** en un archivo configurable. Otra opción es un archivo JSON con escritura atómica, si el proyecto evita dependencias nativas.
- La ruta se configura por variable de entorno (p. ej. `KRAKBOT_STATE_PATH`), con un valor por defecto dentro del directorio de datos de la aplicación.
- Las escrituras deben ser atómicas, para que un cierre abrupto no corrompa el estado.
- La estructura debe tener un número de versión de esquema para permitir migraciones futuras.
- La capa de estado se aísla detrás de una interfaz (`StateStore`), de modo que el backend se pueda cambiar sin tocar a los consumidores.

## 6. API de la capa de estado

Interfaz propuesta (agnóstica del lenguaje):

- `startExecution(kind, sourceId, action) → ExecutionRecord`: crea un registro en curso.
- `finishExecution(id, status, result?, error?, skipReason?)`: cierra el registro.
- `recordProcessed(executionId, resources[])`: marca los recursos como procesados, junto con sus `dedup_key`.
- `hasProcessed(action, resourceRef) → boolean`: indica si ese trabajo ya se hizo con éxito.
- `filterUnprocessed(action, resources[]) → resources[]`: filtra en lote lo no procesado.
- `getLastExecution(kind, sourceId) → ExecutionRecord | null`: devuelve la última ejecución de una task o pulse.
- `listExecutions({ kind?, sourceId?, status?, since?, limit? }) → ExecutionRecord[]`: consulta del historial.

## 7. Comportamiento esperado

1. **Inicio:** cada ejecución llama a `startExecution` antes de actuar y obtiene su `id`.
2. **Deduplicación:** antes de actuar sobre un recurso, la ejecución consulta `hasProcessed` o `filterUnprocessed`.
   - Si el recurso ya fue procesado, se omite y se registra con `status = skipped` y `skip_reason = duplicate`.
3. **Cierre:** al terminar llama a `finishExecution` con `completed` o `failed`, y `recordProcessed` solo para los recursos procesados con éxito.
4. **Fallos:** un recurso en una ejecución `failed` no se marca como procesado, así que puede reintentarse en el próximo ciclo.
5. **Reinicio del proceso:** una ejecución que quedó en curso (sin `finished_at`) al reiniciar se marca como `failed` con un error del tipo `interrupted` al arrancar. Sus recursos no cuentan como procesados.
6. **Scheduled Tasks:** al arrancar, cada task consulta `getLastExecution` para conocer su última ejecución (fecha, estado, resultado) y decidir si le corresponde correr.
7. **Autonomous Pulse:** cada pulse consulta `listExecutions` y `hasProcessed` para no repetir lo ya hecho en pulses anteriores.

## 8. Concurrencia

- Se asume una sola instancia de KrakBot por archivo de estado.
- Las operaciones de escritura deben serializarse (transacciones en SQLite o un lock de archivo), para que un pulse y una scheduled task simultáneos no se pisen.
- La verificación de duplicado y el registro deben ser lo bastante atómicos para evitar que dos ejecuciones concurrentes procesen el mismo recurso. Se propone una restricción de unicidad sobre `dedup_key`.

## 9. Retención

- Por defecto, el historial se conserva sin límite.
- Se deja preparado un mecanismo opcional de limpieza por antigüedad (p. ej. `KRAKBOT_STATE_RETENTION_DAYS`).
- La limpieza no debe borrar las `dedup_key` de recursos aún relevantes sin una política explícita. Hay que decidir esto antes de habilitarla (ver preguntas abiertas).

## 10. Criterios de aceptación

- [ ] **El estado sobrevive reinicios del proceso.**
  - Verificación: se registra una ejecución, se reinicia el proceso y la ejecución sigue consultable con todos sus campos.
- [ ] **Un nuevo pulse puede consultar ejecuciones anteriores.**
  - Verificación: un pulse nuevo obtiene vía `listExecutions` las ejecuciones de pulses previos.
- [ ] **Las scheduled tasks conocen su última ejecución.**
  - Verificación: `getLastExecution` devuelve fecha, estado y resultado de la última corrida de esa task, y `null` si nunca corrió.
- [ ] **Se pueden detectar y evitar acciones duplicadas.**
  - Verificación: procesar el mismo recurso dos veces resulta en una acción real y una ejecución `skipped` (`duplicate`). Un recurso con `fingerprint` distinto sí se reprocesa.
- [ ] **La capa de estado puede ser usada tanto por Autonomous Pulse como por Scheduled Tasks.**
  - Verificación: ambos consumidores usan la misma interfaz `StateStore` sin código específico de uno u otro en la capa.

## 11. Plan de pruebas

- **Unitarias:** creación y cierre de registros, `hasProcessed` y `filterUnprocessed`, comportamiento ante `fingerprint` distinto, y transiciones de estado válidas.
- **Persistencia:** escribir, simular reinicio (reabrir el store) y leer.
- **Interrupción:** una ejecución sin cierre se marca como `failed` (`interrupted`) al reabrir.
- **Concurrencia:** dos escrituras simultáneas sobre la misma `dedup_key` resultan en un único registro de recurso procesado.
- **Integración:** un flujo de pulse y uno de scheduled task contra el mismo store.

## 12. Riesgos y consideraciones

- **Corrupción del archivo de estado:** se mitiga con escrituras atómicas y con la opción de SQLite en modo WAL.
- **Crecimiento ilimitado:** se mitiga con la política de retención opcional.
- **Datos sensibles en `result` o `resources`:** conviene guardar referencias y resúmenes en lugar de contenido completo de conversaciones o leads.
- **Falsos duplicados:** se mitigan con el `fingerprint`, que distingue versiones de un mismo recurso.

## 13. Preguntas abiertas

1. ¿Qué backend de almacenamiento prefiere el proyecto (SQLite o archivo JSON)? ¿Hay restricciones de dependencias?
2. ¿Cuál es el lenguaje y la estructura del repo? Esto define los nombres de módulos y la forma de la interfaz.
3. ¿Qué ventana de retención de `dedup_key` es aceptable? ¿Un recurso procesado hace meses debe poder reprocesarse?
4. ¿El campo `result` debe guardar el contenido completo o solo una referencia?
5. ¿Se prevé más de una instancia de KrakBot compartiendo estado?