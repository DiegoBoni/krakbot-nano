---
{
  "name": "mcp_composio",
  "description": "Read or manage the user's email (Gmail), calendar (Google Calendar), Google Drive files, Google Tasks and other connected apps via the 'composio' MCP server: 'can you see my emails', 'ver/leer mis correos', 'mi agenda', 'crear una tarea', 'mandar un mail'. The user's accounts are already connected: try it instead of asking them to set anything up or saying you lack access.",
  "metadata": {
    "cap_groups": ["cap_mcp_client"],
    "manage_mode": "readonly"
  }
}
---

# Composio MCP Server

The device's `composio` MCP server (`server_id: "composio"`, see `mcp_list_servers`) is a
single gateway that fronts hundreds of third-party app connections (Gmail, Google Drive,
Google Calendar, Slack, GitHub, ...). It does **not** expose each app action as its own
top-level MCP tool. Calling `mcp_call_tool` with `tool_name` set directly to an app action
slug — e.g. `GMAIL_FETCH_EMAILS`, `GOOGLECALENDAR_EVENTS_LIST_ALL_CALENDARS`,
`GOOGLEDRIVE_GET_ABOUT` — returns `Tool not found` even when that app's connection shows
`ACTIVE`. That error is expected behavior for this server, not a bug: those action slugs are
not registered as callable tools on the gateway itself.

## Why this happens

`connect.composio.dev/mcp` exposes only a small, fixed set of routing/meta tools. The
hundreds of per-app actions exist behind those meta tools, not as MCP tools in their own
right. This lets one MCP server front an entire app catalog without listing thousands of
tools.

## Correct workflow

1. Call `mcp_list_tools` with `server_id: "composio"` to see the actual top-level tool names
   this gateway currently exposes. Do this at the start of a session before guessing names —
   the exact tool names can change; never assume them from a previous conversation.
2. Among the returned tools, find the one meant for discovering/searching action slugs (its
   name and description will mention searching or listing available tools/actions). Call it
   with a short description of what the user wants (e.g. "fetch recent Gmail emails") to get
   back the real action slug(s), such as `GMAIL_FETCH_EMAILS`.
3. If a schema-lookup tool is also listed, call it with that slug to confirm the exact
   argument shape before executing — do not guess argument names.
4. Call the execute/run meta-tool from the same `mcp_list_tools` result — **not** the app
   action slug — via `mcp_call_tool` with `server_id: "composio"` and `tool_name` set to that
   meta-tool's real name. Pass the target action slug and its arguments as parameters inside
   that call's `arguments` object (the meta-tool's schema from step 1/3 defines exactly where
   the slug and arguments go).

## Common mistake to avoid

Do not retry the same direct call (`tool_name: "GMAIL_FETCH_EMAILS"`) expecting a different
result. If it returned `Tool not found` once, it will every time — the fix is routing the
call through the gateway's execute meta-tool from step 4, not retrying step 4 as if it were
step 1.

## Connection status is separate from tool routing

`ACTIVE` in the device's MCP settings (or in the connections list this server reports) means
the OAuth connection for that app succeeded. It says nothing about which MCP tool name to
call — that's still governed by steps 1-4 above.

## Several accounts connected for one app (e.g. two Gmail accounts)

When an app has more than one connected account, the execute meta-tool fails with a "specify
which account" style error. The fix is the per-tool `account` field, not giving up:

1. Call the connections meta-tool (its name contains `MANAGE_CONNECTIONS`) with
   `action: "list"` for that toolkit (e.g. `name: "gmail"`). It returns each account's ID,
   alias and status.
2. If the accounts have no useful alias, call it again with `action: "rename"`, the
   `account_id`, and an `alias` such as `"personal"` / `"work"` so the choice is stable and
   readable. Ask the user which account is which if it is not obvious from the listing.
3. In the execute meta-tool (`MULTI_EXECUTE_TOOL`), set `account` to that alias (or account ID)
   on **every** entry of the `tools` array that targets the app. It sits next to `tool_slug`
   and `arguments`, not inside `arguments`.

The error itself lists the valid ids, e.g. `Multiple gmail accounts connected. Specify which to
use via the 'account' field: - "gmail_xxx" - "gmail_yyy"`. Pick one from that list and repeat the
same call with `"account": "gmail_xxx"` -- verified to work. Match the id to an email address;
never invent ids. If the `list` result does not show the emails, run `GMAIL_GET_PROFILE` once per
account id (set `account`) and read `emailAddress` -- verified to return the real address.

Always name the account you actually used, by its real email from `user_info.email`, in your
answer (e.g. "from dfboni@gmail.com"). If the user asks for an email address that is not in the
`list` result (for example an address that is only connected to Calendar, not Gmail), say that
address is not connected to that app and offer the accounts that are -- never present another
account's data under the address the user asked for. Pick the same account for follow-up
questions in the same conversation unless the user changes it.

If the user did not say which account to use and the choice changes the outcome (sending mail,
creating events), ask them instead of guessing.

## An action "does not exist" (e.g. no CREATE_TASK for Google Tasks)

Action slugs are not always named the way you expect (creating a Google Task is `GOOGLETASKS_INSERT_TASK`,
which needs `title` and `tasklist_id`; get the id from `GOOGLETASKS_LIST_TASK_LISTS` or use `@default`). Never conclude an action is unavailable from guessing a slug. Call the search meta-tool
(`SEARCH_TOOLS`) with a plain-English use case such as "create a task in Google Tasks" and use
the slug it returns; if it points to a schema reference, fetch it with the schema meta-tool
(`GET_TOOL_SCHEMAS`) before executing. Re-run the search with `search_strategy: "tool_search"`
if the first plan misses the action.

## Transient connection errors

`ESP_ERR_HTTP_CONNECT` means the device could not reach the gateway (Wi-Fi/TLS hiccup). The
firmware already retries these a few times; if it still surfaces, tell the user it is a
temporary network problem and retry the same call once. It does not mean the app or account is
misconfigured.
