from __future__ import annotations

import json
import threading
from typing import Any, Dict

from vm_channel_tunnel import VmChannelTunnel
from vsock_client import VsockError

JSON = Dict[str, Any]

try:
    import grpc  # type: ignore
except ImportError as exc:  # pragma: no cover - depends on runtime image
    grpc = None
    _GRPC_IMPORT_ERROR = exc
else:
    _GRPC_IMPORT_ERROR = None


class VmGrpcError(VsockError):
    pass


class VmGrpcClient:
    def __init__(
        self,
        local_host: str = "127.0.0.1",
        local_port: int = 50052,
        host_cid: int = 2,
        host_port: int = 101,
        connect_timeout_secs: float = 3.0,
    ) -> None:
        self._local_host = local_host
        self._local_port = local_port
        self._connect_timeout_secs = connect_timeout_secs
        self._task_id = ""
        self._channel = None
        self._stub = None
        self._lock = threading.Lock()
        self._tunnel = VmChannelTunnel(
            local_host=local_host,
            local_port=local_port,
            host_cid=host_cid,
            host_port=host_port,
        )
        self._pb2 = None
        self._pb2_grpc = None

    def _load_grpc_modules(self) -> None:
        if grpc is None:
            raise VmGrpcError(f"grpcio 未安装，无法启用 gRPC VM Channel: {_GRPC_IMPORT_ERROR}")

        if self._pb2 is not None and self._pb2_grpc is not None:
            return

        try:
            from generated import vm_channel_pb2, vm_channel_pb2_grpc
        except ImportError as exc:  # pragma: no cover - depends on runtime image
            raise VmGrpcError(
                f"protobuf stubs 未安装，无法启用 gRPC VM Channel: {exc}"
            ) from exc

        self._pb2 = vm_channel_pb2
        self._pb2_grpc = vm_channel_pb2_grpc

    def connect(self) -> None:
        self._load_grpc_modules()
        assert grpc is not None
        assert self._pb2_grpc is not None

        if self.is_connected():
            return

        try:
            self._tunnel.start()
            target = f"{self._local_host}:{self._local_port}"
            self._channel = grpc.insecure_channel(target)
            grpc.channel_ready_future(self._channel).result(timeout=self._connect_timeout_secs)
            self._stub = self._pb2_grpc.VmChannelServiceStub(self._channel)
        except Exception as exc:
            self.close()
            raise VmGrpcError(f"连接 gRPC VM Channel 失败: {exc}") from exc

    def close(self) -> None:
        if self._channel is not None:
            try:
                self._channel.close()
            except Exception:
                pass
        self._channel = None
        self._stub = None
        self._tunnel.stop()

    def is_connected(self) -> bool:
        return self._channel is not None and self._stub is not None

    def begin_task(self, session_id: str, root_description: str) -> str:
        self._ensure_connected()
        assert self._stub is not None
        assert self._pb2 is not None

        request = self._pb2.BeginTaskRequest(
            session_id=session_id,
            root_description=root_description,
            description="",
            parent_task_id="",
        )
        response = self._stub.BeginTask(request)
        if not response.task_id:
            raise VmGrpcError("BeginTask 响应缺少 task_id")
        self._task_id = response.task_id
        return response.task_id

    def end_task(self, status: str = "success") -> None:
        self._ensure_connected()
        assert self._stub is not None
        assert self._pb2 is not None

        request = self._pb2.EndTaskRequest(
            task_id=self._task_id,
            success=(status == "success"),
        )
        response = self._stub.EndTask(request)
        if not response.success:
            raise VmGrpcError("EndTask 返回 success=false")
        self._task_id = ""

    def call_capability(self, capability: str, operation: str, params: JSON) -> JSON:
        self._ensure_connected()
        assert self._stub is not None
        assert self._pb2 is not None

        with self._lock:
            request = self._pb2.CallCapabilityRequest(
                task_id=self._task_id,
                capability=capability,
                operation=operation,
                params_json=json.dumps(params, ensure_ascii=False),
            )
            response = self._stub.CallCapability(request)

        if not response.success:
            raise RuntimeError(
                f"宿主机拒绝请求[{response.error_code}]: {response.error_message}"
            )

        try:
            result = json.loads(response.result_json) if response.result_json else {}
        except json.JSONDecodeError as exc:
            raise VmGrpcError(f"CallCapability result_json 非法: {exc}") from exc

        return {
            "success": True,
            "result": result,
        }

    def list_windows(self) -> JSON:
        return self.call_capability("capability_ax", "list_windows", {})

    def get_ui_tree(
        self,
        window_id: str,
        max_depth: int = 8,
        include_bounds: bool = False,
    ) -> JSON:
        return self.call_capability(
            "capability_ax",
            "get_ui_tree",
            {
                "window_id": window_id,
                "max_depth": max_depth,
                "include_bounds": include_bounds,
            },
        )

    def click(self, element_path: str) -> JSON:
        return self.call_capability("capability_ax", "click", {"element_path": element_path})

    def double_click(self, element_path: str) -> JSON:
        return self.call_capability(
            "capability_ax",
            "double_click",
            {"element_path": element_path},
        )

    def right_click(self, element_path: str) -> JSON:
        return self.call_capability(
            "capability_ax",
            "right_click",
            {"element_path": element_path},
        )

    def set_value(self, element_path: str, value: str) -> JSON:
        return self.call_capability(
            "capability_ax",
            "set_value",
            {
                "element_path": element_path,
                "value": value,
            },
        )

    def key_press(self, window_id: str, key: str) -> JSON:
        return self.call_capability(
            "capability_ax",
            "key_press",
            {
                "key": key,
                "window_id": window_id,
            },
        )

    def focus(self, element_path: str) -> JSON:
        return self.call_capability("capability_ax", "focus", {"element_path": element_path})

    def scroll(self, element_path: str, direction: str, amount: int) -> JSON:
        return self.call_capability(
            "capability_ax",
            "scroll",
            {
                "element_path": element_path,
                "direction": direction,
                "amount": amount,
            },
        )

    def activate_window(self, window_id: str) -> JSON:
        return self.call_capability(
            "capability_ax",
            "activate_window",
            {"window_id": window_id},
        )

    def _ensure_connected(self) -> None:
        if not self.is_connected():
            self.connect()

