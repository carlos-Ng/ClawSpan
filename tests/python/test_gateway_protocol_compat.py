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


class GatewayProtocolCompatTest(unittest.TestCase):
    def test_load_old_envelope_with_missing_fields(self) -> None:
        old = {
            "version": "v1",
            "message_id": "m1",
            "session_id": "s1",
            "payload": {"heartbeat": {"sent_at_ms": 1}},
        }
        parsed = Envelope.from_json(old)
        self.assertEqual(parsed.version, "v1")
        self.assertEqual(parsed.message_id, "m1")
        self.assertEqual(parsed.session_id, "s1")
        self.assertEqual(parsed.trace_id, "")
        self.assertEqual(parsed.task_id, "")
        self.assertEqual(parsed.stream_id, "")
        self.assertEqual(parsed.timestamp_ms, 0)

    def test_load_new_envelope_with_extra_fields(self) -> None:
        future = {
            "version": "v1",
            "message_id": "m2",
            "session_id": "s2",
            "trace_id": "t2",
            "task_id": "task2",
            "stream_id": "stream2",
            "timestamp_ms": 123,
            "payload": {"control": {"control_type": "handshake"}},
            "new_field": "ignore-me",
            "nested": {"x": 1},
        }
        parsed = Envelope.from_json(future)
        self.assertEqual(parsed.version, "v1")
        self.assertEqual(parsed.trace_id, "t2")
        self.assertEqual(parsed.task_id, "task2")
        self.assertEqual(parsed.stream_id, "stream2")
        self.assertEqual(parsed.timestamp_ms, 123)

    def test_validate_version(self) -> None:
        VmGatewayClient.validate_envelope_version("v1")
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.validate_envelope_version("")
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.validate_envelope_version("v2")

    def test_loads_root_type_must_be_object(self) -> None:
        with self.assertRaises(VmGatewayError):
            VmGatewayClient.loads(json.dumps(["bad-root"]))


if __name__ == "__main__":
    unittest.main()

