# pyperformance 跑分交接手册（3.11 dry-run 分支）

> 面向接手继续优化的同事。环境为 manylinux aarch64 容器（预演机为
> Apple Silicon Docker，容器名 dryrun-perf）；openEuler 正式目标机
> 需按 REPORT.md 第六节的环境差异项复核。总报告见 REPORT.md，
> 各轮技术细节见 M9-*-log.md。

## 一、环境布局

| 路径 | 含义 |
|---|---|
| `/src` | 本仓库工作区（宿主机绑定挂载，checkout 直通容器） |
| `/src/scratch/temp.linux-aarch64-cpython-311` | CMake 构建树 |
| `/src/scratch/lib.linux-aarch64-cpython-311/_cinderx.so` | 构建产物 |
| `/opt/python/cp311-cp311/bin/python3.11` | 跑分解释器（manylinux，含 pyperformance/pyperf） |
| `/opt/rh/gcc-toolset-14/enable` | 编译工具链（构建前 source） |
| `/root/localdeps` | 本地依赖（CINDERX_LOCAL_DEPS_DIR 布局） |

pyperformance 基准文件位于
`/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks/`。

## 二、构建

增量构建（日常）：

```bash
docker exec dryrun-perf bash -c '
  source /opt/rh/gcc-toolset-14/enable
  cd /src/scratch/temp.linux-aarch64-cpython-311 && make _cinderx -j8
  md5sum /src/scratch/lib.linux-aarch64-cpython-311/_cinderx.so'
```

**红线：构建前后必须 md5 比对确证已重链**——macOS 绑定挂载下存在
陈旧构建陷阱（docker cp 保留宿主 mtime 亦会使 make 判定源码未变）。

正式性能口径 = PGO+LTO 三相构建（A/B 实测 +3.1pp）：

```bash
BUILD_TREE=/src/scratch/temp.linux-aarch64-cpython-311 \
CMAKE_BIN=<cmake 路径> \
TRAIN_CMD='bash /src/ci_pipeline/scripts/train_full_311.sh' \
  bash /src/ci_pipeline/scripts/build_pgo_lto_311.sh
```

训练负载定稿为 16 基准多形态混合 + 冒烟、全程 JIT-on
（train_full_311.sh，训练集实验矩阵结论见
M10-pgo-trainset-log.md；勿加解释态训练遍——实测稀释 JIT 态
画像净负，解释器纯本底对训练内容不敏感）。

注意：PGO-use 状态下改源码重编会报 coverage-mismatch 错——**开发
迭代期先复原普通构建**（`cmake -DENABLE_PGO_USE=OFF -DENABLE_LTO=OFF .`），
收口时再整套重跑配方。从零重建构建树用 `setup.py build`（需
setuptools 与 `CINDERX_LOCAL_DEPS_DIR=/root/localdeps`）。

## 三、运行时接入

cinderx 经 sitecustomize 挂载（样例已入库 `docs/dryrun/bench/sitecustomize.py`）：

```bash
mkdir -p /tmp/m9sc && cp /src/docs/dryrun/bench/sitecustomize.py /tmp/m9sc/
export PYTHONPATH=/tmp/m9sc:/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib
export PYTHONJITAUTO=2
```

## 四、A/B 主入口（run_ab.py）

```bash
docker exec dryrun-perf bash -c \
  'cd /src/docs/dryrun && /opt/python/cp311-cp311/bin/python3.11 run_ab.py --out /tmp/my-ab'
```

- `--benches a,b,c`：只跑指定基准（19 项清单见脚本内 BENCHES 表）；
- `--timeout N`：单基准超时（默认 900s）；
- 输出：`<out>/summary.json`（各项 ratio=stock/jit 与 GEOMEAN）+
  每基准 pyperf json；
- 协议：pyperf p3/v5/w3（预演严谨度；正式口径换 pyperformance 默认
  约 20 进程）；A 侧 = 同解释器不带 cinderx。

run_ab 专用环境变量：

| 变量 | 含义 |
|---|---|
| `M9_THRESHOLD` | B 侧 PYTHONJITAUTO 阈值（默认 2） |
| `AB_LEGACY_PREFIX=1` | 恢复历史"范围阀"口径（编译限 site-packages）——仅用于对照 m9par 之前的历史存档；默认为全表面诚实口径 |
| `M9_MCS` | 设 PYTHONJITMULTIPLECODESECTIONS 做代码布局对照 |

**红线：A/B 跑动期间不得重建或替换 .so**（子进程逐个加载，换库即
污染整轮）；单跑离群项必复测；跨构建绝对值漂移 ±6%，单项优化判据
以受控进程内稳态为准（下节），A/B 作套件级回归检查。

## 五、进程内快测与计数（docs/dryrun/bench/，拷入容器 /tmp 使用）

| 脚本 | 用途 |
|---|---|
| `rich_ab.py` | richards 稳态 per-iter |
| `tri_time.py` | deltablue/raytrace |
| `lag_time.py go` | go（`frame_time.py` 的 go 分支公式有误勿用） |
| `frame_time.py generators\|pickle` | generators/unpickle |
| `ic_stats_run.py` / `lag_stats.py` | IC 计数矩阵（richards/deltablue/raytrace/hexiom；go/generators/unpickle） |
| `pmp_bench.py` | PMP 采样驱动（attach 循环：`gdb -batch -p PID -ex "bt 1"`） |

双模对照法（判定"该不该编"）：同一二进制下
`PYTHONJITAUTO=2`（编译态）对 `PYTHONJITAUTO=2000000`（巨阈值纯
解释态）。编译价值分化数据见 M9-parity-log.md 第三节。

IC 计数矩阵：`PYTHONJITCOLLECTINLINECACHESTATS=1` 运行后调
`cinderjit.get_and_clear_inline_cache_stats()["globals"]`（含
la/lm/sa 各快慢路径、`la_hit_kind_0..7` kind 直方图、站点直方图）。

## 六、运行时环境变量总表

核心：

| 变量 | 含义 |
|---|---|
| `PYTHONJITAUTO=N` | 帧压栈计数达 N 触发编译；0=关；巨值≈纯解释 |
| `CI_JIT_AUTO_ONLY_PREFIX=p1:p2` | 编译范围限定为 co_filename 前缀（不设=全表面） |
| `PYTHONJITALL=1` | 全量编译口径 |

测量/排障开关（默认关，勿用于正确性口径）：

| 变量 | 含义 |
|---|---|
| `CI_JIT_NO_ENTRY_GUARD=1` | 裸编译入口（跳过入口守卫序言，放弃递归/tracing 语义）——守卫成本上限测量 |
| `CI_JIT_NO_INLINE_FRAME=1` | 关闭帧仪式行内化（回落 C helper 路径） |
| `CI_JIT_INLINER=1` | normal 帧模式强开 HIR 内联器（实验件；已证跨版本净负，见 M9-inline-verdict-log.md） |
| `CI_EXC_INJECT=count\|<n>` | 异常注入 fuzz |
| `PYTHONJITLIGHTWEIGHTFRAME=1` | **3.11 勿开**（无布局感知即 SEGV；LWF 判决为不移植） |

自适应策略层（默认全关；重启决策依据见 M9-parity-log.md 编译价值
分化表）：`CINDERX_AUTOJIT_ROI_BACKOFF`、
`CINDERX_AUTOJIT_IC_PRESSURE_RATIO`、`CINDERX_AUTOJIT_PROBATION`（+`_MARGIN`）。

日志/诊断：`PYTHONJITDEBUG=1`、`PYTHONJITDEBUGINLINER=1`、
`PYTHONJITLOGFILE=<path>`、`PYTHONJITDUMPHIR=1`（DUMPASM 需
ENABLE_DISASSEMBLER 构建）。

## 七、改动后的最低验证门（提交前必过）

```bash
# 冒烟四件（docs/dryrun/smoke/，拷入容器后 PYTHONJITAUTO=0 逐个跑）
smoke_laggards.py  smoke_ic_round.py  smoke_frame_inline.py  smoke_entry_guard.py
# 差分门禁（分钟级）
cd /src/scratch/m4-diffgate && python3.11 run_diffgate.py --corpus corpus \
  --out out/report.json --baseline /src/docs/dryrun/m8-diffgate-baseline.json
# refcount 矩阵（六组，interp/jit 两模式对比）
python3.11 /src/docs/dryrun/refcount_matrix.py corpus <组名> <interp|jit> out.json
# 基础 libtest 差分（基线内已知项：test_builtin 微漂移、test_scope 追踪中）
cd /src/scratch/m2-libtest && python3.11 run_libtest_diff.py --out /tmp/lt.json \
  --baseline baseline-m2-cp31113-microdrift.json
# 3.14 反向编译（dryrun-m314 容器：cd /tmp/b314 && ninja _cinderx + 两个 smoke314）
```

方法论红线全表与未决专项清单见 REPORT.md 第五、六节；接手优化前
务必通读（尤其：编译价值分化判决、内联器净负判决、LWF 不移植判决，
避免重走已证伪的路）。
