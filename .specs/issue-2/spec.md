# Spec: tareas programadas de punta a punta

**Issue:** #2 — `DiegoBoni/krakbot-nano`  
**Dependencia:** #4 — `cap_activity_log`

## Objetivo

Permitir que instrucciones como «haceme acordar de X mañana a las 17:30» o «revisá Y todos los lunes» se conviertan en tareas programadas persistentes, con entrega y seguimiento de resultados.

El scheduler determina **cuándo** disparar una tarea. El agente principal determina **cómo** resolver su instrucción. No se crean agentes independientes.

## Base existente

Según el contexto verificado del issue:

- `cap_scheduler` ya implementa `once`, `interval`, `cron`, persistencia JSON + NVS, `max_runs`, contadores, `trigger_now` y pause/resume.
- El skill `scheduled_task` y `scripts/add_scheduled_task.lua` ya crean tarea y regla de router con rollback.
- Los modos existentes son `wake_agent`, `send_message` y `run_script`.
- `wake_agent` utiliza la regla compartida `im_any_message_agent`.

La implementación debe extender estas piezas sin reconstruirlas.

El repositorio no está disponible en el workspace de especificación. Los contratos y nombres de APIs adicionales deben confirmarse leyendo el código antes de implementar; este documento no presupone funciones que no hayan sido verificadas.

## Alcance

1. Incorporar programación `once` al skill y su script.
2. Incorporar borrado coherente de tarea y regla asociada, con rollback.
3. Exponer un listado coherente y las operaciones existentes de activación/desactivación.
4. Registrar disparos y resultados mediante `cap_activity_log`.
5. Agregar validaciones y documentar destinos de entrega.

Quedan fuera de alcance reescribir el scheduler, desarrollar UI, modificar `claw_core`, incorporar SQLite o agregar dependencias externas.

## Contrato funcional

### Creación

Conservar los parámetros y comportamientos existentes de `cron`, `interval`, `wake_agent`, `send_message` y `run_script`. Agregar:

| Campo | Contrato |
|---|---|
| `kind` | Acepta también `once`. |
| `at` | Obligatorio para `once`; fecha y hora local completas o epoch en milisegundos. |
| `chat_id` | Identifica el chat de destino dentro del canal IM seleccionado. |

Para `once`:

- Un valor numérico de `at` representa epoch en milisegundos; no inferir segundos.
- Un valor textual representa fecha y hora local completas, con formato documentado en `SKILL.md`, por ejemplo `YYYY-MM-DDTHH:mm:ss`.
- Convertir la fecha local usando la zona horaria configurada por el firmware, nunca la del entorno de desarrollo.
- Resolver expresiones como «mañana a las 17:30» antes de invocar el script. Si falta información necesaria, pedir aclaración.
- Rechazar horas inexistentes o ambiguas por cambios de zona horaria hasta contar con una fecha inequívoca.
- Pasar el instante convertido al `start_at_ms` existente.
- Configurar una sola ejecución mediante los mecanismos existentes del scheduler. No aproximar con cron.
- Rechazar `at` para otros tipos y parámetros de frecuencia incompatibles con `once`.
- No implementar reintentos automáticos nuevos. Una ejecución fallida de `once` conserva su resultado y no vuelve a programarse por sí sola.

La creación mantiene el rollback existente: si falla la configuración de la regla o del scheduler, no debe quedar una tarea parcialmente creada.

### Destino de entrega

En este issue, los recordatorios y respuestas de tareas agénticas se entregan por **IM con `chat_id`**. El canal local/web no se admite como destino programado.

- Se puede reutilizar el canal y `chat_id` de la conversación que origina la solicitud cuando estén disponibles y sean válidos.
- Si no existe un destino IM válido, rechazar la creación de tareas que requieren entrega, con un error que indique configurar un canal IM y proporcionar `chat_id`.
- `run_script` puede ejecutarse sin destino IM si su contrato existente no requiere entregar mensajes. Esto debe quedar explícito en el skill.
- Si el canal deja de estar disponible luego de crear la tarea, registrar la falla de entrega y conservar el funcionamiento de las demás tareas.

`wake_agent` debe conservar el contexto de destino hasta la respuesta del agente principal. Aceptar el evento en el router no equivale a completar la tarea ni a entregar su respuesta.

### Borrado

Agregar y exponer `remove_scheduled_task`, siguiendo el patrón existente de scripts del skill.

La operación recibe el identificador de una tarea creada por `scheduled_task` y:

1. Identifica la entrada del scheduler y su regla asociada.
2. Conserva la información necesaria para restaurarlas.
3. Impide nuevos disparos durante la eliminación mediante los mecanismos existentes.
4. Elimina la regla exclusiva de la tarea, si existe, y la entrada del scheduler.
5. Si una operación falla, restaura el estado previo, incluida su condición de activada/desactivada.

No eliminar la regla compartida `im_any_message_agent` ni reglas ajenas al skill. La asociación entre tarea y regla debe derivarse del contrato existente o de metadatos mínimos retrocompatibles; no depender de coincidencias amplias de nombres.

El borrado debe ser idempotente: si ambos recursos ya están ausentes, devolver éxito indicando que la tarea no existe. Si solo queda uno, limpiar el recurso residual identificado inequívocamente.

Si también falla el rollback, devolver un error explícito con los recursos pendientes de recuperación; no informar éxito.

Una ejecución ya iniciada puede terminar después del borrado. Borrar no implica cancelar el trabajo activo ni eliminar su historial.

### Listado y activación

Exponer desde el skill una operación de listado, reutilizando las APIs existentes. Cada tarea debe mostrar, cuando corresponda:

- Identificador, nombre, modo e instrucción u objetivo.
- Activada/desactivada y estado de finalización.
- Tipo de programación y frecuencia o instante.
- Último disparo y próxima ejecución.
- Estado y resultado resumido de la última ejecución.
- Destino IM y estado de su regla asociada.

Distinguir datos inexistentes de valores válidos: una tarea nueva no tiene última ejecución; una tarea `once` consumida no tiene próxima ejecución.

Los resultados proceden del historial, sin duplicar un registro de ejecuciones en el estado del scheduler. El listado debe señalar recursos asociados ausentes o inconsistentes, sin ocultarlos ni recrearlos automáticamente.

Documentar cómo activar y desactivar individualmente usando pause/resume existentes. Desactivar impide nuevos disparos; no cancela una ejecución en curso.

## Validaciones y errores

Validar antes de persistir o crear reglas. Reutilizar validadores y límites del firmware cuando existan.

| Condición | Comportamiento esperado |
|---|---|
| Cron inválido | Rechazar e indicar el formato admitido por el parser real. |
| Identificador duplicado | Rechazar sin sobrescribir tarea ni regla. |
| Intervalo inferior al mínimo | Rechazar e informar el mínimo configurado. |
| `once` en el pasado o igual al instante actual | Rechazar e indicar que requiere una hora futura. |
| `at` inválido o fuera de rango | Rechazar sin conversiones silenciosas ni overflow. |
| Reloj sin sincronizar | Rechazar `once` y cron con un error accionable. |
| Destino IM requerido y ausente | Rechazar e indicar canal y `chat_id` necesarios. |
| Fallo de persistencia o router | Informar la causa y el resultado del rollback. |

Los intervalos deben conservar su semántica actual si funcionan sin reloj civil sincronizado; verificarlo en el código. El mínimo debe provenir de la configuración existente o de una constante/Kconfig documentada, sin números mágicos.

El reloj válido se determina mediante la fuente de sincronización existente, no mediante un umbral de fecha arbitrario.

## Historial de ejecución

Esta parte depende de #4. No crear un log paralelo ni implementar una segunda capacidad de almacenamiento.

Cada disparo debe generar actividad con `origin=scheduled_task`, incluyendo los disparos manuales mediante `trigger_now`. El registro debe permitir correlacionar inicio y resultado de una misma ejecución.

Datos mínimos, adaptados al contrato real de `cap_activity_log`:

- Identificador de tarea y correlación de ejecución.
- Modo y origen automático/manual.
- Fecha de disparo, inicio y finalización, cuando estén disponibles.
- Estado: pendiente/en ejecución, completada o fallida.
- Resultado resumido o error.

La finalización se determina según el modo:

| Modo | Resultado terminal |
|---|---|
| `send_message` | Resultado de la operación de envío; no implica confirmación de lectura. |
| `run_script` | Finalización o error del script. |
| `wake_agent` | Finalización o error del trabajo del agente principal, incluida la entrega cuando corresponda. |

Usar eventos y hooks existentes para observar la finalización. No marcar una ejecución como completada por haber encolado su evento. Si el contrato disponible no permite correlacionar el resultado, esa integración debe resolverse antes de aceptar la parte de historial.

Una falla de ejecución, entrega o escritura del log no debe detener el scheduler ni bloquear otras tareas. No registrar como éxito una operación fallida.

El historial conserva su política acotada de retención de #4. El borrado de una tarea no borra sus ejecuciones retenidas. Las ejecuciones interrumpidas por reinicio no deben aparecer como completadas.

## Restricciones técnicas

Antes de implementar, leer:

- `CLAUDE.md`.
- `.agents/design.md`.
- `.agents/gotchas.md`.
- El skill y script actuales de `scheduled_task`.
- Los headers reales `cap_scheduler.h`, `claw_memory.h`, `claw_event.h` y `claw_paths`.
- Los contratos y hooks de `cap_activity_log` una vez disponible #4.
- Los patrones de tests existentes.

Implementar en skills, capabilities, router y hooks existentes. Si se requiere registrar una capability, utilizar `components/common/app_claw/app_capabilities.c`. No modificar el loop de `claw_core`.

Para código C:

- Handles opacos, configuración por struct, ciclo de vida `create/delete/start/stop` según corresponda y retornos `esp_err_t`.
- Liberación de recursos y protección con mutex del estado compartido.
- Constantes o Kconfig para límites y valores configurables.
- Archivos menores a 1500 líneas.

Para persistencia:

- Resolver toda ruta escribible con `claw_paths_join(CLAW_PATH_DATA, ...)`.
- Mantener compatibilidad con los datos existentes sin migración destructiva.
- Cualquier dato nuevo debe tener tamaño acotado.
- Escrituras atómicas mediante temporal + rename.
- Archivos ausentes o corruptos deben permitir arrancar vacío, sin crash.
- Agrupar escrituras y evitar `fsync` por entrada, respetando el desgaste de flash y los aproximadamente 3 MB de DATA.

No usar variables de entorno para configuración ni asumir que `idf.py flash` actualiza la partición DATA.

## Criterios de aceptación

- [ ] Se crea una tarea `once` desde hora local y desde epoch en milisegundos, con `start_at_ms` correcto y una sola ejecución.
- [ ] `cron` e `interval` mantienen su comportamiento y persistencia existentes.
- [ ] El agente principal ejecuta las tareas `wake_agent`.
- [ ] Se pueden listar, activar y desactivar tareas individualmente.
- [ ] El listado distingue última ejecución, próximo disparo y resultado final.
- [ ] El borrado elimina scheduler y regla exclusiva, preservando reglas compartidas y ajenas.
- [ ] Creación y borrado restauran el estado anterior ante fallas parciales recuperables.
- [ ] Las validaciones producen errores claros y no dejan recursos parciales.
- [ ] Cada disparo tiene historial con `origin=scheduled_task` y resultado correlacionado, integrado con #4.
- [ ] Una tarea fallida no bloquea las demás.
- [ ] `SKILL.md` documenta `once`, borrado, listado, pause/resume, formatos de tiempo, zona horaria y destino IM.
- [ ] Se conserva compatibilidad con tareas persistidas antes del cambio.

El issue no se considera completo hasta integrar y verificar la dependencia de historial de #4.

## Verificación

Agregar tests siguiendo los patrones existentes en `components/**/test_apps/` y tests Lua junto al módulo, donde estén disponibles.

Cubrir:

- Conversión de `at`, límites, pasado, reloj no sincronizado y horarios locales ambiguos.
- Cron inválido, identificador duplicado e intervalo demasiado corto.
- Creación y borrado con fallas parciales, rollback y preservación de la regla compartida.
- Listado de tareas nuevas, pausadas, finalizadas e inconsistentes.
- Correlación entre disparo y resultado en los tres modos.
- Compatibilidad con datos anteriores y recuperación ante estado ausente o corrupto.

En NightShift no hay toolchain ESP-IDF: no declarar compilación ni tests exitosos sin haberlos ejecutado. La descripción del PR debe incluir el estado real de verificación y estos comandos para un entorno con ESP-IDF:

`idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`

### Pruebas manuales en placa

1. Crear un recordatorio `once` futuro con reloj sincronizado; verificar envío IM, historial y ausencia de segundo disparo.
2. Crear una tarea cron y una de intervalo; verificar ejecución automática por el agente principal.
3. Pausar y reanudar una tarea; verificar el próximo disparo informado.
4. Reiniciar antes del vencimiento de una tarea `once` y verificar persistencia; reiniciar después de ejecutarla y comprobar que no se repite.
5. Borrar tareas de cada modo; comprobar eliminación de recursos exclusivos y conservación de `im_any_message_agent`.
6. Probar destino IM ausente al crear y canal no disponible al ejecutar.
7. Provocar falla de script o agente y verificar resultado fallido mientras otra tarea continúa.
8. Verificar `trigger_now` y su correlación en historial.
9. Arrancar con datos anteriores, ausentes y corruptos; comprobar recuperación sin crash.
10. Superar el límite de retención del log y verificar la política acotada de #4.