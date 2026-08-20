# Common Gotchas

## Capability and Lua selection are config-driven

Enabled capability groups, LLM-visible groups, and enabled Lua modules come from app configuration. Empty selections usually mean "use defaults" or "enable available modules"; unknown tokens are ignored with warnings.

When adding a new capability group or Lua module, update the app registration table and the relevant Kconfig/default configuration path.

## Lua driver READMEs are for agents

`components/lua_modules/lua_driver_xxx/README.md` files are agent-facing documentation. They explain how an agent should use the Lua driver and are meant to be read as operating instructions, not as user marketing docs or full developer manuals.

Keep these READMEs concise, capability-oriented, and accurate for runtime Lua usage.

## The `storage` (DATA) partition is excluded from `idf.py flash` on purpose

`storage` is the writable DATA root (`CLAW_PATH_DATA`) — router rules, memory, sessions, MCP/Composio registry, scheduler jobs. Its FATFS image (`application/edge_agent/CMakeLists.txt`, `fatfs_create_spiflash_image(storage ...)`) deliberately omits `FLASH_IN_PROJECT`, so the default `idf.py flash` / `idf.py app-flash` targets never touch it and routine firmware updates preserve live device data.

To deliberately (re)seed DATA from the build's seed image (first-time provisioning, factory reset, or recovering from an unrecoverable corrupt file), run `idf.py storage-flash` explicitly — warn the user first, since it is destructive and unrecoverable. Do not add `FLASH_IN_PROJECT` back to the `storage` image; that reintroduces silent data loss on every full flash.
