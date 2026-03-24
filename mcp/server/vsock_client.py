"""
vsock_client.py -- VM 侧 AF_VSOCK 客户端（ClawSpan FrameCodec 协议）

通过 AF_VSOCK 连接宿主机 vsock_server（Channel 3），收发 JSON 消息。

帧格式（ClawSpan FrameCodec）：
    length(4B 大端 uint32) + body(length 字节 UTF-8 JSON)

与旧版 AI-agent-sec 协议的区别：
    - 移除 magic(2B) / type(1B) / task_id(16B)
    - 操作类型和 task_id 全部放入 JSON body
    - Host 侧收到帧后由 FrameHandler 统一路由
"""

from __future__ import annotations

import json
import socket
import struct
import threading
from typing import Any, Dict, Optional

# ── 帧常量 ─────────────────────────────────────────────────────────────────

FRAME_HEADER_SIZE = 4           # 仅 4B 长度前缀
MAX_PAYLOAD_SIZE = 4 * 1024 * 1024  # 4 MiB，与 C++ FrameCodec 一致

# vsock 地址：宿主机 CID=2，端口=100（与 Host vsock_server 一致）
HOST_CID = 2
HOST_PORT = 100

JSON = Dict[str, Any]


# ── 编解码函数 ─────────────────────────────────────────────────────────────

def encode_frame(payload: JSON) -> bytes:
    """
    encode_frame 将 JSON payload 序列化为 ClawSpan FrameCodec 帧。

    入参:
    - payload: 待编码的 JSON 对象

    出参/返回: 完整帧的字节串（4B 长度前缀 + UTF-8 JSON body）

    raises ValueError: body 超过 MAX_PAYLOAD_SIZE
    """
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload 大小 {len(body)} 超过上限 {MAX_PAYLOAD_SIZE}")
    header = struct.pack(">I", len(body))
    return header + body


def decode_frame(data: bytes) -> tuple[Optional[JSON], int]:
    """
    decode_frame 从字节串中解析一个完整的 FrameCodec 帧。

    入参:
    - data: 输入字节串（可能包含多个帧）

    出参/返回:
    - (JSON, consumed): 成功，consumed 为已消耗字节数
    - (None, 0):        数据不足，等待更多字节

    raises ValueError: payload 超限或 JSON 解析失败
    """
    if len(data) < FRAME_HEADER_SIZE:
        return None, 0

    (length,) = struct.unpack_from(">I", data, 0)
    if length > MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload 长度超限: {length}")

    total = FRAME_HEADER_SIZE + length
    if len(data) < total:
        return None, 0

    body = data[FRAME_HEADER_SIZE:total]
    payload = json.loads(body.decode("utf-8")) if body else {}
    return payload, total


# ── VsockError ────────────────────────────────────────────────────────────

class VsockError(Exception):
    """vsock 连接或帧解析错误。"""


# ── VsockClient ───────────────────────────────────────────────────────────

class VsockClient:
    """
    VsockClient 管理到宿主机 vsock 服务器的连接，提供同步请求-响应接口。

    所有操作类型和 task_id 通过 JSON body 传递，不再使用二进制帧头。

    使用方式：
        client = VsockClient()
        client.connect()
        resp = client.send_request("list_windows", {})
        client.close()

    线程安全性：send_request 内部加锁，支持多线程并发调用。
    """

    def __init__(
        self,
        cid: int = HOST_CID,
        port: int = HOST_PORT,
    ) -> None:
        self._cid = cid
        self._port = port
        self._task_id: str = ""  # 当前任务 ID（十六进制字符串，空=无任务）
        self._sock: Optional[socket.socket] = None
        self._recv_buf = bytearray()
        self._lock = threading.Lock()

    # ── 连接管理 ──────────────────────────────────────────────────────────

    def connect(self) -> None:
        """
        connect 建立到宿主机的 AF_VSOCK 连接。

        raises VsockError: 连接失败
        """
        try:
            self._sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
            self._sock.connect((self._cid, self._port))
        except OSError as exc:
            self._sock = None
            raise VsockError(
                f"连接宿主机 vsock CID={self._cid} Port={self._port} 失败: {exc}"
            ) from exc

    def close(self) -> None:
        """close 关闭 vsock 连接。"""
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def is_connected(self) -> bool:
        """is_connected 返回当前连接是否有效。"""
        return self._sock is not None

    # ── 帧收发 ────────────────────────────────────────────────────────────

    def _send_raw(self, data: bytes) -> None:
        """发送所有字节。"""
        if self._sock is None:
            raise VsockError("未连接")
        total = len(data)
        sent = 0
        while sent < total:
            n = self._sock.send(data[sent:])
            if n == 0:
                raise VsockError("连接已断开（send 返回 0）")
            sent += n

    def _recv_frame(self) -> JSON:
        """
        阻塞读取并解析一个完整帧，返回 JSON payload。

        raises VsockError: 连接断开或帧格式错误
        """
        if self._sock is None:
            raise VsockError("未连接")

        while True:
            payload, consumed = decode_frame(bytes(self._recv_buf))
            if payload is not None:
                del self._recv_buf[:consumed]
                return payload

            chunk = self._sock.recv(4096)
            if not chunk:
                raise VsockError("连接已断开（recv 返回空）")
            self._recv_buf.extend(chunk)

    def send_request(self, method: str, params: JSON) -> JSON:
        """
        send_request 同步发送请求并等待响应。

        请求帧 JSON 格式：
            {"type": "<method>", "task_id": "...", ...params}

        响应帧 JSON 格式：
            {"type": "<method>_result", "success": true, ...data}
            {"type": "<method>_result", "success": false, "code": ..., "message": ...}

        入参:
        - method: 操作名称（如 "list_windows", "click" 等）
        - params: 请求参数（Python dict）

        出参/返回: 宿主机返回的响应 JSON

        raises VsockError:   连接断开
        raises RuntimeError: 宿主机返回失败响应
        """
        with self._lock:
            request: JSON = {"type": method, **params}
            if self._task_id:
                request["task_id"] = self._task_id
            self._send_raw(encode_frame(request))
            resp = self._recv_frame()

        if resp.get("success") is False:
            code = resp.get("error_code", -1)
            msg = resp.get("error_message", "未知错误")
            raise RuntimeError(f"宿主机拒绝请求 [{code}]: {msg}")

        return resp

    # ── 任务生命周期 ──────────────────────────────────────────────────────

    def begin_task(self, session_id: str, root_description: str) -> str:
        """
        begin_task 向 Host TaskRegistry 注册新任务，返回分配的 task_id。

        入参:
        - session_id:       用户会话 ID
        - root_description: 用户原始意图文本

        出参/返回: task_id 十六进制字符串
        """
        resp = self.send_request(
            "beginTask",
            {"session_id": session_id, "root_description": root_description},
        )
        task_id: str = resp.get("task_id", "")
        if not task_id:
            raise RuntimeError("beginTask 响应中缺少 task_id 字段")
        self._task_id = task_id
        return task_id

    def end_task(self, status: str = "success") -> None:
        """
        end_task 通知 Host TaskRegistry 当前任务已结束。

        入参:
        - status: 任务完成状态（"success" / "failure" / "user_aborted"）
        """
        self.send_request(
            "endTask",
            {"task_id": self._task_id, "status": status},
        )
        self._task_id = ""

    # ── capability 调用 ──────────────────────────────────────────────────

    def call_capability(self, capability: str, operation: str, params: JSON) -> JSON:
        """
        call_capability 统一的 capability 调用入口。

        对应 Host 侧 Channel 1 的 "capability" 消息类型，
        通过 CapabilityService 路由到具体插件。

        入参:
        - capability: 插件名（如 "ax"）
        - operation:  操作名（如 "list_windows", "click"）
        - params:     操作参数

        出参/返回: Host 返回的 result payload
        """
        resp = self.send_request(
            "capability",
            {
                "capability": capability,
                "operation": operation,
                "params": params,
            },
        )
        return resp

    # ── 便捷方法（ax 插件） ──────────────────────────────────────────────

    def list_windows(self) -> JSON:
        """list_windows 获取所有可见窗口列表。"""
        return self.call_capability("capability_ax", "list_windows", {})

    def get_ui_tree(
        self,
        window_id: str,
        max_depth: int = 8,
        include_bounds: bool = False,
    ) -> JSON:
        """获取指定窗口的 UI 控件树。"""
        return self.call_capability("capability_ax", "get_ui_tree", {
            "window_id": window_id,
            "max_depth": max_depth,
            "include_bounds": include_bounds,
        })

    def click(self, element_path: str) -> JSON:
        """点击指定路径的 UI 控件。"""
        return self.call_capability("capability_ax", "click", {"element_path": element_path})

    def double_click(self, element_path: str) -> JSON:
        """双击指定路径的 UI 控件。"""
        return self.call_capability("capability_ax", "double_click", {"element_path": element_path})

    def right_click(self, element_path: str) -> JSON:
        """右键单击指定路径的 UI 控件。"""
        return self.call_capability("capability_ax", "right_click", {"element_path": element_path})

    def set_value(self, element_path: str, value: str) -> JSON:
        """向指定路径的输入控件设置文本值。"""
        return self.call_capability("capability_ax", "set_value", {
            "element_path": element_path,
            "value": value,
        })

    def key_press(self, window_id: str, key: str) -> JSON:
        """向指定窗口发送按键事件。"""
        return self.call_capability("capability_ax", "key_press", {
            "key": key,
            "window_id": window_id,
        })

    def focus(self, element_path: str) -> JSON:
        """将输入焦点设置到指定控件。"""
        return self.call_capability("capability_ax", "focus", {"element_path": element_path})

    def scroll(self, element_path: str, direction: str, amount: int) -> JSON:
        """在指定控件上执行滚动操作。"""
        return self.call_capability("capability_ax", "scroll", {
            "element_path": element_path,
            "direction": direction,
            "amount": amount,
        })

    def activate_window(self, window_id: str) -> JSON:
        """将指定窗口置于前台并激活。"""
        return self.call_capability("capability_ax", "activate_window", {"window_id": window_id})

    def shell_exec(
        self, path: str, args: str = "", working_dir: str | None = None
    ) -> JSON:
        """在宿主机上启动可执行文件。"""
        params: JSON = {"program": path, "args": args}
        if working_dir:
            params["working_dir"] = working_dir
        return self.call_capability("capability_ax", "shell_exec", params)

    def minimize_all(self) -> JSON:
        """最小化所有可见窗口。"""
        return self.call_capability("capability_ax", "minimize_all", {})

    def wait_window(self, title_contains: str, timeout_ms: int = 10000) -> JSON:
        """轮询直到找到标题包含指定子串的窗口或超时。"""
        return self.call_capability("capability_ax", "wait_window", {
            "title_contains": title_contains,
            "timeout": timeout_ms,
        })

    def get_sysinfo(self) -> JSON:
        """获取宿主机系统信息。"""
        return self.call_capability("capability_ax", "get_sysinfo", {})

    def move_mouse(self, x: int, y: int) -> JSON:
        """将鼠标移动到屏幕绝对坐标。"""
        return self.call_capability("capability_ax", "move_mouse", {"x": x, "y": y})

    def type_keyboard(self, window_id: str, text: str) -> JSON:
        """使用键盘输入文本。"""
        return self.call_capability("capability_ax", "type_keyboard", {
            "window_id": window_id,
            "text": text,
        })

    def write_file(self, path: str, content: str, encoding: str = "utf8") -> JSON:
        """在宿主机上写入文本文件。"""
        return self.call_capability("capability_ax", "write_file", {
            "path": path,
            "content": content,
            "encoding": encoding,
        })

    def drag_window(self, window_id: str, target_x: int, target_y: int) -> JSON:
        """拖拽窗口到目标坐标。"""
        return self.call_capability("capability_ax", "drag_window", {
            "window_id": window_id,
            "target_x": target_x,
            "target_y": target_y,
        })

    def mouse_click(self, x: int, y: int) -> JSON:
        """移动鼠标到坐标并单击左键。"""
        return self.call_capability("capability_ax", "mouse_click", {"x": x, "y": y})

    def get_window_rect(self, window_id: str) -> JSON:
        """获取窗口矩形信息。"""
        return self.call_capability("capability_ax", "get_window_rect", {"window_id": window_id})

    def set_topmost(self, window_id: str, enabled: bool) -> JSON:
        """设置窗口置顶/取消置顶。"""
        return self.call_capability("capability_ax", "set_topmost", {
            "window_id": window_id,
            "enabled": enabled,
        })

    def send_security_hook(
        self,
        intent: str,
        plan: str = "",
        **kwargs: str | int | float | bool,
    ) -> JSON:
        """向宿主机汇报 Agent 意图（安全审计）。"""
        params: JSON = {"intent": intent}
        if plan:
            params["plan"] = plan
        params.update(kwargs)
        return self.call_capability("capability_ax", "security_hook", params)
