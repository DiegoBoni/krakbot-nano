---
{
  "name": "light_switch",
  "description": "Turn a board GPIO light on or off. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Light Switch

Use this skill when the user asks to turn a board GPIO light on or off, or adjust its brightness.

Note: the board's onboard RGB LED strip is owned by the automatic status indicator
(boot/idle/thinking/Wi-Fi/error animations) and is not controllable through this skill.

Run exactly one script with `lua_run_script` after reading `board_hardware_info`.

You MUST pass args when using `lua_run_script` to control the light, as specified in the input schema shown below in the section `GPIO Light Script Args Schema`.

If `lua_run_script` returns an error, report that error directly to the user.
Do not retry, change arguments, or run another light script in the same turn unless the user explicitly asks.

## GPIO Light Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "io": {
      "type": "integer",
      "default": 38,
      "minimum": 0
    },
    "enabled": {
      "type": "boolean",
      "default": true
    },
    "active_level": {
      "type": "integer",
      "default": 1,
      "minimum": 0,
      "maximum": 1
    },
    "brightness": {
      "type": "integer",
      "default": 255,
      "minimum": 0,
      "maximum": 255
    }
  }
}
```

GPIO lights are binary. Ignore color for GPIO lights. Treat `brightness: 0` as off and any non-zero brightness as on.

## GPIO Light Tool Call Inputs

Turn off an active-high GPIO light:
```json
{"path":"{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua","args":{"io":23,"enabled":false,"active_level":1}}
```

Turn on an active-low GPIO light:
```json
{"path":"{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua","args":{"io":43,"enabled":true,"active_level":0}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and query the board hardware info before operating hardware.
2. Use the `Device Inventory` headings and occupied IO lines from `board_hardware_info`; do not guess pins outside that skill.
3. Look for a GPIO light by device name, such as `flashlight`, `led_*`, `*_led`, or names containing `light`.
4. If `chip: gpio_led` or `type: gpio_ctrl` is visible in hardware info, treat that as a GPIO light signal when the name also indicates a light.
5. Use the GPIO from the device occupied IO such as `gpio` or `pin`.
6. Use `active_level` from hardware info when available. If explicit active-high or active-low text is visible, map it to `1` or `0`.
7. If no active-level information is available, use the script default `active_level: 1` and do not claim active-low behavior.
8. Do not use the default `io` value if `board_hardware_info` provides one.
9. Run `{CUR_SKILL_DIR}/scripts/gpio_light_switch.lua` with the resolved `io`, `active_level`, and requested enabled state.
10. Ignore color and map `brightness: 0` to off.
11. If no GPIO light hardware is listed, inform the user that the board does not declare a controllable light and stop.
12. Report the result or error directly to the user.
