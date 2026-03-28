#pragma once

// vmm_channel.h — VM 控制通道协议定义
//
// VM 控制通道负责 vmm.exe（host 侧进程）与 daemon 中 VmManager 组件的双向通信。
// 传输层：Windows Named Pipe，每个 vmm 实例对应独立管道：
//   \\.\pipe\clawspan-vmm-{distro_name}
//
// 协议：ClawSpan FrameCodec（4B 大端长度前缀 + UTF-8 JSON body），
//        消息通过 "type" 字段路由，与 UI 事件通道 / VM Gateway 通道格式完全一致。
//
// ─────────────────────────────────────────────────────────────────────────────
// 消息流向：
//
//   vmm → daemon（主动上报）:
//     vmm_register       — vmm 启动后立即发送，向 VmManager 注册自身
//     vmm_heartbeat      — vmm 内部定时器自动触发，上报 distro 当前状态
//     vmm_event          — distro 状态变化时主动推送事件
//     vmm_cmd_response   — 响应 daemon 下发的具体操作命令
//
//   daemon → vmm（操作命令）:
//     vmm_start          — 启动 distro（调用 IWSLDistribution::Launch 或 wsl.exe）
//     vmm_stop           — 停止 distro（调用 IWSLDistribution::Terminate 或 wsl.exe）
//     vmm_snapshot       — 导出快照（调用 wsl.exe --export）
//     vmm_restore        — 从快照恢复（调用 wsl.exe --import）
//     vmm_destroy        — 注销并删除 distro（调用 wsl.exe --unregister）
//
// 每条操作命令携带 "id" 字段；vmm_cmd_response 用同一 id 与命令对应。
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

namespace clawspan {
namespace vmm {

// ── 管道路径规范 ──────────────────────────────────────────────────────────────

// vmmPipeName 根据 distro 名称生成 VM 控制通道命名管道路径。
//
// 入参:
// - distro_name: distro 唯一名称（ASCII，如 "agent-enclave-vm"）
//
// 出参/返回: 形如 \\.\pipe\clawspan-vmm-{distro_name} 的管道路径
inline std::string vmmPipeName(const std::string& distro_name)
{
	return "\\\\.\\pipe\\clawspan-vmm-" + distro_name;
}

// ── DistroRunState ────────────────────────────────────────────────────────────

// DistroRunState 描述 vmm_heartbeat / vmm_event 中上报的 distro 运行状态。
// 与 DistroState 对应但以字符串形式在协议中传输，便于扩展和日志可读性。
//
// 协议值（JSON string）：
//   "running"   — distro 正在运行
//   "stopped"   — distro 已停止（正常）
//   "error"     — distro 异常退出，watchdog 正在决策是否重启
//   "restarting"— watchdog 正在重启 distro
constexpr const char* DISTRO_STATE_RUNNING    = "running";
constexpr const char* DISTRO_STATE_STOPPED    = "stopped";
constexpr const char* DISTRO_STATE_ERROR      = "error";
constexpr const char* DISTRO_STATE_RESTARTING = "restarting";

// ── VmmEvent ──────────────────────────────────────────────────────────────────

// VmmEvent 定义 vmm_event 消息中 "event" 字段的取值。
// 每个取值对应 distro 生命周期中的一个离散事件。
constexpr const char* VMM_EVENT_STARTED    = "started";    // distro 启动成功
constexpr const char* VMM_EVENT_STOPPED    = "stopped";    // distro 正常停止
constexpr const char* VMM_EVENT_CRASHED    = "crashed";    // distro 意外退出
constexpr const char* VMM_EVENT_RESTARTED  = "restarted";  // watchdog 完成重启
constexpr const char* VMM_EVENT_RESTART_FAILED = "restart_failed"; // 超过最大重试次数

// ── 消息类型字符串常量 ────────────────────────────────────────────────────────
//
// 消息类型对应 JSON 消息的 "type" 字段值。
// 命名规范：
//   vmm_*_request / vmm_*  — daemon 发出的操作命令
//   vmm_*_response / vmm_* — vmm 发出的响应或主动上报

// vmm → daemon
constexpr const char* MSG_VMM_REGISTER     = "vmm_register";     // 注册
constexpr const char* MSG_VMM_HEARTBEAT    = "vmm_heartbeat";    // 心跳
constexpr const char* MSG_VMM_EVENT        = "vmm_event";        // 事件
constexpr const char* MSG_VMM_CMD_RESPONSE = "vmm_cmd_response"; // 命令响应

// daemon → vmm
constexpr const char* MSG_VMM_START    = "vmm_start";    // 启动 distro
constexpr const char* MSG_VMM_STOP     = "vmm_stop";     // 停止 distro
constexpr const char* MSG_VMM_SNAPSHOT = "vmm_snapshot"; // 导出快照
constexpr const char* MSG_VMM_RESTORE  = "vmm_restore";  // 恢复快照
constexpr const char* MSG_VMM_DESTROY  = "vmm_destroy";  // 注销并删除 distro

// ─────────────────────────────────────────────────────────────────────────────
// 消息格式说明（JSON schema，供实现参考）
// ─────────────────────────────────────────────────────────────────────────────
//
// vmm_register（vmm → daemon）:
//   {"type":"vmm_register","distro_name":"agent-enclave-vm","pid":4096}
//
// vmm_heartbeat（vmm → daemon，定时自动发送）:
//   {"type":"vmm_heartbeat","distro_name":"agent-enclave-vm",
//    "state":"running","uptime_sec":3600,"restart_count":0}
//
// vmm_event（vmm → daemon）:
//   {"type":"vmm_event","distro_name":"agent-enclave-vm",
//    "event":"crashed","message":"distro process exited with code 1"}
//
// vmm_cmd_response（vmm → daemon）:
//   {"type":"vmm_cmd_response","id":42,"success":true}
//   {"type":"vmm_cmd_response","id":42,"success":false,"error":"Terminate failed: 0x80070005"}
//
// vmm_start（daemon → vmm）:
//   {"type":"vmm_start","id":1}
//
// vmm_stop（daemon → vmm）:
//   {"type":"vmm_stop","id":2}
//
// vmm_snapshot（daemon → vmm）:
//   {"type":"vmm_snapshot","id":3,
//    "snapshot_dir":"C:\\snapshots","snapshot_name":"snap-20260313"}
//
// vmm_restore（daemon → vmm）:
//   {"type":"vmm_restore","id":4,"snapshot_path":"C:\\snapshots\\snap-20260313.vhdx"}
//
// vmm_destroy（daemon → vmm）:
//   {"type":"vmm_destroy","id":5}

} // namespace vmm
} // namespace clawspan
