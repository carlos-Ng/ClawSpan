#!/usr/bin/env python3
import argparse
import json
import pathlib
import subprocess
import sys


def _bootstrap_import_path() -> None:
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    mcp_server_dir = repo_root / "mcp" / "server"
    sys.path.insert(0, str(mcp_server_dir))


_bootstrap_import_path()
from vm_channel_grpc_client import VmGrpcClient  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ClawSpan gRPC over vsock smoke test")
    parser.add_argument("--host-cid", type=int, default=2, help="Host CID (default: 2)")
    parser.add_argument("--host-port", type=int, default=101, help="Host vsock port (default: 101)")
    parser.add_argument("--local-host", default="127.0.0.1", help="Local tunnel host (default: 127.0.0.1)")
    parser.add_argument("--local-port", type=int, default=50052, help="Local tunnel port (default: 50052)")
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=5.0,
        help="gRPC channel connect timeout seconds (default: 5.0)",
    )
    parser.add_argument("--session-id", default="grpc-smoke-session", help="session id")
    parser.add_argument("--root-description", default="grpc-smoke", help="root description")
    parser.add_argument("--capability", default="capability_ax", help="capability name")
    parser.add_argument("--operation", default="list_windows", help="operation name")
    parser.add_argument("--params-json", default="{}", help='JSON object string for params')
    parser.add_argument(
        "--auto-install-deps",
        action="store_true",
        help="auto install grpcio/protobuf when missing",
    )
    return parser.parse_args()


def ensure_python_deps(auto_install: bool) -> None:
    try:
        import grpc  # type: ignore  # noqa: F401
        import google.protobuf  # type: ignore  # noqa: F401
        return
    except Exception:
        if not auto_install:
            raise RuntimeError(
                "缺少 Python 依赖，请先执行: python3 -m pip install --user grpcio protobuf "
                "或使用 --auto-install-deps 自动安装"
            )

    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", "--user", "grpcio", "protobuf"],
        stdout=sys.stdout,
        stderr=sys.stderr,
    )


def main() -> int:
    args = parse_args()
    ensure_python_deps(args.auto_install_deps)
    params = json.loads(args.params_json)
    require(isinstance(params, dict), "--params-json must be a JSON object")

    client = VmGrpcClient(
        local_host=args.local_host,
        local_port=args.local_port,
        host_cid=args.host_cid,
        host_port=args.host_port,
        connect_timeout_secs=args.connect_timeout,
    )

    task_id = ""
    try:
        task_id = client.begin_task(args.session_id, args.root_description)
        print(json.dumps({"step": "beginTask", "task_id": task_id}, ensure_ascii=False))

        capability_result = client.call_capability(args.capability, args.operation, params)
        require(bool(capability_result.get("success")), "capability call did not succeed")
        print(json.dumps({"step": "capability", "result": capability_result}, ensure_ascii=False))

        client.end_task("success")
        print(json.dumps({"step": "endTask", "success": True}, ensure_ascii=False))
        print("SMOKE_OK grpc beginTask/capability/endTask (all success)")
        return 0
    except Exception:
        if task_id:
            try:
                client.end_task("failed")
            except Exception:
                pass
        raise
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
