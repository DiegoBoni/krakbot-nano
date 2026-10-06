---
{
  "name": "scheduled_task",
  "description": "Add, list, pause/resume, or remove timer-based scheduled tasks (cron, interval, one-shot), periodic reminders, scheduled agent wake-ups, fixed IM messages, or scheduled Lua script runs. Prefer to use this skill rather than calling scheduler_add and add_router_rule directly.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_scheduler",
      "cap_router_mgr",
      "cap_activity_log"
    ],
    "manage_mode": "readonly"
  }
}
---

# Scheduled Task

Use this skill when the user asks to add a scheduled task, timer, periodic reminder, timed agent wake-up, fixed IM reminder, or scheduled Lua script run.

Run exactly one bundled Lua script with `lua_run_script`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{},"timeout_ms":60000}
```

Other bundled scripts (same invocation pattern): `scripts/list_scheduled_tasks.lua` (args: optional `task_id`), `scripts/remove_scheduled_task.lua` (args: `task_id`).

If script execution returns an error, report that error directly to the user.
Do not retry with changed arguments or run another script in the same turn unless the user explicitly asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "task_id": {
      "type": "string",
      "description": "Stable schedule id. Use lowercase letters, digits, underscore, or hyphen."
    },
    "kind": {
      "type": "string",
      "enum": ["cron", "interval", "once"]
    },
    "at": {
      "description": "Required when kind is once, invalid otherwise. Either epoch milliseconds (number) or a local date-time string YYYY-MM-DDTHH:mm:ss in the firmware time zone. Must be strictly in the future."
    },
    "cron_expr": {
      "type": "string",
      "description": "Required when kind is cron. Use a 5-field cron expression: minute hour mday month wday."
    },
    "interval_ms": {
      "type": "integer",
      "description": "Required when kind is interval. Must be greater than 0."
    },
    "mode": {
      "type": "string",
      "enum": ["wake_agent", "send_message", "run_script"]
    },
    "text": {
      "type": "string",
      "description": "Required for wake_agent and send_message. For run_script it is optional schedule text."
    },
    "script_path": {
      "type": "string",
      "description": "Required for run_script. Must be an absolute .lua path and must not contain .."
    },
    "script_args": {
      "type": "object",
      "default": {}
    },
    "chat_channel": {
      "type": "string",
      "description": "Required for wake_agent and send_message, for example feishu, weixin, qq, or telegram."
    },
    "chat_id": {
      "type": "string",
      "description": "Required for wake_agent and send_message."
    },
    "enabled": {
      "type": "boolean",
      "default": true
    },
    "trigger_count": {
      "type": "integer",
      "default": 0,
      "description": "Maximum number of times to trigger. Use 0 for unlimited. Preferred over max_runs."
    },
    "max_runs": {
      "type": "integer",
      "default": 0,
      "description": "Compatibility alias for trigger_count. Use trigger_count for new calls."
    }
  },
  "required": ["task_id", "kind", "mode"]
}
```

## Tool Call Inputs

Send a fixed IM message every day at 17:06:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"drink_water_reminder","kind":"cron","cron_expr":"6 17 * * *","mode":"send_message","text":"该喝水了","chat_channel":"feishu","chat_id":"ou_xxx","trigger_count":0},"timeout_ms":60000}
```

Wake the agent every day at 17:09:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"weather_outfit_reminder","kind":"cron","cron_expr":"9 17 * * *","mode":"wake_agent","text":"查询今天的天气，然后根据天气情况告诉我应该穿什么衣服。","chat_channel":"feishu","chat_id":"ou_xxx","trigger_count":0},"timeout_ms":60000}
```

Run a Lua script every 120 seconds:

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"hello_world_timer","kind":"interval","interval_ms":120000,"mode":"run_script","text":"hello world timer","script_path":"/absolute/path/to/your_script.lua","script_args":{},"trigger_count":3},"timeout_ms":60000}
```

## Behavior

- `wake_agent` creates only one scheduler entry. It emits a `message`/`text` event from the target IM channel and chat, so the existing `im_any_message_agent` router rule handles the agent run and normal chat session context.
- `send_message` and `run_script` create one scheduler entry and one router rule.
- For `send_message` and `run_script`, the skill script uses `add_router_rule` first and `scheduler_add` second through Lua `capability.call`.
- For `wake_agent`, the skill script uses only `scheduler_add` through Lua `capability.call`.
- It does not update existing rules. If an id already exists, the capability error is reported directly.
- If router rule creation succeeds but scheduler creation fails, the script best-effort deletes the newly added router rule and reports both the scheduler error and rollback result.
- `wake_agent` sets scheduler `event_type: "message"`, `event_key: "text"`, `source_channel` from `chat_channel`, `chat_id` from `chat_id`, `content_type: "text"`, and `session_policy: "chat"`.
- `send_message` and `run_script` use `session_policy: "trigger"` in the scheduler entry.
- `trigger_count` maps to scheduler `max_runs`; `0` means unlimited. If both `trigger_count` and `max_runs` are provided, `trigger_count` wins.

## User Reporting

Before running the script, tell the user the chosen execution strategy in concise terms:

- schedule kind and timing, such as the cron expression or interval in milliseconds
- mode: wake agent, send fixed IM message, or run Lua script. Provide more understandable and detailed explanation for the user, such as "the agent will be woken up to do something" instead of "wake agent".
- target IM channel/chat when applicable
- script path when running a Lua script
- trigger count, where `0` means unlimited
- whether a router rule will be created, or whether `wake_agent` will reuse `im_any_message_agent`

After the script succeeds, summarize what was added: `task_id`, mode, schedule kind, trigger count, scheduler id, and router rule id or reused router rule. If the script fails, report the script error directly.

## Recommended Flow

1. Choose a stable `task_id` in lowercase snake_case or kebab-case.
2. Choose `kind` as `cron` for wall-clock schedules or `interval` for relative periodic schedules.
3. Choose exactly one `mode`: `wake_agent`, `send_message`, or `run_script`.
4. Fill the mode-specific required arguments and resolve `trigger_count`.
5. Tell the user the execution strategy and trigger count before making changes.
6. Run `{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua` with `lua_run_script` and `timeout_ms: 60000`.
7. Report the script result or error directly to the user, including the created scheduler and router behavior.

## One-shot tasks (`kind: once`)

```json
{"path":"{CUR_SKILL_DIR}/scripts/add_scheduled_task.lua","args":{"task_id":"call_mom","kind":"once","at":"2026-12-24T18:30:00","mode":"send_message","text":"Call mom","chat_channel":"telegram","chat_id":"12345"},"timeout_ms":60000}
```

- `at` is either a number (epoch **milliseconds** only, never seconds) or a strict local string `YYYY-MM-DDTHH:mm:ss` interpreted in the firmware time zone. No other formats are accepted.
- Rejected with an error: invalid dates, local times that do not exist (DST gap) or are ambiguous (DST overlap), instants not strictly in the future, and an unsynchronized clock (`once` and `cron` require time sync; `interval` does not).
- `at` is only valid for `once`; `cron_expr`/`interval_ms` are rejected for `once`. A `once` task fires exactly once (`max_runs` 1) and is consumed even if delivery fails; there is no automatic retry. Persisted in the scheduler JSON/NVS like other tasks.
- Before calling the script, resolve relative expressions ("tomorrow at 9", "in 2 hours") into an absolute `at` yourself. Ask the user if the date, time zone, or destination is missing.

## Destination rules

- `send_message` and `wake_agent` need an IM `chat_channel` (not local/web) and `chat_id`. Reuse the current conversation destination only when it is an IM chat with a known `chat_id`; otherwise ask the user to configure IM and provide `chat_id`.
- `run_script` does not need an IM destination unless the script itself delivers messages.
- Creation validates everything first and refuses duplicates (scheduler id or router rule id) without overwriting. If scheduler creation fails after the router rule was created, the rule is rolled back; if rollback also fails the error lists what is left behind.

## List, pause/resume, remove

- `list_scheduled_tasks.lua` shows id, kind, mode, instruction, schedule, state (`scheduled`, `paused`, `consumed`), last fire, next fire, target, and router rule status. A consumed `once` has no next fire (`none`); a never-run task shows `none (never fired)`; a missing router rule is reported as `INCONSISTENT` and is not recreated. The last result is read from `activity_log_list` (origin `scheduled_task`); the list picks the newest record per task id (`last_result`). If none exists it says so explicitly (never fired, or no terminal result recorded / dropped by retention). Clock sync for `once`/`cron` is checked with the scheduler's own `scheduler_time_status` flag, not an epoch heuristic. Removing a consumed `once` task does not pause it first (it cannot fire again).
- Pause/resume use the `scheduler_pause` / `scheduler_resume` capabilities; pausing blocks new firings but lets a running execution finish.
- `remove_scheduled_task.lua` pauses the entry, deletes only the exclusive rule (`<task_id>_notify` or `<task_id>_run`) and the scheduler entry. On failure it restores deleted rules and the original activation and reports any resource it could not restore. Removing a task that does not exist succeeds and says it was already absent. `im_any_message_agent` and rules not created by this skill are never touched; an execution already started is not cancelled and history is kept.
- Tasks created before this version keep working. Only the exact ids `<task_id>_notify` / `<task_id>_run` are considered owned; no name-pattern guessing.

## History

Each fire (automatic or `trigger_now`) is recorded by the scheduler in `cap_activity_log` with origin `scheduled_task` and the task id as ref. The scheduler logs only failed dispatches (`error`, with correlation `<task id>:<planned_ms>`); a successful publish is not logged as completed because it only means the event was queued. Known limitation / pending for #4: terminal completion of `run_script` / `wake_agent` runs is not yet propagated to the log.
