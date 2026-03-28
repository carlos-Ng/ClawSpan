from __future__ import annotations

import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from gateway.vm_gateway_client import Envelope, VmGatewayClient  # noqa: E402


class _FakeHostGateway:
    def __init__(self) -> None:
        self.sessions: dict[str, int] = {}
        self.heartbeat_count = 0

    def handle_vm_envelope(self, env: Envelope) -> Envelope | None:
        payload = env.payload

        if "control" in payload and payload["control"].get("control_type") == "handshake":
            session_id = payload["control"].get("session_id") or env.session_id
            self.sessions[session_id] = self.sessions.get(session_id, 0) + 1
            return Envelope(
                version="v1",
                message_id=f"ack-{env.message_id}",
                session_id=session_id,
                trace_id=env.trace_id,
                task_id=env.task_id,
                timestamp_ms=env.timestamp_ms + 1,
                payload={
                    "ack": {
                        "ack_message_id": env.message_id,
                        "accepted": True,
                        "reason": "handshake accepted",
                        "session_id": session_id,
                    }
                },
            )

        if "heartbeat" in payload:
            self.heartbeat_count += 1
            self.sessions[env.session_id] = self.sessions.get(env.session_id, 0) + 1
            return None

        return None


def _flush(vm: VmGatewayClient, host: _FakeHostGateway) -> list[Envelope]:
    responses: list[Envelope] = []
    for raw in vm.pop_pending():
        response = host.handle_vm_envelope(Envelope.from_json(raw))
        if response is not None:
            responses.append(response)
    return responses


class GatewayReconnectTest(unittest.TestCase):
    def setUp(self) -> None:
        self.vm = VmGatewayClient()
        self.vm.connect(session_id="session-reconnect")
        self.host = _FakeHostGateway()

    def _handshake(self) -> None:
        self.vm.send(self.vm.create_handshake_envelope())
        responses = _flush(self.vm, self.host)
        self.assertEqual(len(responses), 1)
        ack = responses[0].payload["ack"]
        self.vm.apply_handshake_ack(
            {
                "accepted": bool(ack["accepted"]),
                "session_id": str(ack["session_id"]),
            }
        )

    def test_short_disconnect_auto_reconnect_and_heartbeat_recovery(self) -> None:
        self._handshake()
        self.assertTrue(self.vm.is_connected())
        self.assertEqual(self.vm.session_id(), "session-reconnect")

        # 模拟短断链：客户端连接被动断开。
        self.vm.close()
        self.assertFalse(self.vm.is_connected())

        # 心跳发送触发自动重连并继续使用原会话 ID。
        hb = self.vm.send_heartbeat(auto_reconnect=True)
        self.assertEqual(hb.session_id, "session-reconnect")
        self.assertTrue(self.vm.is_connected())

        _flush(self.vm, self.host)
        self.assertEqual(self.host.heartbeat_count, 1)
        self.assertIn("session-reconnect", self.host.sessions)

    def test_jitter_reconnect_multiple_cycles(self) -> None:
        self._handshake()
        self.assertEqual(self.vm.session_id(), "session-reconnect")

        for _ in range(3):
            self.vm.close()
            self.assertFalse(self.vm.is_connected())
            hb = self.vm.send_heartbeat(auto_reconnect=True)
            self.assertEqual(hb.session_id, "session-reconnect")
            _flush(self.vm, self.host)

        self.assertTrue(self.vm.is_connected())
        self.assertEqual(self.host.heartbeat_count, 3)
        self.assertIn("session-reconnect", self.host.sessions)


if __name__ == "__main__":
    unittest.main()

