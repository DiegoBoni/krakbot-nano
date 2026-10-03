---
{
  "name": "proactive_pulse",
  "description": "Run a bounded autonomous pulse: inspect available context, deduplicate useful safe work and always record the outcome.",
  "metadata": {
    "cap_groups": ["cap_proactive", "cap_exec_state", "cap_activity_log", "claw_memory", "cap_session_mgr"],
    "manage_mode": "readonly"
  }
}
---

# Proactive Pulse

A periodic autonomous review. Doing nothing is a valid and often preferred outcome.

MAX_ACTIONS_PER_PULSE = 2

Context budget: N=10 memory entries and M=3 recent sessions, maximum, including context already provided to this request. Never broaden a search to fill the budget. Do not copy private messages, credentials or personal data into logs.

## Procedure

1. Check that `exec_state_check`, `exec_state_mark` and `activity_log_append` are available before acting. If any is missing or fails, do not act. Record the missing dependency through an available system logging facility; if no callable logger is exposed, return a short diagnostic for the runtime/operator rather than inventing a logging capability. If `activity_log_append` is available, append a skipped/error outcome with the missing dependency in `reason`.

   Recover bounded context. From the memory catalog already provided, select at most the latest N=10 entries when recency is known, then call `memory_recall` with `{"summary_labels":["selected exact label"],"limit":10}` (or a smaller remaining budget). Do not call unbounded `memory_list`: it accepts `{}` and has no limit or guaranteed recency ordering. If the catalog cannot identify recent entries, use only already provided context and explicitly record the limitation.

   Review at most M=3 recent sessions already available in context. `session_command` with `{"command":"/session list"}` only lists aliases for the current chat; it does not return session histories, timestamps or global recent sessions. A scheduler trigger normally has no chat context, so do not call it without one, and never switch or create sessions to acquire context. If no recent session content is provided, use none and note that limitation. There is no `recent_sessions` capability. If available context is insufficient to justify an action, decide not to act.

2. Review new information, pending conversations and opportunities such as upcoming meetings. Require evidence in the bounded context. Do not invent deadlines, obligations or recipients.

3. For each candidate form a deterministic key `<tipo_accion>:<id_objeto>`, at most 64 bytes, based on a stable source identifier, not a title or relative date. Call `exec_state_check` with `{"key":"briefing:stable-meeting-id"}`. If `already_processed` is true or the entry has status `completed`, discard it. This check is not an atomic reservation; do not assume it excludes concurrent pulses. Stop if the check fails.

4. Evaluate utility and confidence. If no candidate remains relevant, choose "no actuar" and proceed to step 6.

5. Complete at most MAX_ACTIONS_PER_PULSE safe actions: a local briefing, information card, follow-up draft, or justified update to documentation/reports. Do not send messages to other people, publish, delete, purchase or make irreversible changes autonomously. Record those as proposals and request confirmation. Avoid overwriting user content without authorization; prefer a new draft. Stop on a dependency or action failure; do not mark an unsuccessful or merely proposed action completed.

6. Always record the decision, reason, and completed or proposed actions using `activity_log_append`, for example:
   `{"origin":"proactive_pulse","action":"No actuar","reason":"No relevant new context","result":"skipped","ref":"proactive_pulse","detail":"Reviewed bounded context; no actions"}`.
   Use `result=completed` for completed work and `result=error` for failures. For proposals use `result=skipped` with the proposal and confirmation requirement in `detail`; never imply completion. Keep `detail` under 200 characters. After every successfully completed action, call `exec_state_mark` using the same key and a real kind:
   `{"key":"briefing:stable-meeting-id","kind":"briefing","status":"completed","summary":"Prepared local briefing draft"}`.
   Mark each completed action before moving to the next candidate. If marking or logging fails, stop and report that failure; do not continue with more work. The scheduler's "event published" log is not the final pulse outcome.

7. Finish with a short summary. Do not open a conversation unless confirmation is required. Explain missing context or dependencies briefly when they caused a skipped pulse.

## Bounds and durability

The default minimum interval is 300 seconds, but this does not guarantee exclusion of overlapping runs. Execution state is bounded, older entries can be evicted and deferred writes can be lost on power failure. Use stable keys and tolerate retries; do not treat dedup as an exactly-once guarantee. The activity log is also bounded; do not create an extra per-pulse file or unbounded history.
