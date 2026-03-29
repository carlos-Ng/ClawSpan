using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace ClawSpanUI.Channel
{

// DaemonChannel 负责与 daemon 的 UI 事件通道（确认通道）通信。
// 协议：Length-prefix framing（4 字节大端序长度头 + JSON 消息体）。
// 断线后自动重连，无需外部干预。
public class DaemonChannel : IDisposable
{
	// UI 事件通道 Named Pipe 名称（UIService 事件总线）
	private const string PIPE_NAME = "crew-shell-service-ui";

	// 断线后重连等待时间（毫秒）
	private const int RECONNECT_DELAY_MS = 3000;

	// 允许的最大帧大小（4MB），防止 OOM 攻击
	private const int MAX_FRAME_SIZE = 4 * 1024 * 1024;

	private CancellationTokenSource? _cts;
	private Task? _readTask;

	// 当前活动的管道流，写操作需加锁
	private NamedPipeClientStream? _pipe;
	private readonly SemaphoreSlim _writeLock = new(1, 1);

	// 连接状态变更事件（参数：是否已连接）
	public event Action<bool>? OnConnectionChanged;

	// 收到数据帧事件（参数：原始 JSON 字符串）
	public event Action<string>? OnFrameReceived;

	// ─────────────────────────────────────────────────────────
	// 生命周期
	// ─────────────────────────────────────────────────────────

	// Start 启动后台连接与读取循环。
	public void Start()
	{
		if (_cts != null) {
			return;
		}
		_cts = new CancellationTokenSource();
		_readTask = Task.Run(() => RunConnectLoop(_cts.Token));
	}

	// Stop 停止所有后台任务并关闭管道连接。
	public void Stop()
	{
		_cts?.Cancel();
		_pipe?.Close();
		_pipe?.Dispose();
		_pipe = null;
		_cts = null;
	}

	// Dispose 释放所有资源。
	public void Dispose()
	{
		Stop();
		_writeLock.Dispose();
		_cts?.Dispose();
	}

	// ─────────────────────────────────────────────────────────
	// 发送
	// ─────────────────────────────────────────────────────────

	// SendAsync 将 JSON 字符串以 Length-prefix 帧格式写入管道。
	// 若当前未连接，直接返回（不抛异常）。
	//
	// 入参:
	// - json: 要发送的 JSON 字符串。
	public async Task SendAsync(string json)
	{
		var pipe = _pipe;
		if (pipe == null || !pipe.IsConnected) {
			return;
		}

		var body = Encoding.UTF8.GetBytes(json);
		var header = BuildHeader(body.Length);

		await _writeLock.WaitAsync().ConfigureAwait(false);
		try {
			await pipe.WriteAsync(header).ConfigureAwait(false);
			await pipe.WriteAsync(body).ConfigureAwait(false);
			await pipe.FlushAsync().ConfigureAwait(false);
		} catch (Exception) {
			// 写入失败时忽略，读取循环会检测到断线并重连
		} finally {
			_writeLock.Release();
		}
	}

	// ─────────────────────────────────────────────────────────
	// 内部：连接循环
	// ─────────────────────────────────────────────────────────

	// RunConnectLoop 持续尝试连接 daemon，断线后等待重连。
	//
	// 入参:
	// - token: 用于停止循环的取消令牌。
	private async Task RunConnectLoop(CancellationToken token)
	{
		while (!token.IsCancellationRequested) {
			var pipe = new NamedPipeClientStream(
				".", PIPE_NAME, PipeDirection.InOut, PipeOptions.Asynchronous);

			try {
				await ConnectWithTimeoutAsync(pipe, token).ConfigureAwait(false);

				_pipe = pipe;
				RaiseConnectionChanged(true);

				await RunReadLoop(pipe, token).ConfigureAwait(false);
			} catch (OperationCanceledException) {
				pipe.Dispose();
				break;
			} catch (Exception) {
				// 连接失败或读取中断，准备重连
			} finally {
				_pipe = null;
			}

			RaiseConnectionChanged(false);
			pipe.Dispose();

			// 等待重连间隔
			try {
				await Task.Delay(RECONNECT_DELAY_MS, token).ConfigureAwait(false);
			} catch (OperationCanceledException) {
				break;
			}
		}
	}

	// ConnectWithTimeoutAsync 尝试连接管道，连接超时后会重试。
	//
	// 入参:
	// - pipe:  要连接的管道流。
	// - token: 取消令牌。
	private static async Task ConnectWithTimeoutAsync(
		NamedPipeClientStream pipe, CancellationToken token)
	{
		// 使用独立超时令牌，确保超时后当前连接尝试会被真正取消。
		using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(token);
		timeoutCts.CancelAfter(5000);

		try {
			await pipe.ConnectAsync(timeoutCts.Token).ConfigureAwait(false);
		} catch (OperationCanceledException) when (!token.IsCancellationRequested) {
			throw new TimeoutException("连接 daemon 超时");
		}

		if (!pipe.IsConnected) {
			throw new IOException("连接 daemon 失败");
		}
	}

	// RunReadLoop 在已连接的管道上持续读取帧，直到连接断开或取消。
	//
	// 入参:
	// - pipe:  已连接的管道流。
	// - token: 取消令牌。
	private async Task RunReadLoop(NamedPipeClientStream pipe, CancellationToken token)
	{
		var header = new byte[4];

		while (!token.IsCancellationRequested && pipe.IsConnected) {
			// 读取 4 字节大端序长度头
			await ReadExactAsync(pipe, header, 4, token).ConfigureAwait(false);
			int length = ParseBigEndianInt32(header);

			if (length <= 0 || length > MAX_FRAME_SIZE) {
				throw new InvalidDataException($"非法帧长度: {length}");
			}

			// 读取消息体
			var body = new byte[length];
			await ReadExactAsync(pipe, body, length, token).ConfigureAwait(false);

			var json = Encoding.UTF8.GetString(body);
			RaiseFrameReceived(json);
		}
	}

	// RaiseConnectionChanged 触发连接状态事件，隔离订阅方异常，避免中断重连循环。
	private void RaiseConnectionChanged(bool connected)
	{
		var handlers = OnConnectionChanged;
		if (handlers == null) {
			return;
		}
		foreach (var handler in handlers.GetInvocationList()) {
			try {
				((Action<bool>)handler)(connected);
			} catch {
				// 订阅方异常不应影响通信层重连逻辑
			}
		}
	}

	// RaiseFrameReceived 触发消息事件，隔离订阅方异常，避免中断读取循环。
	private void RaiseFrameReceived(string json)
	{
		var handlers = OnFrameReceived;
		if (handlers == null) {
			return;
		}
		foreach (var handler in handlers.GetInvocationList()) {
			try {
				((Action<string>)handler)(json);
			} catch {
				// 订阅方异常不应导致管道读循环退出
			}
		}
	}

	// ─────────────────────────────────────────────────────────
	// 内部：帧编解码工具
	// ─────────────────────────────────────────────────────────

	// ReadExactAsync 从流中精确读取 count 字节，处理 TCP/Pipe 分片。
	//
	// 入参:
	// - stream: 要读取的流。
	// - buffer: 目标缓冲区。
	// - count:  需要读取的字节数。
	// - token:  取消令牌。
	private static async Task ReadExactAsync(
		Stream stream, byte[] buffer, int count, CancellationToken token)
	{
		int offset = 0;
		while (offset < count) {
			int read = await stream.ReadAsync(
				buffer, offset, count - offset, token).ConfigureAwait(false);
			if (read == 0) {
				throw new EndOfStreamException("管道连接已关闭");
			}
			offset += read;
		}
	}

	// BuildHeader 将帧长度编码为 4 字节大端序头。
	//
	// 入参:
	// - length: 消息体字节数。
	//
	// 返回: 4 字节大端序字节数组。
	private static byte[] BuildHeader(int length)
	{
		return new byte[] {
			(byte)(length >> 24),
			(byte)(length >> 16),
			(byte)(length >> 8),
			(byte)(length),
		};
	}

	// ParseBigEndianInt32 将 4 字节大端序字节数组解析为 int。
	//
	// 入参:
	// - header: 4 字节大端序字节数组。
	//
	// 返回: 解析后的 int 值。
	private static int ParseBigEndianInt32(byte[] header)
	{
		return (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
	}
}

} // namespace ClawSpanUI.Channel
