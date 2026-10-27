# 本地 AI 验收

测试使用源码构建的 seekdb、确定性 HTTP Mock，以及既有 mysqltest 套件。
Mock 的 8000 端口位于独立 Docker 网络命名空间；宿主机只映射数据库端口 2882。
无需真实模型或 API Key。所有用例实际执行并比较原始预期结果。

## 构建与 C++ 单元测试

在仓库根目录配置现有第三方 SDK（`deps/3rd`）、Rust 和 Bazel 后运行：

```bash
./bazel.py build --jobs=24 \
  --action_env=RUSTUP_HOME --action_env=CARGO_HOME \
  --action_env=RUSTUP_TOOLCHAIN=1.98.1-x86_64-unknown-linux-gnu \
  //src/observer:seekdb

./bazel.py test --jobs=24 \
  --action_env=RUSTUP_HOME --action_env=CARGO_HOME \
  --action_env=RUSTUP_TOOLCHAIN=1.98.1-x86_64-unknown-linux-gnu \
  //unittest/sql/engine/expr:test_ob_openai_utils \
  //unittest/sql/engine/expr:ob_expr_ai_prompt_test --test_output=errors
```

本机 Rust 环境位于 `../seekdb-practice-deploy/build-rust`；将其 `cargo/bin` 加入
`PATH`，并将 `RUSTUP_HOME`、`CARGO_HOME` 分别指向其中的 `rustup`、`cargo`。
本机已缓存全部 Rust 依赖，可离线构建。

## 独立测试实例

首次创建容器时，在仓库根目录执行下列命令。基础镜像需提供 Python 3；`/sdk`
挂载的 SDK 需包含 `u01/obclient/bin/{obclient,mysqltest}`。`libaio` 和 `diff`
使用本机相同架构的可执行文件/库。

```bash
python3 -m pip install --target build/test-deps pymysql

docker run -d --name seekdb-ai-test --entrypoint /bin/sleep \
  -p 2882:2881 -v "$PWD:/workspace" \
  -v "$(readlink -f deps/3rd):/sdk:ro" \
  -v /lib/x86_64-linux-gnu/libaio.so.1:/lib64/libaio.so.1:ro \
  quay.io/oceanbase/seekdb:latest infinity

docker cp /usr/bin/diff seekdb-ai-test:/usr/local/bin/diff
docker cp -L build_bazel/bin/src/observer/seekdb seekdb-ai-test:/tmp/seekdb-ai

docker exec -d seekdb-ai-test bash -c \
  'exec /tmp/seekdb-ai --base-dir=/tmp/ai-competition-instance-v3 --port=2881 --nodaemon > /tmp/ai-console.log 2>&1'
```

使用一个全新数据目录初始化内部 AI 列及端点元数据。已有本地测试容器无需重复
创建。替换二进制前，应向 `base-dir/run/seekdb.pid` 中的进程发送 SIGTERM，等待
退出后再复制、启动；不能覆盖正在执行的文件。数据库就绪后运行：

```bash
PYTHONPATH=build/test-deps python3 unittest/sql/ai/run_local_tests.py \
  --container seekdb-ai-test --binary /tmp/seekdb-ai \
  --logdir build/ai-acceptance/local
```

`--binary` 必须是容器当前实际运行的二进制绝对路径，重启恢复用例会重新执行它。
测试数据库、端点及表使用专有名称；回归中的普通表位于容器内 `test` 数据库。
该实例仅用于测试。测试器自动启动本仓库 Mock，顺序运行 16 个 mysqltest 文件，
随后执行 12 个 AI 函数和 13 个 AI 列 Python 测试。

`report.json` 记录分支、基线提交、实际运行二进制 SHA-256、每项退出码和日志
位置。`mysqltest/` 保存实际输出及差异文件。任何失败均使测试器非零退出，
不会修改预期文件、跳过用例或更改排除名单。C++ 单元测试另有 10 个 gtest 用例。

## 覆盖内容

- Prompt 的重复替换、嵌套 JSON、Unicode、转义和长 LOB；Completion 配置与异常响应。
- Embedding 响应索引还原、维度校验、跨行实际合批、过滤、NULL、LIMIT；Ollama 原生协议。
- Rerank 分批、全局顺序、稳定同分排序及对象字段保留。
- AI 列语法和 Schema、事务隔离与回滚、主键及批量更新、条件重试、列级严格读取。
- 多列原子发布、乱序结果、定义修改、删除后迟到响应、同主键重插、优雅重启恢复。
- 429/5xx、断连、超时、非法输出及类型/长度不兼容；失败后的描述符和 RSS 检查。
- 原生向量索引失效、结果写回、ANN 检索及回滚；普通 DML、生成列和事务回归。

异步测试通过可控制的响应屏障和有界轮询等待。资源检查以预热后的进程为基线，
重复故障后允许最多新增 12 个文件描述符、256 MiB RSS；此检查不替代 ASan。
