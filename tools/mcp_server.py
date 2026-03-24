#!/usr/bin/env python3
"""
mcp_server.py — ClawSpan MCP Server（Windows Named Pipe 版）

将 AI Agent 通过 MCP 协议发出的工具调用桥接到 crew-shell-service daemon，
所有操作经过 SecurityChain 审查后再执行 AX 操作。

传输协议：stdio（MCP 标准传输，支持 Claude Desktop 等客户端）

用法：
  python tools\\mcp_server.py [pipe_path]
  python tools\\mcp_server.py \\\\.\\pipe\\crew-shell-service

环境变量：
  CLAWSPAN_SOCK   crew-shell-service Named Pipe 路径（默认 \\\\.\\pipe\\crew-shell-service）

与 Claude Desktop 集成（%APPDATA%\\Claude\\claude_desktop_config.json）：
  {
    "mcpServers": {
      "clawspan": {
        "command": "python",
        "args": ["C:\\\\path\\\\to\\\\ClawSpan\\\\tools\\\\mcp_server.py"]
      }
    }
  }
"""

import json
import os
import struct
import sys
import threading
from typing import Any, Optional

# ── Windows Named Pipe 客户端依赖 ─────────────────────────────────────────────
try:
    import win32file
    import win32pipe
    import pywintypes
except ImportError:
    print(
        "[ERROR] pywin32 not installed.\n"
        "Install with:  pip install pywin32",
        file=sys.stderr,
    )
    sys.exit(1)

# ── MCP SDK ───────────────────────────────────────────────────────────────────
try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    print(
        "[ERROR] mcp package not installed.\n"
        "Install with:  pip install mcp",
        file=sys.stderr,
    )
    sys.exit(1)

# ── 配置 ──────────────────────────────────────────────────────────────────────

DAEMON_PIPE = os.environ.get("CLAWSPAN_SOCK", r"\\.\pipe\crew-shell-service")
CAPABILITY  = "capability_ax"

if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    DAEMON_PIPE = sys.argv[1]

# ── IPC 客户端（Length-prefix framing，Windows Named Pipe） ────────────────────

class IpcClient:
    """
    持久化 Named Pipe 客户端，发送 JSON-RPC 2.0 帧到 crew-shell-service。

    线程安全：通过 _lock 串行化并发调用（MCP server 可能从多线程调用 tool）。
    失败自动重连一次。
    """

    def __init__(self, path: str):
        self._path    = path
        self._handle  = None          # win32file HANDLE
        self._req_id  = 0
        self._lock    = threading.Lock()

    def _connect(self):
        """连接到 Named Pipe，返回 win32file handle。"""
        while True:
            try:
                handle = win32file.CreateFile(
                    self._path,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0,          # dwShareMode
                    None,       # lpSecurityAttributes
                    win32file.OPEN_EXISTING,
                    0,
                    None,
                )
                # 设置为字节读取模式
                win32pipe.SetNamedPipeHandleState(
                    handle,
                    win32pipe.PIPE_READMODE_BYTE,
                    None, None,
                )
                return handle
            except pywintypes.error as e:
                import winerror
                if e.args[0] == winerror.ERROR_PIPE_BUSY:
                    # 管道忙，等待后重试
                    win32pipe.WaitNamedPipe(self._path, 1000)
                    continue
                raise

    def _close(self) -> None:
        if self._handle is not None:
            try:
                win32file.CloseHandle(self._handle)
            except Exception:
                pass
            self._handle = None

    def _send_recv(self, method: str, params: dict) -> dict:
        """发送一条 JSON-RPC 2.0 请求并读取响应（不含重试逻辑）。"""
        self._req_id += 1
        req  = {"jsonrpc": "2.0", "id": self._req_id, "method": method, "params": params}
        body = json.dumps(req).encode("utf-8")
        # 写入 4 字节大端长度前缀 + JSON 正文
        win32file.WriteFile(self._handle, struct.pack(">I", len(body)) + body)

        # 读取 4 字节长度头
        hdr = b""
        while len(hdr) < 4:
            _, chunk = win32file.ReadFile(self._handle, 4 - len(hdr))
            if not chunk:
                raise ConnectionError("daemon disconnected while reading header")
            hdr += chunk
        length = struct.unpack(">I", hdr)[0]

        # 读取正文
        resp_body = b""
        while len(resp_body) < length:
            _, chunk = win32file.ReadFile(self._handle, length - len(resp_body))
            if not chunk:
                raise ConnectionError("daemon disconnected while reading body")
            resp_body += chunk

        return json.loads(resp_body.decode("utf-8"))

    def call(self, method: str, params: dict) -> dict:
        """线程安全的 RPC 调用，失败自动重连一次。"""
        with self._lock:
            for attempt in range(2):
                try:
                    if self._handle is None:
                        self._handle = self._connect()
                    return self._send_recv(method, params)
                except (OSError, ConnectionError, pywintypes.error) as exc:
                    self._close()
                    if attempt == 0:
                        continue
                    return {
                        "error": {
                            "code":    -32000,
                            "message": f"IPC connection failed: {exc}",
                        }
                    }
        return {}

    def ax_call(self, operation: str, params: dict | None = None) -> dict:
        return self.call(f"{CAPABILITY}.{operation}", params or {})


_ipc = IpcClient(DAEMON_PIPE)

# ── 结果格式化 ────────────────────────────────────────────────────────────────

def _fmt(resp: dict) -> str:
    """将 JSON-RPC 响应转换为易读字符串，供 MCP 工具返回。"""
    if "result" in resp:
        r = resp["result"]
        return r if isinstance(r, str) else json.dumps(r, ensure_ascii=False, indent=2)
    if "error" in resp:
        err   = resp["error"]
        code  = err.get("code", "?")
        msg   = err.get("message", "")
        prefix = "[SECURITY:DENIED]" if code in (-32001, -32002, -32003) else "[ERROR]"
        return f"{prefix} ({code}) {msg}"
    return json.dumps(resp, ensure_ascii=False)


# ── MCP Server ────────────────────────────────────────────────────────────────

mcp = FastMCP(
    "clawspan",
    description=(
        "Secure Windows GUI automation gateway. "
        "All operations pass through an independent security chain before execution. "
        "Dangerous operations are blocked or require explicit user confirmation."
    ),
)

# ── 窗口查询 ──────────────────────────────────────────────────────────────────

@mcp.tool()
def list_windows() -> str:
    """List all accessible application windows on the host machine.

    Returns a JSON array of objects with fields:
      window_id   - opaque identifier used by other tools
      title       - window title
      app_name    - owning application name
    """
    return _fmt(_ipc.ax_call("list_windows"))


@mcp.tool()
def get_ui_tree(
    window_id: str,
    max_depth: int = 5,
    include_bounds: bool = False,
) -> str:
    """Get the UI element tree of a window in AXT compact text format.

    Args:
        window_id:      Window identifier from list_windows.
        max_depth:      Maximum tree depth to traverse (1-15, default 5).
        include_bounds: If true, append screen coordinates to each element.
    """
    params: dict[str, Any] = {"window_id": window_id, "max_depth": max_depth}
    if include_bounds:
        params["include_bounds"] = True
    return _fmt(_ipc.ax_call("get_ui_tree", params))


# ── 元素操作（需要用户确认） ─────────────────────────────────────────────────

@mcp.tool()
def click(element_path: str) -> str:
    """Click a UI element.

    ⚠ Requires user confirmation per security policy.

    Args:
        element_path: AXT element path (e.g. /w123_1A2B0/AXButton[OK]).
                      Obtain from get_ui_tree.
    """
    return _fmt(_ipc.ax_call("click", {"element_path": element_path}))


@mcp.tool()
def double_click(element_path: str) -> str:
    """Double-click a UI element.

    ⚠ Requires user confirmation per security policy.

    Args:
        element_path: AXT element path from get_ui_tree.
    """
    return _fmt(_ipc.ax_call("double_click", {"element_path": element_path}))


@mcp.tool()
def right_click(element_path: str) -> str:
    """Right-click a UI element (context menu).

    ⚠ Requires user confirmation per security policy.

    Args:
        element_path: AXT element path from get_ui_tree.
    """
    return _fmt(_ipc.ax_call("right_click", {"element_path": element_path}))


@mcp.tool()
def set_value(element_path: str, value: str) -> str:
    """Set the text value of an editable UI element (text field, search box, etc.).

    ⚠ Requires user confirmation per security policy.
      Content matching sensitive patterns (password, token, api_key…) triggers
      an additional confirmation with a security warning.

    Args:
        element_path: AXT element path from get_ui_tree.
        value:        Text to write into the element.
    """
    return _fmt(_ipc.ax_call("set_value", {"element_path": element_path, "value": value}))


@mcp.tool()
def focus(element_path: str) -> str:
    """Move keyboard focus to a UI element.

    Args:
        element_path: AXT element path from get_ui_tree.
    """
    return _fmt(_ipc.ax_call("focus", {"element_path": element_path}))


@mcp.tool()
def scroll(element_path: str, direction: str, amount: int = 3) -> str:
    """Scroll a scrollable UI element.

    Args:
        element_path: AXT element path from get_ui_tree.
        direction:    One of: up, down, left, right.
        amount:       Number of scroll notches (default 3; each notch = one mouse wheel click).
    """
    return _fmt(_ipc.ax_call("scroll", {
        "element_path": element_path,
        "direction":    direction,
        "amount":       amount,
    }))


# ── 键盘操作 ──────────────────────────────────────────────────────────────────

@mcp.tool()
def key_press(window_id: str, key: str) -> str:
    """Send a single key press to a window.

    ⚠ Requires user confirmation per security policy.

    Args:
        window_id: Target window from list_windows.
        key:       Key name: Return, Escape, Space, Tab, Delete, BackSpace,
                   Up, Down, Left, Right, Home, End, PageUp, PageDown,
                   F1-F12, or a single printable character such as "a", "1".
    """
    return _fmt(_ipc.ax_call("key_press", {"window_id": window_id, "key": key}))


@mcp.tool()
def key_combo(window_id: str, keys: list[str]) -> str:
    """Send a keyboard shortcut (modifier + key) to a window.

    🚫 BLOCKED by security policy — keyboard shortcuts are restricted.

    Args:
        window_id: Target window from list_windows.
        keys:      Key sequence, modifiers first then the main key.
                   Modifiers: Ctrl, Alt, Shift, Win (Cmd is alias for Win).
                   Example: ["Ctrl", "C"], ["Ctrl", "Shift", "Z"].
    """
    return _fmt(_ipc.ax_call("key_combo", {"window_id": window_id, "keys": keys}))


# ── 窗口管理 ──────────────────────────────────────────────────────────────────

@mcp.tool()
def activate_window(window_id: str) -> str:
    """Bring a window to the foreground and give it focus.

    Note: This will steal foreground focus from the user's current activity.

    Args:
        window_id: Window identifier from list_windows.
    """
    return _fmt(_ipc.ax_call("activate", {"window_id": window_id}))


# ── 入口 ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    mcp.run()
