# cap_exec_state

Persistent execution-state store plus three model capabilities (`exec_state_check`, `exec_state_mark`, `exec_state_list`) that let the agent deduplicate work across autonomous pulses and reboots.

## Kconfig defaults

| Symbol | Default | Range |
|---|---|---|
| `CAP_EXEC_STATE_MAX_ENTRIES` | 200 | 10-500 |
| `CAP_EXEC_STATE_MAX_FILE_BYTES` | 49152 | 4096-65535 |
| `CAP_EXEC_STATE_FLUSH_DELAY_MS` | 2000 | 0-60000 |
| `CAP_EXEC_STATE_LIST_DEFAULT_LIMIT` | 20 | 1-500 |

## Storage

State lives in `<DATA>/state/exec_state.json` (via `claw_paths_join(CLAW_PATH_DATA, ...)`):

```json
{"version":1,"entries":[{"key":"email:abc","kind":"email","status":"completed",
  "first_seen_ms":1,"last_run_ms":2,"run_count":1,"summary":"..."}]}
```

Entries are ordered oldest to newest. Marking an existing key bumps `run_count` and moves it to the end; `check` never reorders. When `max_entries` or `max_file_bytes` is exceeded the oldest entries are evicted.

Writes go to `exec_state.json.tmp` and are renamed over the main file (the old file is unlinked first because FAT does not overwrite on rename). On load, a leftover `.tmp` is promoted only if the main file is missing and the `.tmp` parses.

## Corruption handling

A file that is too large, is not valid JSON, lacks `entries`, or has a newer `version` is renamed to `exec_state.json.bad` (single backup) and the store starts empty. Items without a valid key are dropped, unknown `status` becomes `failed`, missing fields get defaults, and duplicate keys keep the last one.

## Tradeoffs

- Flushes are deferred (`FLUSH_DELAY_MS`) and coalesced; the timer is not restarted by later marks. A power loss can lose marks from the last window. `FLUSH_DELAY_MS=0` writes on every mark.
- Evicted `completed` entries may be reprocessed.
- Without SNTP, timestamps are not wall-clock time.
- APIs are thread-safe, not ISR-safe.

## Scheduled tasks

By convention a scheduled task records its last run under the key `task:<scheduler_id>`.

## Notes

- The state file is written to `*.tmp` and then renamed. FATFS `rename()` does not overwrite, so the previous file is unlinked first; a crash between the two leaves only the `.tmp`, which `load` recovers.
- `stop()`/`delete()` must not run concurrently with `mark`/`check`; in the firmware the store lives for the whole uptime.
- Not built or run in CI: validate with `idf.py build` and the test app under `test_apps/exec_state_test` on a board.
