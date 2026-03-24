#!/usr/bin/env python3
"""
confirm_client.py — ClawSpan 用户确认通道测试客户端（Windows Named Pipe 版）

用法：
  python tools\\confirm_client.py [pipe_path]

默认连接 \\\\.\\pipe\\crew-shell-service-confirm。
启动后程序保持连接，当 daemon 发来确认请求时弹出提示，
用户输入 y（确认）或 n（拒绝）后将结果回传给 daemon。

线路协议：与主 IPC 相同的 Length-prefix framing
  [uint32 big-endian 字节数][JSON body]

请求格式（daemon → client）：
  {"id": 1, "capability": "capability_ax", "operation": "click",
   "params": {...}, "reason": "GUI click requires confirmation"}

响应格式（client → daemon）：
  {"id": 1, "confirmed": true}
"""

import json
import struct
import sys
import textwrap

# ── Windows Named Pipe 依赖 ────────────────────────────────────────────────────
try:
    import win32file
    import win32pipe
    import pywintypes
except ImportError:
    print(
        "[ERROR] pywin32 not installed.\n"
        "Install with:  pip install pywin32",
        file=sys.stderr,
    )
    sys.exit(1)

PIPE_PATH = r"\\.\pipe\crew-shell-service-confirm"

# ── Named Pipe 连接 ───────────────────────────────────────────────────────────

def connect_pipe(path: str):
    """连接到 Named Pipe，返回 handle。"""
    import winerror
    while True:
        try:
            handle = win32file.CreateFile(
                path,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None,
                win32file.OPEN_EXISTING,
                0, None,
            )
            win32pipe.SetNamedPipeHandleState(
                handle,
                win32pipe.PIPE_READMODE_BYTE,
                None, None,
            )
            return handle
        except pywintypes.error as e:
            if e.args[0] == winerror.ERROR_PIPE_BUSY:
                win32pipe.WaitNamedPipe(path, 1000)
                continue
            if e.args[0] == winerror.ERROR_FILE_NOT_FOUND:
                print(f"[ERROR] Named Pipe 不存在：{path}")
                print("        请先启动 crew-shell-service：")
                print(r"          .\build\crew-shell-service.exe -f -c config\clawspan.toml")
                sys.exit(1)
            print(f"[ERROR] 连接失败：{e}")
            sys.exit(1)

# ── 帧编解码 ──────────────────────────────────────────────────────────────────

def send_frame(handle, payload: dict) -> None:
    body  = json.dumps(payload).encode("utf-8")
    frame = struct.pack(">I", len(body)) + body
    win32file.WriteFile(handle, frame)


def recv_frame(handle) -> dict:
    header = b""
    while len(header) < 4:
        _, chunk = win32file.ReadFile(handle, 4 - len(header))
        if not chunk:
            raise ConnectionError("连接已关闭")
        header += chunk
    length = struct.unpack(">I", header)[0]

    body = b""
    while len(body) < length:
        _, chunk = win32file.ReadFile(handle, length - len(body))
        if not chunk:
            raise ConnectionError("连接已关闭（读正文时）")
        body += chunk
    return json.loads(body.decode("utf-8"))


# ── 确认请求处理 ──────────────────────────────────────────────────────────────

def handle_request(handle, req: dict) -> None:
    req_id     = req.get("id", 0)
    capability = req.get("capability", "?")
    operation  = req.get("operation", "?")
    params     = req.get("params", {})
    reason     = req.get("reason", "")

    print(textwrap.dedent(f"""
    ╔══════════════════════════════════════════════════════════════╗
    ║  ClawSpan — 需要用户确认                                 ║
    ╠══════════════════════════════════════════════════════════════╣
    ║  能力  : {capability:<52}║
    ║  操作  : {operation:<52}║
    ║  原因  : {reason:<52}║
    ╠══════════════════════════════════════════════════════════════╣
    ║  参数  :                                                     ║
    {_indent_params(params)}
    ╚══════════════════════════════════════════════════════════════╝
    """))

    while True:
        try:
            ans = input("  确认此操作？[y/n] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            ans = "n"
            print()

        if ans in ("y", "yes"):
            confirmed = True
            break
        elif ans in ("n", "no", ""):
            confirmed = False
            break
        else:
            print("  请输入 y 或 n")

    send_frame(handle, {"id": req_id, "confirmed": confirmed})
    status = "✓ 已确认" if confirmed else "✗ 已拒绝"
    print(f"  [{status}] id={req_id}\n")


def _indent_params(params: dict) -> str:
    lines = json.dumps(params, indent=2, ensure_ascii=False).splitlines()
    padded = [f"    ║  {line:<56}║" for line in lines[:8]]
    if len(lines) > 8:
        padded.append(f"    ║  {'... (已截断)':<56}║")
    return "\n".join(padded)


# ── 主循环 ────────────────────────────────────────────────────────────────────

def main() -> None:
    pipe_path = sys.argv[1] if len(sys.argv) > 1 else PIPE_PATH
    print(f"连接到确认通道 {pipe_path} ...")

    handle = connect_pipe(pipe_path)
    print("已连接，等待确认请求...\n")

    try:
        while True:
            try:
                req = recv_frame(handle)
                handle_request(handle, req)
            except ConnectionError as e:
                print(f"\n[断开] {e}")
                break
            except KeyboardInterrupt:
                print("\n[退出]")
                break
    finally:
        win32file.CloseHandle(handle)


if __name__ == "__main__":
    main()
