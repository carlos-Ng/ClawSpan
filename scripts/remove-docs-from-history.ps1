#Requires -Version 5.1
<#
.SYNOPSIS
    从 Git 历史中彻底删除指定文档，但保留本地副本

.DESCRIPTION
    使用 git filter-branch 从所有提交中移除指定文件。
    操作前会备份文件，操作后恢复并加入 .gitignore，避免再次提交。
    需要 force push 到远程。

.PARAMETER Paths
    要删除的文件路径（相对于仓库根目录），如 "docs/xxx.md"
#>

param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Push-Location $repoRoot

try {
    # 1. 备份文件到临时目录
    $backupDir = Join-Path $env:TEMP "clawspan-docs-backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

    foreach ($p in $Paths) {
        $fullPath = Join-Path $repoRoot $p
        if (Test-Path $fullPath) {
            $destDir = Join-Path $backupDir (Split-Path $p -Parent)
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
            Copy-Item $fullPath (Join-Path $backupDir $p) -Force
            Write-Host "[OK] Backed up: $p"
        } else {
            Write-Warning "File not found (may already be removed from working tree): $p"
        }
    }

    # 2. 从历史中移除
    Write-Host ""
    Write-Host "Removing from git history (this may take a while)..." -ForegroundColor Cyan

    $pathsArg = $Paths -join " "
    git filter-branch --force --index-filter "git rm --cached --ignore-unmatch $pathsArg" --prune-empty HEAD

    if ($LASTEXITCODE -ne 0) {
        throw "git filter-branch failed"
    }

    # 3. 恢复本地文件
    Write-Host ""
    Write-Host "Restoring local copies..." -ForegroundColor Cyan
    foreach ($p in $Paths) {
        $backupPath = Join-Path $backupDir $p
        $destPath = Join-Path $repoRoot $p
        if (Test-Path $backupPath) {
            $destParent = Split-Path $destPath -Parent
            if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
            Copy-Item $backupPath $destPath -Force
            Write-Host "[OK] Restored: $p"
        }
    }

    # 4. 加入 .gitignore
    $gitignore = Join-Path $repoRoot ".gitignore"
    $content = Get-Content $gitignore -Raw
    if ($content -notmatch "`n# Docs removed from history`n") {
        Add-Content $gitignore "`n# Docs removed from history (keep local only)`n"
    }
    foreach ($p in $Paths) {
        if ($content -notmatch [regex]::Escape($p)) {
            Add-Content $gitignore $p
            Write-Host "[OK] Added to .gitignore: $p"
        }
    }

    Write-Host ""
    Write-Host "Done. Next steps:" -ForegroundColor Green
    Write-Host "  1. git push --force origin main    # 强制推送更新远程历史"
    Write-Host "  2. If you have other branches: git push --force origin branch-name"
    Write-Host "  3. Collaborators: re-clone or git pull --rebase"
    Write-Host ""
    Write-Host "Backup location: $backupDir"
}
finally {
    Pop-Location
}
