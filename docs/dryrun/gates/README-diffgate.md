# diffgate — CPython 3.11 适配的 JIT on/off 差分门禁（M0）

以解释器为 oracle 的差分正确性门禁。语料中的每个 case 在三种模式下运行，
输出逐行比对（比对前经偏差白名单归一化），任何超出固化基线的差异都使门禁失败。

## 三种模式

| 模式 | 含义 |
|---|---|
| `interp` | 纯解释器，不导入 cinderx —— oracle |
| `jit` | cinderx JIT，case 函数及其声明的 helpers 全部 `force_compile` |
| `jit_deopt` | 同 jit，但 case 可在执行中途调用 `diffgate_rt.checkpoint()` 触发 `force_uncompile`（入口交换式 deopt；挂起中的 generator 在下次恢复时走 deopt 路径） |

限制：`checkpoint()` 是入口交换语义（对齐 cinderjit.force_uncompile），
不是任意 guard 点的强制回退——后者是 M6 的交付物。

## 快速使用

所有脚本都是可移植核心，**直接在目标环境（裸机/容器）里用目标 Python 运行**，
不依赖任何特定开发机。jit 模式要求 cinderx 可导入（调用方设置 PYTHONPATH）。

```bash
# 差分语料门禁（分钟级，PR 层）
python3.11 run_diffgate.py --corpus corpus --out out/report.json \
    --baseline baselines/<target>.json

# 基础 Lib/test 差分（约 26 个语言核心模块，逐模块结果比对）
python3.11 run_libtest_diff.py --out out/libtest.json \
    --baseline baselines/<target>-libtest-basic.json

# 重新固化基线（仅在有意接受当前失败集时使用）：把 --baseline 换成 --update-baseline

# 白名单钉住测试（任何 python3 均可，CI 应常跑）
python3 tests/test_allowlist_pinning.py
```

在开发机上对容器内环境运行时，用 docker 挂载后在容器里执行同样的命令，例如：

```bash
docker run --rm -v <本目录>:/gate -v <被测构建>:/target:ro \
  -e PYTHONPATH=<target 内 cinderx 的 site-packages 与 PythonLib> \
  <目标镜像> python3.11 /gate/run_diffgate.py --corpus /gate/corpus --out /gate/out/report.json
```

退出码：0 = 失败集 ⊆ 基线；1 = 出现新失败；2 = 基础设施错误。

## 语料矩阵（`corpus/`，共 918 case）

| 模块 | case 数 | 覆盖 |
|---|---|---|
| `corpus_unbound` | 31 | 未绑定局部变量/cell/自由变量全路径（3.11 无 LOAD_FAST_CHECK 的 SEGV 高危区），异常消息逐字符比对 |
| `corpus_calls` | 285 | 15 种被调对象 × 19 种调用点形态（exec 生成，保证每种形态是独立字节码），覆盖 PRECALL/CALL/KW_NAMES/CALL_FUNCTION_EX 与精确 TypeError 消息 |
| `corpus_operators` | 529 | 运算符 × 操作数类型矩阵（20 种二元/比较 × 23 对操作数、一元、增强赋值、下标/切片/del、`__radd__`/NotImplemented 协议、compact 整数边界 2^62） |
| `corpus_controlflow` | 36 | with 语句族（`__exit__` 吞/抛、多管理器）、try/finally 交互（finally 内 return/break/continue）、match 四类模式、布尔短路与回跳分支、set/dict 构建、assert、EXTENDED_ARG 合成用例 |
| `corpus_ic_mutation` | 15 | attr/method/global 缓存在类字典变异、`__class__` 赋值、MRO 修改、globals/builtins 增删后的失效正确性 |
| `corpus_frames` | 16 | `sys._getframe`、traceback（含 PEP 657 位置）、异常链、generator send/throw/close/yield-from（含挂起中 checkpoint 反优化）、递归限制 |
| `corpus_hotloops` | 6 | 持续高温执行 + 结果校验和（整数/浮点/字符串/异常密集/generator 管道/跑热后强制反优化） |

**opcode 覆盖有客观刻度**：`tools/opcode_coverage.py` 用目标 Python 统计语料触达的
opcode 集合——当前 3.11 范围内 98/98 全覆盖，10 个 deferred（async/import/注解）
每条带理由记录在工具的 DEFERRED 表中。"语料够不够"用它回答，不靠感觉。

新增 case：在模块里定义 `case_*` 函数即可（按名字典序执行）；
需要参与编译的辅助函数挂在 `case_xxx.helpers = [...]`。
语料准入纪律：确定性（禁随机/时间/网络，字典输出 sorted）；自我恢复全局状态
（finally）；输出即断言（不写期望值）；一案一行为；**单 case 秒级完成**
（生成式矩阵必须过滤天文代价组合，如巨大右操作数的 pow/lshift）。

## 偏差白名单（P0 分级差分契约）

`allowlist.toml` 是"允许偏差类"的唯一载体。纪律：

1. 每条规则必须有 rationale；
2. 每条规则必须在 `tests/test_allowlist_pinning.py` 有钉住测试
   （证明命中目标偏差 + 证明不会洗掉真差异）；
3. 硬等价类（返回值、异常类型与消息、行号）严禁经白名单豁免。

## 崩溃恢复

case 级 SEGV 不会吞掉模块内后续 case：runner 把崩溃归因到具体 case
（记为 `CASE <name> CRASH exit=-11`），跳过它继续跑完矩阵。
崩溃本身作为失败进入基线，可被逐个修复销账。

## 基线工作流

基线按被测目标命名为 `baselines/<target>.json`（diffgate 语料）与
`baselines/<target>-libtest-basic.json`（基础 libtest），内容是排序的失败 id 列表。

- 首次接入一个目标：跑一轮 `--update-baseline` 固化当前失败集；
- 日常门禁：带 `--baseline` 运行，失败集 ⊆ 基线即绿（exit 0），新增失败即红（exit 1）；
- 治理纪律：**基线只准收缩**——新增条目需基线 owner 批准并记录理由；
  修复后重新 `--update-baseline` 收窄基线（fixed 列表会在报告中列出）。

历史运行的失败明细见对应 `--out` 报告的 `detail` 字段；已知问题的分类与销账
清单维护在技术设计书（`docs/cinderx-311-adaptation-plan.md` §8），不在本目录。
