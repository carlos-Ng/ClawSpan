// vm_manager_test.cc — WslVMManager 单元/集成测试
//
// 测试策略：
//   - 工厂函数、纯查询方法：始终运行（无副作用）
//   - 需要实际 WSL2 环境的生命周期操作（create/start/stop/destroy/snapshot/restore）：
//     标记为 DISABLED_，手动 --gtest_also_run_disabled_tests 执行
//
// 注意：本文件的 DISABLED_ 测试会在系统上创建/删除真实 WSL2 distro，
//       仅在开发机手动运行。

#include "vmm/vm_manager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace clawspan {
namespace vmm {
namespace {

// 集成测试使用的 distro 名称，加 _test 后缀避免与真实 distro 冲突
static const std::wstring kTestDistroName = L"clawspan-vmm-test";

// ── 工厂函数 ────────────────────────────────────────────────────────────────

TEST(VMManagerFactory, CreateReturnsNonNull)
{
	auto mgr = createVMManager();
	ASSERT_NE(mgr, nullptr);
}

// ── 查询方法（无副作用，始终可运行）────────────────────────────────────────

class VMManagerQueryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		mgr_ = createVMManager();
		ASSERT_NE(mgr_, nullptr);
	}
	std::unique_ptr<VMManagerInterface> mgr_;
};

// 查询不存在的 distro 应返回 NOT_REGISTERED
TEST_F(VMManagerQueryTest, GetStateUnregistered)
{
	auto state = mgr_->getDistroState(L"nonexistent-distro-12345");
	EXPECT_EQ(state, DistroState::NOT_REGISTERED);
}

// listDistros 不崩溃、返回合理值
TEST_F(VMManagerQueryTest, ListDistrosDoesNotCrash)
{
	auto list = mgr_->listDistros();
	// 不断言具体内容，只确认不崩溃
	// 若系统装有 WSL distro，列表非空
	(void)list;
}

// lastDiagnostics 初始为空或有效字符串
TEST_F(VMManagerQueryTest, LastDiagnosticsInitiallyEmpty)
{
	auto diag = mgr_->lastDiagnostics();
	// 初始状态 last_hr_ == S_OK，应为空
	EXPECT_TRUE(diag.empty());
}

// 对不存在的 distro 执行 runCommand
// WslLaunch 行为不保证对不存在的 distro 返回 INVALID_HANDLE_VALUE，
// 可能返回一个立即退出的句柄。此测试仅验证不崩溃。
TEST_F(VMManagerQueryTest, RunCommandNonExistentDistroDoesNotCrash)
{
	void* h = mgr_->runCommand(L"nonexistent-distro-12345", L"echo hello");
	if (h != INVALID_HANDLE_VALUE) {
		// 若返回了有效句柄，等待并关闭
		WaitForSingleObject(static_cast<HANDLE>(h), 5000);
		CloseHandle(static_cast<HANDLE>(h));
	}
}

// startDistro 对不存在的 distro 应返回 NOT_FOUND
TEST_F(VMManagerQueryTest, StartNonExistentDistroReturnsNotFound)
{
	auto st = mgr_->startDistro(L"nonexistent-distro-12345");
	EXPECT_EQ(st.code, Status::NOT_FOUND);
}

// destroyDistro 对不存在的 distro 应返回 OK（幂等语义）
TEST_F(VMManagerQueryTest, DestroyNonExistentDistroIsIdempotent)
{
	auto st = mgr_->destroyDistro(L"nonexistent-distro-12345");
	EXPECT_TRUE(st.ok());
}

// snapshotDistro 对不存在的 distro 应返回 NOT_FOUND
TEST_F(VMManagerQueryTest, SnapshotNonExistentDistroReturnsNotFound)
{
	std::wstring out;
	auto st = mgr_->snapshotDistro(L"nonexistent-distro-12345",
	                                L"C:\\Temp", L"test", out);
	EXPECT_EQ(st.code, Status::NOT_FOUND);
	EXPECT_TRUE(out.empty());
}

// ── 生命周期集成测试（需要 WSL2 + rootfs tarball）────────────────────────────
//
// 默认 DISABLED，通过 --gtest_also_run_disabled_tests 手动启用。
// 需要环境变量 CLAWSPAN_TEST_ROOTFS 指向 Ubuntu rootfs .tar.gz 文件。

class DISABLED_VMManagerLifecycleTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		mgr_ = createVMManager();
		ASSERT_NE(mgr_, nullptr);

		// 清理可能的残留 distro
		mgr_->destroyDistro(kTestDistroName);

		// 获取 rootfs 路径
		const char* rootfs = std::getenv("CLAWSPAN_TEST_ROOTFS");
		if (!rootfs || std::string(rootfs).empty()) {
			GTEST_SKIP() << "CLAWSPAN_TEST_ROOTFS not set, skipping lifecycle test";
		}
		rootfs_path_ = std::wstring(rootfs, rootfs + std::strlen(rootfs));
	}

	void TearDown() override
	{
		if (mgr_) {
			mgr_->destroyDistro(kTestDistroName);
		}
	}

	DistroConfig makeTestConfig()
	{
		DistroConfig cfg;
		cfg.name = kTestDistroName;
		cfg.rootfs_path = rootfs_path_;
		cfg.default_uid = 1000;
		return cfg;
	}

	std::unique_ptr<VMManagerInterface> mgr_;
	std::wstring rootfs_path_;
};

TEST_F(DISABLED_VMManagerLifecycleTest, CreateStartStopDestroy)
{
	auto cfg = makeTestConfig();

	// 创建
	auto st = mgr_->createDistro(cfg);
	ASSERT_TRUE(st.ok()) << "createDistro failed: " << st.message;

	// 创建后应为 STOPPED 或 RUNNING（取决于 WSL 行为）
	auto state = mgr_->getDistroState(kTestDistroName);
	EXPECT_NE(state, DistroState::NOT_REGISTERED);

	// 启动
	st = mgr_->startDistro(kTestDistroName);
	EXPECT_TRUE(st.ok()) << "startDistro failed: " << st.message;

	state = mgr_->getDistroState(kTestDistroName);
	// COM 路径下应为 RUNNING；wslapi 回退下可能仍报 STOPPED
	EXPECT_NE(state, DistroState::NOT_REGISTERED);

	// 停止
	st = mgr_->stopDistro(kTestDistroName);
	EXPECT_TRUE(st.ok()) << "stopDistro failed: " << st.message;

	// 销毁
	st = mgr_->destroyDistro(kTestDistroName);
	EXPECT_TRUE(st.ok()) << "destroyDistro failed: " << st.message;

	state = mgr_->getDistroState(kTestDistroName);
	EXPECT_EQ(state, DistroState::NOT_REGISTERED);
}

TEST_F(DISABLED_VMManagerLifecycleTest, DuplicateCreateReturnsAlreadyExists)
{
	auto cfg = makeTestConfig();

	auto st = mgr_->createDistro(cfg);
	ASSERT_TRUE(st.ok()) << "first createDistro failed: " << st.message;

	// 重复创建应返回 ALREADY_EXISTS
	st = mgr_->createDistro(cfg);
	EXPECT_EQ(st.code, Status::ALREADY_EXISTS);
}

TEST_F(DISABLED_VMManagerLifecycleTest, SnapshotAndRestore)
{
	auto cfg = makeTestConfig();

	auto st = mgr_->createDistro(cfg);
	ASSERT_TRUE(st.ok()) << "createDistro failed: " << st.message;

	// 快照
	std::wstring out_path;
	st = mgr_->snapshotDistro(kTestDistroName, L"C:\\Temp\\clawspan-test",
	                           L"test-snapshot", out_path);
	ASSERT_TRUE(st.ok()) << "snapshotDistro failed: " << st.message;
	EXPECT_FALSE(out_path.empty());

	// 销毁
	st = mgr_->destroyDistro(kTestDistroName);
	ASSERT_TRUE(st.ok());

	// 从快照恢复
	st = mgr_->restoreFromSnapshot(cfg, out_path);
	EXPECT_TRUE(st.ok()) << "restoreFromSnapshot failed: " << st.message;

	auto state = mgr_->getDistroState(kTestDistroName);
	EXPECT_NE(state, DistroState::NOT_REGISTERED);

	// 清理快照文件
	DeleteFileW(out_path.c_str());
}

TEST_F(DISABLED_VMManagerLifecycleTest, RunCommandInDistro)
{
	auto cfg = makeTestConfig();

	auto st = mgr_->createDistro(cfg);
	ASSERT_TRUE(st.ok());

	st = mgr_->startDistro(kTestDistroName);
	ASSERT_TRUE(st.ok());

	// 执行命令
	void* handle = mgr_->runCommand(kTestDistroName, L"echo hello");
	ASSERT_NE(handle, INVALID_HANDLE_VALUE);

	// 等待命令完成
	DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle), 30000);
	EXPECT_EQ(result, WAIT_OBJECT_0);

	DWORD exit_code = 1;
	GetExitCodeProcess(static_cast<HANDLE>(handle), &exit_code);
	EXPECT_EQ(exit_code, 0u);

	CloseHandle(static_cast<HANDLE>(handle));
}

TEST_F(DISABLED_VMManagerLifecycleTest, ListDistrosContainsCreated)
{
	auto cfg = makeTestConfig();

	auto st = mgr_->createDistro(cfg);
	ASSERT_TRUE(st.ok());

	auto list = mgr_->listDistros();
	bool found = std::any_of(list.begin(), list.end(),
	                          [](const std::wstring& n) {
		return n == kTestDistroName;
	});
	EXPECT_TRUE(found) << "created distro not in listDistros()";
}

} // namespace
} // namespace vmm
} // namespace clawspan
