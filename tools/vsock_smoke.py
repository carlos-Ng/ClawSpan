#!/usr/bin/env python3
import argparse
import json
import socket
import struct


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
    return buf


def send_request(sock: socket.socket, req: dict) -> dict:
    body = json.dumps(req, ensure_ascii=False).encode("utf-8")
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = recv_exact(sock, 4)
    size = struct.unpack(">I", header)[0]
    resp_raw = recv_exact(sock, size).decode("utf-8")
    print(resp_raw)
    return json.loads(resp_raw)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ClawSpan vsock smoke test")
    parser.add_argument("--cid", type=int, default=2, help="Host CID (default: 2)")
    parser.add_argument("--port", type=int, default=100, help="Host vsock port (default: 100)")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    parser.add_argument(
        "--capability",
        default="capability_ax",
        help="capability name for smoke call (default: capability_ax)",
    )
    parser.add_argument(
        "--operation",
        default="list_windows",
        help="operation name for smoke call (default: list_windows)",
    )
    parser.add_argument(
        "--params-json",
        default="{}",
        help='JSON params for capability call (default: "{}")',
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    capability_params = json.loads(args.params_json)
    require(isinstance(capability_params, dict), "--params-json must decode to a JSON object")
    sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    sock.settimeout(args.timeout)
    sock.connect((args.cid, args.port))
    try:
        begin_resp = send_request(sock, {
            "type": "beginTask",
            "session_id": "smoke-session",
            "root_description": "vsock-smoke",
        })
        require(begin_resp.get("type") == "beginTask_response", "unexpected beginTask response type")
        require(bool(begin_resp.get("success")), "beginTask did not succeed")
        task_id = str(begin_resp.get("task_id", ""))
        require(task_id != "", "beginTask returned empty task_id")

        capability_resp = send_request(sock, {
            "type": "capability",
            "id": 1,
            "task_id": task_id,
            "capability": args.capability,
            "operation": args.operation,
            "params": capability_params,
        })
        require(capability_resp.get("type") == "capability_result",
                "unexpected capability response type")
        require(bool(capability_resp.get("success")), "capability_result did not succeed")

        end_resp = send_request(sock, {
            "type": "endTask",
            "task_id": task_id,
            "success": True,
        })
        require(end_resp.get("type") == "endTask_response", "unexpected endTask response type")
        require(bool(end_resp.get("success")), "endTask did not succeed")
    finally:
        sock.close()

    print("SMOKE_OK beginTask/capability/endTask (all success)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
