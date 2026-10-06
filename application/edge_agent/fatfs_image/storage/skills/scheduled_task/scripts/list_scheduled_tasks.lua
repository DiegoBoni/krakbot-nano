local capability = require("capability")
local json = require("json")

local raw_args = type(args) == "table" and args or {}
local OWNED_RULE_SUFFIXES = { "_notify", "_run" }

local ERR_NOT_FOUND = "ESP_ERR_NOT_FOUND"
local ACTIVITY_LIST_LIMIT = 50

local function call(name, payload)
    return capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 16384,
    })
end

local function describe(item, snap_status, next_fire_ms, last_fire_ms, last_result, log_available)
    local mode = item.event_type == "message" and "wake_agent" or nil
    local rule_state = "shared:im_any_message_agent"

    if not mode then
        rule_state = "missing"
        for _, suffix in ipairs(OWNED_RULE_SUFFIXES) do
            local rok, _, rerr = call("get_router_rule", { id = item.id .. suffix })
            if rok then
                mode = suffix == "_notify" and "send_message" or "run_script"
                rule_state = item.id .. suffix
                break
            elseif rerr ~= ERR_NOT_FOUND then
                rule_state = "unknown"
            end
        end
        mode = mode or "unknown"
    end

    local state
    local consumed = snap_status == "completed"
        or (item.kind == "once" and next_fire_ms <= 0 and last_fire_ms > 0)
    if consumed then
        state = "consumed"
    elseif snap_status == "paused" or item.enabled == false or snap_status == "disabled" then
        state = "paused"
    else
        state = snap_status
    end

    local line = {
        id = item.id,
        kind = item.kind,
        mode = mode,
        instruction = item.text,
        cron_expr = item.cron_expr ~= "" and item.cron_expr or nil,
        interval_ms = item.interval_ms > 0 and item.interval_ms or nil,
        start_at_ms = item.start_at_ms > 0 and item.start_at_ms or nil,
        enabled = item.enabled,
        state = state,
        last_fire_ms = last_fire_ms > 0 and last_fire_ms or "none (never fired)",
        next_fire_ms = next_fire_ms > 0 and next_fire_ms
            or (consumed and "none (one-shot already consumed)" or "none"),
        last_result = last_result or (not log_available and "unavailable (activity log query failed)") or (last_fire_ms > 0 and "not in activity log (removed by retention, outside the latest 50 entries, or not yet recorded)"
            or "none (never fired)"),
        target = item.source_channel ~= "time" and (item.source_channel .. ":" .. item.chat_id) or nil,
        router_rule = rule_state,
        consistency = (rule_state == "missing" and "INCONSISTENT: router rule missing (task cannot deliver)")
            or (rule_state == "unknown" and "UNKNOWN: router rule query failed")
            or "ok",
    }
    return line
end

local function run()
    local ok, out, err = call("scheduler_list", {})
    if not ok then
        error("scheduler_list failed: err=" .. tostring(err) .. " out=" .. tostring(out))
    end
    local decoded = json.decode(out)
    local entries = type(decoded) == "table" and (decoded.items or decoded.schedules or decoded) or {}
    local last_by_task = {}
    local lok, lout = call("activity_log_list", { origin = "scheduled_task", limit = ACTIVITY_LIST_LIMIT })
    local logged = lok and json.decode(lout) or nil
    -- Last result lives in the activity log only (newest first, under "entries"); the scheduler keeps no second history.
    local log_entries = type(logged) == "table" and type(logged.entries) == "table" and logged.entries or nil
    if log_entries then
        for _, rec in ipairs(log_entries) do
            if type(rec.ref) == "string" and not last_by_task[rec.ref] then
                last_by_task[rec.ref] = { result = rec.result, detail = rec.detail, ts = rec.ts }
            end
        end
    end
    local only_id = type(raw_args.task_id) == "string" and raw_args.task_id or ""
    local result = {}

    for _, entry in ipairs(entries) do
        if only_id == "" or entry.id == only_id then
            local gok, gout = call("scheduler_get", { id = entry.id })
            local full = gok and json.decode(gout) or entry
            local item = type(full) == "table" and (full.item or full) or entry
            result[#result + 1] = describe(item, full.status or entry.status or "unknown",
                full.next_fire_ms or -1, full.last_fire_ms or 0, last_by_task[entry.id], log_entries ~= nil)
        end
    end
    print("[scheduled_task] tasks=" .. json.encode(result))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
