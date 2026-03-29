from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from gateway.vm_gateway_client import Envelope, VmGatewayClient, VmGatewayError  # noqa: E402


class VmGatewayClientTest(unittest.TestCase):
    def setUp(self) -> None:
        self.client = VmGatewayClient()

    def test_connect_and_build_envelope(self) -> None:
        self.client.connect(session_id="session-1")
        env = self.client.build_envelope({"command": {"command_id": "cmd-1"}})

        self.assertEqual(env.session_id, "session-1")
        self.assertTrue(env.message_id)
        self.assertGreater(env.timestamp_ms, 0)

    def test_send_requires_connection(self) -> None:
        with self.assertRaises(VmGatewayError):
            self.client.send(Envelope(payload={"heartbeat": {}}))

    def test_send_and_pop_pending(self) -> None:
        self.client.connect(session_id="session-2")
        env = self.client.build_envelope({"heartbeat": {"sent_at_ms": 1}})
        self.client.send(env)

        pending = self.client.pop_pending()
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0]["session_id"], "session-2")
        self.assertEqual(self.client.pop_pending(), [])

    def test_send_heartbeat_auto_reconnect(self) -> None:
        self.client.connect(session_id="session-heartbeat")
        self.client.close()
        self.assertFalse(self.client.is_connected())

        env = self.client.send_heartbeat(auto_reconnect=True)
        self.assertEqual(env.session_id, "session-heartbeat")
        self.assertTrue(self.client.is_connected())
        self.assertEqual(len(self.client.pop_pending()), 1)

    def test_send_heartbeat_without_auto_reconnect_fails(self) -> None:
        with self.assertRaises(VmGatewayError):
            self.client.send_heartbeat(auto_reconnect=False)

    def test_dumps_and_loads_round_trip(self) -> None:
        self.client.connect(session_id="session-3")
        env = self.client.build_envelope({"ack": {"ack_message_id": "m-1"}})

        text = VmGatewayClient.dumps(env)
        parsed = VmGatewayClient.loads(text)
        self.assertEqual(parsed.session_id, "session-3")
        self.assertEqual(parsed.payload["ack"]["ack_message_id"], "m-1")

    def test_loads_rejects_invalid_json(self) -> None:
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads("{not-json")

        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads(json.dumps(["not-object"]))

    def test_reconnect_creates_session_when_missing(self) -> None:
        self.assertEqual(self.client.session_id(), "")
        self.client.reconnect()
        self.assertTrue(self.client.is_connected())
        self.assertTrue(self.client.session_id())

    def test_create_handshake_envelope(self) -> None:
        self.client.connect(session_id="session-hs")
        env = self.client.create_handshake_envelope()
        self.assertEqual(env.payload["control"]["control_type"], "handshake")
        self.assertEqual(env.payload["control"]["session_id"], "session-hs")

    def test_apply_handshake_ack_updates_session(self) -> None:
        self.client.apply_handshake_ack(
            {
                "accepted": True,
                "session_id": "session-host-1",
            }
        )
        self.assertTrue(self.client.is_connected())
        self.assertEqual(self.client.session_id(), "session-host-1")

    def test_apply_handshake_ack_rejects_invalid_payload(self) -> None:
        with self.assertRaises(VmGatewayError):
            self.client.apply_handshake_ack({"accepted": False, "session_id": "s"})
        with self.assertRaises(VmGatewayError):
            self.client.apply_handshake_ack({"accepted": True})
        with self.assertRaises(VmGatewayError):
            self.client.apply_handshake_ack([])  # type: ignore[arg-type]

    def test_create_command_envelope(self) -> None:
        self.client.connect(session_id="session-cmd")
        env = self.client.create_command_envelope(
            command_id="cmd-1",
            capability="capability_ax",
            operation="click",
            params={"element_path": "root/button"},
            trace_id="trace-cmd-1",
            task_id="task-cmd-1",
        )
        command = env.payload["command"]
        self.assertEqual(command["command_id"], "cmd-1")
        self.assertEqual(command["operation"], "click")
        self.assertEqual(env.trace_id, "trace-cmd-1")
        self.assertEqual(env.task_id, "task-cmd-1")
        self.assertEqual(
            json.loads(command["params_json"]),
            {"element_path": "root/button"},
        )

    def test_create_command_result_and_parse_payload(self) -> None:
        self.client.connect(session_id="session-result")
        env = self.client.create_command_result_envelope(
            command_id="cmd-1",
            success=True,
            result={"clicked": True},
            trace_id="trace-res-1",
            task_id="task-res-1",
        )
        payload = env.payload["command_result"]
        parsed = VmGatewayClient.parse_command_result_payload(payload)
        self.assertEqual(env.trace_id, "trace-res-1")
        self.assertEqual(env.task_id, "task-res-1")
        self.assertEqual(parsed["command_id"], "cmd-1")
        self.assertTrue(parsed["success"])
        self.assertEqual(parsed["result"]["clicked"], True)

    def test_parse_command_result_rejects_invalid_payload(self) -> None:
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.parse_command_result_payload([])  # type: ignore[arg-type]
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.parse_command_result_payload({"success": True})
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.parse_command_result_payload(
                {"command_id": "cmd", "result_json": "{bad-json"}
            )

    def test_create_log_event_envelope(self) -> None:
        self.client.connect(session_id="session-log")
        env = self.client.create_log_event_envelope(
            event_id="log-1",
            level="info",
            source="vm-agent",
            message="log message",
            fields={"a": 1},
            trace_id="trace-log-1",
            task_id="task-log-1",
        )
        payload = env.payload["log_event"]
        self.assertEqual(payload["event_id"], "log-1")
        self.assertEqual(payload["level"], "info")
        self.assertEqual(env.trace_id, "trace-log-1")
        self.assertEqual(env.task_id, "task-log-1")
        self.assertEqual(
            json.loads(payload["fields_json"]),
            {
                "a": 1,
                "_context": {"trace_id": "trace-log-1", "task_id": "task-log-1"},
            },
        )

    def test_create_risk_event_envelope(self) -> None:
        self.client.connect(session_id="session-risk")
        env = self.client.create_risk_event_envelope(
            event_id="risk-1",
            severity="high",
            behavior_type="file_delete",
            detail="danger action",
            extra={"path": "C:/tmp/x"},
            trace_id="trace-risk-1",
            task_id="task-risk-1",
        )
        payload = env.payload["risk_event"]
        self.assertEqual(payload["event_id"], "risk-1")
        self.assertEqual(payload["severity"], "high")
        self.assertEqual(env.trace_id, "trace-risk-1")
        self.assertEqual(env.task_id, "task-risk-1")
        self.assertEqual(
            json.loads(payload["extra_json"]),
            {
                "path": "C:/tmp/x",
                "_context": {"trace_id": "trace-risk-1", "task_id": "task-risk-1"},
            },
        )


if __name__ == "__main__":
    unittest.main()

