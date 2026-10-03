# cap_activity_log

Bounded, persistent history of autonomous actions (proactive pulses, scheduled tasks, manual actions). Exposes `activity_log_list` and `activity_log_append` to the agent; `cap_scheduler` logs every fire automatically.

## Storage format

JSON Lines under `<DATA>/activity/`:

- `activity.jsonl` – active segment
- `activity.1.jsonl` – previous segment

One object per line, keys in this order:

| Field | Type | Notes |
|---|---|---|
| `ts` | integer | epoch ms. If the clock is not synced yet, uptime ms (not wall time) |
| `origin` | string | `proactive_pulse` / `scheduled_task` / `manual` |
| `action` | string | ≤ 96 bytes |
| `reason` | string | ≤ 128 bytes |
| `result` | string | `completed` / `skipped` / `error` |
| `ref` | string, optional | ≤ 128 bytes; task id or path |
| `detail` | string, optional | ≤ 200 bytes |

Backward compatibility: new fields must be optional; readers ignore unknown fields. Lines that are corrupt, truncated or have unknown enum values are skipped.

## Configuration (Kconfig, menu "Activity log")

| Option | Default |
|---|---|
| `CAP_ACTIVITY_LOG_MAX_BYTES` | 131072 |
| `CAP_ACTIVITY_LOG_FLUSH_INTERVAL_MS` | 60000 |
| `CAP_ACTIVITY_LOG_FLUSH_BATCH` | 16 |
| `CAP_ACTIVITY_LOG_BUFFER_ENTRIES` | 32 |
| `CAP_ACTIVITY_LOG_FLUSH_ON_ERROR` | y |
| `CAP_ACTIVITY_LOG_TASK_STACK` | 4096 |

`APP_CLAW_CAP_ACTIVITY_LOG` (app_claw) controls whether the app creates, starts and registers the log.

## Disk budget

Worst case is 2 × `MAX_BYTES` plus one line (~3.5 KB). The default is 256 KB, 15 % of the smallest `storage` partition (8 MB layout: 1664 KB; 16 MB: 3 MB; 32 MB: 12 MB).

## Write strategy

`append` only copies into an in-RAM ring (no I/O, no allocation, ≤ 5 ms lock wait). A low-priority task flushes when `FLUSH_BATCH` entries are pending, on an `error` entry (if enabled), every `FLUSH_INTERVAL_MS`, and on `stop`. Writes use `fopen("a")`/`fclose` with no `fsync`, so a power cut can lose the unflushed RAM entries and leave a truncated last line. On start, if the file does not end in `\n`, the next write begins with one.

If the ring is full the oldest pending entry is dropped (`dropped` stat). A failed flush counts in `flush_errors` and the batch is not retried.

## Rotation

When a write would exceed `MAX_BYTES`, `activity.1.jsonl` is unlinked and the active file renamed over it. FATFS `rename` fails if the destination exists, hence the explicit unlink; the pair is not atomic. If rotation fails, writing continues in the active file.

## Locks

`buf_mutex` (ring/stats) and `file_mutex` (files) are leaf locks and are never held together. `flush_mutex` serialises flushers and is only held by the flush path. Nothing external is called while holding any of them.

## Scheduler entries

`cap_scheduler` appends `origin=scheduled_task`, `action="scheduled task fired"`, `reason="schedule <kind>/<id>"`, `ref=<id>`, `result=completed` (`detail="event published"`) or `error` (`detail="publish failed: ..."`). `completed` means the event was published, not that the agent finished. The agent should add its own entry with the real outcome and the same `ref`.

## Coverage limits

Proactive pulses and final task outcomes are only logged if the agent calls `activity_log_append` (see the `cap_activity_log` skill). Events that do not go through the scheduler are not logged automatically; a `claw_event_router` hook is a possible follow-up.
