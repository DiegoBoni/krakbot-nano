# Tasks: tareas programadas de punta a punta

**Issue:** #2 — `DiegoBoni/krakbot-nano`  
**Dependencia obligatoria:** #4 — `cap_activity_log`

## Alcance

Extender `scheduled_task` para crear tareas persistentes de una sola ejecución, listar su estado, activarlas/desactivarlas y borrarlas coherentemente, con entrega IM e historial correlacionado.

Conservar cron, intervalos, los tres modos existentes, JSON + NVS, contadores, `max_runs`, `trigger_now` y pause/resume. El scheduler decide cuándo disparar y el agente principal ejecuta las instrucciones de `wake_agent`.

No reconstruir el scheduler, modificar el loop de `claw_core`, crear agentes independientes, desarrollar UI, incorporar SQLite ni agregar dependencias externas.

## 1. Confirmar contratos y dependencia

- [ ] **T01 — Relevar instrucciones y patrones del repositorio.** Leer `CLAUDE.md`, `.agents/design.md`, `.agents/gotchas.md`, el `SKILL.md` de `scheduled_task`, `scripts/add_scheduled_task.lua` y los patrones de pruebas C y Lua existentes.
- [ ] **T02 — Documentar contratos reales antes de editar.** Revisar `cap_scheduler.h`, `claw_memory.h`, `claw_event.h`, `claw_paths` y las implementaciones del scheduler, router y los tres modos. Confirmar:
  - Creación, consulta, eliminación, pause/resume, `start_at_ms`, ejecución única, contadores y persistencia.
  - Zona horaria, sincronización, conversión local y detección de horarios ambiguos o inexistentes.
  - Parser cron, rangos, mínimo de intervalo y comportamiento sin reloj civil.
  - Asociación tarea/regla, consulta y restauración, reglas exclusivas y uso compartido de `im_any_message_agent`.
  - Selección y disponibilidad de canal IM, validación de `chat_id` y resultado observable del envío.
  - Hooks de inicio/finalización de scripts y del agente principal, propagación de contexto y correlación.
- [ ] **T03 — Confirmar el contrato de #4.** Documentar escritura, consulta, correlación, retención y tratamiento de ejecuciones interrumpidas por reinicio en `cap_activity_log`. Identificar hooks faltantes y resolverlos en capabilities, router o hooks existentes antes de aceptar la integración.

**Criterio de aceptación:** los contratos, archivos y APIs necesarios están identificados en el código; no se presupone la existencia de APIs adicionales.

## 2. Definir asociación y contexto de ejecución

**Depende de:** T02 y T03.

- [ ] **T04 — Definir la asociación retrocompatible entre tarea y recursos.** Reutilizar el contrato existente; agregar metadatos mínimos y acotados únicamente si no permite identificar y restaurar recursos inequívocamente.
- [ ] **T05 — Distinguir recursos y datos operativos.** Identificar entrada del scheduler, regla exclusiva, regla compartida `im_any_message_agent`, modo, instrucción y destino necesarios para operar y listar.
- [ ] **T06 — Resolver tareas anteriores con seguridad.** Mantener su funcionamiento; mostrar asociaciones indeterminadas como inconsistencias y evitar modificar reglas ajenas. No inferir propiedad por coincidencias amplias de nombres.
- [ ] **T07 — Definir contexto independiente de la tarea almacenada.** Conservar correlación y destino de una ejecución iniciada para que pueda terminar y entregar su resultado después del borrado.

**Criterio de aceptación:** la propiedad de recursos es inequívoca y el trabajo activo no depende de que la tarea siga almacenada.

## 3. Extender creación y validaciones

**Depende de:** T02 y T04–T07.

- [ ] **T08 — Incorporar `kind=once` en `scripts/add_scheduled_task.lua`.** Exigir `at`, convertirlo a `start_at_ms` y configurar una única ejecución mediante los mecanismos existentes, conservando parámetros y comportamientos actuales.
- [ ] **T09 — Validar tiempo sin conversiones silenciosas.**
  - Interpretar números exclusivamente como epoch en milisegundos.
  - Admitir fecha local completa en formato estricto `YYYY-MM-DDTHH:mm:ss`, sujeto al soporte confirmado del firmware.
  - Usar la zona horaria del firmware y rechazar horarios ambiguos o inexistentes.
  - Validar formato, rango, precisión representable y overflow.
  - Exigir reloj sincronizado e instante estrictamente futuro.
- [ ] **T10 — Validar compatibilidad de parámetros y frecuencia.** Rechazar `at` en otros tipos y parámetros de frecuencia incompatibles con `once`. Reutilizar validadores reales de cron, identificadores, intervalos y límites; exigir sincronización para cron y `once`, preservando la semántica de intervalos sin reloj civil. Obtener el mínimo de intervalo de configuración, constante o Kconfig documentada.
- [ ] **T11 — Validar destino antes de crear recursos.** Para `send_message` y `wake_agent`, comprobar canal IM y `chat_id`; reutilizar el destino de conversación solo si está disponible y es válido. Rechazar local/web y emitir un error que indique configurar IM y proporcionar `chat_id`. Conservar la posibilidad de `run_script` sin IM cuando no requiera entrega.
- [ ] **T12 — Prevenir duplicados y extender rollback de creación.** Comprobar duplicados en scheduler y recursos de router sin sobrescribirlos. Incluir los nuevos datos en la recuperación; informar causa, resultado y recursos pendientes si también falla el rollback.
- [ ] **T13 — Mantener consumo y comportamiento existente.** Consumir la ejecución `once` aunque falle, sin reintentos automáticos nuevos. Preservar contadores, `max_runs`, `trigger_now` y pause/resume.

**Criterio de aceptación:** las validaciones preceden a la creación de recursos y toda falla parcial se recupera o se informa explícitamente.

## 4. Implementar borrado coherente

**Depende de:** T04–T07 y T12.

- [ ] **T14 — Agregar y exponer `remove_scheduled_task`.** Seguir el patrón de scripts del skill y localizar tarea y regla mediante la asociación verificada.
- [ ] **T15 — Implementar eliminación protegida y recuperable.** Guardar los datos de restauración, incluida la activación original; impedir nuevos disparos y proteger frente a cambios concurrentes; eliminar regla exclusiva y entrada del scheduler. Ante falla, restaurar el estado previo e informar el resultado.
- [ ] **T16 — Implementar idempotencia y limpieza de residuales.** Devolver éxito indicando ausencia si ambos recursos no existen; limpiar recursos residuales inequívocamente identificados. Si falla el rollback, devolver error con los recursos pendientes.
- [ ] **T17 — Preservar recursos compartidos y ejecuciones activas.** Nunca eliminar `im_any_message_agent` ni reglas ajenas; permitir finalizar trabajo iniciado con su contexto de entrega y correlación. No cancelar trabajo activo ni borrar historial.

**Criterio de aceptación:** el borrado restaura la activación original ante fallas y no afecta recursos ajenos ni resultados retenidos.

## 5. Exponer listado y activación individual

**Depende de:** T04–T06 y contrato de consulta de T03.

- [ ] **T18 — Agregar listado mediante APIs existentes.** Mostrar identificador, nombre, modo, instrucción, programación, activación, finalización, último disparo, próxima ejecución, destino y estado de recursos asociados.
- [ ] **T19 — Consultar la última ejecución en `cap_activity_log`.** Mostrar estado y resultado resumido sin almacenar un segundo historial en el scheduler.
- [ ] **T20 — Representar ausencias e inconsistencias.** Distinguir pausa de programación consumida; mostrar ausencia de última ejecución para tareas nuevas, de próxima ejecución para `once` consumidas y de resultados eliminados por retención. Señalar recursos faltantes o inconsistentes sin ocultarlos ni recrearlos.
- [ ] **T21 — Exponer pause/resume individual.** Reutilizar los mecanismos existentes; desactivar debe impedir nuevos disparos y permitir finalizar una ejecución activa.

**Criterio de aceptación:** el listado refleja datos reales y diferencia pausa, consumo, ausencia de historial e inconsistencia de recursos.

## 6. Integrar historial y resultados terminales

**Depende de:** T03 y T07. **Obligatorio para cerrar #2.**

- [ ] **T22 — Registrar cada disparo automático y manual.** Usar `origin=scheduled_task`, identificador de tarea y correlación de ejecución. Adaptar al contrato real modo, origen automático/manual, fechas de disparo/inicio/finalización, estado y resumen o error.
- [ ] **T23 — Propagar contexto hasta la finalización real.** Extender contexto de capabilities, router o hooks existentes cuando sea necesario, sin modificar el loop de `claw_core`. Aceptar o encolar un evento no debe completar la ejecución.
- [ ] **T24 — Correlacionar los tres modos.**
  - `send_message`: finalizar con el resultado del envío, sin asumir lectura.
  - `run_script`: finalizar con la terminación o error del script.
  - `wake_agent`: finalizar con la terminación o error del agente principal, incluida la entrega requerida.
- [ ] **T25 — Aislar fallas y respetar retención.** Una falla de ejecución, entrega o escritura del log no debe detener el scheduler ni bloquear otras tareas. No registrar éxito ante fallas; registrar entrega fallida si desaparece el canal. Conservar historial retenido tras borrar tareas y ejecuciones interrumpidas como no completadas según #4.

**Criterio de aceptación:** cada disparo tiene correlación hasta su resultado terminal observable y las fallas no bloquean otras tareas.

## 7. Asegurar persistencia y restricciones técnicas

**Aplica a:** T04–T25.

- [ ] **T26 — Mantener persistencia retrocompatible.** Conservar JSON + NVS y datos anteriores sin migraciones destructivas; permitir arranque vacío ante archivos ausentes o corruptos sin crash.
- [ ] **T27 — Implementar escrituras y límites adecuados.** Resolver rutas con `claw_paths_join(CLAW_PATH_DATA, ...)`; usar temporal + rename, agrupar escrituras y evitar `fsync` por entrada. Acotar metadatos y resultados, respetar retención, desgaste de flash y aproximadamente 3 MB de DATA.
- [ ] **T28 — Revisar contratos C y concurrencia.** Usar handles opacos, configuración por struct, ciclo de vida existente y retornos `esp_err_t`; liberar recursos y proteger estado mediante mutex sin mantener bloqueos durante trabajo prolongado.
- [ ] **T29 — Verificar integración y configuración.** Usar constantes o Kconfig para límites, mantener archivos menores a 1500 líneas y registrar capabilities en `components/common/app_claw/app_capabilities.c` si corresponde. No configurar firmware mediante variables de entorno ni asumir que `idf.py flash` actualiza DATA.

## 8. Actualizar documentación del skill

**Depende de:** implementación y contratos confirmados.

- [ ] **T30 — Documentar creación y tiempo.** Incluir parámetros y ejemplos de `once`, cron e intervalos; formato de `at`, epoch en milisegundos, zona horaria del firmware, sincronización y errores de fechas ambiguas, inexistentes o pasadas.
- [ ] **T31 — Documentar resolución de intención y destino.** Indicar que el skill resuelve expresiones relativas antes de invocar el script y solicita aclaración cuando falte fecha, zona horaria o destino necesario. Explicar requisitos IM, reutilización del contexto y condición sin destino de `run_script`.
- [ ] **T32 — Documentar operaciones y resultados.** Cubrir borrado, idempotencia, rollback, continuidad del trabajo activo, listado, datos ausentes, inconsistencias, pause/resume, historial y ausencia de reintentos nuevos.

## 9. Agregar y ejecutar verificación automatizada

**Depende de:** implementación correspondiente. Seguir patrones de `components/**/test_apps/` y pruebas Lua junto al módulo cuando estén disponibles.

- [ ] **T33 — Probar tiempo.** Fecha local y epoch equivalentes; límites, overflow, precisión, pasado e igualdad con el presente; reloj no sincronizado y horarios ambiguos/inexistentes.
- [ ] **T34 — Probar validaciones.** Cron inválido, duplicados de scheduler y router, intervalo inferior al mínimo, parámetros incompatibles y destinos inválidos o ausentes.
- [ ] **T35 — Probar creación con inyección de fallas.** Fallas de router y persistencia; rollback recuperable y fallido; ausencia de éxito falso y detalle de recursos pendientes.
- [ ] **T36 — Probar borrado con inyección de fallas.** Tareas activas/pausadas, recursos ausentes/residuales, fallas parciales, restauración de activación original y conservación de reglas compartidas/ajenas. Verificar continuidad del trabajo activo.
- [ ] **T37 — Probar listado y pause/resume.** Tareas nuevas, pausadas, consumidas e inconsistentes; resultados ausentes por retención; ausencia explícita de próxima ejecución y último resultado.
- [ ] **T38 — Probar ejecución e historial.** Correlación automática y manual en los tres modos; finalización terminal real, entrega fallida, falla de escritura del log y continuidad de otra tarea.
- [ ] **T39 — Probar persistencia y reinicios.** Datos anteriores, ausentes y corruptos; reinicio antes y después de consumir `once`; ejecución interrumpida sin éxito falso; historial conservado tras borrar.
- [ ] **T40 — Registrar resultados reales.** Ejecutar los checks disponibles y documentar resultados y pendientes. Verificar que ninguna ruta deje recursos parciales recuperables sin informar y que cada rollback restaure la activación original.

## 10. Verificar en ESP-IDF y placa

**Depende de:** T26–T40 y entorno con ESP-IDF.

- [ ] **T41 — Compilar para la placa objetivo.** Ejecutar `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build` y registrar el resultado real.
- [ ] **T42 — Verificar `once` de punta a punta.** Crear una tarea futura con reloj sincronizado; comprobar entrega IM, historial y ausencia de segundo disparo. Reiniciar antes y después de ejecutarla y comprobar persistencia sin repetición.
- [ ] **T43 — Verificar programación y activación.** Crear cron e intervalo en `wake_agent`; comprobar ejecución por el agente principal. Pausar/reanudar y verificar el próximo disparo informado.
- [ ] **T44 — Verificar borrado y destinos.** Borrar tareas de cada modo; comprobar eliminación exclusiva y conservación de `im_any_message_agent`. Probar destino ausente al crear y canal no disponible al ejecutar.
- [ ] **T45 — Verificar aislamiento y disparo manual.** Provocar falla de script o agente mientras otra tarea continúa; ejecutar `trigger_now` y comprobar correlación de historial.
- [ ] **T46 — Verificar recuperación y retención.** Arrancar con datos anteriores, ausentes y corruptos sin crash; superar la retención y comprobar la política acotada de #4.

**Restricción:** NightShift no dispone de toolchain ESP-IDF. Las verificaciones que no puedan ejecutarse deben quedar pendientes; no declarar compilación ni pruebas exitosas sin evidencia de ejecución.

## 11. Preparar entrega y comprobar cierre

- [ ] **T47 — Preparar descripción del PR.** Incluir cambios, compatibilidad, integración con #4, pruebas realmente ejecutadas, verificaciones pendientes y comando de compilación.
- [ ] **T48 — Comprobar condiciones de cierre.** No cerrar #2 hasta completar y verificar la integración obligatoria con #4.

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