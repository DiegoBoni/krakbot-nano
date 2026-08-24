---
{
  "name": "sensor_driver_authoring",
  "description": "Write and install a Lua driver for a new sensor/actuator the user physically wired to a free pin, tracking which pins are already claimed so a new driver never collides with an existing one. Use when the user says they added new hardware to a port and wants the device to read or control it.",
  "metadata": {
    "cap_groups": ["cap_lua", "cap_files", "cap_boards", "cap_skill"],
    "manage_mode": "readonly"
  }
}
---

# Sensor Driver Authoring

Use this skill to build a runtime skill that drives a sensor or actuator the user
just wired up, without ever picking a pin that is already in use.

This skill only covers what is specific to hardware: picking a free pin and
writing the driver logic. For everything else — `SKILL.md` frontmatter shape,
the `register_skill`/`unregister_skill` flow, and the Lua script pattern —
activate `skill_creator` and follow it; do not duplicate those rules here.

`board_hardware_info` only knows about pins the *board* occupies out of the box.
It has no idea which pins previous runs of this skill already handed out to
sensors added later. This skill adds that missing half: a small on-device ledger
of pins claimed at runtime.

## Step 1: Know what is already occupied

1. Read the `board_hardware_info` skill first. It lists every GPIO the board's
   built-in peripherals already use — never reuse one of those pins.
2. Read the runtime pin ledger with `read_file`:
   `storage.get_root_dir() .. "/hw_pins/claimed_pins.json"`
   If the file does not exist, treat the ledger as `{"claims": []}` — do not
   treat a missing file as an error.
3. The free pins are: all GPIOs valid for this chip, minus the ones from
   `board_hardware_info`, minus every `pin` (and `sda`/`scl`/`mosi`/`miso`/
   `sclk`/`cs`, whichever apply) already listed in the ledger's `claims`.
4. If more than one free pin fits, ask the user which one to use only when the
   physical wiring is ambiguous (e.g. multiple free GPIOs and no stated
   preference); otherwise just pick one and say which pin you used.
5. For I2C sensors, prefer reusing an existing I2C bus already listed in
   `board_hardware_info` (same `sda`/`scl`) instead of claiming new pins —
   most I2C sensors can share a bus at a distinct 7-bit address. Only claim
   new SDA/SCL pins if the board has no I2C bus to reuse.

## Step 2: Write the driver

You can only add a plain `.lua` script — you cannot compile a new native Lua
module (`luaopen_*`) at runtime; that requires a firmware rebuild. Build the
driver on top of the low-level hardware modules already compiled into
firmware and reachable with `require(...)`:

- `gpio` — `set_direction(pin, mode)`, `set_level(pin, level)`, `get_level(pin)`
- `i2c` — `i2c.new(port, sda, scl, freq_hz)`, `bus:device(addr)`,
  `dev:read_byte/read/write_byte/write`, `dev:close()`, `bus:close()`
- `adc`, `mcpwm`, `pcnt`, `rmt`, `touch`, `uart` — read each module's
  `README.md` under `components/lua_modules/lua_driver_<name>/` (via
  `read_file`, absolute path) before using one you have not used before;
  never guess a function signature.

Activate `skill_creator` and read its `references/write_lua.md` for the
required Lua file pattern (`arg_schema`, `run()`/`cleanup()` wrapped in
`xpcall`, `builtin_lua_modules` doc lookup) before writing the script. On top
of that pattern:
- Put the pin number(s) in one named local/constant near the top of the
  script — never scatter the literal pin number through the file.
- Expose small, purpose-named functions (e.g. `read_temperature_c()`), not a
  raw byte-shuffling script the caller has to reinterpret every call.
- Release the bus/pin in `cleanup()` (`dev:close()`, `bus:close()`,
  `gpio.set_direction(pin, "disable")`).
- Raise a clear Lua error naming the sensor and pin on I/O failure — do not
  silently return nil.

Write it with `write_file` to:
`storage.get_root_dir() .. "/skills/<sensor_id>/scripts/driver.lua"`

## Step 3: Write, test, and register the new skill

Follow `skill_creator`'s "Create A Skill" and "Register Every Created Skill"
flows exactly for `SKILL.md` frontmatter, the `{CUR_SKILL_DIR}/scripts/...`
reference, running the script with `lua_run_script` per
`skill_creator/references/run_lua.md`, and calling `register_skill` +
`activate_skill` — `manage_mode` must be `"runtime"` since this skill is
created on the device, not built into firmware. Document each function the
driver exposes and which pin(s) it uses in the new `SKILL.md` body.

Two hardware-specific additions on top of that flow:
1. Test the driver with `lua_run_script` before registering it — do not
   register a driver you have not run.
2. After `register_skill({skill_id, file: "<skill_id>/SKILL.md"})` and
   `activate_skill({skill_id})` both succeed, claim the pin(s) as below.

## Step 4: Claim the pin

Append the claim(s) to the ledger and write it back with `write_file`
(create the file/parent dir if it did not exist):

```json
{
  "claims": [
    {
      "skill_id": "<sensor_id>",
      "bus": "gpio",
      "pin": 17,
      "role": "data",
      "note": "e.g. DHT22 data line"
    }
  ]
}
```

For a bus with multiple named lines (I2C, SPI), add one claim entry per line
(`role: "sda"`, `role: "scl"`, etc.), or omit the claim entirely when reusing
an already-listed bus from `board_hardware_info` — only claim pins that are
newly occupied because of this skill.

## Step 5: Clean up on removal

If the user later asks to remove this sensor, call
`unregister_skill({skill_id: "<sensor_id>"})` and then remove every ledger
entry whose `skill_id` matches from `claimed_pins.json`, writing the file
back. Leaving stale claims in the ledger permanently blocks those pins from
ever being reused.
