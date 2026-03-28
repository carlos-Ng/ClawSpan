from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from gateway.vm_gateway_client import VmGatewayClient, VmGatewayError  # noqa: E402


class GatewayRobustnessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.client = VmGatewayClient()

    def test_loads_rejects_malformed_payload_type(self) -> None:
        malformed = {
            "version": "v1",
            "message_id": "m-1",
            "session_id": "s-1",
            "trace_id": "",
            "task_id": "",
            "stream_id": "",
            "timestamp_ms": 1,
            "payload": "not-an-object",
        }
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads(json.dumps(malformed))

    def test_loads_rejects_malformed_numeric_field(self) -> None:
        malformed = {
            "version": "v1",
            "message_id": "m-2",
            "session_id": "s-2",
            "trace_id": "",
            "task_id": "",
            "stream_id": "",
            "timestamp_ms": "bad-int",
            "payload": {"heartbeat": {"sent_at_ms": 1}},
        }
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads(json.dumps(malformed))

    def test_loads_rejects_unsupported_version(self) -> None:
        malformed = {
            "version": "v2",
            "message_id": "m-3",
            "session_id": "s-3",
            "trace_id": "",
            "task_id": "",
            "stream_id": "",
            "timestamp_ms": 1,
            "payload": {"heartbeat": {"sent_at_ms": 1}},
        }
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads(json.dumps(malformed))

    def test_auth_failure_handshake_ack_keeps_disconnected(self) -> None:
        self.client.connect(session_id="session-auth-fail")
        self.client.close()
        self.assertFalse(self.client.is_connected())

        with self.assertRaises(VmGatewayError):
            self.client.apply_handshake_ack(
                {
                    "accepted": False,
                    "session_id": "session-auth-fail",
                }
            )

        self.assertFalse(self.client.is_connected())
        self.assertEqual(self.client.session_id(), "session-auth-fail")


if __name__ == "__main__":
    unittest.main()

