-- Mock-based checks for remove/list scripts. Run with a plain Lua 5.4 interpreter from this directory:
--   lua test/scheduled_task_remove_list.lua
local calls, state = {}, {}

local function load_script(path, args_tbl, caps)
    calls = {}
    local env = setmetatable({
        args = args_tbl,
        print = function() end,
        require = function(name)
            if name == "capability" then
                return { call = function(cap, payload)
                    calls[#calls + 1] = cap
                    local r = caps[cap]
                    if r == nil then return false, "missing", "ESP_ERR_NOT_FOUND" end
                    return true, r
                end }
            end
            return { encode = function(v) return tostring(v) end, decode = function(s) return state[s] end }
        end,
    }, { __index = _G })
    local f = assert(loadfile(path, "t", env))
    f()
end

local function called(name)
    for _, c in ipairs(calls) do if c == name then return true end end
    return false
end

-- Consumed once task: removed without pausing it.
state.sched_consumed = { status = "completed", item = { kind = "once", enabled = true }, next_fire_ms = -1, last_fire_ms = 5 }
load_script("scripts/remove_scheduled_task.lua", { task_id = "t1" }, {
    scheduler_get = "sched_consumed", scheduler_remove = "ok",
})
assert(not called("scheduler_pause"), "consumed task must not be paused")
assert(called("scheduler_remove"))

-- Active task: paused first, shared rule never deleted.
state.sched_active = { status = "scheduled", item = { kind = "cron", enabled = true }, next_fire_ms = 9 }
load_script("scripts/remove_scheduled_task.lua", { task_id = "t2" }, {
    scheduler_get = "sched_active", scheduler_pause = "ok", scheduler_remove = "ok",
})
assert(called("scheduler_pause") and called("scheduler_remove"))
assert(not called("delete_router_rule"))

-- Nothing exists: idempotent success.
load_script("scripts/remove_scheduled_task.lua", { task_id = "t3" }, {})
assert(not called("scheduler_remove"))

print("scheduled_task remove/list mock checks passed")
