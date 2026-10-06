local capability = require("capability")
local json = require("json")

local raw_args = type(args) == "table" and args or {}
local task_id = type(raw_args.task_id) == "string" and raw_args.task_id or ""

-- Router rules created by add_scheduled_task.lua. wake_agent tasks own no rule: they reuse the shared
-- im_any_message_agent rule, which is never touched here.
local OWNED_RULES = {
    { suffix = "_notify", action = "send_message" },
    { suffix = "_run", action = "run_script" },
}
local SCHEDULE_EVENT_TYPE = "schedule"
local ERR_NOT_FOUND = "ESP_ERR_NOT_FOUND"
local STATUS_COMPLETED = "completed"
local STATUS_PAUSED = "paused"
local STATUS_DISABLED = "disabled"

local function call(name, payload)
    return capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })
end

-- Lookup that separates "absent" (NOT_FOUND) from a failed query; failures abort the removal.
local function lookup(name, payload)
    local ok, out, err = call(name, payload)
    if ok then
        return true, out
    end
    if err == ERR_NOT_FOUND then
        return false, nil
    end
    error(string.format("%s failed (not treated as absent): err=%s out=%s", name, tostring(err), tostring(out)))
end

-- A rule is ours only if it has the exact id AND matches this task's schedule event with the expected
-- action. Anything else with the same id belongs to someone else and is left untouched.
local function is_owned_rule(rule_json, owned)
    local rule = json.decode(rule_json)
    local match = type(rule) == "table" and type(rule.match) == "table" and rule.match or {}
    local first = type(rule) == "table" and type(rule.actions) == "table" and rule.actions[1] or {}
    return match.event_type == SCHEDULE_EVENT_TYPE and match.event_key == task_id
        and type(first) == "table" and first.type == owned.action
end

local function find_rules()
    local rules, foreign = {}, {}
    for _, owned in ipairs(OWNED_RULES) do
        local rule_id = task_id .. owned.suffix
        local ok, out = lookup("get_router_rule", { id = rule_id })
        if ok then
            if is_owned_rule(out, owned) then
                rules[#rules + 1] = { id = rule_id, json = out }
            else
                foreign[#foreign + 1] = rule_id
            end
        end
    end
    return rules, foreign
end

-- A once task is consumed after its single fire (success or failure). It can no longer fire, and
-- pausing it would overwrite its terminal status.
local function is_consumed(decoded, item)
    return decoded.status == STATUS_COMPLETED
        or (item.kind == "once" and (decoded.next_fire_ms or 0) <= 0 and (decoded.last_fire_ms or 0) > 0)
end

-- Pause only entries that can still fire. Paused/disabled entries cannot fire already, and pausing a
-- disabled one would replace its status; so restoring "the original activation" means doing nothing.
local function needs_pause(decoded, item, consumed)
    return not consumed and item.enabled ~= false
        and decoded.status ~= STATUS_PAUSED and decoded.status ~= STATUS_DISABLED
end

local function rollback(removed_rules, paused)
    local pending = {}
    for _, rule in ipairs(removed_rules) do
        local ok, out, err = call("add_router_rule", { rule_json = rule.json })
        if not ok then
            pending[#pending + 1] = "router rule " .. rule.id .. " not restored: " .. tostring(err) .. " " .. tostring(out)
        end
    end
    if paused then
        local ok, out, err = call("scheduler_resume", { id = task_id })
        if not ok then
            pending[#pending + 1] = "scheduler entry " .. task_id .. " left paused: " .. tostring(err) .. " " .. tostring(out)
        end
    end
    return pending
end

local function fail_with_rollback(failure, removed_rules, paused)
    local pending = rollback(removed_rules, paused)
    if #pending > 0 then
        error(failure .. "; rollback incomplete, pending: " .. table.concat(pending, "; "))
    end
    error(failure .. "; rollback restored previous state")
end

local function run()
    local paused = false

    if not string.match(task_id, "^[a-z0-9_-]+$") then
        error("args.task_id must use lowercase letters, digits, underscore, or hyphen")
    end

    local sched_ok, sched_out = lookup("scheduler_get", { id = task_id })
    local rules, foreign = find_rules()
    local note = #foreign > 0 and (" foreign_rules_left_untouched=" .. table.concat(foreign, ",")) or ""

    if not sched_ok and #rules == 0 then
        print("[scheduled_task] remove: nothing to remove, task_id=" .. task_id
            .. " does not exist (already absent)" .. note)
        return
    end

    if sched_ok then
        local decoded = json.decode(sched_out)
        if type(decoded) ~= "table" then
            error("cannot read scheduler entry state for task_id=" .. task_id .. "; nothing was changed")
        end
        local item = decoded.item or decoded
        -- Block new firings first; work already running keeps its own delivery context.
        if needs_pause(decoded, item, is_consumed(decoded, item)) then
            local ok, out, err = call("scheduler_pause", { id = task_id })
            if not ok then
                error("could not pause scheduler entry before removal: err=" .. tostring(err) .. " out=" .. tostring(out))
            end
            paused = true
        end
    end

    local removed_rules = {}
    for _, rule in ipairs(rules) do
        local ok, out, err = call("delete_router_rule", { id = rule.id })
        if not ok then
            fail_with_rollback("delete_router_rule " .. rule.id .. " failed: err=" .. tostring(err) .. " out=" .. tostring(out),
                removed_rules, paused)
        end
        removed_rules[#removed_rules + 1] = rule
    end

    if sched_ok then
        local ok, out, err = call("scheduler_remove", { id = task_id })
        if not ok then
            fail_with_rollback("scheduler_remove failed: err=" .. tostring(err) .. " out=" .. tostring(out),
                removed_rules, paused)
        end
    end

    print(string.format("[scheduled_task] removed task_id=%s scheduler_entry=%s router_rules=%d (%s untouched)%s",
        task_id, tostring(sched_ok), #removed_rules, "im_any_message_agent", note))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
