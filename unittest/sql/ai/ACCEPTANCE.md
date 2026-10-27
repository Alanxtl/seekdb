# AI 函数增强与 AI 列本地验收

验收日期：2026-10-27。最终构建和本轮全部选定测试通过。

## 版本

- 分支：`2026-competition`。
- 基线提交：`5bce9e06d1281347ad61344e4b01fab9158c58a2`；实现位于当前工作区。
- 程序：OceanBase seekdb 1.4.0.0，`RelWithDebInfo`，由当前源码完整构建。
- 主机二进制：`build_bazel/bin/src/observer/seekdb`。
- 实测容器二进制：`/tmp/seekdb-competition-ai-44`。
- SHA-256：`218ceae9168a6507fcf0f8fdb2a284e7d55bc7393fc3ebc5168ee18d01162274`。
- 运行环境：独立 Docker 容器 `seekdb-ai-test`；容器数据库端口 2881、Mock 端口 8000；宿主机数据库端口 2882。

## 最终结果

| 分类 | 实际执行数量 | 结果 |
| --- | ---: | --- |
| C++ gtest | 10 个用例，2 个 Bazel 目标 | 全部通过，零禁用 |
| 现有 AI 与函数增强 mysqltest | 6 个入口 | 全部通过 |
| 官方 AI 列 mysqltest | 5 个入口 | 全部通过 |
| 受影响 SQL / 事务 / 向量回归 | 5 个入口 | 全部通过 |
| AI 函数 Python 集成测试 | 12 个用例 | 全部通过 |
| AI 列 Python 集成测试 | 13 个用例 | 全部通过 |

mysqltest 逐个比较仓库原始预期结果，未修改预期、排除名单或跳过失败用例。
五项回归分别为 `update_ignore`、`delete_from_mysql`、`geometry_generated_col_mysql`、
`repeatable_read_transaction`、`vector_index_rr_snapshot`。

AI 列补充测试实际验证了定义修改与隐藏字段清理、事务可见性和回滚、严格读取、
READY NULL、LOB 与嵌套 Prompt、错误输出类型和长度、INSERT IGNORE / UPSERT / REPLACE、
迟到响应与同主键重插、删除期间取消任务、优雅停止及重启恢复、429/5xx、断连及 HTTP 超时。
官方用例另覆盖多列原子发布、批量和主键更新、条件重试、向量索引移除与写回、ANN 查询及回滚。

重复故障后的文件描述符为 **57 → 57**；RSS 为 **856000 → 846156 KiB**。
异步补充测试采用响应屏障和有界轮询。所有模型请求均由本地 Mock 接收，无真实模型依赖。

## 命令与证据

完整环境说明、构建及 Bazel 命令见 [README](README.md)。本轮集成测试命令：

```bash
PYTHONPATH=build/test-deps python3 unittest/sql/ai/run_local_tests.py \
  --binary /tmp/seekdb-competition-ai-44 \
  --logdir build/ai-acceptance/final-44
```

- 机器可读报告：`build/ai-acceptance/final-44/report.json`。
- 完整构建日志：`build/ai-acceptance/final-44/build.log`。
- C++ 测试日志及 XML：`build/ai-acceptance/final-44/unit.log`、`cpp/`。
- SQL 测试实际输出：`build/ai-acceptance/final-44/mysqltest/`。
- Python 日志：`build/ai-acceptance/final-44/python.ai_functions.log`、`python.ai_columns.log`。
- 运行汇总：`build/ai-acceptance/final-44/run.log`。
- 本次源码与测试文件校验清单：`build/ai-acceptance/final-44/source-manifest.sha256`。

本报告覆盖上述实际执行的本地范围；线上隐藏测试结果以赛事评测为准。
