# M9 第十三轮：vendored 循环 PGO/LTO（构建配置轮）

日期：2026-07-05　分支：`dryrun/m9-pgo-lto`　基线：拍板件轮
（几何均值 0.933x；进程内 richards 11.46 / deltablue 1.40 /
raytrace 153.4 / go ~97-101 / pickle 5.16）

## 一、背景与既有接线

性能归因轮实测 vendored 循环 -O2 无 PGO 在纯解释执行上比 stock
慢 11%（stock python 自带 --enable-optimizations 的 PGO+LTO）。
盘点发现 **CMakeLists 的 PGO/LTO 接线早已完备**：ENABLE_LTO
（GCC：-flto -fuse-linker-plugin -ffat-lto-objects）、
ENABLE_PGO_GENERATE/-USE（GCC：-fprofile-generate →
-fprofile-use -fprofile-correction，.gcda 就地读写）——本轮为
操作化：三相配方脚本 + 实测定价。

## 二、实施

`ci_pipeline/scripts/build_pgo_lto_311.sh`：同一构建树三相就地
（插桩全量重编 → 训练负载 → PGO-use 全量重编），LTO 全程开启。
本轮训练集 = 代表性 6 基准（richards/deltablue/raytrace/go/
pickle/generators）+ 三套冒烟（覆盖解释器循环、JIT 编译路径、
IC/帧 helper）。gcda 161 份。全量重编约 1.5 分钟（-j8，容器）。

## 三、量化（进程内稳态，同日对基线）

| 基准 | 基线 | LTO 单独 | PGO+LTO |
|---|---|---|---|
| richards | 11.46ms | 11.92 | **11.08（-3%）** |
| deltablue | 1.40ms | 1.52 | **1.37（-2%）** |
| raytrace | 153.4ms | 158.4 | **149.1（-3%）** |
| go | ~97-101ms | 100.2 | 99.6（带内） |
| unpickle | 5.16ms | 5.79 | 5.17（带内） |
| generators | 26.3-27.7ms | 26.27 | 27.16（带内） |

判读：**LTO 单独中性偏噪声；PGO+LTO 对 JIT 密集项一致 -2~3%**。
归因轮的 +11% 是纯解释执行口径（巨阈值对照）——auto=2 体制下
热代码均已编译，解释残余占比小，PGO 真实红利落在 helper/编译器
C++ 路径。解释残余占比更高的项（sqlglot/regex_compile）以 A/B
为准。

## 四、门禁

diffgate 923 全绿；refcount 矩阵六组零漂移；三套冒烟全过；
libtest 两项既存 DIVERGE 无新增。本轮零 C++ 源变更（diff 仅
构建脚本与文档），3.14 反向编译门空适用。

## 五、A/B（19 基准，PGO+LTO 构建，19/19 全绿，存档
m9pgo-ab-summary.json）

几何均值 **0.933 → 0.964（+3.1pp）**——显著大于进程内稳态的
-2~3%：pyperf 新进程协议下热身与编译窗口占比高，PGO 同时提速了
**JIT 编译器本身**的 C++ 路径与解释器循环。19 项几乎全线上行：
richards 2.105 / richards_super 2.116 / deltablue 1.166 /
raytrace 0.913 / generators 0.793 / unpickle 0.782 /
regex_compile 0.839 / spectral_norm 0.845 / sqlglot 0.831/0.810 /
go 0.658 / hexiom 0.797 / nqueens 0.789 / scimark 0.877 /
float 0.950 / chaos 0.965。构建配置单轮 +3.1pp，投入产出比为
全战役最高。

## 六、工程注记

- GCC PGO 相间约束：三相须同一源码状态；后续增量编译新 TU 缺
  .gcda 仅降级告警（-fprofile-correction 兜底），源码大改后须
  整套重跑配方；
- 复原普通构建：`cmake -DENABLE_PGO_USE=OFF -DENABLE_LTO=OFF .`；
- 训练负载即优化倾向：更换负载集应重新定价；
- 正式移交：配方应并入 openEuler 侧构建管线（与 CPython 自身
  --enable-optimizations 同批次考虑），训练集换正式基准全集。
