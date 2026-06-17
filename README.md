# ClawSpan

ClawSpan 是为 **AI Agent** 设计的**宿主机侧能力网关**。它使用虚拟化技术将 AI Agent 隔离在独立环境中运行，同时将其能力安全、可控地延伸到宿主机，使 Agent 能够在用户授权与审查下操作真实桌面。

**核心理念**：AI Agent 运行在隔离环境中，通过 ClawSpan 这扇受控的窗户访问宿主机；所有操作经安全网关审查，用户始终掌握最终决策权。

## 赞助商

<p align="center">
  <a href="https://code.cyan-land.com/"><strong>cyan-land｜Codex 专用中转站</strong></a><br>
  Codex 专用中转站，主打接入稳定、价格划算、小众隐私、安全省心，支持 GPT-5.5、GPT-5.4模型，适合日常开发、团队协作和长期项目工作流。
</p>

---

## 项目愿景

系统由两个隔离域构成：

| 域 | 组成 | 职责 |
|----|------|------|
| **隔离环境** | AI Agent + MCP Client | Agent 在虚拟化环境中运行，无法直接访问宿主机 |
| **宿主机** | ClawSpan daemon + 能力插件 + WinForms UI | 接收 Agent 请求，经安全审查后执行 GUI 操作 |

Agent 想做任何事，必须经过 ClawSpan 的审批。安全网关运行在宿主机上，独立于 Agent，即使 Agent 被劫持，攻击者能发出的也只是「请求」，能否执行由宿主机单独决定。

**能力延伸**：ClawSpan 将宿主机的能力（窗口枚举、UI 树获取、点击、输入、滚动、按键等）以 MCP Tool 形式暴露给 Agent，使 Agent 在隔离环境中仍能操作用户真实桌面，但全程处于安全可控状态。

---

## 特性

- **虚拟化隔离** — AI Agent 运行在独立环境中，无法直接访问宿主机，所有操作经 ClawSpan 网关审批
- **能力延伸** — 将宿主机 GUI 能力以 MCP Tool 形式安全暴露给 Agent，支持窗口枚举、UI 树、点击、输入等
- **结构化 GUI 描述** — 使用 UI Automation 输出 AXT 紧凑文本，无需截图，纯文本模型即可理解
- **安全网关** — 操作前/后双向审查，危险操作需用户确认，用户始终掌握最终决策权
- **意图指纹缓存** — 用户勾选「本任务内不再询问」后，相同类型操作自动放行，减少重复弹窗
- **任务生命周期管理** — 每次 Agent 会话对应一个任务，授权缓存随任务结束自动清除，防止权限跨任务扩散
- **UI 事件总线** — WinForms UI 通过 UI 事件通道实时接收任务状态、操作日志与确认请求
- **C++ 实现** — 宿主机 daemon 单一二进制分发，无需 Python 运行时
- **MCP 协议** — 兼容 Claude Desktop、Cursor、OpenClaw 等 MCP 客户端

---

## 快速安装（推荐）

### 系统要求

| 要求 | 说明 |
|------|------|
| Windows 10 2004 (Build 19041) 或更高 | WSL2 最低要求 |
| WSL2 已启用 | 见下方说明 |
| 硬盘空间 ≥ 4 GB | rootfs + 二进制文件 |
| 内存 ≥ 8 GB | WSL2 + Agent 运行 |

**WSL2 未安装时**，在管理员 PowerShell 中执行：

```powershell
wsl --install --no-distribution
```

安装完成后**重启电脑**，再继续。

### 一键安装

在 **PowerShell** 中执行（无需管理员权限）：

```powershell
# 安装最新版本
irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex

# 安装指定版本
irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex -Version 0.1.0

# 或直接下载该版本的安装脚本（URL 本身固定了版本，无需 -Version 参数）
irm https://github.com/carlos-Ng/ClawSpan/releases/download/v0.1.0/install.ps1 | iex
```

安装程序会自动完成：检查 WSL2、下载组件、导入 VM 镜像、配置 AI 后端、生成 Gateway Token、配置开机自启并启动 ClawSpan。安装结束后终端会显示 OpenClaw WebUI 地址与 Token。

### 离线安装（本地包）

适用于网络受限场景。先从 Release 页面下载以下文件到本地：

- `install.ps1`
- `clawspan-windows-<ver>.zip`
- `clawspan-rootfs.tar.gz`

#### 默认离线模式（仅 `-Offline`）

将三个文件放在同一目录后执行：

```powershell
.\install.ps1 -Offline
```

脚本会自动在 `install.ps1` 同级目录搜索安装包并开始安装。

#### 进阶离线模式（手动指定路径）

```powershell
# 方式 1：分别指定 zip 和 rootfs
.\install.ps1 -Offline -WindowsZipPath "D:\pkg\clawspan-windows-0.1.1.zip" -RootfsPath "D:\pkg\clawspan-rootfs.tar.gz"

# 方式 2：指定包含资源的目录（脚本会自动搜索）
.\install.ps1 -Offline -AssetDir "D:\pkg"
```

> 当前版本离线模式仅支持全新安装，离线升级暂不支持。

### 升级与卸载

```powershell
# 升级到最新版（保留数据）
irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex -Upgrade

# 卸载
irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex -Uninstall
```

---

## 首次使用

1. **确认 ClawSpan 运行**：系统托盘出现 ClawSpan 图标，或手动启动 `%LOCALAPPDATA%\ClawSpan\bin\claw_span_ui.exe`
2. **访问 OpenClaw**：浏览器打开 `http://localhost:18789`，输入安装时显示的 Token（保存在 `%LOCALAPPDATA%\ClawSpan\gateway-token.txt`）
3. **Claude Desktop 集成**：在 `%APPDATA%\Claude\claude_desktop_config.json` 的 `mcpServers` 中添加 `clawspan`，`args` 指向 `%LOCALAPPDATA%\ClawSpan\mcp\mcp_server.py`

---

## 架构概览

```
╔══════════════════════════════════════════════════════════════╗
║  隔离环境（WSL2 虚拟机）                                      ║
║  ┌────────────────────────────────────────────────────────┐  ║
║  │  AI Agent（OpenClaw / Claude Desktop / Cursor 等）      │  ║
║  └────────────────────────────┬───────────────────────────┘  ║
║                               │ MCP (stdio)                  ║
║  ┌────────────────────────────▼───────────────────────────┐  ║
║  │  mcp_server.py — MCP 协议桥接，通过 AF_VSOCK 转发       │  ║
║  └────────────────────────────┬───────────────────────────┘  ║
╚═══════════════════════════════╪══════════════════════════════╝
                                │ VM Gateway 通道（AF_VSOCK）
╔═══════════════════════════════╪══════════════════════════════╗
║  宿主机                        ▼                              ║
║  ┌────────────────────────────────────────────────────────┐  ║
║  │  claw_span_service + VsockServer + claw_span_vmm       │  ║
║  │  ├── TaskRegistry、SecurityChain、CapabilityService     │  ║
║  │  ├── capability_ax.dll、security_filter.dll             │  ║
║  │  └── UIService — UI 事件通道事件总线                     │  ║
║  └──────────────────────┬─────────────────────────────────┘  ║
║                         │ UI 事件通道（Named Pipe）           ║
║                         ▼ status/task/op_log/confirm         ║
║  ┌─────────────────────────────────────────────────────────┐  ║
║  │  ClawSpan UI (WinForms) — 托盘 · 任务监控 · 确认弹窗     │  ║
║  └─────────────────────────────────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════╝
```

> **Phase 1 兼容**：MCP Server 也可直接运行在宿主机，通过 Named Pipe 与 daemon 通信，用于开发调试与 Claude Desktop 等本地 MCP 客户端集成。

---

## IPC 通道

ClawSpan 使用两条独立的 Named Pipe 通道：

### VM 控制通道：VM → Daemon（能力调用）

MCP Server 与 daemon 之间的主通信通道，使用类型化消息协议：

| 消息类型 | 方向 | 说明 |
|---------|------|------|
| `beginTask` | VM→Daemon | 开始一个 Agent 任务，返回 `task_id` |
| `endTask` | VM→Daemon | 结束任务（通知语义，无响应） |
| `capability` | VM→Daemon | 调用能力（携带 `task_id`、操作名、参数） |
| `capability_result` | Daemon→VM | 能力调用结果（成功或失败） |

### UI 事件通道：Daemon ↔ UI（事件总线）

daemon 与宿主机 WinForms UI 之间的双向事件通道：

| 消息类型 | 方向 | 说明 |
|---------|------|------|
| `status` | Daemon→UI | daemon 连接状态 |
| `task_begin` | Daemon→UI | 任务开始通知 |
| `task_end` | Daemon→UI | 任务结束通知 |
| `op_log` | Daemon→UI | 操作日志（allowed / confirmed / denied） |
| `confirm` | Daemon→UI | 需用户确认的操作请求 |
| `confirm_response` | UI→Daemon | 用户确认结果（含 `trust_fingerprint`） |

---

## 支持的操作

| 操作 | 说明 |
|------|------|
| `list_windows` | 枚举可访问的顶层窗口 |
| `get_ui_tree` | 获取窗口的 UI 元素树（AXT 格式） |
| `click` / `double_click` / `right_click` | 点击元素 |
| `set_value` | 向输入框写入文本 |
| `focus` | 将焦点移至元素 |
| `scroll` | 滚动可滚动区域 |
| `key_press` / `key_combo` | 按键与组合键 |
| `activate_window` | 激活窗口到前台 |

---

## 构建

### 环境要求

- Windows 10 及以上
- CMake 3.20+
- Visual Studio 2022（或 Ninja + MSVC）
- C++20 编译器

### 构建步骤

```powershell
# 配置
cmake -S . -B build-release

# 编译
cmake --build build-release --config Release

# 生成 dist 发布目录（exe + dll + config）
cmake --build build-release --config Release --target dist

# 打包 Release zip（供 GitHub Release 上传）
cmake --build build-release --config Release --target release
# 输出：clawspan-windows-<version>.zip（位于项目根目录）
```

### 构建产物

| 路径 | 说明 |
|------|------|
| `build\Debug\claw_span_service.exe` | 主 daemon |
| `build\Debug\claw_span_vmm.exe` | VM 管理器 |
| `build\daemon-service\Debug\capability_ax.dll` | AX 能力模块 |
| `build\Debug\security_filter.dll` | 安全过滤模块 |
| `ui\bin\Release\net8.0-windows\win-x64\publish\claw_span_ui.exe` | 托盘 UI |
| `dist\` | 发布目录（`--target dist` 后生成） |

---

## 运行

### 方式一：从 dist 目录运行（推荐）

```powershell
cd dist
.\bin\claw_span_service.exe -f -c config\clawspan.toml
```

`-f` 表示前台运行，日志输出到控制台。

### 方式二：从项目根目录运行（开发模式）

```powershell
.\build\Debug\claw_span_service.exe -f
```

顶层 `clawspan.toml` 的 `module_dir` 指向 `build\daemon-service`，适合开发调试。

### 命令行参数

| 参数 | 说明 |
|------|------|
| `-c, --config <path>` | 配置文件路径（默认 `clawspan.toml`） |
| `-f, --foreground` | 前台运行，日志输出到控制台 |
| `--log-level <level>` | 日志级别：trace / debug / info / warn / error |
| `--socket <path>` | 覆盖 VM 控制通道管道路径 |
| `--module-dir <path>` | 覆盖模块 DLL 目录 |

---

## 测试工具

### 安装 Python 依赖

```powershell
pip install -r tools\requirements.txt
```

### AX 测试客户端（直连 VM 控制通道）

```powershell
# 先启动 daemon，再在另一终端运行
python tools\ax_test_client.py

# 批量执行脚本
python tools\ax_test_client.py -f tools\scripts\01_normal_task.txt
```

### MCP Server（供 AI Agent 使用）

```powershell
python tools\mcp_server.py
```

### 构建 VM rootfs

rootfs 包含 WSL2 虚拟机镜像（Debian + OpenClaw + ClawSpan MCP Server）。在 Linux/WSL2 环境中执行：

```bash
bash scripts/build-rootfs.sh
```

生成 `clawspan-rootfs.tar.gz`，上传到 GitHub Release 供 `install.ps1` 下载。

### 发布 Release

每次发布时，将以下文件上传到 GitHub Release（tag 格式：`v<version>`）：

| 文件 | 来源 |
|------|------|
| `clawspan-windows-<ver>.zip` | `cmake --build build-release --config Release --target release` 生成 |
| `clawspan-rootfs.tar.gz` | `bash scripts/build-rootfs.sh` 生成 |
| `install.ps1` | `scripts/install.ps1` |

---

## 与 Claude Desktop 集成

在 `%APPDATA%\Claude\claude_desktop_config.json` 中添加：

```json
{
  "mcpServers": {
    "clawspan": {
      "command": "python",
      "args": ["C:\\path\\to\\ClawSpan\\tools\\mcp_server.py"]
    }
  }
}
```

将 `C:\path\to\ClawSpan` 替换为实际项目路径。

---

## 安全机制

### 安全审查链

每次能力调用依次经过安全模块的 `preHook`（入站审查）和 `postHook`（出站过滤）：

```
请求到达
  │
  ▼ preHook（执行前）
  │  Deny        → 拒绝，不执行，推送 op_log(denied/rule_deny)
  │  NeedConfirm → 检查意图指纹缓存
  │                └─ 命中  → 直接放行，推送 op_log(allowed/fingerprint_cache)
  │                └─ 未命中 → 向 UI 弹出确认弹窗
  │                           └─ 拒绝 → 推送 op_log(denied/user_confirm)
  │                           └─ 确认 → 可选缓存指纹，推送 op_log(confirmed/user_confirm)
  │  Pass        → 继续
  │
  ▼ 能力执行
  │
  ▼ postHook（执行后）
  │  可对返回内容脱敏
  │
  ▼ 返回结果，推送 op_log(allowed/auto_allow)
```

**裁决合并**：多个安全模块按优先级执行，最严格者胜出（Deny > NeedConfirm > Pass > Skip）。

### 意图指纹缓存

当用户在确认弹窗中勾选「本任务内相同操作不再询问」时，框架将该操作类型缓存为意图指纹（capability + operation）。同一任务内再次触发相同操作时直接放行，无需重复弹窗。任务结束时缓存自动清除，防止授权跨任务扩散。

### 规则配置

安全规则在 `config/security_filter_rules.toml` 中配置：

```toml
# 拒绝规则
[[deny]]
capability = "capability_ax"
operations = ["key_combo"]
params_field = "keys"
params_patterns = ["Win", "Cmd"]
reason = "Win/Cmd 组合键可触发系统级操作，已拦截"

# 确认规则（需用户确认）
[[confirm]]
capability = "capability_ax"
operations = ["click", "double_click", "right_click"]
reason = "GUI 点击操作需用户确认"
```

---

## 配置说明

### 主配置（clawspan.toml）

```toml
[daemon]
socket_path      = "\\\\.\\pipe\\crew-shell-service"  # VM 控制通道管道
thread_pool_size = 4
log_level        = "debug"
module_dir       = "lib"                              # DLL 目录（相对 dist）

[ui]
pipe_path    = "\\\\.\\pipe\\crew-shell-service-ui"   # UI 事件通道管道
timeout_mode = "timeout_deny"   # wait_forever / timeout_deny / timeout_allow
timeout_secs = 60               # 用户无响应超时（秒），wait_forever 时忽略

[[modules]]
name = "capability_ax"

[[modules]]
name       = "security_filter"
priority   = 10
rules_file = "config\\security_filter_rules.toml"
```

### timeout_mode 说明

| 值 | 行为 |
|----|------|
| `timeout_deny` | 超时后自动拒绝（默认，安全优先） |
| `timeout_allow` | 超时后自动允许（应用优先） |
| `wait_forever` | 无限等待用户响应 |

### 路径说明

- **从 dist 运行**：`module_dir = "lib"`，`rules_file` 使用 `config\` 相对路径
- **从项目根运行**：使用顶层 `clawspan.toml`，`module_dir = "build\\daemon-service"`

---

## 项目结构

```
ClawSpan/
├── include/                    # 公开头文件
├── daemon-service/             # C++ daemon 实现
├── vmm/                        # VM 管理器（claw_span_vmm.exe）
├── ui/                         # ClawSpan WinForms UI（C# .NET 8）
├── mcp/                        # MCP Server（Python，运行在 VM 内）
├── config/                     # 配置文件模板
├── scripts/                    # build-rootfs.sh、install.ps1
├── tools/                      # 开发测试工具（Python）
├── tests/                      # 单元测试
├── third_party/                # 第三方 header-only 库
└── clawspan.toml              # 项目根开发配置
```

---

## 技术栈

| 组件 | 选型 |
|------|------|
| 语言 | C++20 |
| 构建 | CMake |
| JSON | nlohmann/json |
| 错误处理 | tl::expected |
| 日志 | spdlog |
| 配置 | toml++ |
| 命令行 | cxxopts |
| GUI API | Windows UI Automation |

---

## 许可证

本项目采用 [MIT License](LICENSE) 开源协议。

- **商业使用**：允许
- **修改与分发**：允许，需保留版权声明与许可证全文
- **专利与责任**：无担保，详见 [LICENSE](LICENSE) 文件

Copyright (c) 2025 [carlos-Ng](https://github.com/carlos-Ng)
