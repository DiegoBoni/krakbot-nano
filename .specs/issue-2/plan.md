# Plan de implementación: tareas programadas de punta a punta

**Issue:** #2 — `DiegoBoni/krakbot-nano`  
**Dependencia obligatoria:** #4 — `cap_activity_log`

## Objetivo y límites

Extender `scheduled_task` y las capacidades existentes para crear tareas persistentes de una sola ejecución, listar su estado, activarlas/desactivarlas y borrarlas coherentemente, con entrega IM e historial correlacionado.

El scheduler decide cuándo disparar. El agente principal resuelve las instrucciones de `wake_agent`; no se crean agentes independientes.

Se conservarán `cron`, `interval`, los tres modos existentes, persistencia JSON + NVS, contadores, `max_runs`, `trigger_now` y pause/resume. Quedan fuera de alcance reconstruir el scheduler, modificar el loop de `claw_core`, desarrollar UI, incorporar SQLite o agregar dependencias externas.

## 1. Confirmar contratos antes de implementar

El repositorio no está disponible en el workspace de especificación. Los nombres de APIs adicionales, estructuras, eventos y archivos de integración deben confirmarse en el código; este plan no los da por existentes.

Leer:

- `CLAUDE.md`, `.agents/design.md` y `.agents/gotchas.md`.
- El `SKILL.md` de `scheduled_task` y `scripts/add_scheduled_task.lua`.
- `cap_scheduler.h`, `claw_memory.h`, `claw_event.h` y el contrato de `claw_paths`.
- Implementación del scheduler, router y ejecución de los tres modos.
- Contratos, retención y hooks de `cap_activity_log` de #4.
- Patrones de pruebas en `components/**/test_apps/` y módulos Lua.

Confirmar y documentar antes de editar:

| Área | Contrato que debe verificarse |
|---|---|
| Scheduler | Creación, consulta, eliminación, pause/resume, `start_at_ms`, ejecución única, contadores y persistencia. |
| Tiempo | Zona horaria del firmware, fuente de sincronización, conversión local y detección de horarios ambiguos o inexistentes. |
| Validaciones | Parser cron real, rangos admitidos, mínimo de intervalo y semántica de intervalos sin reloj civil. |
| Router | Asociación tarea/regla, consulta y restauración de reglas, reglas exclusivas y uso compartido de `im_any_message_agent`. |
| Entrega | Selección y disponibilidad del canal IM, validación de `chat_id` y resultado observable del envío. |
| Ejecución | Hooks de inicio/finalización de scripts y del agente principal; propagación de contexto y correlación. |
| Historial | Escritura, consulta, correlación, retención y tratamiento de ejecuciones interrumpidas por reinicio. |

Si falta un hook necesario para correlacionar un resultado, resolverlo en las capabilities, router o hooks existentes antes de aceptar la integración de historial.

## 2. Extender creación y validaciones

Modificar el skill y `scripts/add_scheduled_task.lua`, conservando sus parámetros y comportamientos actuales.

### Programación `once`

- Admitir `kind=once` y exigir `at`.
- Interpretar valores numéricos exclusivamente como epoch en milisegundos.
- Admitir fecha y hora local completas en un formato estricto documentado, inicialmente `YYYY-MM-DDTHH:mm:ss`, sujeto al soporte verificado del firmware.
- Usar la zona horaria configurada por el firmware. Rechazar fechas inexistentes o ambiguas hasta recibir un instante inequívoco.
- Validar formato, rango, precisión representable y overflow, sin conversiones silenciosas.
- Exigir reloj sincronizado y un instante estrictamente futuro.
- Pasar el instante convertido a `start_at_ms` y configurar una ejecución mediante los mecanismos existentes.
- Rechazar `at` en otros tipos y parámetros de frecuencia incompatibles con `once`.
- Mantener el consumo de la ejecución aunque falle; no incorporar reintentos automáticos.

El skill resolverá expresiones relativas antes de invocar el script y pedirá aclaración cuando falte fecha, zona horaria o destino necesario.

### Destino IM

Para `send_message` y `wake_agent`, validar canal IM y `chat_id` antes de crear recursos. Reutilizar el destino de la conversación únicamente cuando esté disponible y sea válido.

Rechazar destinos local/web y emitir un error accionable cuando falte IM: configurar un canal y proporcionar `chat_id`.

Conservar el contrato actual de `run_script`: permitir ausencia de IM si no requiere entregar mensajes y documentar esa condición.

### Validación y rollback de creación

Reutilizar validadores del firmware para cron, identificadores, intervalos y límites. Exigir sincronización para cron y `once`; preservar la semántica actual de intervalos si funcionan sin reloj civil.

Comprobar duplicados tanto en scheduler como en los recursos de router que se crearán, sin sobrescribirlos. Obtener el mínimo de intervalo de configuración existente o de una constante/Kconfig documentada.

Mantener el rollback existente y extenderlo a los nuevos datos. Ante falla de persistencia o router, informar causa y resultado de recuperación. Si la recuperación falla, identificar los recursos pendientes.

## 3. Asociación retrocompatible entre tarea y regla

Usar primero la asociación que ya establezca el contrato de `scheduled_task`. Si no permite identificar y restaurar recursos inequívocamente, agregar metadatos mínimos, acotados y retrocompatibles.

La asociación debe distinguir:

- Entrada del scheduler.
- Regla exclusiva, cuando exista.
- Regla compartida `im_any_message_agent`.
- Modo, instrucción y destino necesarios para operar y listar.

No inferir propiedad mediante coincidencias amplias de nombres. Las tareas antiguas deben seguir funcionando; cuando su asociación no pueda determinarse con seguridad, el listado debe mostrar la inconsistencia y las operaciones deben evitar modificar reglas ajenas.

## 4. Implementar borrado coherente

Agregar y exponer `remove_scheduled_task` siguiendo el patrón de scripts del skill.

Secuencia:

1. Localizar la tarea y su regla asociada mediante la asociación verificada.
2. Guardar la información necesaria para restaurarlas, incluida la condición activada/desactivada.
3. Impedir nuevos disparos mediante los mecanismos existentes y proteger la operación frente a cambios concurrentes.
4. Eliminar la regla exclusiva, si existe, y la entrada del scheduler.
5. Ante una falla, restaurar el estado previo y comunicar el resultado del rollback.

Nunca eliminar `im_any_message_agent` ni reglas ajenas al skill.

La operación será idempotente: si ambos recursos están ausentes, devolver éxito indicando que la tarea no existe; si queda un recurso residual inequívocamente identificado, limpiarlo. Si falla también el rollback, devolver error explícito con los recursos pendientes de recuperación.

Una ejecución iniciada podrá finalizar después del borrado. Conservar su contexto de entrega y correlación sin depender de que la tarea siga almacenada. No cancelar trabajo activo ni borrar historial.

## 5. Exponer listado y activación

Agregar una operación de listado utilizando las APIs existentes y consultar `cap_activity_log` para obtener los resultados, sin almacenar un segundo historial en el scheduler.

Mostrar, cuando corresponda:

| Información | Tratamiento |
|---|---|
| Identificador, nombre, modo e instrucción | Datos de la tarea y asociación verificada. |
| Activación y finalización | Distinguir pausa de programación consumida. |
| Programación | Tipo, frecuencia o instante. |
| Último disparo y próxima ejecución | Representar explícitamente la ausencia de datos. |
| Última ejecución | Estado y resultado resumido provenientes del historial. |
| Destino y regla | Canal IM, `chat_id` y estado de recursos asociados. |

Una tarea nueva no tendrá última ejecución. Una tarea `once` consumida no tendrá próxima ejecución. Si la retención del historial ya eliminó un resultado, mostrar su ausencia sin inventarlo.

Señalar recursos faltantes o inconsistentes, sin ocultarlos ni recrearlos automáticamente.

Exponer y documentar activación/desactivación individual mediante pause/resume existentes. Desactivar impedirá nuevos disparos y permitirá finalizar una ejecución activa.

## 6. Integrar historial con #4

Esta etapa depende del contrato disponible de `cap_activity_log`; el issue #2 no podrá cerrarse sin completarla.

Registrar cada disparo automático o manual con `origin=scheduled_task`, identificador de tarea y correlación de ejecución. Adaptar al contrato real los datos de modo, origen automático/manual, fechas de disparo/inicio/finalización, estado y resultado resumido o error.

Propagar la correlación y, cuando corresponda, el destino IM hasta el resultado terminal:

| Modo | Momento de finalización |
|---|---|
| `send_message` | Resultado de envío, sin asumir lectura. |
| `run_script` | Finalización o error del script. |
| `wake_agent` | Finalización o error del agente principal, incluida la entrega requerida. |

Aceptar o encolar un evento en el router no completará la ejecución. Usar los hooks existentes y extender su contexto cuando sea necesario, sin modificar el loop de `claw_core`.

Una falla de ejecución, entrega o escritura del log no debe detener el scheduler ni bloquear otras tareas. No registrar éxito ante operaciones fallidas. Si el canal desaparece después de crear una tarea, registrar la falla de entrega.

Respetar la retención acotada de #4 y conservar ejecuciones retenidas después del borrado. Las ejecuciones interrumpidas por reinicio deberán permanecer identificadas como no completadas, según el contrato del log.

## 7. Persistencia y restricciones de implementación

- Mantener compatibilidad con JSON + NVS y datos anteriores, sin migraciones destructivas.
- Resolver rutas escribibles mediante `claw_paths_join(CLAW_PATH_DATA, ...)`.
- Acotar metadatos y resultados; usar la retención de #4.
- Realizar escrituras atómicas con temporal + rename; agrupar escrituras y evitar `fsync` por entrada.
- Permitir arranque vacío ante archivos ausentes o corruptos, sin crash.
- Respetar el desgaste de flash y los aproximadamente 3 MB de DATA.
- Usar handles opacos, configuración por struct, ciclo de vida existente y retornos `esp_err_t` en C.
- Liberar recursos y proteger estado compartido mediante mutex, sin mantener bloqueos durante trabajo prolongado.
- Usar constantes o Kconfig para límites; mantener archivos menores a 1500 líneas.
- Si se requiere registrar una capability, hacerlo en `components/common/app_claw/app_capabilities.c`.
- No usar variables de entorno para configurar el firmware ni asumir que `idf.py flash` actualiza DATA.

## 8. Documentación del skill

Actualizar `SKILL.md` con:

- Parámetros y ejemplos de `once`, cron e intervalos.
- Formato textual de `at`, epoch en milisegundos y zona horaria del firmware.
- Resolución previa de fechas relativas y aclaraciones necesarias.
- Requisitos de reloj y errores de fechas ambiguas, inexistentes o pasadas.
- Destinos IM, reutilización de contexto y condición sin destino de `run_script`.
- Borrado, idempotencia, rollback y continuidad del trabajo activo.
- Listado, interpretación de datos ausentes y recursos inconsistentes.
- Pause/resume, historial y ausencia de reintentos nuevos.

## 9. Verificación automatizada

Agregar pruebas siguiendo los patrones existentes de `components/**/test_apps/` y tests Lua junto al módulo cuando estén disponibles.

| Área | Casos mínimos |
|---|---|
| Tiempo | Fecha local y epoch equivalentes; límites, overflow, pasado/igual al presente, reloj no sincronizado y horarios ambiguos/inexistentes. |
| Validaciones | Cron inválido, identificador duplicado, intervalo inferior al mínimo y parámetros incompatibles. |
| Creación | Fallas de router y persistencia; rollback recuperable y rollback fallido sin éxito falso. |
| Borrado | Tarea activa/pausada, recursos ausentes o residuales, fallas parciales, restauración del estado y preservación de reglas compartidas/ajenas. |
| Listado | Tareas nuevas, pausadas, consumidas, inconsistentes y resultados ausentes por retención. |
| Ejecución | Correlación automática/manual en los tres modos; entrega fallida; otra tarea continúa tras falla. |
| Persistencia | Datos anteriores, ausentes y corruptos; reinicio antes y después de consumir `once`; ejecución interrumpida sin éxito falso. |

Verificar mediante inyección de fallas que ninguna ruta deje recursos parciales recuperables sin informar y que cada rollback restaure la activación original.

## 10. Verificación en placa y entrega

Ejecutar en un entorno con ESP-IDF:

`idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`

Realizar las pruebas manuales:

1. Crear `once` futuro con reloj sincronizado; comprobar entrega IM, historial y ausencia de segundo disparo.
2. Crear cron e intervalo en modo `wake_agent`; comprobar ejecución automática por el agente principal.
3. Pausar y reanudar; verificar el próximo disparo informado.
4. Reiniciar antes y después de ejecutar `once`; comprobar persistencia y ausencia de repetición.
5. Borrar tareas de cada modo; comprobar eliminación exclusiva y conservación de `im_any_message_agent`.
6. Probar destino ausente al crear y canal no disponible al ejecutar.
7. Provocar falla de script o agente mientras otra tarea continúa.
8. Ejecutar `trigger_now` y verificar correlación de historial.
9. Arrancar con datos anteriores, ausentes y corruptos sin crash.
10. Superar la retención del log y comprobar la política acotada de #4.

La descripción del PR incluirá cambios, compatibilidad, integración con #4, pruebas realmente ejecutadas, verificaciones pendientes y el comando de compilación. NightShift no dispone de toolchain ESP-IDF: no declarar compilación ni tests exitosos sin ejecutarlos.

## Condiciones de cierre

- [ ] `once` convierte correctamente fecha local y epoch en milisegundos, persiste y se ejecuta una sola vez.
- [ ] Cron, intervalos y tareas persistidas anteriormente conservan su comportamiento.
- [ ] `wake_agent` usa el agente principal y conserva el destino hasta entregar su respuesta.
- [ ] Listado, pause/resume y borrado cumplen sus contratos.
- [ ] Creación y borrado recuperan fallas parciales o informan explícitamente la recuperación pendiente.
- [ ] Validaciones no dejan recursos parciales y producen errores accionables.
- [ ] Disparos automáticos y manuales tienen resultados correlacionados en `cap_activity_log`.
- [ ] Fallas de ejecución, entrega o log no bloquean las demás tareas.
- [ ] Documentación y estado real de verificación están incluidos.
- [ ] La dependencia #4 está integrada y verificada antes de cerrar #2.