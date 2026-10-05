# C/D/E 联调小词库

`ecdict-test.db` 是符合 ECDICT `stardict` 表结构的最小 SQLite 词库，供本地联调使用。

在全新的 Lexica 数据目录中导入后包含：

| word | 用途 |
| --- | --- |
| `run` | 精确查询；`running`、`ran`、`runs` 等词形回退 |
| `apple` | 精确查询；`apples` 词形回退；前缀建议 |
| `banana` | 精确查询；前缀建议 |

导入示例：

```text
POST /api/import
{"path":".../src/dict/testdata/ecdict-test.db"}
```

首次导入空库时，目标词条 ID 按 `run=1`、`apple=2`、`banana=3` 生成。重复导入不会新增词条，已有 ID 保持不变。
