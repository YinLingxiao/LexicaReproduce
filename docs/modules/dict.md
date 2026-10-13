# 词库与导入

实现位于 `src/dict`，构建目标为 `lexica_dict`，依赖公共模块和 SQLite。

## 查询流程

`lookup` 先去除单词两端空白并转小写，然后精确查询；未命中才通过词形表回到原型，例如 running → run。查词本身不写历史，由 App 在正式查词命中后调用个人词汇和复习模块。`suggest` 返回最多 8 个前缀候选，`get` 按编号读取词条。

空词库返回 `DICT_NOT_READY`，有词库但找不到单词返回 `WORD_NOT_FOUND`。

## 数据库与导入

共享数据库位于所选数据目录的 `lexica.db`。启动时检查完整性，并通过 `schema_migrations` 创建或升级词库、复习状态和个人词汇表。当前版本为 dict 2、review 1、vocab 2；复习成绩表由复习模块打开时创建。

支持 ECDICT CSV 和包含 `stardict` 表的 SQLite 文件。导入先在独立连接的临时表中读取词条、建立词形关系，全部通过后在一个事务中更新正式词库。已有词条按词头保留编号，失败不保留部分正式更新。小写原拼写优先于仅大小写不同的来源记录。

`importFile` 是同步入口；网页使用 `importStart` 创建后台任务，再通过 `importTask` 查询状态、计数和错误。后台任务不持有接口层全局锁。暂存批次使用延迟事务，只写临时表，主库允许并发查词、收藏和笔记写入；每批次释放读取游标和快照，最终应用时才取得主库写锁。

## 验收

```bat
build.bat
ctest --test-dir build -R "^dict_test$" --output-on-failure
```

`tests/dict_test.cpp` 当前有 32 项检查，覆盖超过 5000 行后损坏的 CSV、同步与后台失败回滚、成功重新导入的编号稳定、词形关系和大小写冲突。联调样本见 `tests/data/README.md`。

`import_concurrency_test` 通过 SQLite 语句跟踪，在词条及词形的暂存事务内部暂停导入，确认查词、收藏和笔记在暂停解除前已完成；并验证最终更新保留词条编号和并发个人数据。测试钩子只存在于测试程序，不改变业务接口。
