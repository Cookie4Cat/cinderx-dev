# 950 真机性能复现手册（3.11 dry-run 交付口径）

> 目的：在 950（鲲鹏 aarch64）真机上复现当前交付口径的 pyperformance
> 性能。对照基线为预演机存档 `full113-ab-v12-summary.json`（86 项
> 几何均值 1.027，含 !42-!55 全部合入项）。预演机为 Apple Silicon
> 容器（强乱序核），多轮判决明确标注"微架构敏感、以 950 真机为准"，
> 本手册第七节列出真机重点观察清单与判别开关。

## 一、交付口径说明

以 `dryrun-311-base` 分支 HEAD 构建即为完整交付口径，**全部策略与
产物优化均为二进制内默认值，运行期只需设置一个环境变量
`PYTHONJITAUTO=4`**。默认包含：提前 quickening、守卫自适应去特化、
同步生成器不自动编译、协程解释路径三修、LOAD_GLOBAL_BUILTIN
守卫式装载、产物侧调用直派（VectorCall/CallMethod）等。属性/方法
精确类型投机默认关闭（三审维持，勿开启）。

## 二、环境准备

| 组件 | 要求 | 备注 |
|---|---|---|
| Python | CPython 3.11.x，**A/B 两侧同一解释器二进制** | 预演机用 manylinux cp311（3.11.13/3.11.15 均可）；A 侧即该解释器裸跑 |
| 编译器 | GCC 14（预演机 gcc-toolset-14） | GCC 12+ 预计可用但未验证；PGO 配方假定 GCC（-fprofile-*） |
| CMake | ≥ 3.20 | |
| pyperformance | 1.13.0，`pip install --target /tmp/pp113 pyperformance==1.13.0` | 基准脚本从该目录直跑，不装入解释器环境 |
| 基准三方依赖 | 逐基准装入 `/tmp/ppdeps`（独立目录，A/B 两侧同挂） | django/sqlalchemy/docutils/sqlglot 等；缺依赖的基准会被跑分脚本记为 FAIL 并跳过，不阻塞整轮 |
| 本仓库 | checkout `dryrun-311-base`，**建议布局在 `/src`**（或建立符号链接） | `run_ab_full113.py` 与训练脚本的缺省路径常量按 `/src` 书写；不便时按第五节环境变量覆盖 |

## 三、构建（正式口径 = PGO+LTO 三相）

1. 从零建构建树（一次性）：

```bash
cd /src
CINDERX_LOCAL_DEPS_DIR=<本地依赖目录> python3.11 setup.py build
# 产物树：/src/scratch/temp.linux-aarch64-cpython-311（CMake 树）
# 产物库：/src/scratch/lib.linux-aarch64-cpython-311/_cinderx.so
```

2. PGO+LTO 三相（正式性能口径，实测 +3pp 级）：

```bash
BUILD_TREE=/src/scratch/temp.linux-aarch64-cpython-311 \
CMAKE_BIN=cmake JOBS=<核数> \
TRAIN_CMD='bash /src/ci_pipeline/scripts/train_full_311.sh' \
  bash /src/ci_pipeline/scripts/build_pgo_lto_311.sh
```

训练脚本的路径常量可经环境覆盖（非 `/src` 布局时必设）：
`TRAIN_PP`（PYTHONPATH）、`TRAIN_PY`（解释器）、`TRAIN_BM`
（pyperformance benchmarks 目录）、`TRAIN_SMOKE`（冒烟目录）。

3. **构建验证红线**：

- 构建前后 `md5sum _cinderx.so` 必须变化（陈旧构建陷阱预演期
  两次实际发生）；
- 相四产物正确性验收由配方内置（PGO-use 相曾出现概率性错译，
  验收失败会整链报错并归档取证）；
- 源码变更后必须整套重跑三相；开发迭代期先复原普通构建
  （`cmake -DENABLE_PGO_USE=OFF -DENABLE_PGO_GENERATE=OFF
  -DENABLE_LTO=OFF <树>` 并删除全部 `.gcda/.gcno` 后 make）。

## 四、运行时接入与自检

```bash
mkdir -p /tmp/m9sc && cp /src/docs/dryrun/bench/sitecustomize.py /tmp/m9sc/
export PYTHONPATH=/tmp/m9sc:/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib
export PYTHONJITAUTO=4
```

**JIT 在场自检（必做）**——JIT 静默缺席是历史高发坑（多见于
libstdc++ 版本不匹配：非系统 GCC 构建时需
`export LD_LIBRARY_PATH=<gcc 安装>/lib64`）：

```bash
python3.11 - <<'EOF'
def hot(x):
    return x + 1
for i in range(100):
    hot(i)
from cinderx import jit
n = len(jit.get_compiled_functions())
assert n > 0, "JIT 未生效——检查 PYTHONPATH/LD_LIBRARY_PATH/构建产物"
print("JIT 在场，已编译", n)
EOF
```

注意：经 stdin/`-c` 执行的代码其文件名带尖括号，会被自动编译范围
阀排除——自检与冒烟脚本必须落盘为真实文件执行。

## 五、跑分

全量 86 项 A/B（**950 上 A 侧必须实测，勿用 --a-from 复用预演机
A 侧**）：

```bash
cd /src/docs/dryrun
python3.11 run_ab_full113.py --out /tmp/full113-950
# 子集：--benches deepcopy,pprint,richards
# B 侧阈值：环境变量 M9_THRESHOLD（缺省 4，即交付阈值，勿改）
```

脚本内路径常量（`PP113=/tmp/pp113`、`DEPS=/tmp/ppdeps`、
`CINDERX_PP=/src/scratch/...:/src/cinderx/PythonLib`、
`SC_DIR=/tmp/m9sc`）按第二节布局即可直用；自定义布局需改脚本头部
常量。协议：pyperf `--processes 3 --warmups 3 --values 5`，
`summary.json` 逐基准增量落盘可中途查看。

进程内快测脚本见 `docs/dryrun/bench/`（用法见 RUNBOOK.md 第五节）；
门禁套件（冒烟/diffgate/refcount）用法同 RUNBOOK.md 第六节，性能
复现可不跑，若发现疑似正确性问题再启用定界。

## 六、判读与测量纪律

- 对照 `docs/dryrun/full113-ab-v12-summary.json`：预演机几何 1.027、
  ≥1.0 共 52 项、<0.90 共 7 项（其中 2to3/python_startup×2/
  concurrent_imap 为既定忽略的短命进程税族，实余 deepcopy/sphinx/
  gc_collect）。真机单项与预演机存在微架构差异属预期，重点看
  几何均值与劣化项名单的形态是否一致；
- **A/B 跑动期间不得重建或替换 .so**（子进程逐个加载，换库污染
  整轮）；
- 单项显著越出预期先做**同构建定向复测**（`--benches <项>` 双跑）
  再归因——预演期 regex_v8 曾出现 −25pp 的单次测量伪影，双复测
  即还原；共享机器的环境使用冲突可伪造 10pp 级假信号（950 曾有
  pprint/bpe 误报先例），判定异常前确认机器独占；
- 概率性崩溃/失败禁单跑判定（N≥5）；跨构建绝对值漂移 ±6%，单项
  结论以同构建对照为准。

## 七、950 重点观察清单（微架构敏感项）

以下判决在预演机（强乱序核，间接跳/PLT 近乎免费）上为中性或以
剖面推定，真机可能不同。每项附判别开关，可在同一构建上直接 A/B：

| 项 | 预演机判决 | 判别方法 |
|---|---|---|
| 产物侧调用直派（!55，csel 选径） | richards −16% | 无开关；对照 v11/v12 存档形态，若 950 上 richards 族增幅显著偏离 ±21pp 量级请报告 |
| LOAD_GLOBAL_BUILTIN（!54） | pprint −20%/全局普涨 | 无开关；观察 pprint/django_template/sqlglot 组 |
| 同步生成器不自动编译（!51） | generators 0.666→1.012 | `PYTHONJITCOMPILESYNCGENERATORS=1` 恢复旧行为对照 |
| 恢复派发三层合一（!51） | 墙钟中性（份额 −7pp 未兑现） | 无开关；若 generators/coroutines 真机显著优于存档比例，此项为候选归因 |
| 守卫自适应去特化（!49） | raytrace 风暴熄灭 +15pp | `CINDERX_ADAPTIVE_DESPEC=0` 关闭对照（预期 raytrace 显著回退） |
| [P3] 恢复不计数/[P7] 行内门（!52） | coroutines +18.7pp | 无开关；观察 coroutines/async 族 |

以上判别跑法均为进程内或 `--benches` 子集级，单项半小时内可出数。

## 八、结果回传

请回传：`/tmp/full113-950/summary.json`、构建 `_cinderx.so` 的
md5、GCC/CMake 版本、以及第七节判别项的对照数字（如做）。若出现
崩溃项，附 `<out>/<bench>-b.json` 与复现命令（N≥5 复跑结论）。
