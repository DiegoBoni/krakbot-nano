# Autonomous Pulse — implementation and PR notes

## Dependencies and verified APIs
#3 and #4 are present and registered in app_capabilities.c. Runtime selection can still disable them.
- exec_state_check: {key}; exec_state_mark: {key,kind,status,summary?}, status completed/failed/skipped. Keys at most 64 bytes.
- activity_log_append: {origin,action,reason,result,ref?,detail?}, result completed/skipped/error; activity_log_list: {limit?,origin?,since_ms?}.
- cap_scheduler_add/update(const cap_scheduler_item_t *), remove(const char *), enable(const char *,bool), get_snapshot(const char *,cap_scheduler_snapshot_t *). INTERVAL uses interval_ms; snapshot exposes next_fire_ms.
- cap_router_mgr only registers capabilities. Actual C CRUD/lookup is claw_event_router_add_rule/update_rule/delete_rule/get_rule, with free_rule required for returned owned members. JSON equivalents also exist.
- Scheduler emits source_cap=cap_scheduler, event_type from item (schedule by default), event_key via message_id; payload.schedule_id identifies the item. Rule matches event_key=proactive_pulse, not a nonexistent payload-match API. run_agent input.text supplies the prompt.
- memory_recall accepts summary_labels and limit; memory_list has no limit or recency ordering. session_command {command:"/session list"} lists aliases for the current chat only, without timestamps, limit or history retrieval. No global recent-session capability exists. The skill uses available bounded context and skips when it cannot obtain it; it does not invent APIs.
- claw_paths_join(CLAW_PATH_DATA, subpath, out, size) is available in claw_core; no fallback required.

## Design decisions (Phase 0)
| Question | Answer | Consequence |
|---|---|---|
| Persists disabled items? | Yes, save_items serializes every occupied entry, including enabled=false, in NVS | No proactive.json |
| Update interval? | Yes, cap_scheduler_update preserves enabled and rolls runtime state back on persistence error | Update under object mutex |
| Router persists rules? | Yes, CRUD writes the rules file | Verify/repair enabled rule at start |
| Overlap protection? | Router submits requests asynchronously; no completion hook exposed | Option A: default minimum 300 seconds and per-resource exec_state dedup; no strict exclusion guarantee |
| Exposes next_run? | Yes, snapshot.next_fire_ms | Return next_run in Unix ms only if positive and enabled |

Scheduler uses recursive mutex with portMAX_DELAY; router stop uses a bounded 5000 ms wait. Proactive uses a named 5000 ms mutex timeout because operations can persist state. Large snapshots/rules are heap allocated. Dependencies are global APIs, so config stores availability flags rather than invented handles. App flags describe compiled runtime services, independent of whether scheduler/router-manager tools are selected by the runtime whitelist; their actual APIs detect initialization errors. The app owns the object; the context-free registry adapter references one registered object. Registry calls must be quiesced before deletion.

## Summary and review values
Adds cap_proactive with enable/disable/set_interval/status, scheduler-backed persistence, owned-rule repair, safe startup and proactive_pulse skill. OFF by default; MAX=86400 seconds with clamp; MAX_ACTIONS_PER_PULSE = 2. Both are reviewable choices. Kconfig permits minimum down to 60, reducing overlap mitigation if changed. Dedup entries are bounded/evictable and are not atomic reservations, so overlapping runs can race.

Files: cap_proactive component/skill/test app; app_claw registration, conditional component manifest and Kconfig; project context; these notes. claw_core is unchanged. Component skills are automatically synced into SYSTEM by skill_builder.

## Verification
No firmware build or test execution: NightShift has no ESP-IDF toolchain (idf.py and IDF_PATH absent). Unity sources are provided for device execution. Remaining runtime scenarios require manual board validation; do not claim successful build/tests.

From application/edge_agent after exporting ESP-IDF:
`idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1 && idf.py build`

## Manual board checks
1. First boot: proactive_status is OFF, item absent/default interval.
2. Enable creates one item and one rule and reports ON.
3. Repeated enable creates no duplicates; corrupt/disabled rule is repaired.
4. Interval below minimum clamps and reports requested/applied/clamped; above maximum also clamps.
5. Pulse runs agent and records final outcome with activity_log_append (scheduler fire entry is not final outcome).
6. No relevant context: no action and a skipped log entry.
7. Completed resource keys are skipped via exec_state_check.
8. Destructive actions stay proposals pending confirmation.
9. Disable stops future pulses without deleting item/rule. Already submitted work may finish.
10. ON/OFF and interval survive restart; interval change while ON preserves ON.
11. Missing/corrupt scheduler definitions start OFF; rule repair failure disables item and warns.
12. Many pulses keep activity log/exec state bounded; inspect DATA and flash use.
13. Missing runtime dependencies cause no autonomous action, with diagnostic logging when available.
14. Rule persistence failure rolls back newly created item; existing item's prior state is preserved.

## Written Unity coverage (not executed)
T1–T4 clamp boundaries; T5 missing/string/fractional/overflow input; T6–T10 OFF default, repeated enable/disable, interval while ON, OFF persistence via reload, missing-rule repair; T11 capacity failure rollback and preexisting-item preservation; T12 invalid enabled configuration is disabled; corrupt router-file repair and mismatched-rule replacement; interval changes while OFF remain OFF. Corrupt scheduler NVS/file recovery across actual cold boots, real agent execution, dedup/logging, and flash bounds remain manual checks.

Startup normally reports warnings and returns OK on read/repair failure after disabling the item. If disabling itself fails (for example persistence failure), startup returns that error so app initialization cannot launch the scheduler with an unsafe enabled item. This is a deliberate fail-closed exception to nonfatal startup.
