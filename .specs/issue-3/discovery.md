# Issue #3 — Fase 0: descubrimiento del checkout

Fecha: 2026-10-03. Branch verificada: `nightshift/issue-3`.

## Stack confirmado (T0.1)

El runtime de la aplicación es firmware C con ESP-IDF/FreeRTOS en ESP32;
Lua se usa para scripts y módulos. No es una aplicación Node ni Python.
TypeScript/pnpm pertenecen a la UI, documentación y simulador, y Python a
herramientas de build. Las dependencias de firmware usan CMake e IDF Component
Manager (`application/edge_agent/main/idf_component.yml`).

La CI de firmware usa ESP-IDF v5.5 (`.gitlab/ci/build.yml`). Hay test apps
ESP-IDF bajo `components/claw_modules/*/test_apps/` y tests Lua junto a módulos.
No se detectó un comando único de tests para este issue. El build documentado es:

```sh
. "$IDF_PATH/export.sh"
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1
idf.py build
```

En este entorno `IDF_PATH` no está definido e `idf.py` no está en PATH.
No se ejecutó un build ni se verificó el target físicamente.

## Entrypoints y límites (T0.2, T0.4)

- `application/edge_agent/main/main.c`: configura los roots de almacenamiento
  y llama a `app_claw_start()`.
- `components/common/app_claw/app_claw.c`: `build_storage_paths()` resuelve las
  rutas; `app_claw_start()` inicializa router y scheduler antes de arrancarlos.
  Un store futuro debería abrirse y recuperar interrupciones después de resolver
  rutas y antes de inicializar los consumidores.
- `components/claw_capabilities/cap_scheduler/include/cap_scheduler.h`: contrato
  real de tareas y configuración. No hay callbacks de negocio con `ctx`.
- `components/claw_capabilities/cap_scheduler/src/cap_scheduler.c`:
  `cap_scheduler_publish_entry_locked()` publica un evento usando `publish_event`.
  `ESP_OK` representa aceptación de la publicación, no fin del trabajo del agente.
  No debe marcarse un recurso de negocio como procesado en ese punto.
- `components/claw_capabilities/cap_scheduler/src/cap_scheduler_store.c`:
  las definiciones se guardan en JSON y el estado de scheduler en blobs NVS
  con `nvs_commit()`. Ya hay persistencia de contadores y timestamps, pero no
  el historial/deduplicación solicitado.
- `components/claw_modules/claw_event_router/`: entrega asíncrona de eventos.
  Se necesita decidir dónde observar el resultado final antes de integrar tracking.

No se encontró Autonomous Pulse en el código de ejecución. Las coincidencias
de pulse/heartbeat corresponden a hardware, UI o conexiones IM. No hay un
entrypoint que se pueda envolver sin definir o agregar comportamiento nuevo.
T0.2 permanece bloqueada. Tampoco se encontró una API de shutdown de app_claw;
debe definirse ownership y cierre antes de implementar T4.3.

## Configuración y rutas (T0.3)

La configuración real vive en `application/edge_agent/components/app_config/`
y usa `settings_store` (NVS), no variables de entorno de un proceso host.
DATA se resuelve mediante `claw_paths` y puede ser flash o SD; SYSTEM es read-only.
El scheduler usa `claw_paths_join(CLAW_PATH_DATA, "scheduler/schedules.json", ...)`.

Propuesta pendiente: componente C `components/claw_modules/claw_state/`, con
handle opaco e inyección vía configuración de consumidores. Resolver el archivo
mediante `claw_paths_join(CLAW_PATH_DATA, "state/krakbot-state.db", ...)` si se
aprueba SQLite, o extensión `.json` si se aprueba el fallback. Las variables
`KRAKBOT_STATE_*` del plan aún no se leen en ningún lugar: hace falta aprobar
su equivalencia en settings/Kconfig. No se agregó configuración sin efecto.

## Decisiones bloqueantes — NEEDS CLARIFICATION

1. T0.5: no hay SQLite ni binding de firmware detectado. `better-sqlite3` y
   `sqlite3` de Python no corren en este runtime. Hace falta aprobar una dependencia
   SQLite compatible con ESP-IDF/FATFS y validarla en CI/target, o rechazar SQLite
   y autorizar el fallback JSON adaptado a C. No se asumió una aprobación.
2. T0.2/T0.8: identificar qué flujo representa Autonomous Pulse o autorizar su
   creación; definir cómo observar el cierre real del trabajo iniciado por scheduler.
3. T0.3/T0.4: aprobar configuración de firmware y ownership/lifecycle del store.
4. T0.7: `commitProcessed` no aprobado; no implementar la desviación opt-in.

Los defaults explícitos de T0.6 se mantienen como propuesta: retención apagada,
deduplicación indefinida, una instancia por archivo, resumen JSON de hasta 16 KB.
Todavía no son comportamiento del firmware.

## Estado del trabajo

La fase 0 produjo evidencia y propuestas locales. No se iniciaron las fases 1–6
porque el usuario las condicionó a completar T0.1–T0.5. No se publicaron comentarios,
se cerró el issue ni se crearon PRs; esos pasos externos no se presentan como hechos.
No se hizo commit ni push.
