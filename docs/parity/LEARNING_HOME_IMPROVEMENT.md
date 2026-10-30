# 首页学习层级与到期复习入口

分支 `codex/learning-home-improvement`。worktree：`D:\lexica-learning-home`。起始 SHA `38e56d97b339be213e92f7df30943a31faef50df`。提交 SHA 见本文件末尾，由该提交写入。

标题「让查过的词真正被记住。」是产品目标，不是已经测得的记忆效果。页面只展示查词、学习记录、理解反馈和到期复习这些现有流程。

## 信息层级

首页自上而下：

1. 目标句，以及四步真实流程：查词、留下学习记录、反馈理解程度、按计划复习。
2. 当前学习任务。这是主行动区。
3. 查词框。
4. 最近查过的词。没有记录时，四个示例词是可点击的查词入口，不假装成历史。
5. 次要的学习统计和离线词典条数。词典行仍使用真实的 `word_count`，文案保留 `N entries · offline`。

## 状态与数据

| 状态 | 判定 | 界面 |
|---|---|---|
| 读取中 | `stats_summary` 尚未返回；或记录数为 0 且最近查词也未返回 | 「正在读取学习记录」。不显示待复习 0，也不显示暂无到期。 |
| 失败 | `stats_summary` 失败；或记录数为 0 且 `recent_words` 失败 | 错误原文和「重试」。不把失败显示成任务完成。 |
| 新用户 | `encountered = 0`、`due_now = 0`、最近查词为空 | 「先查一个词」。说明查到的词会留下学习记录。 |
| 有到期 | `due_now > 0` | 「当前待复习 N 个」和「开始复习」。N 是 `stats_summary.due_now`。 |
| 有记录但未到期 | 已有记录且 `due_now = 0` | 「当前暂无到期复习」。可继续查词、查看词库，或走单独的「学习新词」。 |

没有「今日已完成」「今日学习时长」「连续打卡」或「记忆提升」。`fading_words` 实际是到期队列，不是单独的薄弱词分类，所以首页不展示「薄弱词」。

「本轮已完成」只在到期模式里，用户把当前这一组做完之后出现。第一次打开空队列时只显示「当前暂无到期复习」。

## 到期复习入口

- 「开始复习」指向 `/review?mode=due`。该模式只调用 `review_group`，不调用 `study_group`。
- 「学习新词」指向 `/review?mode=new`。该模式只调用 `study_group`。页面写明这是当前词书的新词，不是到期复习。
- 不带 `mode` 的 `/review` 保持原行为：先尝试 `study_group`，没有卡片或失败时再回退 `review_group`。侧栏 Review 仍指向这条旧路径，因为侧栏不在本次可改文件里。
- 进入页面、刷新、前进后退和 `mode` 变化只重新取队列。浏览阶段不调用 `study_grade`、`start_review_item` 或 `submit_review`。
- 认识、模糊、忘记仍走 `study_grade`。组内练习仍走 `start_review_item`、`submit_review` 和只读重考。
- `review_group` 每次最多返回一组。完成后「继续获取到期词」再次调用 `review_group`。同时读取一次 `due_now`，并说明这个数字和下一组可能不一致。下一组为空时显示真实空状态。

## 查词后的学习提示

提示放在 `WordView` 外面。

- 命中且 `encounter_id` 有值：显示「查词记录已保存」。
- `memory.visit_count` 和 `memory.status` 有值才显示次数和学习状态。
- 具体复习时间只来自随后的只读 `library_words` 里同一词条的 `next_review_at`。`lookup_word` 的 `MemorySummary` 没有这个字段。没有该字段时不显示时间，也不写成「已安排复习」。
- 「刷新学习状态」只重读统计、最近查词和词库，不再调用 `lookup_word`。
- 重复查询同一个词不会重新加载 `WordView`，未保存笔记留在输入框里。

## 修改文件

- `ui/src/routes/+page.svelte`
- `ui/src/routes/review/+page.svelte`
- `ui/src/lib/components/HomeLearningTask.svelte`
- `ui/src/lib/components/LookupRecordHint.svelte`
- `ui/tests/learning-home.spec.ts`
- `ui/tests/learning-home.live.mjs`
- `ui/playwright.learning-home.config.ts`
- `ui/playwright.learning-home-live.config.ts`
- `docs/parity/evidence/learning-home/`

未改 `api.ts`、`WordView.svelte`、`StudyWordCard.svelte`、设置页、TTS、合同、后端和全局 CSS。

## 测试

在 `ui/` 下：

| 检查 | 结果 |
|---|---|
| `npm run check` | 0 错误，0 警告 |
| `npm run build` | 通过 |
| `npx playwright test -c playwright.learning-home.config.ts` | 8/8 |
| `npx playwright test -c playwright.fixture.config.ts` | 24/24，原有首页和复习回归 |
| `npx playwright test -c playwright.learning-home-live.config.ts` | 1/1 |

新夹具覆盖：新用户、到期任务、有记录但未到期、加载与失败重试、开始复习只请求 `review_group`、到期队列不混入新词、一组完成后的剩余词、查词记录提示、重复查询和刷新不丢失未保存笔记，以及刷新、后退和 `mode` 切换。

真实服务使用 Release `lexica_parity_server.exe`，端口 18773，数据目录是 worktree 里的 `tmp-learning-home-live`。`--no-auto-setup`。流程是：空首页、查 `meticulous`、看到查词记录和约 6 小时后的复习安排、用 RPC 再查 `subtle`、只在这个测试库把 `memory_states.next_review_at` 改到过去、首页显示待复习 2 个、开始复习只走到期队列、两次「认识」、回到首页后 `due_now` 为 0。没有改用户自己的学习库，也没有停用户正在运行的服务。480 宽检查了没有横向溢出。

截图：

- `docs/parity/evidence/learning-home/home-new.png`
- `docs/parity/evidence/learning-home/home-due.png`
- `docs/parity/evidence/learning-home/home-due-480.png`
- `docs/parity/evidence/learning-home/home-clear.png`
- `docs/parity/evidence/learning-home/home-loading.png`
- `docs/parity/evidence/learning-home/home-error.png`
- `docs/parity/evidence/learning-home/lookup-saved.png`
- `docs/parity/evidence/learning-home/home-live-new.png`
- `docs/parity/evidence/learning-home/home-live-lookup.png`
- `docs/parity/evidence/learning-home/home-live-due.png`
- `docs/parity/evidence/learning-home/home-live-clear.png`
- `docs/parity/evidence/learning-home/review-live-due.png`

## 已知限制

- 侧栏 Review 仍进入不带 `mode` 的 `/review`，因此仍可能先进入默认词书的新词学习。
- 查词响应本身没有 `next_review_at`。复习时间依赖 `library_words` 第一页能按词条编号找到该词。找不到就不显示时间。
- 新查的词默认大约 0.25 天后才到期。要立刻复习只能改测试库，不能靠再查一次词。
- 统计里的到期数和下一次 `review_group` 可能不一致。界面保留刷新和再取一组，不用旧数字冒充队列。

## 审核者复现

```powershell
cd D:\lexica-learning-home\ui
npm run check
npm run build
npx playwright test -c playwright.learning-home.config.ts
npx playwright test -c playwright.fixture.config.ts
if (Test-Path ..\tmp-learning-home-live) { Remove-Item -Recurse -Force ..\tmp-learning-home-live }
npx playwright test -c playwright.learning-home-live.config.ts
```

实时测试默认使用 `D:\C++自选项目\build-parity-msvc\src\parity\Release\lexica_parity_server.exe`。可用环境变量 `LEXICA_SERVER_EXE` 和 `LEXICA_HOME_LIVE_PORT` 改位置和端口。

## 提交

功能提交 `530f64b31296ba40c3b321011dc40ab26f740992`。本行是随后补上的记录；审核以 `git rev-parse HEAD` 的分支尖端为准。
