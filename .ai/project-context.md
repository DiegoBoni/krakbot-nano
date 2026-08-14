# Project Context

## Metadata

- Project: KrakBot Nano (fork of ESP-Claw; frontend package `krakbot-nano-frontend`, docs package `krakbot-nano-doc`, simulator package `@krakbot-nano/simulator-pages`)
- Version: 0.1.0 (per firmware/frontend package metadata; see CHANGELOG.md for release history)
- Last updated: 2026-08-12
- Owner: Espressif (esp-claw org, upstream) — see LICENSE (Apache-2.0); this repo is a KrakBot Nano fork (N16R8 flash config, pioarduino build) per commit `524bb58`

## Stack

- Languages: C (ESP-IDF firmware), Lua (dynamically-loaded device behavior/modules), TypeScript (embedded settings UI + docs site), Python (CI build/packaging scripts)
- Frameworks: ESP-IDF (firmware), SolidJS + Tailwind CSS v4 (device settings UI), Astro + Starlight (docs site)
- Runtime: Espressif ESP32-series chips (dual FS roots: read-only SYSTEM, writable DATA/SD)
- Package managers: pnpm (frontend UI, docs), idf.py/CMake (firmware components), pip (CI scripts, implicit)

## Architecture

- System type: embedded monolith firmware with a modular component architecture (agent runtime + capability/plugin registry), plus two decoupled web frontends (device local-config UI, public docs site)
- Main modules: `claw_core` (agent loop: context, LLM calls, tool execution), `claw_event_router` (declarative event routing), `claw_cap`/`claw_capabilities/*` (callable capability registry), `claw_memory` (session/profile/long-term memory), `claw_skill` (skill docs/activation), `lua_modules/*` (hardware drivers + higher-level Lua APIs), board manager (`application/edge_agent/boards/`)
- Key data flow: IM channels/scheduler/Lua/CLI → `claw_event_router` (matches `router_rules/router_rules.json`) → capability call / script run / agent run / message send / event emit → `claw_core` builds context (memory, session, skills) → LLM backend → tool-call loop → persist context → response delivered via IM bindings or local/web channels

## Engineering Standards

- Code style: ESP-IDF-style C-style OOP (opaque `xxx_handle_t` handles, `xxx_create/delete/start/stop/...` API shape, `esp_err_t` returns, `xxx_ops_t` for polymorphism) — see CLAUDE.md "Code Style"; enforced partly via astyle (`.gitlab/ci/astyle-rules.yml`) in pre-commit
- Naming conventions: snake_case C APIs; components/capabilities prefixed by domain (`cap_*`, `claw_*`, `lua_module_*`, `lua_driver_*`)
- Branch/commit conventions: Conventional Commits enforced by `conventional-precommit-linter` pre-commit hook (subject min length 15); branch names must not contain more than one `/` or uppercase letters (pre-commit `no-commit-to-branch` hooks)
- PR and review rules: GitLab is the primary CI (`.gitlab-ci.yml` + `.gitlab/ci/*`, stages: pre-check → build → deploy); GitHub Actions has a lighter `pr_approved.yml` workflow. Copyright header check enforced via `check-copyright` pre-commit hook

## Agent Instructions

- Do: follow CLAUDE.md/AGENTS.md conventions exactly (opaque handles, DATA/SYSTEM path rules via `claw_paths`/`storage` module, keep `claw_core` narrow); run `idf.py build` for firmware changes and `pnpm build`/`pnpm typecheck` for frontend changes
- Avoid: hard-coding `/fatfs`; adding board-specific assumptions to generic Lua modules; growing `claw_core` with logic that belongs in a capability/skill/router rule
- Definition of done: builds pass for the affected board config; relevant component/Lua test apps updated; frontend typecheck/build pass when UI touched; docs updated if user-facing behavior changed

---

<!-- DOMAIN CONTEXTS
     Each section below is loaded independently by its matching domain skill.
     Skills read only their own [context.<domain>] block — not the full file.
     Run `cmd.context` to populate all sections from repository evidence.
-->

## [context.backend]

- Language + runtime: C on ESP-IDF (FreeRTOS-based), running on ESP32-series chips
- Framework: ESP-IDF component model; custom agent-runtime layer (`claw_core`, `claw_cap`, `claw_event_router`)
- Entry points: `application/edge_agent/main/main.c` (boot flow, storage paths, capability/Lua module registration, CLI, agent startup); secondary app `application/mcp_server_point/` (separate main/boards/components, MCP server device variant)
- Key services / business logic modules: `components/claw_modules/claw_core` (agent loop), `claw_event_router` (routing), `claw_memory` (session/profile/long-term memory + request gating), `claw_skill` (skill registry), `claw_manager`; capabilities under `components/claw_capabilities/*` (im_platform, mcp_client/mcp_server, scheduler, skill_mgr, lua exec, cli, web_search, agent_mgr, session_mgr, llm_config/llm_inspect, boards, system, http_request, im_local, router_mgr, files)
- Auth strategy: no session/user auth on the device-local HTTP config server itself; `http_server_config_api.c` persists per-integration credentials (e.g. `llm_auth_type`, Telegram/WeChat bot tokens) as config fields rather than gating server access — IM platform and LLM backend bindings handle their own upstream credentials (see `cap_im_platform`, `cap_llm_config`)
- External APIs consumed: LLM backend(s) via `claw_core` runtime (Anthropic, OpenAI-compatible, and custom backends under `claw_core/src/llm/backends/`, configurable via `cap_llm_config`), IM platforms via `cap_im_platform`, MCP servers via `cap_mcp_client`, generic HTTP via `cap_http_request`, web search via `cap_web_search`
- Error handling conventions: `esp_err_t` return codes per CLAUDE.md Code Style; check return values, handle allocation failures, clean up partially-initialized resources (CLAUDE.md "General Engineering Rules")
- Logging approach: standard ESP-IDF logging (`esp_log.h`), used throughout `claw_core` and other components (e.g. `claw_core_utils.c`, `claw_core_context_persist.c`, `claw_core_control.c`)
- Performance constraints: avoid local stack variables >128 bytes; use pre-allocated buffers/pools/ring buffers in high-frequency paths (CLAUDE.md "Memory Allocation and Release"); keep `claw_core` loop narrow per `.agents/design.md`

## [context.frontend]

- Framework + version: SolidJS ^1.9.14 (device settings UI, `application/edge_agent/components/http_server/frontend_source/`); Astro ^7 + Starlight ^0.41 (docs site, `docs/`)
- State management: SolidJS reactive primitives (`createSignal`/`createStore`), confirmed in 23 files under `frontend_source/src`
- Routing: NEEDS CLARIFICATION — not inspected this scan (Astro handles docs routing via file-based pages; settings UI likely single-page)
- Design system / component library: `@kobalte/core` (Solid UI primitives), `lucide-solid` (icons), Tailwind CSS v4 for styling
- API communication layer: dedicated client module `frontend_source/src/api/client.ts` wrapping calls to the device-local HTTP config server (`components/http_server/`)
- Styling conventions: Tailwind CSS v4 via `@tailwindcss/vite`
- i18n / accessibility requirements: `@solid-primitives/i18n` present — i18n supported in settings UI
- Build tooling: Vite ^8 with `vite-plugin-solid`, `vite-plugin-singlefile` (single-file embedded build), `vite-plugin-compression2`; `pnpm build` / `pnpm typecheck` are the required gates per CLAUDE.md

## [context.data]

- Databases (type + name): none (no SQL/NoSQL DB detected) — persistence is filesystem-based (FATFS) on-device
- ORM / query layer: N/A — data is stored as files (JSON, skill docs, router rules) under two logical roots (`CLAW_PATH_SYSTEM` read-only, `CLAW_PATH_DATA` writable), resolved via `claw_paths` (C) / `storage` module (Lua)
- Migration strategy: N/A — recovery defaults under `/system/.recovery` are copied into DATA only when missing (acts as seed/migration mechanism)
- Key models / entities: router rules (`router_rules/router_rules.json`), scheduler rules, memory/session/profile records (`claw_memory`), skill documents (`SKILL.md` + assets), inbox/user files
- Caching layer: NEEDS CLARIFICATION — none detected in this scan
- Data validation approach: NEEDS CLARIFICATION — not inspected (likely JSON schema or manual validation in capability handlers)
- Backup / retention policy: recovery seed files under `/system/.recovery`, copied to DATA root on first boot / when missing

## [context.testing]

- Unit test framework: ESP-IDF component test apps (`idf.py` based) under `components/claw_modules/*/test_apps/` (e.g. `claw_event_router`, `claw_core`, `claw_manager`) and `components/lua_modules/{lua_module_audio,lua_module_thread}/test_apps/`
- Integration test approach: Lua module tests live beside modules under `components/lua_modules/<module>/test/*.lua` (descriptive names, e.g. `json_roundtrip.lua`) — run against the Lua runtime on-device or in simulator
- E2E tooling: NEEDS CLARIFICATION — a device simulator exists (`pages/simulator/`, `tools/lua_lvgl_web_sim/`) but its role in E2E testing wasn't confirmed this scan
- Coverage targets: NEEDS CLARIFICATION — none found
- Test data strategy: NEEDS CLARIFICATION — not inspected
- CI gate (pass/fail criteria): GitLab CI `pre-check` → `build` → `deploy` stages (`.gitlab/ci/pre-check.yml`, `build.yml`); required local gate per CLAUDE.md is `idf.py build` for the affected board config, plus `pnpm build`/`pnpm typecheck` for frontend changes

## [context.devops]

- Cloud provider: N/A (embedded firmware project); docs site likely deployed via GitLab Pages/CI (`.gitlab/ci/build-doc.yml`, `deploy.yml`) — exact host NEEDS CLARIFICATION
- Deployment method: firmware — build/package/upload via CI scripts (`.gitlab/ci/build_apps.py`, `merge_bin.py`, `package_firmware.py`, `upload_firmware.py`); end users flash via "Online Flashing" web flow (per README) or `idf.py flash`
- CI/CD platform: GitLab CI is primary (`.gitlab-ci.yml`, includes `rules.yml`, `pre-check.yml`, `build.yml`, `build-doc.yml`, `simulator.yml`, `deploy.yml`); GitHub Actions used for a lightweight `pr_approved.yml` workflow only
- Environment names (dev / staging / prod): N/A — no explicit environment tiers detected; board configs (via board manager, e.g. `esp32_S3_DevKitC_1`) act as the build-target axis instead
- Secrets management: NEEDS CLARIFICATION — not inspected (likely GitLab CI/CD variables for firmware signing/upload credentials)
- Monitoring / alerting: N/A — no monitoring stack detected (embedded device project; on-device logging only)
- Rollback procedure: NEEDS CLARIFICATION — not inspected; recovery seed mechanism (`/system/.recovery`) provides data-layer defaults but firmware rollback process itself wasn't confirmed
