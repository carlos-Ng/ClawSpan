#!/usr/bin/env bash
# build-rootfs.sh — ClawSpan VM rootfs 构建工具
#
# 在 WSL2 内构建 Debian bookworm rootfs，预装 OpenClaw + ClawSpan MCP Server。
# 生成的 tar.gz 可直接用于 wsl --import。
#
# 支持断点续建：每个阶段完成后保存 checkpoint（仅保留最近一个），
# 构建中断后重跑自动从上次完成的位置继续。构建成功后自动清除 checkpoint。
#
# 依赖：debootstrap, tar, gzip（preflight 阶段会自动检查并安装）

set -euo pipefail

VERSION="1.0.0"

# ── 配置 ─────────────────────────────────────────────────────────────────

SUITE="bookworm"
MIRROR="https://deb.debian.org/debian"
ROOTFS_DIR="/tmp/clawspan-rootfs"
OUTPUT="$(pwd)/clawspan-rootfs.tar.gz"
NODE_MAJOR=22
CLAWSPAN_USER="clawspan"
MCP_INSTALL_DIR="/opt/clawspan/mcp"

# 持久缓存目录
CACHE_DIR="/var/cache/clawspan-build"
APT_CACHE_DIR="$CACHE_DIR/apt-archives"
DEB_TARBALL="$CACHE_DIR/debootstrap-debs.tar"

# checkpoint：只保留一个
CHECKPOINT_FILE="$CACHE_DIR/checkpoint.tar.gz"
CHECKPOINT_STAGE_FILE="$CACHE_DIR/checkpoint.stage"

# debootstrap --include
DEBOOTSTRAP_INCLUDE="systemd,systemd-sysv,dbus,dbus-user-session,libpam-systemd,ca-certificates,curl,wget,gnupg,git,locales,procps,iproute2,iputils-ping,less,vim-tiny,sudo"

# ClawSpan 源码目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLAWSPAN_SRC=""

TOTAL_STAGES=7

# ── 帮助信息 ─────────────────────────────────────────────────────────────

show_help() {
    cat <<'HELP'
用法: sudo ./build-rootfs.sh [命令] [选项]

构建 ClawSpan VM rootfs（Debian bookworm + OpenClaw + MCP Server）。

命令（互斥，默认为构建）:
  (无)                 构建 rootfs（支持断点续建）
  --rebuild            清除所有缓存，完全重新构建
  --clean              清除构建缓存（checkpoint + 下载缓存），不构建
  --list               查看缓存状态，不构建
  --help               显示此帮助信息

构建选项:
  --src DIR            指定 ClawSpan 源码目录（含 mcp/、scripts/）
                       默认自动检测脚本所在位置或 /mnt/c/Users/*/ClawSpan
  -o, --output FILE    指定输出文件路径（默认: 当前目录/clawspan-rootfs.tar.gz）

构建阶段:
  1  debootstrap       创建 Debian 基础系统（预下载 deb 包，离线安装）
  2  基础配置          apt 源、locale、hostname
  3  Python 3          python3 + pip + venv
  4  Node.js + pnpm    Node.js 22 + corepack + pnpm
  5  OpenClaw          pnpm 全局安装 openclaw
  6  ClawSpan MCP     部署 MCP Server + OpenClaw skill + 用户配置
  7  瘦身              清理缓存、文档、日志

断点续建:
  每个阶段完成后保存 checkpoint（仅保留最近一个，节省磁盘）。
  构建中断后重跑自动从上次位置继续。构建成功后自动清除 checkpoint。
  网络不稳定时 debootstrap 包和 apt 包都有本地缓存，不会重复下载。

示例:
  sudo ./build-rootfs.sh                                    # 构建（断点续建）
  sudo ./build-rootfs.sh --src /mnt/c/Users/me/ClawSpan    # 指定源码目录
  sudo ./build-rootfs.sh -o /mnt/c/rootfs.tar.gz            # 指定输出路径
  sudo ./build-rootfs.sh --rebuild                          # 全部重来
  sudo ./build-rootfs.sh --clean                            # 清除缓存
  sudo ./build-rootfs.sh --list                             # 查看状态
HELP
}

# ── 参数解析 ──────────────────────────────────────────────────────────────

ACTION="build"       # build | rebuild | clean | list | help

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            ACTION="help"
            shift
            ;;
        --rebuild)
            ACTION="rebuild"
            shift
            ;;
        --clean)
            ACTION="clean"
            shift
            ;;
        --list)
            ACTION="list"
            shift
            ;;
        --src)
            CLAWSPAN_SRC="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT="$(cd "$(dirname "$2")" 2>/dev/null && pwd)/$(basename "$2")"
            shift 2
            ;;
        -*)
            echo "未知选项: $1（使用 --help 查看用法）" >&2
            exit 1
            ;;
        *)
            OUTPUT="$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")"
            shift
            ;;
    esac
done

# ── --help ───────────────────────────────────────────────────────────────

if [[ "$ACTION" == "help" ]]; then
    show_help
    exit 0
fi

# ── checkpoint 工具函数 ──────────────────────────────────────────────────

save_checkpoint() {
    local stage=$1
    echo "  ✓ 保存 checkpoint (stage $stage) ..."
    # 先写临时文件，成功后原子替换
    tar -czf "$CHECKPOINT_FILE.tmp" -C "$ROOTFS_DIR" .
    mv "$CHECKPOINT_FILE.tmp" "$CHECKPOINT_FILE"
    echo "$stage" > "$CHECKPOINT_STAGE_FILE"
    local size
    size=$(du -sh "$CHECKPOINT_FILE" | cut -f1)
    echo "  ✓ checkpoint 已保存 (stage $stage, $size)"
}

restore_checkpoint() {
    echo "  ↻ 从 checkpoint 恢复 ..."
    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"
    tar -xzf "$CHECKPOINT_FILE" -C "$ROOTFS_DIR"
    echo "  ✓ 恢复完成"
}

get_checkpoint_stage() {
    if [[ -f "$CHECKPOINT_FILE" && -f "$CHECKPOINT_STAGE_FILE" ]]; then
        cat "$CHECKPOINT_STAGE_FILE"
    else
        echo 0
    fi
}

clean_checkpoint() {
    rm -f "$CHECKPOINT_FILE" "$CHECKPOINT_FILE.tmp" "$CHECKPOINT_STAGE_FILE"
}

clean_download_cache() {
    rm -f "$DEB_TARBALL"
    rm -rf "$APT_CACHE_DIR"
    mkdir -p "$APT_CACHE_DIR"
}

show_status() {
    echo ""
    echo "ClawSpan rootfs 构建状态"
    echo "────────────────────────────────────────"

    # checkpoint
    local ckpt_stage
    ckpt_stage=$(get_checkpoint_stage)
    if [[ $ckpt_stage -gt 0 ]]; then
        local size ts
        size=$(du -sh "$CHECKPOINT_FILE" | cut -f1)
        ts=$(date -r "$CHECKPOINT_FILE" '+%Y-%m-%d %H:%M:%S')
        echo "  checkpoint:  stage $ckpt_stage/$TOTAL_STAGES  $size  $ts"
        echo "               下次构建从 stage $((ckpt_stage + 1)) 继续"
    else
        echo "  checkpoint:  无（从头构建）"
    fi

    # 下载缓存
    echo ""
    if [[ -f "$DEB_TARBALL" ]]; then
        printf "  debootstrap: %s\n" "$(du -sh "$DEB_TARBALL" | cut -f1)"
    else
        echo "  debootstrap: 未缓存"
    fi

    local apt_count
    apt_count=$(find "$APT_CACHE_DIR" -name '*.deb' 2>/dev/null | wc -l)
    if [[ $apt_count -gt 0 ]]; then
        printf "  apt 缓存:    %s (%d 个 deb)\n" "$(du -sh "$APT_CACHE_DIR" | cut -f1)" "$apt_count"
    else
        echo "  apt 缓存:    空"
    fi

    echo ""
    echo "  缓存目录:    $CACHE_DIR"
    echo "  总占用:      $(du -sh "$CACHE_DIR" 2>/dev/null | cut -f1)"
    echo ""
}

# ── --list ───────────────────────────────────────────────────────────────

if [[ "$ACTION" == "list" ]]; then
    mkdir -p "$CACHE_DIR" "$APT_CACHE_DIR"
    show_status
    exit 0
fi

# ── --clean ──────────────────────────────────────────────────────────────

if [[ "$ACTION" == "clean" ]]; then
    echo "清除所有构建缓存 ..."
    clean_checkpoint
    clean_download_cache
    rm -rf "$ROOTFS_DIR"
    echo "✓ 已清除：checkpoint、下载缓存、临时 rootfs"
    exit 0
fi

# ── 前置检查（Preflight） ─────────────────────────────────────────────────

echo ""
echo "══════════════════════════════════════════"
echo " Preflight 检查"
echo "══════════════════════════════════════════"
echo ""

PREFLIGHT_ERRORS=()
PREFLIGHT_WARNINGS=()

# 1. 权限
if [[ $EUID -ne 0 ]]; then
    echo "  ✗ 需要 root 权限" >&2
    echo "" >&2
    echo "请使用：sudo $0 $*" >&2
    exit 1
fi
echo "  ✓ root 权限"

# 2. 宿主机工具
REQUIRED_TOOLS=(
    "debootstrap:创建 Debian 基础系统:debootstrap"
    "tar:打包/解包 rootfs:tar"
    "gzip:压缩/解压:gzip"
    "mount:挂载虚拟文件系统:mount"
    "umount:卸载虚拟文件系统:umount"
    "chroot:在 rootfs 内执行命令:coreutils"
)

MISSING_TOOLS=()
AUTO_INSTALL_PKGS=()

for entry in "${REQUIRED_TOOLS[@]}"; do
    IFS=: read -r cmd purpose pkg <<< "$entry"
    if command -v "$cmd" &>/dev/null; then
        echo "  ✓ $cmd"
    else
        echo "  ✗ $cmd — $purpose"
        MISSING_TOOLS+=("$cmd")
        AUTO_INSTALL_PKGS+=("$pkg")
    fi
done

if [[ ${#MISSING_TOOLS[@]} -gt 0 ]]; then
    echo ""
    echo "  缺少 ${#MISSING_TOOLS[@]} 个工具，尝试自动安装 ..."
    UNIQUE_PKGS=($(printf '%s\n' "${AUTO_INSTALL_PKGS[@]}" | sort -u))
    if apt-get update -qq 2>/dev/null && apt-get install -y -qq "${UNIQUE_PKGS[@]}" 2>/dev/null; then
        STILL_MISSING=()
        for cmd in "${MISSING_TOOLS[@]}"; do
            if command -v "$cmd" &>/dev/null; then
                echo "  ✓ $cmd (已自动安装)"
            else
                STILL_MISSING+=("$cmd")
            fi
        done
        if [[ ${#STILL_MISSING[@]} -gt 0 ]]; then
            PREFLIGHT_ERRORS+=("以下工具安装失败: ${STILL_MISSING[*]}")
        fi
    else
        PREFLIGHT_ERRORS+=("自动安装失败，请手动执行: apt-get install ${UNIQUE_PKGS[*]}")
    fi
fi

# 3. ClawSpan 源码目录
if [[ -z "$CLAWSPAN_SRC" ]]; then
    _auto="$(dirname "$SCRIPT_DIR")"
    if [[ -f "$_auto/mcp/server/mcp_server.py" ]]; then
        CLAWSPAN_SRC="$_auto"
    fi
fi

if [[ -z "$CLAWSPAN_SRC" || ! -f "$CLAWSPAN_SRC/mcp/server/mcp_server.py" ]]; then
    for _try in /mnt/c/Users/*/ClawSpan /mnt/d/ClawSpan /mnt/c/ClawSpan; do
        # shellcheck disable=SC2086
        for _dir in $_try; do
            if [[ -f "$_dir/mcp/server/mcp_server.py" ]]; then
                CLAWSPAN_SRC="$_dir"
                break 2
            fi
        done
    done
fi

if [[ -z "$CLAWSPAN_SRC" || ! -d "$CLAWSPAN_SRC" ]]; then
    echo "  ✗ ClawSpan 源码目录未找到"
    PREFLIGHT_ERRORS+=("请用 --src 指定源码目录: sudo $0 --src /mnt/c/Users/你的用户名/ClawSpan")
else
    echo "  ✓ 源码目录: $CLAWSPAN_SRC"
    for _sf in \
        mcp/server/mcp_server.py \
        mcp/server/launch_mcp_server.py \
        mcp/server/vsock_client.py \
        mcp/server/channel3_grpc_client.py \
        mcp/server/channel3_tunnel.py \
        mcp/server/generated/channel3_pb2.py \
        mcp/server/generated/channel3_pb2_grpc.py \
        mcp/client/clawspan-gui/SKILL.md; do
        if [[ -f "$CLAWSPAN_SRC/$_sf" ]]; then
            echo "  ✓ $_sf"
        else
            echo "  ✗ $_sf"
            PREFLIGHT_ERRORS+=("源码文件缺失: $CLAWSPAN_SRC/$_sf")
        fi
    done
fi

# 4. 磁盘空间
check_disk_space() {
    local path=$1 need_gb=$2 label=$3
    local avail_kb
    avail_kb=$(df --output=avail "$path" 2>/dev/null | tail -1)
    if [[ -n "$avail_kb" ]]; then
        local avail_gb=$(( avail_kb / 1024 / 1024 ))
        if [[ $avail_gb -lt $need_gb ]]; then
            echo "  ⚠ $label: ${avail_gb}GB 可用，建议 ${need_gb}GB"
            PREFLIGHT_WARNINGS+=("$label: ${avail_gb}GB 可用, 建议 ${need_gb}GB")
        else
            echo "  ✓ $label: ${avail_gb}GB 可用"
        fi
    fi
}

check_disk_space "/tmp" 3 "/tmp"
check_disk_space "$CACHE_DIR" 2 "$CACHE_DIR"

# 5. 网络
if curl -sf --max-time 5 -o /dev/null "$MIRROR/dists/$SUITE/Release" 2>/dev/null; then
    echo "  ✓ 网络可达"
elif wget -q --timeout=5 --spider "$MIRROR/dists/$SUITE/Release" 2>/dev/null; then
    echo "  ✓ 网络可达"
else
    echo "  ⚠ 无法连接 Debian 镜像"
    if [[ -f "$DEB_TARBALL" ]]; then
        echo "    (已有本地缓存，stage 1 可离线)"
    else
        PREFLIGHT_WARNINGS+=("网络不可用且无本地缓存")
    fi
fi

# 结果
echo ""
if [[ ${#PREFLIGHT_WARNINGS[@]} -gt 0 ]]; then
    echo "⚠ 警告:"
    for w in "${PREFLIGHT_WARNINGS[@]}"; do echo "  - $w"; done
    echo ""
fi

if [[ ${#PREFLIGHT_ERRORS[@]} -gt 0 ]]; then
    echo "✗ 无法继续:" >&2
    for e in "${PREFLIGHT_ERRORS[@]}"; do echo "  - $e" >&2; done
    echo "" >&2
    exit 1
fi

echo "✓ Preflight 通过"
echo ""

mkdir -p "$CACHE_DIR" "$APT_CACHE_DIR"

# ── apt 缓存 helper ──────────────────────────────────────────────────────

mount_apt_cache() {
    mkdir -p "$ROOTFS_DIR/var/cache/apt/archives/partial"
    mount --bind "$APT_CACHE_DIR" "$ROOTFS_DIR/var/cache/apt/archives"
}

umount_apt_cache() {
    umount "$ROOTFS_DIR/var/cache/apt/archives" 2>/dev/null || true
}

# ── 挂载/卸载 helper ─────────────────────────────────────────────────────

mount_vfs() {
    umount "$ROOTFS_DIR/sys"  2>/dev/null || true
    umount "$ROOTFS_DIR/proc" 2>/dev/null || true
    umount "$ROOTFS_DIR/dev"  2>/dev/null || true
    mount --bind /dev  "$ROOTFS_DIR/dev"
    mount --bind /proc "$ROOTFS_DIR/proc"
    mount --bind /sys  "$ROOTFS_DIR/sys"
}

umount_vfs() {
    umount_apt_cache
    umount "$ROOTFS_DIR/sys"  2>/dev/null || true
    umount "$ROOTFS_DIR/proc" 2>/dev/null || true
    umount "$ROOTFS_DIR/dev"  2>/dev/null || true
}

trap umount_vfs EXIT

# ── --rebuild 模式：清除后继续构建 ────────────────────────────────────────

if [[ "$ACTION" == "rebuild" ]]; then
    echo "清除所有缓存，全新构建 ..."
    clean_checkpoint
    clean_download_cache
    rm -rf "$ROOTFS_DIR"
    echo ""
fi

# ── 确定从哪个阶段开始 ──────────────────────────────────────────────────

LAST_COMPLETED=$(get_checkpoint_stage)

if [[ $LAST_COMPLETED -gt 0 && $LAST_COMPLETED -lt $TOTAL_STAGES ]]; then
    echo "════════════════════════════════════════"
    echo " 从 stage $((LAST_COMPLETED + 1)) 继续（stage 1~$LAST_COMPLETED 已缓存）"
    echo "════════════════════════════════════════"
    echo ""
    restore_checkpoint
    RESUME_FROM=$((LAST_COMPLETED + 1))
elif [[ $LAST_COMPLETED -ge $TOTAL_STAGES ]]; then
    echo "所有阶段已完成，直接打包。"
    echo "如需重新构建，使用 --rebuild"
    restore_checkpoint
    RESUME_FROM=$((TOTAL_STAGES + 1))
else
    echo "从头开始构建 ..."
    RESUME_FROM=1
fi

# ── 运行阶段 helper ──────────────────────────────────────────────────────

run_stage() {
    local stage=$1 name=$2
    if [[ $RESUME_FROM -gt $stage ]]; then
        echo "=== [${stage}/${TOTAL_STAGES}] ${name} === (跳过)"
        return 0
    fi
    echo ""
    echo "=== [${stage}/${TOTAL_STAGES}] ${name} ==="
    echo ""
    return 1
}

# ── 阶段 1：debootstrap ──────────────────────────────────────────────────

if ! run_stage 1 "debootstrap：Debian $SUITE 基础系统"; then

    # 1a：预下载
    if [[ -f "$DEB_TARBALL" ]]; then
        echo "  ✓ deb 包已缓存，跳过下载 ($(du -sh "$DEB_TARBALL" | cut -f1))"
    else
        echo "  ↓ 预下载 deb 包 ..."
        DOWNLOAD_TMP="/tmp/clawspan-debootstrap-download"
        rm -rf "$DOWNLOAD_TMP"
        mkdir -p "$DOWNLOAD_TMP"

        debootstrap \
            --make-tarball="$DEB_TARBALL" \
            --variant=minbase \
            --include="$DEBOOTSTRAP_INCLUDE" \
            "$SUITE" "$DOWNLOAD_TMP" "$MIRROR"

        rm -rf "$DOWNLOAD_TMP"
        echo "  ✓ 下载完成 ($(du -sh "$DEB_TARBALL" | cut -f1))"
    fi

    # 1b：离线安装
    echo "  ⚙ 离线安装基础系统 ..."
    [[ -d "$ROOTFS_DIR" ]] && rm -rf "$ROOTFS_DIR"

    debootstrap \
        --unpack-tarball="$DEB_TARBALL" \
        --variant=minbase \
        --include="$DEBOOTSTRAP_INCLUDE" \
        "$SUITE" "$ROOTFS_DIR"

    echo "  ✓ 基础系统完成 ($(du -sh "$ROOTFS_DIR" | cut -f1))"
    save_checkpoint 1
fi

# ── 阶段 2：基础配置 ─────────────────────────────────────────────────────

if ! run_stage 2 "基础配置"; then
    mount_vfs
    cp /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"

    cat > "$ROOTFS_DIR/etc/apt/sources.list" <<SOURCES
deb $MIRROR $SUITE main contrib
deb $MIRROR ${SUITE}-updates main contrib
deb http://security.debian.org/debian-security ${SUITE}-security main contrib
SOURCES

    mkdir -p "$ROOTFS_DIR/etc/apt/apt.conf.d"
    cat > "$ROOTFS_DIR/etc/apt/apt.conf.d/80clawspan-retry" <<'APTCONF'
Acquire::Retries "3";
Acquire::https::Timeout "30";
Acquire::http::Timeout "30";
APTCONF

    chroot "$ROOTFS_DIR" bash -c "
        sed -i 's/# en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
        locale-gen
    "
    echo "clawspan" > "$ROOTFS_DIR/etc/hostname"

    umount_vfs
    save_checkpoint 2
fi

# ── 阶段 3：Python 3 ─────────────────────────────────────────────────────

if ! run_stage 3 "Python 3"; then
    mount_vfs
    mount_apt_cache
    cp /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"

    chroot "$ROOTFS_DIR" bash -c "
        apt-get update -qq
        apt-get install -y -qq python3 python3-pip python3-venv
        python3 -m pip install --break-system-packages grpcio protobuf
        python3 --version
    "

    umount_vfs
    save_checkpoint 3
fi

# ── 阶段 4：Node.js + pnpm ──────────────────────────────────────────────

if ! run_stage 4 "Node.js $NODE_MAJOR + pnpm"; then
    mount_vfs
    mount_apt_cache
    cp /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"

    chroot "$ROOTFS_DIR" bash -c "
        mkdir -p /etc/apt/keyrings
        curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
            | gpg --dearmor -o /etc/apt/keyrings/nodesource.gpg
        echo \"deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_${NODE_MAJOR}.x nodistro main\" \
            > /etc/apt/sources.list.d/nodesource.list

        apt-get update -qq
        apt-get install -y -qq nodejs

        corepack enable
        corepack prepare pnpm@latest --activate

        export PNPM_HOME=/usr/local/share/pnpm
        mkdir -p \$PNPM_HOME

        cat > /etc/profile.d/pnpm.sh <<'PNPMSH'
export PNPM_HOME=/usr/local/share/pnpm
export PATH=\$PNPM_HOME:\$PATH
PNPMSH

        node --version
        pnpm --version
    "

    umount_vfs
    save_checkpoint 4
fi

# ── 阶段 5：OpenClaw ─────────────────────────────────────────────────────

if ! run_stage 5 "OpenClaw"; then
    mount_vfs
    mount_apt_cache
    cp /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"

    chroot "$ROOTFS_DIR" bash -c "
        export PNPM_HOME=/usr/local/share/pnpm
        export PATH=\$PNPM_HOME:\$PATH

        # openclaw 依赖需要 git（chroot 隔离，宿主的 git 不可见）
        apt-get update -qq
        apt-get install -y -qq git build-essential python3-dev lsof

        pnpm add -g openclaw@latest

        # 修复 acpx 扩展目录权限（pnpm 全局安装为 root，acpx 需要写入 node_modules）
        OPENCLAW_DIR=\$(pnpm root -g)/openclaw
        if [[ -d \"\$OPENCLAW_DIR/extensions/acpx\" ]]; then
            chown -R $CLAWSPAN_USER:$CLAWSPAN_USER \"\$OPENCLAW_DIR/extensions/acpx\"
        fi

        # 严格验证
        if ! command -v openclaw &>/dev/null; then
            echo '错误：openclaw 安装失败' >&2
            exit 1
        fi
        openclaw --version
    "

    umount_vfs
    save_checkpoint 5
fi

# ── 阶段 6：ClawSpan MCP Server ─────────────────────────────────────────

if ! run_stage 6 "ClawSpan MCP Server"; then
    mount_vfs

    # MCP Server
    mkdir -p "$ROOTFS_DIR$MCP_INSTALL_DIR"
    cp "$CLAWSPAN_SRC/mcp/server/vsock_client.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/"
    cp "$CLAWSPAN_SRC/mcp/server/launch_mcp_server.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/"
    cp "$CLAWSPAN_SRC/mcp/server/channel3_grpc_client.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/"
    cp "$CLAWSPAN_SRC/mcp/server/channel3_tunnel.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/"
    cp "$CLAWSPAN_SRC/mcp/server/mcp_server.py"   "$ROOTFS_DIR$MCP_INSTALL_DIR/"
    mkdir -p "$ROOTFS_DIR$MCP_INSTALL_DIR/generated"
    cp "$CLAWSPAN_SRC/mcp/server/generated/__init__.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/generated/"
    cp "$CLAWSPAN_SRC/mcp/server/generated/channel3_pb2.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/generated/"
    cp "$CLAWSPAN_SRC/mcp/server/generated/channel3_pb2_grpc.py" "$ROOTFS_DIR$MCP_INSTALL_DIR/generated/"

    # OpenClaw skill
    mkdir -p "$ROOTFS_DIR/opt/clawspan/skills/clawspan-gui"
    cp "$CLAWSPAN_SRC/mcp/client/clawspan-gui/SKILL.md" \
       "$ROOTFS_DIR/opt/clawspan/skills/clawspan-gui/"

    # 用户
    chroot "$ROOTFS_DIR" bash -c "
        useradd -m -s /bin/bash -G sudo $CLAWSPAN_USER
        echo '$CLAWSPAN_USER ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/$CLAWSPAN_USER
    "

    # OpenClaw 配置
    OPENCLAW_CONF_DIR="$ROOTFS_DIR/home/$CLAWSPAN_USER/.openclaw"
    mkdir -p "$OPENCLAW_CONF_DIR"
    cat > "$OPENCLAW_CONF_DIR/openclaw.json" <<CONF
{
  "gateway": {
    "port": 18789,
    "mode": "local",
    "bind": "lan",
    "auth": {
      "mode": "token"
    }
  },
  "plugins": {
    "entries": {
      "acpx": {
        "enabled": true,
        "config": {
          "mcpServers": {
            "clawspan-gui": {
              "command": "python3",
              "args": ["$MCP_INSTALL_DIR/launch_mcp_server.py"]
            }
          }
        }
      }
    }
  },
  "skills": {
    "load": {
      "extraDirs": ["/opt/clawspan/skills"]
    }
  }
}
CONF

    chroot "$ROOTFS_DIR" chown -R "$CLAWSPAN_USER:$CLAWSPAN_USER" "/home/$CLAWSPAN_USER/.openclaw"

    # 启用 linger，确保 systemd user 服务在 VM 启动时自动运行（无需用户登录）
    mkdir -p "$ROOTFS_DIR/var/lib/systemd/linger"
    touch "$ROOTFS_DIR/var/lib/systemd/linger/$CLAWSPAN_USER"

    # WSL 配置
    cat > "$ROOTFS_DIR/etc/wsl.conf" <<WSL
[user]
default=$CLAWSPAN_USER

[boot]
systemd=true
command=/bin/bash -c 'loginctl enable-linger $CLAWSPAN_USER 2>/dev/null; true'

[network]
generateResolvConf=true

[automount]
enabled=false
mountFsTab=false

[interop]
enabled=false
appendWindowsPath=false
WSL

    umount_vfs
    save_checkpoint 6
fi

# ── 阶段 7：瘦身 ─────────────────────────────────────────────────────────

if ! run_stage 7 "清理瘦身"; then
    mount_vfs

    chroot "$ROOTFS_DIR" bash -c "
        export PNPM_HOME=/usr/local/share/pnpm
        export PATH=\$PNPM_HOME:\$PATH

        apt-get clean
        rm -rf /var/lib/apt/lists/*
        rm -rf /root/.cache/pip /home/$CLAWSPAN_USER/.cache/pip
        pnpm store prune 2>/dev/null || true

        rm -rf /usr/share/doc/* /usr/share/man/* /usr/share/info/*
        find /usr/share/locale -mindepth 1 -maxdepth 1 -type d \
            ! -name 'en' ! -name 'en_US' -exec rm -rf {} +

        find /var/log -type f -delete
        find / -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
        rm -rf /tmp/* /var/tmp/*
        rm -f /etc/apt/apt.conf.d/80clawspan-retry
    "

    umount_vfs
    # stage 7 不存 checkpoint，马上就打包了
fi

# ── 打包 ─────────────────────────────────────────────────────────────────

echo ""
echo "rootfs 大小: $(du -sh "$ROOTFS_DIR" | cut -f1)"
echo "正在打包 ..."
tar -czf "$OUTPUT" -C "$ROOTFS_DIR" .

# 构建成功，自动清除 checkpoint 节省空间
clean_checkpoint
echo "  ✓ checkpoint 已自动清除"

echo ""
echo "=========================================="
echo " ✓ 构建完成！"
echo ""
echo " 输出: $OUTPUT"
echo " 大小: $(du -sh "$OUTPUT" | cut -f1)"
echo "=========================================="
