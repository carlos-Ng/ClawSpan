# ClawSpan 开发规范

本文档规定 ClawSpan 项目的开发流程与提交规范，所有贡献者须遵守。

---

## 1. 分支与主线开发模式

### 1.1 主线保护

- **禁止**直接向 `main` 分支提交（push）代码
- `main` 为受保护主线，仅接受经审批的合并

### 1.2 分支开发流程

1. **拉取分支**：从 `main` 拉取功能分支进行开发
   ```bash
   git checkout main
   git pull origin main
   git checkout -b feat/your-feature   # 或 fix/xxx、docs/xxx 等
   ```

2. **开发与提交**：在分支内完成开发并提交
   ```bash
   git add .
   git commit -m "feat: your change description"
   ```

3. **推送分支**：将分支推送到远程
   ```bash
   git push -u origin feat/your-feature
   ```

4. **创建 Pull Request**：在 GitHub 上创建 PR，请求合并到 `main`

5. **审批合并**：经 Code Review 审批通过后，由维护者合并到 `main`

### 1.3 分支命名约定

| 前缀 | 用途 |
|------|------|
| `feat/` | 新功能 |
| `fix/` | Bug 修复 |
| `docs/` | 文档更新 |
| `refactor/` | 重构 |
| `chore/` | 构建、工具等杂项 |

---

## 2. 提交信息规范

### 2.1 禁止在提交中加入广告或品牌推广

**严禁**在 commit 的元数据中加入任何形式的广告、品牌推广或工具署名，包括但不限于：

- **Trailer 类**：
  - `Made-with: Cursor`
  - `Made with: Cursor`
  - `Powered-by: Anthropic`
  - `Generated-by: Claude`
  - 或任何形式的工具/AI 品牌 trailer

- **Co-authored-by**：不得在 `Co-authored-by` 中提及 Cursor、Anthropic、Claude 等商业品牌

- **Signed-off-by**：不得在 `Signed-off-by` 中加入商业品牌推广

- **正文/标题**：不得在 commit message 的标题或正文中插入工具/服务推广

### 2.2 提交信息格式

建议使用 [Conventional Commits](https://www.conventionalcommits.org/) 格式：

```
<type>: <short description>

[optional body]
```

示例：
```
feat: add WSL2 VM auto-start on daemon boot
fix(install): correct uninstall script path in registry
docs: update README installation section
```

---

## 3. docs 目录规范

- **可提交到仓库的文档**：`docs/coding-style.md`、`docs/development-standards.md`
- **仅本地保留的文档**：设计草稿、进度记录、集成计划等内部文档不应提交到 GitHub，已通过 `.gitignore` 排除

---

## 4. 适用范围

- 所有分支的提交
- 手动提交与自动化提交（包括 AI 辅助开发）
- 所有贡献者（包括维护者）

---

## 5. 执行

- AI 助手在协助 Git 提交时，须遵守本规范，不得在 `-m`、`--trailer`、`--message` 等参数中加入上述禁止内容
- 维护者在合并 PR 前应检查提交信息是否符合规范
