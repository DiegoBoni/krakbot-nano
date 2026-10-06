local capability = require("capability")
local json = require("json")

local raw_args = type(args) == "table" and args or {}
local task_id = type(raw_args.task_id) == "string" and raw_args.task_id or ""

-- Router rule ids created by add_scheduled_task.lua. wake_agent tasks own no rule
-- (they reuse the shared im_any_message_agent rule, which is never touched here).
local OWNED_RULE_SUFFIXES = { "_notify", "_run" }

local ERR_NOT_FOUND = "ESP_ERR_NOT_FOUND"

-- Lookup that separates "absent" (NOT_FOUND) from a failed query; failures abort the removal.
local function lookup(name, payload)
    local ok, out, err = capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })
    if ok then
        return true, out
    end
    if err == ERR_NOT_FOUND then
        return false, nil
    end
    error(string.format("%s failed (not treated as absent): err=%s out=%s", name, tostring(err), tostring(out)))
end

local function call(name, payload)
    return capability.call(name, payload, {
        source_cap = "scheduled_task",
        max_output_bytes = 8192,
    })
end

local function run()
    local pending = {}
    local restore = {}
    local was_enabled = false
    local consumed = false

    if not string.match(task_id, "^[a-z0-9_-]+$") then
        error("args.task_id must use lowercase letters, digits, underscore, or hyphen")
    end

    -- Existing resources, identified only by the exact ids this skill creates.
    local sched_ok, sched_out = lookup("scheduler_get", { id = task_id })
    local rules = {}
    for _, suffix in ipairs(OWNED_RULE_SUFFIXES) do
        local rule_id = task_id .. suffix
        local ok, out = lookup("get_router_rule", { id = rule_id })
        if ok then
            rules[#rules + 1] = { id = rule_id, json = out }
        end
    end

    if not sched_ok and #rules == 0 then
        print("[scheduled_task] remove: nothing to remove, task_id=" .. task_id .. " does not exist (already absent)")
        return
    end

    if sched_ok then
        local decoded = json.decode(sched_out)
        if type(decoded) == "table" then
            local item = decoded.item or decoded
            -- A once task is consumed after its single fire, whether it succeeded or failed.
            consumed = decoded.status == "completed"
                or (item.kind == "once" and (decoded.next_fire_ms or 0) <= 0 and (decoded.last_fire_ms or 0) > 0)
            was_enabled = item.enabled ~= false and decoded.status ~= "paused" and not consumed
        end
        -- Block new firings first; running work keeps its own delivery context.
        -- A consumed once task can no longer fire, and pausing it would clobber its terminal status.
        if not consumed then
            local ok, out, err = call("scheduler_pause", { id = task_id })
            if not ok then
                error("could not pause scheduler entry before removal: err=" .. tostring(err) .. " out=" .. tostring(out))
            end
        end
    end

    local removed_rules = {}
    local failure
    for _, rule in ipairs(rules) do
        local ok, out, err = call("delete_router_rule", { id = rule.id })
        if ok then
            removed_rules[#removed_rules + 1] = rule
        else
            failure = "delete_router_rule " .. rule.id .. " failed: err=" .. tostring(err) .. " out=" .. tostring(out)
            break
        end
    end

    if not failure and sched_ok then
        local ok, out, err = call("scheduler_remove", { id = task_id })
        if not ok then
            failure = "scheduler_remove failed: err=" .. tostring(err) .. " out=" .. tostring(out)
        end
    end

    if failure then
        -- Roll back: restore deleted rules and the original activation.
        for _, rule in ipairs(removed_rules) do
            local ok, out, err = call("add_router_rule", { rule_json = rule.json })
            if not ok then
                pending[#pending + 1] = "router rule " .. rule.id .. " not restored: " .. tostring(err) .. " " .. tostring(out)
            end
        end
        if sched_ok and was_enabled then
            local ok, out, err = call("scheduler_resume", { id = task_id })
            if not ok then
                pending[#pending + 1] = "scheduler entry " .. task_id .. " left paused: " .. tostring(err) .. " " .. tostring(out)
            end
        end
        if #pending > 0 then
            error(failure .. "; rollback incomplete, pending: " .. table.concat(pending, "; "))
        end
        error(failure .. "; rollback restored previous state")
    end

    print(string.format("[scheduled_task] removed task_id=%s scheduler_entry=%s router_rules=%d (im_any_message_agent untouched)",
        task_id, tostring(sched_ok), #removed_rules))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    error(err)
end
