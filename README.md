# Lexica

Windows 上的本地词汇学习程序，提供词库导入、查词、个人词汇、复习、统计和可选 AI 例句。项目按功能组织，不再按开发人员分目录。

## 从哪里开始读

先看 [程序入口](src/main.cpp)，再看 [App 业务编排](src/app/app.cpp)。App 展示一次查词、一次复习如何调用其他模块；具体实现继续进入对应模块。

| 目录 | 职责 |
|---|---|
| `src/common` | 共用类型、错误码、时间和文本工具 |
| `src/dict` | 词库查询、CSV/SQLite 导入、共享数据库的建表升级 |
| `src/vocab` | 收藏、笔记、暂停、查词历史和活动统计 |
| `src/review` | 到期选题、提示、判分、错词重练和成绩保存 |
| `src/ai` | AI 配置、连接测试、后台例句任务和缓存 |
| `src/app` | 协调以上业务模块，提供界面调用入口 |
| `src/api`、`src/cli` | 网页 HTTP 接口与控制台菜单 |
| `web` | 网页资源；目前主页面只展示服务状态 |
| `tests`、`tests/data` | 验收测试及只读测试词库 |
| `third_party` | SQLite、cpp-httplib、nlohmann/json |
| `docs` | 运行、接口、需求和模块说明；旧分工仅保存在 `docs/history` |

模块保持独立的 CMake 库目标。接口与控制台通过 App 调用业务；复习依赖词库和个人词汇；个人词汇通过公共的 `StageSource` 接口读取阶段，不反向依赖复习实现。

## 构建和运行

使用 Windows x64、MSYS2 UCRT64、C++17。需要的 MSYS2 软件包：

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

在项目根目录运行：

```bat
build.bat
build\lexica.exe
```

浏览器打开 `http://127.0.0.1:8765`。控制台模式用 `build\lexica.exe --cli`。网页会复制到 exe 旁边的 `web` 目录，默认用户数据放在 exe 旁边的 `data` 目录；已有数据不随源码迁移。

更多启动参数、AI 配置和备份方法见 [运行说明](docs/运行说明.md)，HTTP 数据格式见 [接口说明](docs/接口说明.md)。VS Code 的调试入口指向 `build/lexica.exe`，调试前先构建。

## 测试

默认同时构建四个测试程序，编译完成后运行：

```bat
ctest --test-dir build --output-on-failure
```

`dict_test` 验证词库导入，`vocab_tests` 验证个人词汇和统计，`review_test` 验证复习与应用联调，`import_concurrency_test` 验证词条和词形暂存期间查词、收藏、笔记仍可写入。测试链接同一份业务库，不需要手工编译源码。也可以从任意工作目录直接运行构建目录里的测试 exe。

测试词库使用 CMake 配置的绝对路径。可写数据位于 `<构建目录>/test-output/<模块>/<进程编号>`，不会写进用户的 `data` 或修改样本；目录清理会先检查路径边界。

只需要主程序时，在 UCRT64 环境中配置 `cmake -S . -B build -G Ninja -DBUILD_TESTING=OFF`。需要恢复测试时重新配置 `-DBUILD_TESTING=ON`。采用其他构建目录时，程序、网页和测试输出也放在该目录下。

## 模块说明与已知问题

- [词库与导入](docs/modules/dict.md)
- [个人词汇与统计](docs/modules/vocab.md)
- [复习与记忆调度](docs/modules/review.md)

目录整理保留现有接口、数据库结构和业务行为。后台导入暂存期间不再提前取得共享库写锁；最后原子应用正式词库时仍需短暂取得写锁，其他写操作可能等待。前端完整业务页面仍需另行交付。
