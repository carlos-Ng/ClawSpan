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
        self.sessions: set[str] = set()
        self.command_results: dict[str, dict] = {}
        self.log_events: list[dict] = []
        self.risk_events: list[dict] = []

    def handle_vm_envelope(self, env: Envelope) -> Envelope | None:
        payload = env.payload
        if "control" in payload and payload["control"].get("control_type") == "handshake":
            session_id = payload["control"].get("session_id") or env.session_id
            self.sessions.add(session_id)
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

        if "command_result" in payload:
            command_result = dict(payload["command_result"])
            self.command_results[command_result["command_id"]] = command_result
            return None

        if "log_event" in payload:
            self.log_events.append(dict(payload["log_event"]))
            return None

        if "risk_event" in payload:
            self.risk_events.append(dict(payload["risk_event"]))
            return None

        return None

    def build_command_envelope(
        self,
        *,
        session_id: str,
        command_id: str,
        capability: str,
        operation: str,
        params_json: str,
    ) -> Envelope:
        return Envelope(
            version="v1",
            message_id=f"host-{command_id}",
            session_id=session_id,
            trace_id="trace-host",
            task_id="task-host",
            timestamp_ms=1,
            payload={
                "command": {
                    "command_id": command_id,
                    "capability": capability,
                    "operation": operation,
                    "params_json": params_json,
                }
            },
        )


def _flush_vm_outbox_to_host(vm: VmGatewayClient, host: _FakeHostGateway) -> list[Envelope]:
    responses: list[Envelope] = []
    for raw in vm.pop_pending():
        env = Envelope.from_json(raw)
        response = host.handle_vm_envelope(env)
        if response is not None:
            responses.append(response)
    return responses


class GatewayE2ESmokeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.vm = VmGatewayClient()
        self.vm.connect(session_id="session-e2e")
        self.host = _FakeHostGateway()

    def _handshake(self) -> None:
        self.vm.send(self.vm.create_handshake_envelope())
        responses = _flush_vm_outbox_to_host(self.vm, self.host)
        self.assertEqual(len(responses), 1)
        ack_payload = responses[0].payload["ack"]
        self.vm.apply_handshake_ack(
            {
                "accepted": bool(ack_payload["accepted"]),
                "session_id": str(ack_payload["session_id"]),
            }
        )
        self.assertIn("session-e2e", self.host.sessions)

    def test_command_path_smoke(self) -> None:
        self._handshake()

        host_command = self.host.build_command_envelope(
            session_id="session-e2e",
            command_id="cmd-e2e-1",
            capability="capability_ax",
            operation="click",
            params_json='{"element_path":"root/button"}',
        )
        command_payload = host_command.payload["command"]

        self.vm.send(
            self.vm.create_command_result_envelope(
                command_id=command_payload["command_id"],
                success=True,
                result={"clicked": True},
                trace_id=host_command.trace_id,
                task_id=host_command.task_id,
            )
        )
        _flush_vm_outbox_to_host(self.vm, self.host)

        stored = self.host.command_results.get("cmd-e2e-1")
        self.assertIsNotNone(stored)
        self.assertTrue(stored["success"])

    def test_log_event_path_smoke(self) -> None:
        self._handshake()

        self.vm.send(
            self.vm.create_log_event_envelope(
                event_id="log-e2e-1",
                level="info",
                source="vm-agent",
                message="hello log",
                fields={"kind": "smoke"},
                trace_id="trace-log",
                task_id="task-log",
            )
        )
        _flush_vm_outbox_to_host(self.vm, self.host)

        self.assertEqual(len(self.host.log_events), 1)
        self.assertEqual(self.host.log_events[0]["event_id"], "log-e2e-1")

    def test_risk_event_path_smoke(self) -> None:
        self._handshake()

        self.vm.send(
            self.vm.create_risk_event_envelope(
                event_id="risk-e2e-1",
                severity="high",
                behavior_type="file_delete",
                detail="danger operation",
                extra={"path": "C:/tmp/x"},
                trace_id="trace-risk",
                task_id="task-risk",
            )
        )
        _flush_vm_outbox_to_host(self.vm, self.host)

        self.assertEqual(len(self.host.risk_events), 1)
        self.assertEqual(self.host.risk_events[0]["event_id"], "risk-e2e-1")


if __name__ == "__main__":
    unittest.main()

