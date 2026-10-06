local capability = require("capability")
local json = require("json")

local raw_args = type(args) == "table" and args or {}

-- Router rule id suffix -> mode, for the rules add_scheduled_task.lua creates.
local OWNED_RULES = {
    { suffix = "_notify", mode = "send_message", action = "send_message" },
    { suffix = "_run", mode = "run_script", action = "run_script" },
}
local SHARED_AGENT_RULE = "im_any_message_agent"
local ERR_NOT_FOUND = "ESP_ERR_NOT_FOUND"
local SCHEDULE_EVENT_TYPE = "schedule"
local AGENT_EVENT_TYPE = "message"

local function call(name, payload)
    return capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 16384,
    })
end

-- A rule belongs to the task only when it has the exact id and matches this task's schedule event.
-- Same-named rules that do not match are reported as foreign and never attributed to the task.
local function inspect_rule(task_id, owned)
    local rule_id = task_id .. owned.suffix
    local ok, out, err = call("get_router_rule", { id = rule_id })
    if not ok then
        if err == ERR_NOT_FOUND then
            return "absent"
        end
        return "unknown"
    end
    local rule = json.decode(out)
    local match = type(rule) == "table" and type(rule.match) == "table" and rule.match or {}
    local first = type(rule) == "table" and type(rule.actions) == "table" and rule.actions[1] or {}
    if match.event_type == SCHEDULE_EVENT_TYPE and match.event_key == task_id
        and type(first) == "table" and first.type == owned.action then
        return "owned"
    end
    return "foreign"
end

local function resolve_mode(item)
    if item.event_type == AGENT_EVENT_TYPE then
        return "wake_agent", "shared:" .. SHARED_AGENT_RULE, "ok"
    end
    local states = {}
    for _, owned in ipairs(OWNED_RULES) do
        states[owned.suffix] = inspect_rule(item.id, owned)
        if states[owned.suffix] == "owned" then
            return owned.mode, item.id .. owned.suffix, "ok"
        end
    end
    for _, owned in ipairs(OWNED_RULES) do
        if states[owned.suffix] == "unknown" then
            return "unknown", "unknown", "UNKNOWN: router rule query failed"
        end
    end
    for _, owned in ipairs(OWNED_RULES) do
        if states[owned.suffix] == "foreign" then
            return "unknown", item.id .. owned.suffix,
                "INCONSISTENT: router rule " .. item.id .. owned.suffix
                .. " exists but does not belong to this task (left untouched)"
        end
    end
    return "unknown", "missing", "INCONSISTENT: router rule missing (task cannot deliver)"
end

-- Last result comes from the activity log only (filtered by task id before the limit, so another
-- task's newer entries cannot hide it); the scheduler keeps no second history.
local function last_result(task_id, last_fire_ms)
    local ok, out = call("activity_log_list", { origin = "scheduled_task", ref = task_id, limit = 1 })
    local decoded = ok and json.decode(out) or nil
    if type(decoded) ~= "table" or type(decoded.entries) ~= "table" then
        return "unavailable (activity log query failed)"
    end
    local rec = decoded.entries[1]
    if type(rec) ~= "table" then
        if last_fire_ms > 0 then
            return "not in activity log (removed by retention, or the fire has not reached a terminal result)"
        end
        return "none (never fired)"
    end
    if last_fire_ms > 0 and type(rec.ts) == "number" and rec.ts < last_fire_ms then
        return {
            status = "no terminal result recorded for the latest fire (still running, interrupted, or not delivered)",
            previous = { result = rec.result, detail = rec.detail, ts = rec.ts },
        }
    end
    return { result = rec.result, detail = rec.detail, ts = rec.ts }
end

local function describe(item, snap_status, next_fire_ms, last_fire_ms)
    local mode, rule_state, consistency = resolve_mode(item)

    local consumed = snap_status == "completed" or (item.kind == "once" and next_fire_ms <= 0 and last_fire_ms > 0)
    local state
    if consumed then
        state = "consumed"
    elseif snap_status == "paused" or item.enabled == false or snap_status == "disabled" then
        state = "paused"
    else
        state = snap_status
    end

    return {
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
        last_result = last_result(item.id, last_fire_ms),
        target = item.source_channel ~= "time" and (item.source_channel .. ":" .. item.chat_id) or nil,
        router_rule = rule_state,
        consistency = consistency,
    }
end

local function run()
    local ok, out, err = call("scheduler_list", {})
    if not ok then
        error("scheduler_list failed: err=" .. tostring(err) .. " out=" .. tostring(out))
    end
    local decoded = json.decode(out)
    local entries = type(decoded) == "table" and (decoded.items or decoded.schedules or decoded) or {}
    local only_id = type(raw_args.task_id) == "string" and raw_args.task_id or ""
    local result = {}

    for _, entry in ipairs(entries) do
        if only_id == "" or entry.id == only_id then
            local gok, gout = call("scheduler_get", { id = entry.id })
            local full = gok and json.decode(gout) or entry
            local item = type(full) == "table" and (full.item or full) or entry
            result[#result + 1] = describe(item, full.status or entry.status or "unknown",
                full.next_fire_ms or -1, full.last_fire_ms or 0)
        end
    end
    print("[scheduled_task] tasks=" .. json.encode(result))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
