from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "mcp" / "server"
if str(SERVER_DIR) not in sys.path:
	sys.path.insert(0, str(SERVER_DIR))

import mcp_server  # noqa: E402
from vsock_client import VsockError  # noqa: E402


class _DummyClient:
	def __init__(self) -> None:
		self.connected = False
		self._task_id = ""

	def connect(self) -> None:
		self.connected = True

	def close(self) -> None:
		self.connected = False

	def is_connected(self) -> bool:
		return self.connected

	def list_windows(self):
		return {"success": True, "result": {"windows": []}}

	def begin_task(self, session_id: str, root_description: str) -> str:
		self._task_id = f"{session_id}:{root_description}"
		return self._task_id

	def end_task(self, status: str = "success") -> None:
		self.connected = self.connected

	def click(self, element_path: str):
		return {"success": True, "result": {"element_path": element_path}}

	def get_ui_tree(self, window_id: str, max_depth: int = 8, include_bounds: bool = False):
		return {
			"success": True,
			"result": {
				"window_id": window_id,
				"max_depth": max_depth,
				"include_bounds": include_bounds,
			},
		}

	def set_value(self, element_path: str, value: str):
		return {"success": True, "result": {"element_path": element_path, "value": value}}

	def activate_window(self, window_id: str):
		return {"success": True, "result": {"window_id": window_id}}

	def key_press(self, window_id: str, key: str):
		return {"success": True, "result": {"window_id": window_id, "key": key}}


class _FailingClient(_DummyClient):
	def connect(self) -> None:
		raise VsockError("connect failed")


class McpServerTransportTest(unittest.TestCase):
	def test_parse_args_default_is_legacy(self) -> None:
		args = mcp_server._parse_args([])
		self.assertEqual(args.channel3_transport, "legacy")

	def test_parse_args_accepts_auto(self) -> None:
		args = mcp_server._parse_args(["--channel3-transport", "auto"])
		self.assertEqual(args.channel3_transport, "auto")

	def test_legacy_mode_uses_legacy_client(self) -> None:
		server = mcp_server.McpServer(channel3_transport="legacy")
		dummy = _DummyClient()

		with mock.patch.object(mcp_server, "VsockClient", return_value=dummy):
			server._ensure_connected()

		self.assertIs(server._client, dummy)
		self.assertTrue(dummy.is_connected())
		self.assertEqual(server._selected_transport, "legacy")

	def test_grpc_mode_uses_grpc_client(self) -> None:
		server = mcp_server.McpServer(channel3_transport="grpc")
		dummy = _DummyClient()

		with mock.patch.object(server, "_create_grpc_client", return_value=dummy):
			server._ensure_connected()

		self.assertIs(server._client, dummy)
		self.assertTrue(dummy.is_connected())
		self.assertEqual(server._selected_transport, "grpc")

	def test_auto_mode_falls_back_to_legacy(self) -> None:
		server = mcp_server.McpServer(channel3_transport="auto")
		legacy = _DummyClient()

		with mock.patch.object(server, "_create_grpc_client", return_value=_FailingClient()):
			with mock.patch.object(mcp_server, "VsockClient", return_value=legacy):
				server._ensure_connected()

		self.assertIs(server._client, legacy)
		self.assertTrue(legacy.is_connected())
		self.assertEqual(server._selected_transport, "legacy")


if __name__ == "__main__":
	unittest.main()
