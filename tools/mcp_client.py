#!/usr/bin/env python3
"""
mcp_client.py — ClawSpan MCP 测试客户端

通过 MCP 协议（stdio 传输）连接 mcp_server.py，覆盖完整调用链：
  mcp_client.py  →  MCP stdio  →  mcp_server.py  →  IPC  →  crew-shell-service

与 ax_test_client.py 的区别：
  ax_test_client.py 直接调用 IPC（跳过 MCP 层，只测安全链）
  mcp_client.py    走完整 MCP 协议（端到端测试，同时覆盖 MCP 层行为）

用法：
  python3 tools/mcp_client.py                         # 交互模式
  python3 tools/mcp_client.py -f script.txt            # 批量执行脚本
  python3 tools/mcp_client.py --sock /tmp/custom.sock  # 指定 daemon socket
  python3 tools/mcp_client.py --list-tools             # 列出所有可用工具后退出

脚本格式（每行一条命令，与交互模式相同）：
  # 注释行和空行均被忽略
  # @expect: PASS              ← 可选注解，声明下一条命令的预期安全结果
  list_windows
  # @expect: PASS
  get_ui_tree {"window_id": "w123_0", "max_depth": 3}
  # @expect: DENY
  key_combo {"window_id": "w123_0", "keys": ["Cmd", "C"]}
  # @expect: CONFIRM
  click {"element_path": "/w123_0/AXButton[OK]"}

@expect 注解取值：
  PASS    — 操作应被安全层放行并正常执行
  DENY    — 操作应被安全层直接拒绝
  CONFIRM — 操作应触发用户确认（无 confirm_client 时自动拒绝，本工具记为符合预期）
"""

import argparse
import asyncio
import json
import os
import re
import sys
import textwrap
import time
from pathlib import Path
from typing import Optional

# ── MCP SDK ───────────────────────────────────────────────────────────────────
try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print(
        "[ERROR] mcp package not installed.\n"
        "Install with:  pip install -r tools/requirements.txt",
        file=sys.stderr,
    )
    sys.exit(1)

# ── ANSI 颜色 ─────────────────────────────────────────────────────────────────

class C:
    _on = sys.stdout.isatty()

    RESET  = "\033[0m"  if _on else ""
    BOLD   = "\033[1m"  if _on else ""
    DIM    = "\033[2m"  if _on else ""
    RED    = "\033[91m" if _on else ""
    GREEN  = "\033[92m" if _on else ""
    YELLOW = "\033[93m" if _on else ""
    BLUE   = "\033[94m" if _on else ""
    CYAN   = "\033[96m" if _on else ""

    @staticmethod
    def disable() -> None:
        for attr in ("RESET", "BOLD", "DIM", "RED", "GREEN", "YELLOW", "BLUE", "CYAN"):
            setattr(C, attr, "")


# ── 安全结果分类 ───────────────────────────────────────────────────────────────

def classify_result(text: str) -> str:
    """
    从工具返回的文本推断安全层结果。

    mcp_server.py 对安全错误统一加了 [SECURITY:DENIED] / [ERROR] 前缀，
    此函数据此分类：
      PASS    — 返回正常结果（无错误前缀）
      DENY    — [SECURITY:DENIED] 且消息含 deny / restrict / blocked 关键词
      CONFIRM — [SECURITY:DENIED] 且消息含 confirm / denied by confirm 关键词
      ERROR   — [ERROR] 或连接异常
    """
    if text.startswith("[SECURITY:DENIED]"):
        low = text.lower()
        if "confirm" in low:
            return "CONFIRM"
        return "DENY"
    if text.startswith("[ERROR]"):
        return "ERROR"
    return "PASS"


def outcome_badge(outcome: str) -> str:
    badges = {
        "PASS":    f"{C.GREEN}[PASS   ]{C.RESET}",
        "DENY":    f"{C.RED}[DENY   ]{C.RESET}",
        "CONFIRM": f"{C.YELLOW}[CONFIRM]{C.RESET}",
        "ERROR":   f"{C.DIM}[ERROR  ]{C.RESET}",
    }
    return badges.get(outcome, f"[{outcome:7}]")


# ── 工具调用 ──────────────────────────────────────────────────────────────────

async def call_tool(
    session: "ClientSession",
    tool_name: str,
    params: dict,
) -> tuple[str, float]:
    """
    调用 MCP 工具，返回 (结果文本, 耗时秒数)。

    MCP 工具返回的 content 列表中每个 TextContent 拼接成一个字符串。
    若 result.isError 为 True，取错误信息并加 [ERROR] 前缀。
    """
    t0 = time.monotonic()
    try:
        result = await session.call_tool(tool_name, params)
    except Exception as exc:
        elapsed = time.monotonic() - t0
        return f"[ERROR] MCP call failed: {exc}", elapsed

    elapsed = time.monotonic() - t0

    parts = []
    for content in result.content:
        text = getattr(content, "text", None)
        if text is not None:
            parts.append(text)

    text = "\n".join(parts) if parts else "(empty response)"

    if result.isError:
        text = f"[ERROR] {text}"

    return text, elapsed


# ── 脚本解析 ──────────────────────────────────────────────────────────────────

_EXPECT_RE = re.compile(r"#\s*@expect:\s*(PASS|DENY|CONFIRM)", re.IGNORECASE)


def parse_script(content: str) -> list[tuple[str, Optional[str]]]:
    """
    解析脚本文件，返回 [(命令行, 预期结果 | None)] 列表。

    规则：
    - 空行/纯注释行忽略（但 @expect 注解保留，关联到下一条命令）
    - # @expect: PASS/DENY/CONFIRM 设置下一条命令的预期结果
    - 其余行作为命令行
    """
    items: list[tuple[str, Optional[str]]] = []
    pending_expect: Optional[str] = None

    for raw_line in content.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = _EXPECT_RE.match(line)
            if m:
                pending_expect = m.group(1).upper()
            continue
        items.append((line, pending_expect))
        pending_expect = None

    return items


def parse_command(line: str) -> tuple[str, dict]:
    """
    解析 'tool_name [JSON params]' 格式，返回 (工具名, 参数字典)。

    允许参数省略（等价于空对象 {}）。
    """
    parts = line.split(None, 1)
    tool  = parts[0]
    if len(parts) == 1:
        return tool, {}
    try:
        params = json.loads(parts[1])
        if not isinstance(params, dict):
            raise ValueError("params must be a JSON object")
        return tool, params
    except (json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"invalid params JSON: {exc}") from exc


# ── 输出格式化 ────────────────────────────────────────────────────────────────

def print_result(
    tool_name: str,
    text: str,
    elapsed: float,
    expected: Optional[str] = None,
    line_no: Optional[int] = None,
) -> bool:
    """
    打印工具调用结果。

    返回 True 表示符合预期（或无预期），False 表示不符合预期。
    """
    outcome = classify_result(text)
    badge   = outcome_badge(outcome)
    ms      = elapsed * 1000

    label = f"[{line_no}] {tool_name}" if line_no is not None else tool_name

    # ── 标题行 ────────────────────────────────────────────────────────────────
    if expected:
        matched = _expectations_match(outcome, expected)
        check   = f"{C.GREEN}✓{C.RESET}" if matched else f"{C.RED}✗{C.RESET}"
        exp_str = f"  期望: {C.DIM}{expected}{C.RESET}  {check}"
    else:
        matched = True
        exp_str = ""

    print(f"\n{C.DIM}{'─' * 62}{C.RESET}")
    print(f"  {badge}  {C.BOLD}{label}{C.RESET}  {C.DIM}({ms:.0f}ms){C.RESET}{exp_str}")
    print(f"{C.DIM}{'─' * 62}{C.RESET}")

    # ── 结果正文 ──────────────────────────────────────────────────────────────
    if outcome == "PASS":
        # 尝试美化 JSON 输出
        stripped = text.strip()
        try:
            parsed = json.loads(stripped)
            print(json.dumps(parsed, ensure_ascii=False, indent=2))
        except (json.JSONDecodeError, ValueError):
            print(stripped)
    else:
        color = C.RED if outcome in ("DENY", "ERROR") else C.YELLOW
        print(f"{color}{text}{C.RESET}")

    return matched


def _expectations_match(outcome: str, expected: str) -> bool:
    """
    判断实际结果是否符合预期。

    CONFIRM 的实际结果可能是 CONFIRM（确认等待超时/拒绝）或 DENY（confirm 通道无客户端时），
    两种情况都算符合预期。
    """
    if expected == "CONFIRM":
        return outcome in ("CONFIRM", "DENY")
    return outcome == expected


# ── 帮助系统 ──────────────────────────────────────────────────────────────────

def print_tools_table(tools: list) -> None:
    """打印可用工具列表及简短描述。"""
    print(f"\n{C.BOLD}  可用工具 ({len(tools)} 个){C.RESET}")
    print(f"  {'─' * 58}")
    for tool in sorted(tools, key=lambda t: t.name):
        desc = (tool.description or "").splitlines()[0][:52]
        print(f"  {C.CYAN}{tool.name:<22}{C.RESET}  {C.DIM}{desc}{C.RESET}")
    print()


def print_tool_detail(tool) -> None:
    """打印单个工具的详细信息（描述 + 参数 schema）。"""
    print(f"\n{C.BOLD}  {tool.name}{C.RESET}")
    if tool.description:
        for line in textwrap.wrap(tool.description, width=66):
            print(f"  {line}")

    schema = getattr(tool, "inputSchema", None)
    if schema and isinstance(schema, dict):
        props = schema.get("properties", {})
        req   = set(schema.get("required", []))
        if props:
            print(f"\n  {C.BOLD}参数:{C.RESET}")
            for name, info in props.items():
                type_str = info.get("type", "any")
                desc_str = info.get("description", "")[:44]
                flag     = f"{C.RED}*{C.RESET}" if name in req else " "
                print(f"    {flag} {C.CYAN}{name:<18}{C.RESET} {type_str:<8}  {C.DIM}{desc_str}{C.RESET}")
    print()


# ── 执行单条命令 ──────────────────────────────────────────────────────────────

async def execute_line(
    session: "ClientSession",
    tools_by_name: dict,
    line: str,
    expected: Optional[str] = None,
    line_no: Optional[int] = None,
) -> Optional[bool]:
    """
    解析并执行一行命令。

    返回值：
      True   — 命令执行且符合预期（或无预期）
      False  — 命令执行但不符合预期
      None   — 遇到 quit 类命令，调用方应退出
    """
    line = line.strip()
    if not line or line.startswith("#"):
        return True

    # ── 内置命令 ──────────────────────────────────────────────────────────────
    if line in ("quit", "exit", "q"):
        return None

    if line == "tools":
        print_tools_table(list(tools_by_name.values()))
        return True

    if line.startswith("help"):
        parts = line.split(None, 1)
        if len(parts) == 1:
            _print_interactive_help(tools_by_name)
        else:
            name = parts[1].strip()
            if name in tools_by_name:
                print_tool_detail(tools_by_name[name])
            else:
                print(f"{C.RED}未知工具: {name}{C.RESET}  (输入 'tools' 查看所有工具)")
        return True

    # ── 工具调用 ──────────────────────────────────────────────────────────────
    try:
        tool_name, params = parse_command(line)
    except ValueError as exc:
        print(f"{C.RED}[PARSE ERROR]{C.RESET} {exc}")
        return True

    if tool_name not in tools_by_name:
        print(f"{C.RED}[UNKNOWN TOOL]{C.RESET} '{tool_name}'  (输入 'tools' 查看所有工具)")
        return True

    text, elapsed = await call_tool(session, tool_name, params)
    matched = print_result(tool_name, text, elapsed, expected, line_no)
    return matched


def _print_interactive_help(tools_by_name: dict) -> None:
    print(textwrap.dedent(f"""
    {C.BOLD}命令格式:{C.RESET}
      <工具名> [JSON 参数]

    {C.BOLD}示例:{C.RESET}
      list_windows
      get_ui_tree {{"window_id": "w123_0", "max_depth": 4}}
      click {{"element_path": "/w123_0/AXButton[OK]"}}
      key_combo {{"window_id": "w123_0", "keys": ["Cmd", "C"]}}
      set_value {{"element_path": "/w123_0/AXTextField[0]", "value": "hello"}}

    {C.BOLD}内置命令:{C.RESET}
      tools            列出所有可用工具
      help             显示此帮助
      help <工具名>    显示指定工具的详细说明和参数
      quit / q         退出
    """))


# ── 交互模式 ──────────────────────────────────────────────────────────────────

async def run_interactive(
    session: "ClientSession",
    tools_by_name: dict,
) -> None:
    print(textwrap.dedent(f"""
    {C.BOLD}┌──────────────────────────────────────────────────────────────┐
    │  ClawSpan MCP 测试客户端                                 │
    │  输入 help 查看用法，tools 列出所有工具，quit 退出           │
    └──────────────────────────────────────────────────────────────┘{C.RESET}
    """))

    loop = asyncio.get_event_loop()

    while True:
        try:
            line = await loop.run_in_executor(None, lambda: input("mcp> "))
        except (EOFError, KeyboardInterrupt):
            print(f"\n{C.DIM}bye{C.RESET}")
            break

        result = await execute_line(session, tools_by_name, line)
        if result is None:
            print(f"{C.DIM}bye{C.RESET}")
            break


# ── 脚本模式 ──────────────────────────────────────────────────────────────────

async def run_script(
    session: "ClientSession",
    tools_by_name: dict,
    script_path: str,
) -> None:
    try:
        content = Path(script_path).read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"{C.RED}[ERROR]{C.RESET} 脚本文件不存在：{script_path}")
        sys.exit(1)
    except OSError as exc:
        print(f"{C.RED}[ERROR]{C.RESET} 无法读取脚本：{exc}")
        sys.exit(1)

    items = parse_script(content)
    total = len(items)
    print(f"执行脚本：{script_path}（{total} 条命令）\n")

    matched_count = 0
    expected_count = 0

    for idx, (cmd, expected) in enumerate(items, start=1):
        if cmd in ("quit", "exit", "q"):
            print(f"\n{C.DIM}脚本遇到 quit，提前终止{C.RESET}")
            break

        result = await execute_line(session, tools_by_name, cmd, expected, idx)

        if expected is not None:
            expected_count += 1
            if result:
                matched_count += 1

    # ── 脚本汇总 ──────────────────────────────────────────────────────────────
    print(f"\n{C.DIM}{'─' * 62}{C.RESET}")
    print(f"  脚本执行完成  共 {total} 条命令", end="")
    if expected_count > 0:
        failed = expected_count - matched_count
        status = (
            f"{C.GREEN}全部符合预期{C.RESET}"
            if failed == 0
            else f"{C.RED}{failed} 项不符合预期{C.RESET}"
        )
        print(f"  @expect 验证: {matched_count}/{expected_count}  {status}", end="")
    print()


# ── 主流程 ────────────────────────────────────────────────────────────────────

def find_server_script(hint: Optional[str]) -> str:
    """定位 mcp_server.py：优先使用 hint，其次在同目录查找。"""
    if hint:
        p = Path(hint)
        if p.exists():
            return str(p)
        print(f"{C.RED}[ERROR]{C.RESET} 找不到 MCP server：{hint}", file=sys.stderr)
        sys.exit(1)

    # 自动查找：同目录 → 项目 tools/ 目录
    candidates = [
        Path(__file__).parent / "mcp_server.py",
        Path.cwd() / "tools" / "mcp_server.py",
    ]
    for p in candidates:
        if p.exists():
            return str(p)

    print(
        f"{C.RED}[ERROR]{C.RESET} 找不到 mcp_server.py。\n"
        "  请用 --server 指定路径，或从项目根目录运行。",
        file=sys.stderr,
    )
    sys.exit(1)


async def async_main(args: argparse.Namespace) -> None:
    server_script = find_server_script(args.server)

    # 构造服务器启动参数，将 socket 路径通过环境变量传给 mcp_server.py
    env = {**os.environ}
    if args.sock:
        env["CLAWSPAN_SOCK"] = args.sock

    server_params = StdioServerParameters(
        command=sys.executable,
        args=[server_script],
        env=env,
    )

    print(f"  MCP server : {server_script}")
    print(f"  Daemon sock: {env.get('CLAWSPAN_SOCK', '/tmp/crew-shell-service.sock')}\n")

    try:
        async with stdio_client(server_params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                tools_resp = await session.list_tools()
                tools_by_name = {t.name: t for t in tools_resp.tools}

                if args.list_tools:
                    print_tools_table(list(tools_by_name.values()))
                    return

                print_tools_table(list(tools_by_name.values()))

                if args.file:
                    await run_script(session, tools_by_name, args.file)
                else:
                    await run_interactive(session, tools_by_name)

    except ConnectionError as exc:
        print(f"\n{C.RED}[ERROR]{C.RESET} 连接 MCP server 失败：{exc}")
        print("  请确认 mcp_server.py 能够正常启动，且 crew-shell-service 正在运行。")
        sys.exit(1)
    except KeyboardInterrupt:
        print(f"\n{C.DIM}[中断]{C.RESET}")


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="mcp_client.py",
        description="ClawSpan MCP 测试客户端（完整链路：MCP → mcp_server → IPC → daemon）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
            示例:
              %(prog)s                          # 交互模式
              %(prog)s -f scripts/normal.txt    # 执行脚本
              %(prog)s --list-tools             # 列出所有工具后退出
              %(prog)s --sock /tmp/custom.sock  # 指定 daemon socket

            脚本格式 (script.txt):
              # 注释行
              list_windows
              # @expect: PASS
              get_ui_tree {"window_id": "w123_0", "max_depth": 3}
              # @expect: DENY
              key_combo {"window_id": "w123_0", "keys": ["Cmd", "C"]}
        """),
    )
    parser.add_argument(
        "-f", "--file",
        metavar="SCRIPT",
        help="从脚本文件加载命令并依次执行（批量模式）",
    )
    parser.add_argument(
        "--server",
        metavar="PATH",
        help="mcp_server.py 路径（默认：自动查找同目录下的 mcp_server.py）",
    )
    parser.add_argument(
        "--sock",
        metavar="PATH",
        help="crew-shell-service Unix socket 路径（默认：/tmp/crew-shell-service.sock）",
    )
    parser.add_argument(
        "--list-tools",
        action="store_true",
        help="列出所有可用 MCP 工具后退出",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="禁用 ANSI 颜色输出（用于日志重定向）",
    )

    args = parser.parse_args()

    if args.no_color:
        C.disable()

    asyncio.run(async_main(args))


if __name__ == "__main__":
    main()
