#!/usr/bin/env python3
"""
mcp_server.py -- ClawSpan GUI MCP Server（MCP 标准协议，运行在 VM 内）

通过 stdin/stdout 实现 MCP (Model Context Protocol) 标准 JSON-RPC 2.0 接口。
由 OpenClaw 的 acpx 扩展以 stdio 子进程方式启动。

所有 GUI 操作通过 AF_VSOCK + ClawSpan FrameCodec 发送到宿主机 daemon，
由 CapabilityService 路由到具体插件（如 ax）执行。

MCP 协议方法：
  - initialize          → 握手，返回 server capabilities
  - notifications/initialized → 客户端确认，无需响应
  - tools/list           → 返回可用工具清单
  - tools/call           → 调用指定工具

传输格式：一行一个 JSON（JSON-RPC 2.0）
"""

from __future__ import annotations

import argparse
import json
import sys
import traceback
from dataclasses import dataclass
from typing import Any, Dict

from vsock_client import VsockClient, VsockError

JSON = Dict[str, Any]

# MCP 协议版本
MCP_PROTOCOL_VERSION = "2024-11-05"

# 服务端信息
SERVER_INFO = {
    "name": "clawspan-gui",
    "version": "0.1.1",
}


@dataclass
class RpcRequest:
    id: Any
    method: str
    params: JSON


# ── I/O ──────────────────────────────────────────────────────────────────

def _read_requests():
    """从 stdin 逐行读取 JSON-RPC 请求。"""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as exc:
            _write_error(None, -32700, f"JSON 解析失败: {exc}")
            continue

        if not isinstance(obj, dict):
            _write_error(None, -32600, "无效请求: 根对象必须是 JSON 对象")
            continue

        yield obj


def _write_response(resp: JSON) -> None:
    """将响应写到 stdout（一行一个 JSON）。"""
    sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _write_result(_id: Any, result: JSON) -> None:
    _write_response({"jsonrpc": "2.0", "id": _id, "result": result})


def _write_error(_id: Any, code: int, message: str, data: JSON | None = None) -> None:
    err: JSON = {"code": code, "message": message}
    if data is not None:
        err["data"] = data
    _write_response({"jsonrpc": "2.0", "id": _id, "error": err})


# ── 工具定义 ─────────────────────────────────────────────────────────────

TOOLS = [
    {
        "name": "gui__list_windows",
        "description": "列出当前可见的顶层窗口列表。",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": [],
        },
    },
    {
        "name": "gui__begin_task",
        "description": "开始一个新的 vGUI 任务，会返回 task_id。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "session_id": {"type": "string"},
                "root_description": {"type": "string"},
            },
            "required": ["session_id", "root_description"],
        },
    },
    {
        "name": "gui__end_task",
        "description": "结束当前 vGUI 任务。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "task_id": {"type": "string"},
                "status": {"type": "string"},
            },
            "required": [],
        },
    },
    {
        "name": "gui__click",
        "description": "在指定元素路径上执行一次点击。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "element_path": {"type": "string"},
            },
            "required": ["element_path"],
        },
    },
    {
        "name": "gui__get_ui_tree",
        "description": "获取指定窗口的 UI 控件树。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window_id": {"type": "string"},
                "max_depth": {"type": "integer"},
                "include_bounds": {"type": "boolean"},
            },
            "required": ["window_id"],
        },
    },
    {
        "name": "gui__set_value",
        "description": "向指定路径的输入控件设置文本值。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "element_path": {"type": "string"},
                "value": {"type": "string"},
            },
            "required": ["element_path"],
        },
    },
    {
        "name": "gui__activate_window",
        "description": "将指定窗口置于前台并激活。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window_id": {"type": "string"},
            },
            "required": ["window_id"],
        },
    },
    {
        "name": "gui__key_press",
        "description": "向当前焦点或指定窗口发送单键按键事件。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "按键名，如 Return、Escape、a"},
                "window_id": {"type": "string", "description": "可选，目标窗口 ID；不传则发到前台窗口"},
            },
            "required": ["key"],
        },
    },
]


class McpServer:
    def __init__(self, vm_channel_transport: str = "gateway") -> None:
        self._transport_mode = vm_channel_transport
        self._selected_transport = ""
        self._client: Any | None = None
        self._initialized = False

    def _create_grpc_client(self):
        from vm_channel_grpc_client import VmGrpcClient

        return VmGrpcClient()

    def _connect_with_transport(self, transport: str) -> None:
        if transport in ("legacy", "gateway"):
            client = VsockClient()
        elif transport == "grpc":
            client = self._create_grpc_client()
        else:
            raise ValueError(f"未知 transport: {transport}")

        try:
            client.connect()
        except Exception:
            try:
                client.close()
            except Exception:
                pass
            raise

        self._client = client
        self._selected_transport = transport

    def _ensure_connected(self) -> None:
        if self._client is not None and self._client.is_connected():
            return

        if self._client is not None and not self._client.is_connected():
            self._client.connect()
            return

        if self._transport_mode in ("legacy", "gateway"):
            self._connect_with_transport(self._transport_mode)
            return

        if self._transport_mode == "grpc":
            self._connect_with_transport("grpc")
            return

        try:
            self._connect_with_transport("gateway")
            return
        except Exception:
            pass
        try:
            self._connect_with_transport("grpc")
            return
        except Exception:
            pass
        self._connect_with_transport("legacy")

    def handle_initialize(self, req: RpcRequest) -> JSON:
        self._initialized = True
        return {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {
                "tools": {},
            },
            "serverInfo": SERVER_INFO,
        }

    def handle_tools_list(self, req: RpcRequest) -> JSON:
        return {"tools": TOOLS}

    def handle_tools_call(self, req: RpcRequest) -> JSON:
        params = req.params or {}
        name = params.get("name")
        args = params.get("arguments") or {}

        if not isinstance(name, str):
            raise ValueError("params.name 必须是字符串")
        if not isinstance(args, dict):
            raise ValueError("params.arguments 必须是对象")

        self._ensure_connected()
        assert self._client is not None

        if name == "gui__list_windows":
            result = self._client.list_windows()
        elif name == "gui__begin_task":
            session_id = str(args.get("session_id", "session-unknown"))
            root_desc = str(args.get("root_description", ""))
            task_id = self._client.begin_task(
                session_id=session_id,
                root_description=root_desc,
            )
            result = {"task_id": task_id}
        elif name == "gui__end_task":
            task_id = args.get("task_id", "")
            status = str(args.get("status", "success"))
            if task_id:
                self._client._task_id = str(task_id)
            self._client.end_task(status=status)
            result = {"status": status, "ended": True}
        elif name == "gui__click":
            element_path = args.get("element_path")
            if not isinstance(element_path, str) or not element_path:
                raise ValueError("gui__click 需要非空字符串参数 element_path")
            result = self._client.click(element_path)
        elif name == "gui__get_ui_tree":
            window_id = args.get("window_id")
            if not isinstance(window_id, str) or not window_id:
                raise ValueError("gui__get_ui_tree 需要非空字符串参数 window_id")
            result = self._client.get_ui_tree(
                window_id=window_id,
                max_depth=int(args.get("max_depth", 8)),
                include_bounds=bool(args.get("include_bounds", False)),
            )
        elif name == "gui__set_value":
            element_path = args.get("element_path")
            if not isinstance(element_path, str) or not element_path:
                raise ValueError("gui__set_value 需要非空字符串参数 element_path")
            result = self._client.set_value(element_path, str(args.get("value", "")))
        elif name == "gui__activate_window":
            window_id = args.get("window_id")
            if not isinstance(window_id, str) or not window_id:
                raise ValueError("gui__activate_window 需要非空字符串参数 window_id")
            result = self._client.activate_window(window_id)
        elif name == "gui__key_press":
            key = args.get("key")
            if not isinstance(key, str) or not key:
                raise ValueError("gui__key_press 需要非空字符串参数 key")
            result = self._client.key_press(
                window_id=str(args.get("window_id", "")),
                key=key,
            )
        else:
            raise ValueError(f"未知工具: {name}")

        return {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps(result, ensure_ascii=False),
                }
            ],
        }


def _dispatch(server: McpServer, obj: JSON) -> None:
    _id = obj.get("id")
    method = obj.get("method")
    params = obj.get("params") or {}

    if not isinstance(method, str):
        _write_error(_id, -32600, "无效请求: method 必须是字符串")
        return

    if _id is None and method.startswith("notifications/"):
        return

    req = RpcRequest(id=_id, method=method, params=params)

    try:
        if method == "initialize":
            result = server.handle_initialize(req)
        elif method == "tools/list":
            result = server.handle_tools_list(req)
        elif method == "tools/call":
            result = server.handle_tools_call(req)
        else:
            _write_error(_id, -32601, f"未知方法: {method}")
            return
        _write_result(_id, result)
    except VsockError as exc:
        _write_error(_id, 1001, f"VM Channel 传输错误: {exc}")
    except Exception as exc:  # noqa: BLE001
        _write_error(
            _id,
            1000,
            f"服务器内部错误: {exc}",
            data={"traceback": traceback.format_exc()},
        )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ClawSpan GUI MCP Server")
    parser.add_argument(
        "--vm-channel-transport",
        choices=("gateway", "legacy", "grpc", "auto"),
        default="gateway",
        help="选择 VM Channel 传输模式，默认 gateway",
    )
    args, _ = parser.parse_known_args(argv)
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    server = McpServer(vm_channel_transport=args.vm_channel_transport)
    for obj in _read_requests():
        _dispatch(server, obj)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
