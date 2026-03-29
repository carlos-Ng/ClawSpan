---
name: clawspan-gui
description: "Windows GUI automation via ClawSpan MCP Server. Use when: (1) listing or inspecting visible windows on the Windows host, (2) clicking, double-clicking, or right-clicking UI elements, (3) typing text into input fields or sending keystrokes, (4) activating, focusing, minimizing, or dragging windows, (5) scrolling within controls, (6) reading UI control trees for accessibility inspection, (7) capturing system info or screen coordinates, (8) executing programs on the Windows host, (9) writing files on the host filesystem. NOT for: Linux/macOS GUI operations, web browser automation (use browser tooling), or tasks that don't involve the Windows desktop."
metadata:
  {
    "openclaw":
      {
        "emoji": "🖥️",
        "os": ["linux"],
        "requires": { "bins": ["python3"] },
      },
  }
---

# ClawSpan GUI Skill

Control the Windows host desktop from inside the WSL2 VM via ClawSpan's MCP tools.
All GUI operations are sent through AF_VSOCK to the ClawSpan daemon running on the host,
which routes them to the `ax` capability plugin for execution.

## Setup

The ClawSpan MCP Server must be registered in OpenClaw's acpx plugin config.
Add to `~/.openclaw/openclaw.json`:

```json
{
  "plugins": {
    "entries": {
      "acpx": {
        "config": {
          "mcpServers": {
            "clawspan-gui": {
              "command": "python3",
              "args": ["/path/to/clawspan/mcp/server/launch_mcp_server.py"]
            }
          }
        }
      }
    }
  }
}
```

If you need to switch VM Channel transport during the dual-stack migration, set
`CLAWSPAN_VM_CHANNEL_TRANSPORT=legacy|grpc|auto` in the environment before launching OpenClaw.

The host-side ClawSpan daemon must be running and the vsock server listening on port 100.

## Available Tools

### Task Lifecycle

- `gui__begin_task` — Start a new task session. Returns a `task_id` used for security tracking.
  Required params: `session_id`, `root_description`.
- `gui__end_task` — End the current task. Optional params: `task_id`, `status`.

Always call `gui__begin_task` before performing GUI operations, and `gui__end_task` when done.
The task context enables the host's security chain to audit and authorize operations.

### Window Discovery

- `gui__list_windows` — List all visible top-level windows. Use this first to discover available targets.
- `gui__get_ui_tree` — Get the accessibility control tree of a specific window.
  Required: `window_id`. Optional: `max_depth` (default 8), `include_bounds`.
- `gui__activate_window` — Bring a window to the foreground.
  Required: `window_id`.

### Interaction

- `gui__click` — Click a UI element by accessibility path. Required: `element_path`.
- `gui__set_value` — Set text in an input field. Required: `element_path`. Optional: `value`.
- `gui__key_press` — Send a single key press to the foreground window or a specific window. Required: `key`. Optional: `window_id` (omit to target foreground).  
  **Special key names** (非字母键请用下表，字母/数字直接传字符如 `a`、`1`)。传参时**不要**用方括号包裹，直接写字符串即可，例如 `"key": "Return"`（正确），不要写 `"key": "[Return]"`（错误）。

  | 含义     | key 取值 |
  |----------|----------|
  | 回车     | `Return` 或 `Enter` |
  | 退格     | `BackSpace` 或 `Backspace` |
  | 空格     | `Space` |
  | 制表     | `Tab` |
  | Esc      | `Escape` 或 `Esc` |
  | 删除     | `Delete` 或 `Del` |
  | 方向 ↑↓←→ | `Up` / `Down` / `Left` / `Right` |
  | 行首/行尾 | `Home` / `End` |
  | 上/下页   | `PageUp` / `PageDown` |
  | 插入     | `Insert` |
  | 功能键 F1–F12 | `F1` … `F12` |
  | 修饰键（多用于组合键） | `Ctrl` 或 `Control` · `Alt` · `Shift` · `Win` · `Cmd` 或 `Command`（同 Win）· `Option`（同 Alt）|

### Typical Workflow

```
1. gui__begin_task  → get task_id
2. gui__list_windows  → find target window
3. gui__get_ui_tree  → discover element paths
4. gui__click / gui__set_value / gui__key_press  → interact
5. gui__end_task  → close session
```

## Notes

- Element paths come from `gui.get_ui_tree`. Always inspect the tree before clicking.
- The host security chain may prompt the user for confirmation on sensitive operations.
- All operations are synchronous — each call blocks until the host responds.
- Connection errors (vsock down, daemon not running) surface as JSON-RPC error responses.
