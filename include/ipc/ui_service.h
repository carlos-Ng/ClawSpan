#pragma once

#include "common/error.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace clawspan {
namespace ipc {

// ─────────────────────────────────────────────────────────────────────────────
// UIService：Channel 2 双向事件总线
//
// 职责：
//   1. 维护与 UI 客户端（ClawSpan WinForms）的 Named Pipe 连接
//   2. push()：向 UI 推送任意事件消息（status / task_begin / task_end / op_log）
//   3. askConfirm()：阻断式操作确认请求，等待 UI 用户响应
//
// 线程安全：push() 与 askConfirm() 可同时被多个线程调用，内部以 write_mutex_
// 序列化写入，以 pending_confirms_ + promise/future 管理 confirm 相关等待。
// ─────────────────────────────────────────────────────────────────────────────

// ConfirmTimeoutMode 定义当用户无响应时的处理策略。
// 可通过 clawspan.toml [ui] 节点配置。
enum class ConfirmTimeoutMode
{
    WAIT_FOREVER,  // 无限等待，适合用户在场场景
    TIMEOUT_DENY,  // 超时后自动拒绝（安全优先，适合用户不在场）
    TIMEOUT_ALLOW, // 超时后自动允许（应用优先，适合用户不在场）
};

// UIConfirmRequest 表示一次需要用户确认的操作请求。
struct UIConfirmRequest
{
    std::string    task_id;
    std::string    root_description; // Root Task 原始意图描述
    std::string    capability;
    std::string    operation;
    nlohmann::json params;           // 操作原始参数
    std::string    reason;           // 安全规则中的确认原因
    std::string    fingerprint;      // Intent Fingerprint，如 "click|finder|AXButton"
};

// UIConfirmResult 表示用户对一次确认请求的响应结果。
struct UIConfirmResult
{
    bool confirmed;        // true = 允许，false = 拒绝
    bool trust_fingerprint; // true = 本任务内相同操作不再询问
};

// ─────────────────────────────────────────────────────────────────────────────
// UIMessageFactory：UIService 消息简单工厂
//
// 所有向 UI 推送的消息均通过此类的静态方法构建，
// 调用方不直接拼接 JSON，消息格式集中维护于此。
// ─────────────────────────────────────────────────────────────────────────────
class UIMessageFactory
{
public:
    // createStatus 构造系统状态消息。
    // UI 连接时 daemon 立即推送一次，此后状态变更时再次推送。
    //
    // 入参:
    // - vm:       VM 状态 ("running" / "stopped" / "starting")
    // - openclaw: OpenClaw 状态 ("online" / "offline" / "unknown")
    // - channel:  调用通道状态 ("active" / "idle")
    static std::string createStatus(const std::string& vm,
                                    const std::string& openclaw,
                                    const std::string& channel);

    // createTaskBegin 构造任务开始消息。
    // 由 daemon 在收到 Channel 1 的 beginTask 消息后调用。
    static std::string createTaskBegin(const std::string& task_id,
                                       const std::string& root_description);

    // createTaskEnd 构造任务结束消息。
    // 由 daemon 在收到 Channel 1 的 endTask 消息后调用。
    static std::string createTaskEnd(const std::string& task_id);

    // createOpLog 构造操作日志消息。
    // 由 CapabilityService 在每次操作执行（或拦截）后调用。
    //
    // 入参:
    // - result: "allowed" / "denied" / "confirmed" / "cached"
    // - source: "auto_allow" / "rule_deny" / "user_confirm" / "fingerprint_cache"
    static std::string createOpLog(const std::string& task_id,
                                   const std::string& operation,
                                   const std::string& result,
                                   const std::string& source,
                                   const std::string& detail,
                                   int64_t            timestamp);

private:
    UIMessageFactory() = delete; // 纯静态工厂，禁止实例化
};

// ─────────────────────────────────────────────────────────────────────────────
// UIService 接口
// ─────────────────────────────────────────────────────────────────────────────
class UIService
{
public:
    UIService();
    ~UIService();

    UIService(const UIService&)            = delete;
    UIService& operator=(const UIService&) = delete;

    // start 启动 Named Pipe 服务，开始等待 UI 客户端连接。
    // UI 断线后自动重新监听，无需手动重启。
    //
    // 入参:
    // - pipe_path:      Named Pipe 路径，如 \\.\pipe\crew-shell-service-ui。
    // - timeout_mode:   无响应时的处理策略。
    // - timeout_secs:   超时时长（WAIT_FOREVER 时忽略此参数）。
    // - status_provider: 回调，返回当前状态 JSON，在 UI 连接时立即推送。
    //
    // 出参/返回:
    // - Status::Ok()：启动成功。
    // - Status(error)：创建管道失败。
    Status start(std::string_view                  pipe_path,
                 ConfirmTimeoutMode                timeout_mode,
                 std::chrono::seconds              timeout_secs,
                 std::function<std::string()>      status_provider);

    // stop 停止服务，断开连接，取消所有待处理的 confirm 请求（自动拒绝）。
    void stop();

    // push 非阻断推送一条 JSON 消息到 UI。
    // 若 UI 未连接，忽略此调用。
    void push(const std::string& json);

    // askConfirm 向 UI 发送确认请求并阻断等待用户响应。
    // 行为受 timeout_mode 控制；UI 未连接或 stop() 被调用时均自动返回 {false, false}。
    //
    // 入参:
    // - req: 操作确认请求信息。
    //
    // 出参/返回:
    // - UIConfirmResult：用户确认结果。
    UIConfirmResult askConfirm(const UIConfirmRequest& req);

    // isConnected 返回 UI 客户端当前是否已连接。
    bool isConnected() const;

private:
    struct Implement;
    std::unique_ptr<Implement> impl_;
};

} // namespace ipc
} // namespace clawspan
