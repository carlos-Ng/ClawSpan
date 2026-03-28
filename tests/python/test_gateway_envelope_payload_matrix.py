from __future__ import annotations

import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from gateway.vm_gateway_client import Envelope, VmGatewayClient  # noqa: E402


REQUIRED_ENVELOPE_FIELDS = {
    "version",
    "message_id",
    "session_id",
    "trace_id",
    "task_id",
    "stream_id",
    "timestamp_ms",
    "payload",
}


class GatewayEnvelopePayloadMatrixTest(unittest.TestCase):
    def setUp(self) -> None:
        self.client = VmGatewayClient()
        self.client.connect(session_id="session-matrix")

    def _assert_envelope_fields(self, env: Envelope) -> None:
        obj = env.to_json()
        self.assertEqual(set(obj.keys()), REQUIRED_ENVELOPE_FIELDS)
        self.assertEqual(obj["session_id"], "session-matrix")
        self.assertTrue(obj["message_id"])
        self.assertGreater(obj["timestamp_ms"], 0)

    def test_command_payload(self) -> None:
        env = self.client.create_command_envelope(
            command_id="cmd-1",
            capability="capability_ax",
            operation="click",
            params={"x": 1},
            trace_id="trace-1",
            task_id="task-1",
        )
        self._assert_envelope_fields(env)
        self.assertIn("command", env.payload)

    def test_command_result_payload(self) -> None:
        env = self.client.create_command_result_envelope(
            command_id="cmd-1",
            success=True,
            result={"ok": True},
            trace_id="trace-1",
            task_id="task-1",
        )
        self._assert_envelope_fields(env)
        self.assertIn("command_result", env.payload)

    def test_log_event_payload(self) -> None:
        env = self.client.create_log_event_envelope(
            event_id="log-1",
            level="info",
            source="vm-agent",
            message="hello",
            fields={"k": "v"},
            trace_id="trace-log",
            task_id="task-log",
        )
        self._assert_envelope_fields(env)
        self.assertIn("log_event", env.payload)

    def test_risk_event_payload(self) -> None:
        env = self.client.create_risk_event_envelope(
            event_id="risk-1",
            severity="high",
            behavior_type="file_delete",
            detail="danger",
            extra={"path": "C:/tmp/x"},
            trace_id="trace-risk",
            task_id="task-risk",
        )
        self._assert_envelope_fields(env)
        self.assertIn("risk_event", env.payload)

    def test_ack_payload(self) -> None:
        env = self.client.create_ack_envelope(
            ack_message_id="m-1",
            accepted=True,
            reason="ok",
            trace_id="trace-ack",
            task_id="task-ack",
        )
        self._assert_envelope_fields(env)
        self.assertIn("ack", env.payload)

    def test_heartbeat_payload(self) -> None:
        env = self.client.send_heartbeat(auto_reconnect=True)
        self._assert_envelope_fields(env)
        self.assertIn("heartbeat", env.payload)

    def test_control_payload(self) -> None:
        env = self.client.create_control_envelope(
            control_type="handshake",
            message="hello",
            trace_id="trace-ctrl",
            task_id="task-ctrl",
        )
        self._assert_envelope_fields(env)
        self.assertIn("control", env.payload)


if __name__ == "__main__":
    unittest.main()

