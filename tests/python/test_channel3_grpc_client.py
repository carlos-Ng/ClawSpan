from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
	sys.path.insert(0, str(SERVER_DIR))

import channel3_grpc_client as client_module  # noqa: E402


class _DummyTunnel:
	def __init__(self) -> None:
		self.started = False

	def start(self) -> None:
		self.started = True

	def stop(self) -> None:
		self.started = False


class _DummyChannel:
	def __init__(self) -> None:
		self.closed = False

	def close(self) -> None:
		self.closed = True


class _DummyFuture:
	def result(self, timeout: float) -> None:
		return None


class _DummyStub:
	def __init__(self) -> None:
		self.begin_requests = []
		self.end_requests = []
		self.cap_requests = []

	def BeginTask(self, request):
		self.begin_requests.append(request)
		return SimpleNamespace(task_id="task-123")

	def EndTask(self, request):
		self.end_requests.append(request)
		return SimpleNamespace(success=True)

	def CallCapability(self, request):
		self.cap_requests.append(request)
		return SimpleNamespace(
			success=True,
			result_json=json.dumps({"clicked": True}),
			error_code=0,
			error_message="",
		)


class _DummyPb2:
	class BeginTaskRequest:
		def __init__(self, **kwargs):
			self.__dict__.update(kwargs)

	class EndTaskRequest:
		def __init__(self, **kwargs):
			self.__dict__.update(kwargs)

	class CallCapabilityRequest:
		def __init__(self, **kwargs):
			self.__dict__.update(kwargs)


class GrpcChannel3ClientTest(unittest.TestCase):
	def setUp(self) -> None:
		self.stub = _DummyStub()
		self.channel = _DummyChannel()
		self.grpc_stub_factory = lambda _channel: self.stub
		self.client = client_module.GrpcChannel3Client()
		self.client._tunnel = _DummyTunnel()
		self.client._pb2 = _DummyPb2
		self.client._pb2_grpc = SimpleNamespace(Channel3ServiceStub=self.grpc_stub_factory)

	def test_begin_task_records_task_id(self) -> None:
		self.client._channel = self.channel
		self.client._stub = self.stub

		task_id = self.client.begin_task("session-1", "root task")

		self.assertEqual(task_id, "task-123")
		self.assertEqual(self.client._task_id, "task-123")
		self.assertEqual(self.stub.begin_requests[0].session_id, "session-1")
		self.assertEqual(self.stub.begin_requests[0].root_description, "root task")

	def test_end_task_maps_status_to_success_flag(self) -> None:
		self.client._channel = self.channel
		self.client._stub = self.stub
		self.client._task_id = "task-123"

		self.client.end_task(status="failure")

		self.assertEqual(self.stub.end_requests[0].task_id, "task-123")
		self.assertFalse(self.stub.end_requests[0].success)
		self.assertEqual(self.client._task_id, "")

	def test_call_capability_returns_legacy_shaped_payload(self) -> None:
		self.client._channel = self.channel
		self.client._stub = self.stub
		self.client._task_id = "task-123"

		result = self.client.call_capability("capability_ax", "click", {"x": 1})

		self.assertTrue(result["success"])
		self.assertEqual(result["result"]["clicked"], True)
		self.assertEqual(self.stub.cap_requests[0].task_id, "task-123")
		self.assertEqual(
			json.loads(self.stub.cap_requests[0].params_json),
			{"x": 1},
		)

	def test_connect_builds_stub_and_tunnel(self) -> None:
		original_grpc = client_module.grpc
		try:
			client_module.grpc = SimpleNamespace(
				insecure_channel=lambda _target: self.channel,
				channel_ready_future=lambda _channel: _DummyFuture(),
			)
			self.client._pb2_grpc = SimpleNamespace(Channel3ServiceStub=self.grpc_stub_factory)
			self.client.connect()
		finally:
			client_module.grpc = original_grpc

		self.assertTrue(self.client._tunnel.started)
		self.assertIs(self.client._channel, self.channel)
		self.assertIs(self.client._stub, self.stub)


if __name__ == "__main__":
	unittest.main()
