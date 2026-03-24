#!/usr/bin/env python3
"""
ax_test_client.py — ClawSpan ax capability 手动测试客户端（Windows Named Pipe 版）

用法：
  python tools\\ax_test_client.py [pipe_path]
  python tools\\ax_test_client.py -f script.txt [pipe_path]
  python tools\\ax_test_client.py --file script.txt [pipe_path]

  -f, --file <path>  从脚本文件加载命令并依次执行（批量模式）
                     脚本格式：每行一条命令，与交互模式相同
                     operation [JSON params]
                     支持 # 开头的注释行和空行

默认连接 \\\\.\\pipe\\crew-shell-service，启动 crew-shell-service 后使用。

线路协议：Length-prefix framing
  [uint32 big-endian 字节数][JSON-RPC 2.0 body]
"""

import argparse
import json
import struct
import sys
import textwrap
from typing import Optional

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

# ── 默认配置 ──────────────────────────────────────────────────────────────────

PIPE_PATH  = r"\\.\pipe\crew-shell-service"
CAPABILITY = "capability_ax"

# ── Named Pipe 连接 ───────────────────────────────────────────────────────────

def connect_pipe(path: str):
    """连接到 Named Pipe，返回 win32file handle，失败时打印错误并退出。"""
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
                print("       请先启动 crew-shell-service：")
                print(r"         .\build\crew-shell-service.exe -f -c clawspan.toml")
                sys.exit(1)
            print(f"[ERROR] 连接失败：{e}")
            sys.exit(1)


# ── 帧编解码 ──────────────────────────────────────────────────────────────────

def send_frame(handle, payload: dict) -> None:
    """将 dict 编码为 JSON 并以 length-prefix 帧发送。"""
    body  = json.dumps(payload).encode("utf-8")
    frame = struct.pack(">I", len(body)) + body
    win32file.WriteFile(handle, frame)


def recv_frame(handle) -> dict:
    """从 Named Pipe 读取一个 length-prefix 帧并解析为 dict。"""
    # 读取 4 字节长度头
    header = b""
    while len(header) < 4:
        _, chunk = win32file.ReadFile(handle, 4 - len(header))
        if not chunk:
            raise ConnectionError("连接已关闭")
        header += chunk
    length = struct.unpack(">I", header)[0]

    # 读取正文
    body = b""
    while len(body) < length:
        _, chunk = win32file.ReadFile(handle, length - len(body))
        if not chunk:
            raise ConnectionError("连接已关闭（读正文时）")
        body += chunk
    return json.loads(body.decode("utf-8"))


# ── JSON-RPC 2.0 封装 ─────────────────────────────────────────────────────────

_req_id = 0

def call(handle, method: str, params: dict = None) -> dict:
    """发起一次 JSON-RPC 2.0 同步调用，返回完整响应 dict。"""
    global _req_id
    _req_id += 1
    request = {
        "jsonrpc": "2.0",
        "id":      _req_id,
        "method":  method,
        "params":  params or {},
    }
    send_frame(handle, request)
    return recv_frame(handle)


def ax_call(handle, operation: str, params: dict = None) -> dict:
    """调用 capability_ax 下的指定操作。"""
    return call(handle, f"{CAPABILITY}.{operation}", params)


# ── 结果打印 ──────────────────────────────────────────────────────────────────

def print_result(resp: dict, title: str = "") -> None:
    if title:
        print(f"\n{'─' * 60}")
        print(f"  {title}")
        print(f"{'─' * 60}")
    if "result" in resp:
        result = resp["result"]
        if isinstance(result, str):
            print(result, end="" if result.endswith("\n") else "\n")
        else:
            print(json.dumps(result, indent=2, ensure_ascii=False))
    elif "error" in resp:
        err = resp["error"]
        print(f"[ERROR] code={err.get('code')}  message={err.get('message')}")
    else:
        print(json.dumps(resp, indent=2, ensure_ascii=False))


# ── 测试用例 ──────────────────────────────────────────────────────────────────

def test_list_windows(handle) -> list:
    """列出当前所有窗口，返回 window_id 列表。"""
    resp = ax_call(handle, "list_windows")
    print_result(resp, "list_windows")
    if "result" in resp and isinstance(resp["result"], list):
        return [w.get("window_id") for w in resp["result"] if w.get("window_id")]
    return []


def test_get_ui_tree(handle, window_id: str, max_depth: int = 3) -> None:
    """获取指定窗口的 UI 元素树（限制深度避免输出过大）。"""
    resp = ax_call(handle, "get_ui_tree", {
        "window_id": window_id,
        "max_depth":  max_depth,
    })
    print_result(resp, f"get_ui_tree  window_id={window_id}  max_depth={max_depth}")


def test_activate(handle, window_id: str) -> None:
    """将指定窗口激活到前台。"""
    resp = ax_call(handle, "activate", {"window_id": window_id})
    print_result(resp, f"activate  window_id={window_id}")


def test_key_press(handle, window_id: str, key: str) -> None:
    """向指定窗口发送按键。"""
    resp = ax_call(handle, "key_press", {
        "window_id": window_id,
        "key":       key,
    })
    print_result(resp, f"key_press  window_id={window_id}  key={key}")


# ── 交互式 shell ──────────────────────────────────────────────────────────────

def interactive_shell(handle) -> None:
    """
    命令格式：<operation> [JSON params]

    ── 窗口查询 ────────────────────────────────────────────────────────────────
      list_windows
          列出所有可访问窗口，返回 window_id 列表。
          window_id 格式：w<pid>_<hwnd_hex>，例如 w1234_1A2B0

      get_ui_tree {"window_id": "w1234_1A2B0", "max_depth": 5}
          获取窗口 UI 元素树，输出 AXT 紧凑文本格式。
          max_depth 可选（默认使用模块配置值，建议 3-10）。
          include_bounds 可选（默认 false），设为 true 时每行附带屏幕坐标。
          示例（附带坐标）：
            get_ui_tree {"window_id": "w1234_1A2B0", "max_depth": 5, "include_bounds": true}

    ── 元素操作 ────────────────────────────────────────────────────────────────
      click {"element_path": "/w1234_1A2B0/AXButton[OK]"}
          单击元素（优先 UIA 语义操作；失败自动降级为 SendInput 鼠标事件）。

      double_click {"element_path": "/w1234_1A2B0/AXTextField[0]"}
          双击元素（SendInput 鼠标双击）。

      right_click {"element_path": "/w1234_1A2B0/AXStaticText[0]"}
          右键点击元素。

      set_value {"element_path": "/w1234_1A2B0/AXTextField[0]", "value": "hello"}
          向可编辑元素写入文字（优先 ValuePattern；失败自动降级为 SendInput 键盘模拟）。

      focus {"element_path": "/w1234_1A2B0/AXTextField[0]"}
          将键盘焦点移到目标元素。

      scroll {"element_path": "/w1234_1A2B0/AXScrollArea[0]", "direction": "down", "amount": 200}
          滚动元素，direction 取值：up / down / left / right，amount 单位为像素。

    ── 键盘操作 ────────────────────────────────────────────────────────────────
      key_press {"window_id": "w1234_1A2B0", "key": "Return"}
          向目标窗口发送按键，窗口会先被激活到前台。
          支持的键名（区分大小写）：
            Return  Escape  Space  Tab  Delete  BackSpace
            Up  Down  Left  Right  Home  End  PageUp  PageDown
            F1 ~ F12
            单个可打印字符，如 "a"、"1"、"."

      key_combo {"window_id": "w1234_1A2B0", "keys": ["Ctrl", "C"]}
          向目标窗口发送组合键，修饰键在前，主键在最后。
          支持的修饰键：Ctrl  Alt  Shift  Win（Cmd 为 Win 的别名）
          示例：["Ctrl","C"]  ["Ctrl","Shift","Z"]  ["Alt","F4"]

    ── 窗口管理 ────────────────────────────────────────────────────────────────
      activate {"window_id": "w1234_1A2B0"}
          将窗口激活到前台（注意：此操作会抢夺前台焦点）。

    ── 客户端命令 ──────────────────────────────────────────────────────────────
      help        显示此帮助
      quit / q    退出
    """
    print(textwrap.dedent("""
    ┌──────────────────────────────────────────────────────────────┐
    │  ClawSpan ax 测试客户端（Windows Named Pipe）            │
    │  输入 help 查看所有支持的操作及参数说明                      │
    └──────────────────────────────────────────────────────────────┘
    """))
    while True:
        try:
            line = input("ax> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nbye")
            break
        if not line:
            continue
        if not execute_line(handle, line):
            break


def execute_line(handle, line: str, line_no: Optional[int] = None) -> bool:
    """
    解析并执行单行命令，返回是否应继续执行（False 表示遇到 quit 等）。
    line_no: 脚本行号，用于错误提示；为 None 时表示交互模式。
    """
    line = line.strip()
    if not line or line.startswith("#"):
        return True
    if line in ("quit", "exit", "q"):
        return False
    if line == "help":
        print(interactive_shell.__doc__)
        return True

    parts = line.split(None, 1)
    operation = parts[0]
    params = {}
    if len(parts) > 1:
        try:
            params = json.loads(parts[1])
        except json.JSONDecodeError as e:
            prefix = f"[SCRIPT:{line_no}] " if line_no is not None else ""
            print(f"{prefix}[PARSE ERROR] {e}")
            return True

    try:
        resp  = ax_call(handle, operation, params)
        title = f"[{line_no}] {operation}" if line_no is not None else operation
        print_result(resp, title)
    except Exception as e:
        prefix = f"[SCRIPT:{line_no}] " if line_no is not None else ""
        print(f"{prefix}[ERROR] {e}")
        return False
    return True


def run_script(handle, script_path: str) -> None:
    """从脚本文件加载命令并依次执行。"""
    try:
        with open(script_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"[ERROR] 脚本文件不存在：{script_path}")
        sys.exit(1)
    except OSError as e:
        print(f"[ERROR] 无法读取脚本：{e}")
        sys.exit(1)

    print(f"执行脚本：{script_path}（共 {len(lines)} 行）\n")
    for i, line in enumerate(lines, 1):
        if not execute_line(handle, line, i):
            print(f"\n脚本在第 {i} 行终止")
            break
    else:
        print("\n脚本执行完成")


# ── 入口 ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="ClawSpan ax capability 测试客户端（Windows Named Pipe）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent(r"""
            示例：
              %(prog)s                             # 交互模式
              %(prog)s -f my_script.txt            # 批量执行脚本
              %(prog)s -f script.txt \\.\pipe\crew-shell-service
        """)
    )
    parser.add_argument(
        "-f", "--file",
        metavar="SCRIPT",
        help="从脚本文件加载命令并依次执行（批量模式）",
    )
    parser.add_argument(
        "pipe_path",
        nargs="?",
        default=PIPE_PATH,
        help=f"Named Pipe 路径（默认：{PIPE_PATH}）",
    )
    args = parser.parse_args()

    pipe_path   = args.pipe_path
    script_path = args.file

    print(f"连接到 {pipe_path} ...")
    handle = connect_pipe(pipe_path)
    print("已连接\n")

    try:
        if script_path:
            run_script(handle, script_path)
        else:
            window_ids = test_list_windows(handle)
            if window_ids:
                first_id = window_ids[0]
                test_get_ui_tree(handle, first_id, max_depth=2)
            interactive_shell(handle)
    finally:
        win32file.CloseHandle(handle)


if __name__ == "__main__":
    main()
