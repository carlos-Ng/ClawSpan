#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    proto_dir = repo_root / "proto"
    proto_file = proto_dir / "vm_channel.proto"
    out_dir = repo_root / "mcp" / "server" / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)

    command = [
        sys.executable,
        "-m",
        "grpc_tools.protoc",
        f"-I{proto_dir}",
        f"--python_out={out_dir}",
        f"--grpc_python_out={out_dir}",
        str(proto_file),
    ]

    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as exc:
        print(f"grpc_tools.protoc 执行失败: {exc}", file=sys.stderr)
        return exc.returncode
    except ModuleNotFoundError as exc:
        print(f"缺少 grpc_tools，请先安装 grpcio-tools: {exc}", file=sys.stderr)
        return 1

    grpc_stub = out_dir / "vm_channel_pb2_grpc.py"
    content = grpc_stub.read_text(encoding="utf-8")
    content = content.replace(
        "import vm_channel_pb2 as vm__channel__pb2",
        "from . import vm_channel_pb2 as vm__channel__pb2",
    ).replace(
        "import vm_channel_pb2 as vm_channel__pb2",
        "from . import vm_channel_pb2 as vm_channel__pb2",
    )
    grpc_stub.write_text(content, encoding="utf-8")

    init_file = out_dir / "__init__.py"
    if not init_file.exists():
        init_file.write_text("# Generated VM Channel Python stubs.\n", encoding="utf-8")

    print(f"Generated Python stubs into {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

