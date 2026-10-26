# 并发开发任务提示词

Phase 0 已确定合同 v1.0.0 和独立基座目录。先阅读 `docs/parity/README.md`、`CONTRACT.md`、`SOURCE_MAP.md`，再把下面对应文件的完整内容复制给开发 Agent。

| Agent | 提示词 | 建议分支 |
|---|---|---|
| A | [公共集成与桌面](A-集成与桌面.md) | codex/parity-platform |
| B | [词典与词库](B-词典与词库.md) | codex/parity-dictionary |
| C | [记忆、词书与复习](C-记忆与学习.md) | codex/parity-learning |
| D | [个人词汇与统计](D-词汇与统计.md) | codex/parity-library |
| E | [原版前端与交互](E-前端与交互.md) | codex/parity-ui |
| F | [AI 与凭据](F-AI与凭据.md) | codex/parity-ai |

## 如何启动

由总控先固定同一个 Phase 0 提交 SHA；每个 Agent 从该提交创建独立 worktree/分支。后续公共变更通过 A 的提交发布，各分支按指定 SHA 同步，避免每人独立修改合同。

```powershell
# 总控先将 <BASE_SHA> 替换为交付的同一个 Phase 0 提交。
git worktree add ../lexica-dictionary -b codex/parity-dictionary <BASE_SHA>
```

在每个 worktree 中执行 `tools/bootstrap.ps1`，获取依赖；`tools/sync-reference.ps1` 获取固定原版源码。SDK 可用 CMake 的 LEXICA_WEBVIEW2_ROOT 指向已有只读副本，但数据库、前端构建、测试输出必须独立。

首轮可启动 B、C、D、F 的任意可容纳组合；E 可同步做页面和契约测试。A 只负责公共变更和集成，不重复实现其他模块。任务数与并发槽位数分开考虑，超出资源就分波次。

你后续把各 Agent 的提交分支/SHA 和交付报告交回本聊天，我再执行整体审核和修复。本次没有自动启动这些开发 Agent，也没有代替你向其他聊天派发消息。

## 所有 Agent 的统一交付格式

```text
基线 SHA / 合同版本 / 参考原版 SHA：
本次提交 SHA：
修改文件：
已完成的功能族与命令：
对齐的原版源码位置：
构建与测试命令、结果：
真实 SQLite / 真实传输 / UI 夹具各验证了什么：
已知差异和未实现事项：
需要 A 处理的合同/迁移/平台变更：
可重复演示步骤：
```

每项“已完成”必须附相应证据。修改自己的代码时保持其他 Agent 的未提交改动；独立 worktree 下仍要确认当前分支与根目录。原版提交固定为 c4b92a39402860566bf954f92efc8e59666aaa4a。
