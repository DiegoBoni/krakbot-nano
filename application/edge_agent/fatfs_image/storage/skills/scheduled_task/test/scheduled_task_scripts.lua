-- Mock-based checks for add/remove/list scripts. Run with a plain Lua 5.4 interpreter from this directory:
--   lua test/scheduled_task_scripts.lua
-- The firmware time zone is simulated (US Eastern rules for 2026) so DST gap/overlap are deterministic
-- and independent of the host TZ.

local H = 3600
local registry = {}

local function register(v)
    registry[#registry + 1] = v
    return "#" .. #registry
end

-- ---------- simulated time zone ----------
local function days_from_civil(y, m, d)
    y = m <= 2 and y - 1 or y
    local era = y // 400
    local yoe = y - era * 400
    local doy = (153 * ((m + 9) % 12) + 2) // 5 + d - 1
    return era * 146097 + yoe * 365 + yoe // 4 - yoe // 100 + doy - 719468
end

local function civil_from_days(z)
    z = z + 719468
    local era = z // 146097
    local doe = z - era * 146097
    local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    local mp = (5 * doy + 2) // 153
    local d = doy - (153 * mp + 2) // 5 + 1
    local m = mp < 10 and mp + 3 or mp - 9
    return m <= 2 and y + 1 or y, m, d
end

local DST_START = days_from_civil(2026, 3, 8) * 86400 + 7 * H   -- 02:00 EST -> 03:00 EDT
local DST_END = days_from_civil(2026, 11, 1) * 86400 + 6 * H    -- 02:00 EDT -> 01:00 EST

local function offset_at(t)
    return (t >= DST_START and t < DST_END) and -4 * H or -5 * H
end

local function sim_date(fmt, t)
    assert(fmt == "*t")
    local w = t + offset_at(t)
    local y, m, d = civil_from_days(w // 86400)
    local rem = w % 86400
    return { year = y, month = m, day = d, hour = rem // 3600, min = rem % 3600 // 60, sec = rem % 60 }
end

local function local_epoch(y, mo, d, h, mi, s, offset)
    return days_from_civil(y, mo, d) * 86400 + h * H + mi * 60 + s - offset
end

-- ---------- script loader ----------
local calls, now_s, time_valid

local function new_env(args_tbl, handlers)
    return setmetatable({
        args = args_tbl,
        print = function() end,
        os = { time = function() return now_s end, date = sim_date },
        require = function(name)
            if name == "capability" then
                return { call = function(cap, payload)
                    calls[#calls + 1] = { cap = cap, payload = payload }
                    local h = handlers[cap]
                    if h == nil or h == false then
                        return false, "missing", "ESP_ERR_NOT_FOUND"
                    end
                    if type(h) == "function" then
                        return h(payload)
                    end
                    return true, register(h)
                end }
            elseif name == "arg_schema" then
                return {
                    bool = function(o) return { kind = "bool", default = o.default } end,
                    int = function(o) return { kind = "int", default = o.default } end,
                    parse = function(raw, schema)
                        local out = {}
                        for k, spec in pairs(schema) do
                            out[k] = raw[k] ~= nil and raw[k] or spec.default
                        end
                        return out
                    end,
                }
            end
            return {
                encode = function(v) return register(v) end,
                decode = function(s) return registry[tonumber(string.match(s, "^#(%d+)$") or "0")] end,
            }
        end,
    }, { __index = _G })
end

local function run_script(path, args_tbl, handlers)
    calls = {}
    local f = assert(loadfile(path, "t", new_env(args_tbl, handlers)))
    return pcall(f)
end

local function called(name)
    for _, c in ipairs(calls) do if c.cap == name then return c end end
    return nil
end

local function count(name)
    local n = 0
    for _, c in ipairs(calls) do if c.cap == name then n = n + 1 end end
    return n
end

local function expect_error(ok, err, fragment, label)
    assert(not ok, label .. ": expected an error")
    assert(string.find(tostring(err), fragment, 1, true),
        label .. ": expected error containing '" .. fragment .. "', got: " .. tostring(err))
end

local ADD = "scripts/add_scheduled_task.lua"
local REMOVE = "scripts/remove_scheduled_task.lua"
local LIST = "scripts/list_scheduled_tasks.lua"

-- Fixed "now": 2026-06-15 12:00:00 EDT.
local NOW = local_epoch(2026, 6, 15, 12, 0, 0, -4 * H)

local function base_handlers(extra)
    local h = {
        scheduler_time_status = function()
            return true, register({ time_valid = time_valid, now_ms = now_s * 1000 })
        end,
        scheduler_add = "added",
        add_router_rule = "added",
        delete_router_rule = "deleted",
    }
    for k, v in pairs(extra or {}) do h[k] = v end
    return h
end

local function once_args(at, extra)
    local a = { task_id = "t1", kind = "once", mode = "wake_agent", text = "hi", at = at,
        chat_channel = "telegram", chat_id = "42" }
    for k, v in pairs(extra or {}) do a[k] = v end
    return a
end

local function reset_clock()
    now_s, time_valid = NOW, true
end

-- The schedule table that was handed to scheduler_add.
local function created_schedule()
    local c = assert(called("scheduler_add"), "scheduler_add was not called")
    return registry[tonumber(string.sub(c.payload.schedule_json, 2))]
end

local function assert_nothing_created(label)
    assert(not called("add_router_rule") and not called("scheduler_add"), label .. ": must not create resources")
end

local function fail(code) return function() return false, "boom", code end end

-- ================= time =================
reset_clock()

-- Local date and equivalent epoch ms produce the same start_at_ms.
local expected_ms = local_epoch(2026, 7, 1, 12, 0, 0, -4 * H) * 1000
local ok, err = run_script(ADD, once_args("2026-07-01T12:00:00"), base_handlers())
assert(ok, err)
local sched = created_schedule()
assert(sched.start_at_ms == expected_ms and sched.kind == "once" and sched.max_runs == 1, "local date conversion")
ok, err = run_script(ADD, once_args(expected_ms), base_handlers())
assert(ok, err)
assert(created_schedule().start_at_ms == expected_ms, "epoch ms accepted verbatim")

-- Winter date uses the standard offset.
ok, err = run_script(ADD, once_args("2026-12-25T08:30:15"), base_handlers())
assert(ok, err)
assert(created_schedule().start_at_ms == local_epoch(2026, 12, 25, 8, 30, 15, -5 * H) * 1000, "EST offset")

-- DST gap (02:30 does not exist on 2026-03-08) and overlap (01:30 happens twice on 2026-11-01).
now_s = local_epoch(2026, 1, 1, 0, 0, 0, -5 * H)
ok, err = run_script(ADD, once_args("2026-03-08T02:30:00"), base_handlers())
expect_error(ok, err, "DST gap", "gap")
assert_nothing_created("gap")
ok, err = run_script(ADD, once_args("2026-03-08T02:00:00"), base_handlers())
expect_error(ok, err, "DST gap", "gap start")
ok, err = run_script(ADD, once_args("2026-11-01T01:30:00"), base_handlers())
expect_error(ok, err, "ambiguous", "overlap")
ok, err = run_script(ADD, once_args("2026-11-01T01:00:00"), base_handlers())
expect_error(ok, err, "ambiguous", "overlap start")
assert_nothing_created("overlap")

-- Boundaries next to the transitions are unambiguous.
for _, case in ipairs({
    { "2026-03-08T01:59:59", local_epoch(2026, 3, 8, 1, 59, 59, -5 * H) },
    { "2026-03-08T03:00:00", local_epoch(2026, 3, 8, 3, 0, 0, -4 * H) },
    { "2026-11-01T00:59:59", local_epoch(2026, 11, 1, 0, 59, 59, -4 * H) },
    { "2026-11-01T02:00:00", local_epoch(2026, 11, 1, 2, 0, 0, -5 * H) },
}) do
    ok, err = run_script(ADD, once_args(case[1]), base_handlers())
    assert(ok, case[1] .. ": " .. tostring(err))
    assert(created_schedule().start_at_ms == case[2] * 1000, case[1] .. " converted wrongly")
end
reset_clock()

-- Invalid formats, calendar values, ranges and overflow.
for _, bad in ipairs({ "2026-07-01 12:00:00", "2026-07-01T12:00", "2026-07-01T12:00:00Z", "tomorrow",
                       "2026-7-1T12:00:00", "2026-02-30T10:00:00", "2026-13-01T10:00:00",
                       "2026-07-01T24:00:00", "2026-07-01T12:60:00", "2026-07-01T12:00:60",
                       "1969-12-31T23:59:59", "10000-01-01T00:00:00" }) do
    ok, err = run_script(ADD, once_args(bad), base_handlers())
    assert(not ok, "'" .. bad .. "' must be rejected")
    assert_nothing_created(bad)
end
for _, bad in ipairs({ 1.5, 0, -1, 9007199254740992, 1e300 }) do
    ok, err = run_script(ADD, once_args(bad), base_handlers())
    assert(not ok, tostring(bad) .. " must be rejected")
    assert_nothing_created(tostring(bad))
end
ok, err = run_script(ADD, once_args(nil), base_handlers())
expect_error(ok, err, "args.at is required", "missing at")
ok, err = run_script(ADD, once_args(true), base_handlers())
assert(not ok, "boolean at must be rejected")

-- Past and equal-to-now are rejected, one second ahead is accepted.
ok, err = run_script(ADD, once_args(NOW * 1000), base_handlers())
expect_error(ok, err, "strictly in the future", "equal to now")
ok, err = run_script(ADD, once_args((NOW - 60) * 1000), base_handlers())
expect_error(ok, err, "strictly in the future", "past")
ok, err = run_script(ADD, once_args("2026-06-15T11:59:59"), base_handlers())
expect_error(ok, err, "strictly in the future", "past local")
assert_nothing_created("past")
ok, err = run_script(ADD, once_args((NOW + 1) * 1000), base_handlers())
assert(ok, err)

-- Unsynchronized clock blocks once and cron, never creating anything; interval still works.
time_valid = false
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00"), base_handlers())
expect_error(ok, err, "not synchronized", "unsynced once")
ok, err = run_script(ADD, { task_id = "c1", kind = "cron", cron_expr = "0 8 * * *", mode = "wake_agent",
    text = "x", chat_channel = "telegram", chat_id = "1" }, base_handlers())
expect_error(ok, err, "not synchronized", "unsynced cron")
assert_nothing_created("unsynced")
ok, err = run_script(ADD, { task_id = "i1", kind = "interval", interval_ms = 5000, mode = "run_script",
    script_path = "/system/x.lua" }, base_handlers())
assert(ok, err)
reset_clock()

-- ================= validations =================
local function interval_args(extra)
    local a = { task_id = "i1", kind = "interval", interval_ms = 60000, mode = "run_script",
        script_path = "/system/x.lua" }
    for k, v in pairs(extra or {}) do a[k] = v end
    return a
end

ok, err = run_script(ADD, interval_args({ at = 123 }), base_handlers())
expect_error(ok, err, "only valid when kind is once", "at on interval")
ok, err = run_script(ADD, { task_id = "c1", kind = "cron", cron_expr = "0 8 * * *", at = 123, mode = "run_script",
    script_path = "/system/x.lua" }, base_handlers())
expect_error(ok, err, "only valid when kind is once", "at on cron")
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { cron_expr = "0 8 * * *" }), base_handlers())
expect_error(ok, err, "not valid when kind is once", "cron_expr on once")
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { interval_ms = 5000 }), base_handlers())
expect_error(ok, err, "not valid when kind is once", "interval on once")
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { max_runs = 3 }), base_handlers())
expect_error(ok, err, "must be 0 or 1", "max_runs on once")
assert_nothing_created("incompatible params")

ok, err = run_script(ADD, interval_args({ interval_ms = 999 }), base_handlers())
expect_error(ok, err, "at least 1000", "interval below minimum")
ok, err = run_script(ADD, interval_args({ interval_ms = 0 }), base_handlers())
expect_error(ok, err, "at least 1000", "interval zero")
ok, err = run_script(ADD, interval_args({ interval_ms = 1000 }), base_handlers())
assert(ok, err)

for _, expr in ipairs({ "", "* * * *", "* * * * * *", "60 * * * *", "* 24 * * *", "* * 0 * *", "* * 32 * *",
                        "* * * 13 *", "* * * * 7", "1-5 * * * *", "1,2 * * * *", "*/0 * * * *", "a * * * *",
                        "*/x * * * *", "MON * * * *" }) do
    ok, err = run_script(ADD, { task_id = "c1", kind = "cron", cron_expr = expr, mode = "run_script",
        script_path = "/system/x.lua" }, base_handlers())
    assert(not ok, "cron '" .. expr .. "' must be rejected")
    assert_nothing_created("cron " .. expr)
end
for _, expr in ipairs({ "0 8 * * *", "*/5 * * * *", "59 23 31 12 6", "* * * * *" }) do
    ok, err = run_script(ADD, { task_id = "c1", kind = "cron", cron_expr = expr, mode = "run_script",
        script_path = "/system/x.lua" }, base_handlers())
    assert(ok, "cron '" .. expr .. "' must be accepted: " .. tostring(err))
end

ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { task_id = "Bad Id" }), base_handlers())
expect_error(ok, err, "task_id", "bad task id")

-- Destinations are validated before anything is created.
for _, mode in ipairs({ "wake_agent", "send_message" }) do
    ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { mode = mode, chat_channel = "", chat_id = "" }),
        base_handlers())
    expect_error(ok, err, "configure an IM channel", mode .. " without destination")
    ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { mode = mode, chat_id = "" }), base_handlers())
    expect_error(ok, err, "chat_id", mode .. " without chat_id")
    for _, ch in ipairs({ "local", "web" }) do
        ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { mode = mode, chat_channel = ch }), base_handlers())
        expect_error(ok, err, "local/web", mode .. " via " .. ch)
    end
end
assert_nothing_created("destinations")
-- run_script needs no IM destination.
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00", { mode = "run_script", chat_channel = "", chat_id = "",
    script_path = "/system/x.lua", text = "" }), base_handlers())
assert(ok, err)

-- Duplicates: scheduler entry, router rule; a failing existence query aborts without creating anything.
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00"), base_handlers({ scheduler_get = "exists" }))
expect_error(ok, err, "scheduler entry already exists", "scheduler duplicate")
assert_nothing_created("scheduler duplicate")
ok, err = run_script(ADD, interval_args(), base_handlers({ get_router_rule = "exists" }))
expect_error(ok, err, "router rule already exists", "router duplicate")
assert_nothing_created("router duplicate")
ok, err = run_script(ADD, interval_args(), base_handlers({ scheduler_get = fail("ESP_ERR_INVALID_STATE") }))
assert(not ok)
assert_nothing_created("scheduler_get failure")
ok, err = run_script(ADD, interval_args(), base_handlers({ get_router_rule = fail("ESP_ERR_INVALID_STATE") }))
assert(not ok)
assert_nothing_created("get_router_rule failure")

-- ================= creation fault injection =================
-- scheduler_add fails: router rule is rolled back and the message says so.
ok, err = run_script(ADD, interval_args(), base_handlers({ scheduler_add = fail("ESP_FAIL") }))
expect_error(ok, err, "scheduler_add failed", "scheduler_add failure")
assert(string.find(err, "rollback completed", 1, true), "rollback completed must be reported")
assert(not string.find(err, "pending cleanup", 1, true))
assert(called("delete_router_rule").payload.id == "i1_run", "owned rule rolled back")

-- Rollback fails too: error names the pending resource.
ok, err = run_script(ADD, interval_args(), base_handlers({ scheduler_add = fail("ESP_FAIL"),
    delete_router_rule = fail("ESP_ERR_INVALID_STATE") }))
expect_error(ok, err, "rollback FAILED", "rollback failure")
expect_error(ok, err, "pending cleanup: router rule i1_run", "pending resources")

-- Router rule creation fails: the scheduler is never touched.
ok, err = run_script(ADD, interval_args(), base_handlers({ add_router_rule = fail("ESP_FAIL") }))
assert(not ok)
assert(not called("scheduler_add"), "no scheduler entry after router failure")

-- wake_agent owns no rule: scheduler failure has nothing to roll back and never deletes the shared rule.
ok, err = run_script(ADD, once_args("2026-07-01T12:00:00"), base_handlers({ scheduler_add = fail("ESP_FAIL") }))
assert(not ok)
assert(not called("add_router_rule") and not called("delete_router_rule"))

-- ================= remove =================
local function sched_state(status, item, extra)
    local s = { status = status, item = item, next_fire_ms = 9, last_fire_ms = 0 }
    for k, v in pairs(extra or {}) do s[k] = v end
    return s
end

local function owned_rule(task, suffix, action)
    return { id = task .. suffix, match = { event_type = "schedule", event_key = task },
        actions = { { type = action } } }
end

-- Consumed once task: removed without pausing it.
ok, err = run_script(REMOVE, { task_id = "t1" }, {
    scheduler_get = sched_state("completed", { kind = "once", enabled = true }, { next_fire_ms = -1, last_fire_ms = 5 }),
    scheduler_remove = "ok",
})
assert(ok, err)
assert(not called("scheduler_pause") and called("scheduler_remove"), "consumed: no pause")

-- Consumed once whose single fire failed (status error): same.
ok, err = run_script(REMOVE, { task_id = "t1" }, {
    scheduler_get = sched_state("error", { kind = "once", enabled = true }, { next_fire_ms = -1, last_fire_ms = 5 }),
    scheduler_remove = "ok",
})
assert(ok, err)
assert(not called("scheduler_pause"), "consumed (error): no pause")

-- Consumed once, scheduler_remove fails: nothing to resume, rules restored.
ok, err = run_script(REMOVE, { task_id = "t1" }, {
    scheduler_get = sched_state("completed", { kind = "once", enabled = true }, { next_fire_ms = -1, last_fire_ms = 5 }),
    get_router_rule = function(p)
        if p.id == "t1_notify" then return true, register(owned_rule("t1", "_notify", "send_message")) end
        return false, "nf", "ESP_ERR_NOT_FOUND"
    end,
    delete_router_rule = "ok", add_router_rule = "ok", scheduler_remove = fail("ESP_FAIL"),
})
expect_error(ok, err, "rollback restored previous state", "consumed rollback")
assert(not called("scheduler_resume"), "consumed task is never resumed")
assert(called("add_router_rule"), "deleted rule restored")

-- Active task: paused first, resumed when removal fails, shared rule untouched.
local active = sched_state("scheduled", { kind = "cron", enabled = true })
ok, err = run_script(REMOVE, { task_id = "t2" }, { scheduler_get = active, scheduler_pause = "ok", scheduler_remove = "ok" })
assert(ok, err)
assert(called("scheduler_pause") and called("scheduler_remove") and not called("delete_router_rule"))
ok, err = run_script(REMOVE, { task_id = "t2" }, { scheduler_get = active, scheduler_pause = "ok",
    scheduler_resume = "ok", scheduler_remove = fail("ESP_FAIL") })
expect_error(ok, err, "rollback restored previous state", "active rollback")
assert(called("scheduler_resume"), "active task resumed after failed removal")

-- Pause failure aborts before anything is deleted.
ok, err = run_script(REMOVE, { task_id = "t2" }, { scheduler_get = active, scheduler_pause = fail("ESP_FAIL") })
expect_error(ok, err, "could not pause", "pause failure")
assert(not called("delete_router_rule") and not called("scheduler_remove"))

-- Already paused or disabled entries are not touched: rollback must not turn them into running ones.
for _, status in ipairs({ "paused", "disabled" }) do
    local entry = sched_state(status, { kind = "cron", enabled = status ~= "disabled" })
    ok, err = run_script(REMOVE, { task_id = "t2" }, { scheduler_get = entry, scheduler_remove = fail("ESP_FAIL") })
    expect_error(ok, err, "rollback restored previous state", status .. " rollback")
    assert(not called("scheduler_pause") and not called("scheduler_resume"), status .. ": activation untouched")
end

-- Rollback failure reports pending resources.
ok, err = run_script(REMOVE, { task_id = "t2" }, {
    scheduler_get = active, scheduler_pause = "ok", scheduler_resume = fail("ESP_FAIL"),
    get_router_rule = function(p)
        if p.id == "t2_run" then return true, register(owned_rule("t2", "_run", "run_script")) end
        return false, "nf", "ESP_ERR_NOT_FOUND"
    end,
    delete_router_rule = "ok", add_router_rule = fail("ESP_FAIL"), scheduler_remove = fail("ESP_FAIL"),
})
expect_error(ok, err, "rollback incomplete", "rollback failure")
expect_error(ok, err, "router rule t2_run not restored", "pending rule")
expect_error(ok, err, "scheduler entry t2 left paused", "pending scheduler")

-- Rule delete fails midway: earlier deletions are restored and the entry resumed.
ok, err = run_script(REMOVE, { task_id = "t2" }, {
    scheduler_get = active, scheduler_pause = "ok", scheduler_resume = "ok",
    get_router_rule = function(p)
        local suffix = string.sub(p.id, 3)
        return true, register(owned_rule("t2", suffix, suffix == "_notify" and "send_message" or "run_script"))
    end,
    delete_router_rule = function(p)
        if p.id == "t2_run" then return false, "boom", "ESP_FAIL" end
        return true, "ok"
    end,
    add_router_rule = "ok", scheduler_remove = "ok",
})
expect_error(ok, err, "rollback restored previous state", "midway failure")
assert(count("add_router_rule") == 1 and called("scheduler_resume") and not called("scheduler_remove"))

-- Foreign rule with the same id is never deleted and never attributed to the task.
local foreign_rule = { id = "t3_notify", match = { event_type = "message", event_key = "other" },
    actions = { { type = "send_message" } } }
local function foreign_only(p)
    if p.id == "t3_notify" then return true, register(foreign_rule) end
    return false, "nf", "ESP_ERR_NOT_FOUND"
end
ok, err = run_script(REMOVE, { task_id = "t3" }, {
    scheduler_get = sched_state("scheduled", { kind = "cron", enabled = true }),
    scheduler_pause = "ok", scheduler_remove = "ok", get_router_rule = foreign_only,
})
assert(ok, err)
assert(not called("delete_router_rule"), "foreign rule must not be deleted")
-- ...and with only a foreign rule the task counts as absent.
ok, err = run_script(REMOVE, { task_id = "t3" }, { get_router_rule = foreign_only })
assert(ok, err)
assert(not called("delete_router_rule") and not called("scheduler_remove"))

-- Residual owned rule without a scheduler entry is cleaned up.
ok, err = run_script(REMOVE, { task_id = "t4" }, {
    get_router_rule = function(p)
        if p.id == "t4_run" then return true, register(owned_rule("t4", "_run", "run_script")) end
        return false, "nf", "ESP_ERR_NOT_FOUND"
    end,
    delete_router_rule = "ok",
})
assert(ok, err)
assert(called("delete_router_rule").payload.id == "t4_run" and not called("scheduler_remove"))

-- Nothing exists: idempotent success. A failed query is not read as absent.
ok, err = run_script(REMOVE, { task_id = "t5" }, {})
assert(ok, err)
assert(not called("scheduler_remove") and not called("delete_router_rule"))
ok, err = run_script(REMOVE, { task_id = "t5" }, { scheduler_get = fail("ESP_ERR_INVALID_STATE") })
assert(not ok and not called("scheduler_remove"))
ok, err = run_script(REMOVE, { task_id = "t5" }, { get_router_rule = fail("ESP_ERR_INVALID_STATE") })
assert(not ok and not called("scheduler_remove") and not called("delete_router_rule"))
ok, err = run_script(REMOVE, { task_id = "BAD" }, {})
assert(not ok)

-- ================= list =================
local function list_handlers(entry, rules, log)
    return {
        scheduler_list = { items = { { id = "t6" } } },
        scheduler_get = entry,
        get_router_rule = function(p)
            local r = rules[p.id]
            if r == nil then return false, "nf", "ESP_ERR_NOT_FOUND" end
            if r == false then return false, "boom", "ESP_ERR_INVALID_STATE" end
            return true, register(r)
        end,
        activity_log_list = log,
    }
end

local function listed(entry, rules, log)
    local printed
    local env = new_env({}, list_handlers(entry, rules, log))
    env.print = function(s) printed = s end
    calls = {}
    local f = assert(loadfile(LIST, "t", env))
    local lok, lerr = pcall(f)
    assert(lok, lerr)
    return registry[tonumber(string.match(printed, "#(%d+)$"))][1]
end

local function item(over)
    local i = { id = "t6", kind = "interval", interval_ms = 1000, start_at_ms = 0, cron_expr = "", enabled = true,
        event_type = "schedule", source_channel = "time", chat_id = "", text = "x" }
    for k, v in pairs(over or {}) do i[k] = v end
    return i
end

local run_rule = { t6_run = owned_rule("t6", "_run", "run_script") }
local agent_item = { event_type = "message", source_channel = "tg", chat_id = "1" }

-- Latest result is requested for that task (ref filter + limit 1), so other tasks cannot hide it.
local row = listed(sched_state("scheduled", item(agent_item), { next_fire_ms = 5, last_fire_ms = 3000 }), {},
    { entries = { { ref = "t6", result = "completed", detail = "ok", ts = 4000 } } })
local q = called("activity_log_list").payload
assert(q.ref == "t6" and q.limit == 1 and q.origin == "scheduled_task", "per-task log query")
assert(row.mode == "wake_agent" and row.last_result.result == "completed" and row.consistency == "ok")
assert(row.router_rule == "shared:im_any_message_agent" and row.target == "tg:1")

-- Last fire newer than the last log entry: no terminal result recorded yet (running/interrupted).
row = listed(sched_state("scheduled", item(agent_item), { next_fire_ms = 5, last_fire_ms = 9000 }), {},
    { entries = { { ref = "t6", result = "completed", detail = "old", ts = 4000 } } })
assert(type(row.last_result) == "table" and string.find(row.last_result.status, "no terminal result", 1, true))
assert(row.last_result.previous.result == "completed", "previous result kept separate")

-- New task, retention-removed result, unavailable log.
row = listed(sched_state("scheduled", item()), run_rule, { entries = {} })
assert(row.last_result == "none (never fired)" and row.mode == "run_script" and row.router_rule == "t6_run")
row = listed(sched_state("scheduled", item(), { last_fire_ms = 3000 }), run_rule, { entries = {} })
assert(string.find(row.last_result, "removed by retention", 1, true))
row = listed(sched_state("scheduled", item(), { last_fire_ms = 3000 }), run_rule, fail("ESP_FAIL"))
assert(string.find(row.last_result, "unavailable", 1, true))

-- Paused vs consumed.
row = listed(sched_state("paused", item()), run_rule, { entries = {} })
assert(row.state == "paused")
row = listed(sched_state("completed", item({ kind = "once", start_at_ms = 7 }), { next_fire_ms = -1, last_fire_ms = 3000 }),
    run_rule, { entries = {} })
assert(row.state == "consumed" and string.find(row.next_fire_ms, "consumed", 1, true))

-- Missing, foreign and unreadable rules are reported, never hidden.
row = listed(sched_state("scheduled", item()), {}, { entries = {} })
assert(string.find(row.consistency, "INCONSISTENT: router rule missing", 1, true) and row.mode == "unknown")
row = listed(sched_state("scheduled", item()), { t6_run = { id = "t6_run",
    match = { event_type = "message", event_key = "z" }, actions = { { type = "run_script" } } } }, { entries = {} })
assert(string.find(row.consistency, "does not belong", 1, true) and row.mode == "unknown")
row = listed(sched_state("scheduled", item()), { t6_notify = false }, { entries = {} })
assert(string.find(row.consistency, "UNKNOWN", 1, true))

print("scheduled_task script checks passed")
