---
{
  "name": "cap_exec_state",
  "description": "Deduplicate autonomous work: check/mark processed resource keys and list recent execution state.",
  "metadata": {
    "cap_groups": [
      "cap_exec_state"
    ],
    "manage_mode": "readonly"
  }
}
---

# Execution State

Persistent memory of which resources were already processed, so repeated pulses and scheduled runs do not redo (or re-notify about) the same work.

## When to use
- Autonomous pulses that look at a list of items (emails, tickets, feeds, files) and act on each one.
- Scheduled tasks that process individual resources and must not repeat work on the next run.

## Available capabilities
- `exec_state_check`: input `{"key": "..."}`. Returns `{"found":bool,"already_processed":bool,"entry":{...}}`.
- `exec_state_mark`: input `{"key","kind","status","summary"?}`. `status` is `completed`, `failed` or `skipped`. Returns `{"ok":true,"run_count":N}`.
- `exec_state_list`: input `{"limit"?}`. Returns `{"count":k,"entries":[...],"truncated"?:true}`, newest first.

## Stable keys
- Format: `<type>:<stable_id>`, at most 64 bytes. Examples: `email:18f3a2c`, `issue:DiegoBoni/krakbot-nano#3`, `task:daily_report`.
- Use the identifier given by the source system (message id, issue number, file path hash).
- Never build keys from titles, subjects, or relative dates such as "today" or "yesterday".

## Mandatory flow
1. Call `exec_state_check` with the key before processing a resource.
2. Skip or process according to the rules below.
3. After processing, call `exec_state_mark` with the outcome and a short `summary` (one line, under 200 bytes).

## Dedup rules

| Existing entry | Action |
|---|---|
| `completed` | Skip. Do not process or notify again. |
| `failed` | Retry; mark again with the new outcome (`run_count` grows). |
| `skipped` | Re-evaluate; process it only if the skip reason no longer applies. |
| none | Process, then mark. |

## Last run of a scheduled task
Mark `task:<scheduler_id>` with kind `task` at the end of each scheduled run. Check it at the start to see when the task last ran and how it ended (`last_run_ms`, `summary`).

## Context from previous cycles
Call `exec_state_list` with a small `limit` (for example 10) to recall what earlier cycles did before deciding what to do next.

## Limits
- Only the most recent entries are kept (default 200); the oldest, even `completed` ones, are evicted and may be reprocessed.
- Timestamps come from the device clock; without SNTP sync they are unreliable (near 1970).
- Writes are deferred by a couple of seconds, so a power loss can drop the latest `mark`.
