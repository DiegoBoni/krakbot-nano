---
{
  "name": "cap_activity_log",
  "description": "Record and review autonomous activity: proactive pulses, scheduled tasks and manual actions.",
  "metadata": {
    "cap_groups": [
      "cap_activity_log"
    ],
    "manage_mode": "readonly"
  }
}
---

# Activity Log

The activity log is a persistent, bounded history of what the bot did on its own and why. Use `activity_log_append` to write to it and `activity_log_list` to read it.

## When to record
- When a proactive pulse finishes (`origin=proactive_pulse`).
- When a scheduled task finishes (`origin=scheduled_task`); set `ref` to the task id.
- When you decide not to act (`result=skipped`), and say why in `reason`.
- On any failure (`result=error`) with a short `detail`.
- For user-requested actions worth tracing, use `origin=manual`.

The scheduler logs a failed dispatch as `error`; a successful publish only means the event was queued and is not logged as completed. Terminal outcomes of scheduled `send_message`, `run_script` and `wake_agent` (delivered agent reply) are logged automatically with `ref` = task id and correlation `sched:<task id>:<planned_ms>` in `detail`. Add your own entry with the final outcome only for work the automatic hooks cannot observe.

## How to fill the fields
- `action` and `reason`: concise, one short sentence each.
- `result`: be honest. `completed`, `skipped` or `error`.
- `ref`: a task id or a file path, when there is one.
- `detail`: at most 200 characters (longer text is truncated).

## When to read
- Before repeating an action, to check it was not already done.
- When the user asks what the bot has been doing.

`activity_log_list` accepts `limit` (default 20, max 50), `origin`, `ref` (exact match, applied before `limit`) and `since_ms`. Results are newest first.

## Never record
Secrets, tokens, credentials, private messages or sensitive personal data.

## Examples
Completed:
`{"origin":"proactive_pulse","action":"Prepared briefing for Cliente X","reason":"Meeting with Cliente X in 1 hour","result":"completed","ref":"calendar:cliente-x"}`

Skipped:
`{"origin":"scheduled_task","action":"Check mail","reason":"No new messages since last check","result":"skipped","ref":"mail_check"}`
