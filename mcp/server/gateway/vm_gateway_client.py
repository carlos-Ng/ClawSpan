from __future__ import annotations

import json
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Dict, Optional


JSON = Dict[str, Any]


class VmGatewayError(Exception):
    """VM gateway 通用异常。"""


@dataclass
class Envelope:
    version: str = "v1"
    message_id: str = ""
    session_id: str = ""
    trace_id: str = ""
    task_id: str = ""
    stream_id: str = ""
    timestamp_ms: int = 0
    payload: JSON = field(default_factory=dict)

    def to_json(self) -> JSON:
        return {
            "version": self.version,
            "message_id": self.message_id,
            "session_id": self.session_id,
            "trace_id": self.trace_id,
            "task_id": self.task_id,
            "stream_id": self.stream_id,
            "timestamp_ms": self.timestamp_ms,
            "payload": self.payload,
        }

    @staticmethod
    def from_json(data: JSON) -> "Envelope":
        payload = data.get("payload", {})
        if not isinstance(payload, dict):
            raise VmGatewayError("Envelope payload 必须是对象")
        return Envelope(
            version=str(data.get("version", "v1")),
            message_id=str(data.get("message_id", "")),
            session_id=str(data.get("session_id", "")),
            trace_id=str(data.get("trace_id", "")),
            task_id=str(data.get("task_id", "")),
            stream_id=str(data.get("stream_id", "")),
            timestamp_ms=int(data.get("timestamp_ms", 0)),
            payload=dict(payload),
        )


class VmGatewayClient:
    """VmGatewayClient 管理 VM 到 Host gateway 的会话状态。"""

    SUPPORTED_VERSIONS = {"v1"}

    def __init__(self) -> None:
        self._connected = False
        self._session_id = ""
        self._lock = threading.Lock()
        self._outbox: list[JSON] = []

    def connect(self, session_id: Optional[str] = None) -> None:
        with self._lock:
            if self._connected:
                return
            self._session_id = session_id or f"gw-{uuid.uuid4().hex}"
            self._connected = True

    def reconnect(self) -> None:
        with self._lock:
            if self._connected:
                return
            if not self._session_id:
                self._session_id = f"gw-{uuid.uuid4().hex}"
            self._connected = True

    def close(self) -> None:
        with self._lock:
            self._connected = False

    def is_connected(self) -> bool:
        with self._lock:
            return self._connected

    def session_id(self) -> str:
        with self._lock:
            return self._session_id

    def build_envelope(self, payload: JSON, trace_id: str = "", task_id: str = "") -> Envelope:
        if not isinstance(payload, dict) or not payload:
            raise VmGatewayError("payload 必须是非空对象")
        with self._lock:
            if not self._connected:
                raise VmGatewayError("gateway 未连接")
            return Envelope(
                message_id=uuid.uuid4().hex,
                session_id=self._session_id,
                trace_id=trace_id,
                task_id=task_id,
                timestamp_ms=int(time.time() * 1000),
                payload=payload,
            )

    def send(self, envelope: Envelope) -> None:
        with self._lock:
            if not self._connected:
                raise VmGatewayError("gateway 未连接")
            self._outbox.append(envelope.to_json())

    def send_heartbeat(self, auto_reconnect: bool = True) -> Envelope:
        if auto_reconnect and not self.is_connected():
            self.reconnect()

        env = self.build_envelope(
            {
                "heartbeat": {
                    "sent_at_ms": int(time.time() * 1000),
                }
            }
        )
        self.send(env)
        return env

    def create_ack_envelope(
        self,
        ack_message_id: str,
        accepted: bool,
        reason: str = "",
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not ack_message_id:
            raise VmGatewayError("ack_message_id 不能为空")
        return self.build_envelope(
            {
                "ack": {
                    "ack_message_id": ack_message_id,
                    "accepted": bool(accepted),
                    "reason": str(reason),
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    def create_control_envelope(
        self,
        control_type: str,
        message: str = "",
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not control_type:
            raise VmGatewayError("control_type 不能为空")
        return self.build_envelope(
            {
                "control": {
                    "control_type": control_type,
                    "message": message,
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    def create_handshake_envelope(self, auto_reconnect: bool = True) -> Envelope:
        if auto_reconnect and not self.is_connected():
            self.reconnect()

        with self._lock:
            if not self._connected:
                raise VmGatewayError("gateway 未连接")
            session_id = self._session_id

        return self.build_envelope(
            {
                "control": {
                    "control_type": "handshake",
                    "session_id": session_id,
                }
            }
        )

    def create_command_envelope(
        self,
        command_id: str,
        capability: str,
        operation: str,
        params: JSON,
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not command_id or not capability or not operation:
            raise VmGatewayError("command_id/capability/operation 不能为空")
        if not isinstance(params, dict):
            raise VmGatewayError("params 必须是对象")
        return self.build_envelope(
            {
                "command": {
                    "command_id": command_id,
                    "capability": capability,
                    "operation": operation,
                    "params_json": json.dumps(params, ensure_ascii=False),
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    def create_command_result_envelope(
        self,
        command_id: str,
        success: bool,
        result: JSON | None = None,
        error_code: int = 0,
        error_message: str = "",
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not command_id:
            raise VmGatewayError("command_id 不能为空")
        if result is not None and not isinstance(result, dict):
            raise VmGatewayError("result 必须是对象")
        return self.build_envelope(
            {
                "command_result": {
                    "command_id": command_id,
                    "success": bool(success),
                    "error_code": int(error_code),
                    "error_message": str(error_message),
                    "result_json": json.dumps(result or {}, ensure_ascii=False),
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    def create_log_event_envelope(
        self,
        event_id: str,
        level: str,
        source: str,
        message: str,
        fields: JSON | None = None,
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not event_id or not level or not source:
            raise VmGatewayError("event_id/level/source 不能为空")
        if fields is not None and not isinstance(fields, dict):
            raise VmGatewayError("fields 必须是对象")
        merged_fields = dict(fields or {})
        merged_fields["_context"] = {
            "trace_id": trace_id,
            "task_id": task_id,
        }

        return self.build_envelope(
            {
                "log_event": {
                    "event_id": event_id,
                    "level": level,
                    "source": source,
                    "message": message,
                    "fields_json": json.dumps(merged_fields, ensure_ascii=False),
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    def create_risk_event_envelope(
        self,
        event_id: str,
        severity: str,
        behavior_type: str,
        detail: str,
        extra: JSON | None = None,
        trace_id: str = "",
        task_id: str = "",
    ) -> Envelope:
        if not event_id or not severity or not behavior_type:
            raise VmGatewayError("event_id/severity/behavior_type 不能为空")
        if extra is not None and not isinstance(extra, dict):
            raise VmGatewayError("extra 必须是对象")
        merged_extra = dict(extra or {})
        merged_extra["_context"] = {
            "trace_id": trace_id,
            "task_id": task_id,
        }

        return self.build_envelope(
            {
                "risk_event": {
                    "event_id": event_id,
                    "severity": severity,
                    "behavior_type": behavior_type,
                    "detail": detail,
                    "extra_json": json.dumps(merged_extra, ensure_ascii=False),
                }
            },
            trace_id=trace_id,
            task_id=task_id,
        )

    @staticmethod
    def parse_command_result_payload(payload: JSON) -> JSON:
        if not isinstance(payload, dict):
            raise VmGatewayError("command_result 负载必须是对象")
        command_id = str(payload.get("command_id", ""))
        if not command_id:
            raise VmGatewayError("command_result 缺少 command_id")
        result_json = str(payload.get("result_json", "{}"))
        try:
            parsed_result = json.loads(result_json) if result_json else {}
        except json.JSONDecodeError as exc:
            raise VmGatewayError(f"result_json 非法: {exc}") from exc
        return {
            "command_id": command_id,
            "success": bool(payload.get("success", False)),
            "error_code": int(payload.get("error_code", 0)),
            "error_message": str(payload.get("error_message", "")),
            "result": parsed_result,
        }

    def apply_handshake_ack(self, ack_payload: JSON) -> None:
        if not isinstance(ack_payload, dict):
            raise VmGatewayError("握手 ACK 负载必须是对象")
        accepted = bool(ack_payload.get("accepted", False))
        session_id = str(ack_payload.get("session_id", ""))
        if not accepted:
            raise VmGatewayError("握手被 Host 拒绝")
        if not session_id:
            raise VmGatewayError("握手 ACK 缺少 session_id")

        with self._lock:
            self._session_id = session_id
            self._connected = True

    def pop_pending(self) -> list[JSON]:
        with self._lock:
            pending = list(self._outbox)
            self._outbox.clear()
            return pending

    @staticmethod
    def dumps(envelope: Envelope) -> str:
        return json.dumps(envelope.to_json(), ensure_ascii=False)

    @staticmethod
    def loads(text: str) -> Envelope:
        try:
            data = json.loads(text)
        except json.JSONDecodeError as exc:
            raise VmGatewayError(f"Envelope JSON 无法解析: {exc}") from exc
        if not isinstance(data, dict):
            raise VmGatewayError("Envelope JSON 根节点必须是对象")
        try:
            envelope = Envelope.from_json(data)
        except (TypeError, ValueError, VmGatewayError) as exc:
            raise VmGatewayError(f"Envelope 字段非法: {exc}") from exc
        VmGatewayClient.validate_envelope_version(envelope.version)
        return envelope

    @classmethod
    def validate_envelope_version(cls, version: str) -> None:
        if not version:
            raise VmGatewayError("Envelope version 不能为空")
        if version not in cls.SUPPORTED_VERSIONS:
            raise VmGatewayError(f"不支持的 Envelope version: {version}")

