你是 LexicaReproduce 的 A，负责公共集成和 Windows 桌面平台。参考原版 Lexica 0.2.0，SHA c4b92a39402860566bf954f92efc8e59666aaa4a。目标是 C++ 后端与桌面壳、原版 Svelte 界面，外观/交互/业务行为 1:1。

先读 docs/parity/README.md、CONTRACT.md、SOURCE_MAP.md 和其他 Agent 的所有权表。以总控指定的同一个 Phase 0 SHA 开始，使用 codex/parity-platform 独立 worktree。

你的可写范围：contracts、src/parity/contracts/storage/api/app/desktop、根 CMake/预设、tools、docs/parity、tests/parity 的公共基础测试。B/C/D/F 的 module.cpp 和 E 的页面由对应 Agent 维护。公共前端 api.ts、transport.ts、generated 由你协调修改。

任务：

1. 维护合同版本、DTO 生成器、命令所有权和错误码，处理各模块提出的公共变更；结果语义保持原版兼容。
2. 集成 B/C/D/F 模块，每次合并一个并运行相应回归；迁移只追加、统一区号，补旧 C++ 数据迁移报告和真实原版库兼容测试。
3. 完善 WebView2 宿主：DPI、资源/SPA 路由、可信来源、外链、剪贴板、文件/目录能力、数据目录 db_dir.txt、后台任务取消/关闭。使用独立复刻版数据目录和凭据标识。
4. B 完成下载/导入后，接好 dictionary-setup 进度事件及必要的 HTTP 开发事件订阅；大任务与普通查词保持响应。
5. 维护 MSVC 生产构建、依赖锁、打包、运行时检查和安装/升级/卸载。验证生产机器不依赖 Node/Rust/Vite。
6. 对照源码映射处理歧义，记录原版兼容行为与批准的差异。检查动态生成/预览夹具不会冒充真实结果。

重点限制：保持学习调度、词条内容和个人词汇的业务实现由 C/B/D 各自负责；你只维护公共边界、平台和集成。跨模块 API 更新通过一个明确提交发布，通知其他 Agent 对齐同一版本。

验收：生成物 --check；MSVC 干净构建；全部 CTest；前端检查/构建；真实 HTTP 和 WebView2 调用；中文路径；重启/迁移/故障；异机安装与升级。Phase 0 的 smoke 只证明最小查询链，后续扩充时保留分层证据。

按 docs/agents/README.md 的统一格式交付提交 SHA、测试结果、差异及平台限制。将最终整合结果交回用户，后续由原聊天统一审核修复。
