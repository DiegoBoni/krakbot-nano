---
{
  "name": "mcp_composio",
  "description": "Call Gmail, Google Drive, Google Calendar, Slack, or any other app connected through the device's 'composio' MCP server. Requires the composio MCP server to be saved and enabled in the device's MCP settings.",
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
