# M9 收尾：三类测试套件最小拉通日志（RuntimeTests / test_cinderx / libtest）

日期：2026-07-04　分支：`dryrun/m9-testsuites`　基线：`dryrun-311-base@1dbdbb3c2`（M9R3 合入后）

背景：盘点确认三类正式套件（设计书 §4.3）在预演期均未接入——
diffgate 与基础 libtest 差分是仅有的测试腿，且 M9R3 六根因中至少四个
属于三类套件设计上应捕获的类型。用户拍板收口前最小拉通：三条腿各
完成 3.11 首跑并固化首跑基线，四个 `*_311.toml` 挂入 run_gate。

## 一、RuntimeTests（原生 gtest）3.11 首次构建与首跑

**接入坑清单（正式 M1 的实测输入）**：

1. `ENABLE_RUNTIME_TESTS=ON` 要求 `Development.Embed`（runtime_tests
   为嵌入式可执行）——**manylinux 分发不带任何 libpython**，无法
   链接；目标 openEuler python3 为 `--enable-shared` 无此问题。预演
   对策：以库内 upstream 3.11.6 树（=D7 锚点）构建
   `--enable-shared` 解释器（/opt/py311shared），一并兑现 M2 移交的
   "--enable-shared 环境复测"前置。
2. FetchContent 缓存中 googletest 为 v1.15.2，声明要求 v1.17.0——
   CINDERX_LOCAL_DEPS_DIR 布局校验 tag，需补 fetch（网络依赖记档）。
3. 顶层 CMakeLists 的 `set_flag` 宏对未定义变量直接报错
   （`if(DEFINED X AND ${X})` 空展开）——独立 cmake 配置必须显式
   传全所有 ENABLE_* 选项。
4. 旧 glibc（manylinux2014 2.17）下 dlsym 在独立 libdl，
   runtime_tests 需显式链接（已加 `${CMAKE_DL_LIBS}`，新 glibc 为空
   值无影响）。
5. **10 个测试源无法进入 3.11 构建**（CMake 排除清单
   `CINDERX_RUNTIME_TEST_EXCLUDES_311` 首版）：9 个编译期
   （PyUnstable_Code_New 等 3.12+ API：bytecode/deopt/gen_asm/hir/
   lir×2/osr×3）+ 1 个链接期（sanity_test 引用 Static Python
   classloader thunk 符号，3.11 空壳）。lir_test 属版本无关层却依赖
   3.12 API 构造 code 对象，**收编优先级最高**；deopt/gen_asm 按
   M3/M6 层点亮，hir 按 M4（需 3.11 快照段）。

**首跑基线**（逐套件独立进程，崩溃隔离；存档
m9-testwire-rt311-persuite.tsv）：

- **M1 层白名单（版本无关基础设施 9 套件）56/56 全绿**——M1 出口②
  首次实测兑现；
- 全量 97 套件：572 通过 / 358 失败 / 2 套件 SEGV
  （HIRParserTest、BuiltinLoadMethodEliminationTest——均为 Static
  Python 形态用例，与 _static 空壳一致）；
- 失败主体为 HIR 快照类套件（CleanCFGTest/HIRBuilderStaticTest/
  GuardTypeRemovalTest/InlinerTest 等整套 0 通过）——即设计书 M4
  "hir_tests/*.txt 需 3.11 快照段"的实测形态，属层未点亮而非缺陷；
- 全量单进程连跑会在首个 SEGV 中止（HIRParserTest.
  InvokeStaticFunction）——**正式接入需 gtest 层崩溃隔离**（逐套件
  进程化，diffgate 同款教训）。

## 二、test_cinderx 3.11 首跑与取舍清单首版

**runner 适配（test_cinderx_runner.py，三处均为追加式）**：

1. LWF 环境版本门：原 runner 对除 test_osr 外所有套件强制注入
   `PYTHONJITLIGHTWEIGHTFRAME=1`，3.11 上 cinderx.init() 即 SEGV；
   目标解释器 <3.12 时降级为 0。
2. 3.11 取舍清单首版（EXCLUDED_TEST_FILES_311 /
   EXCLUDED_SUITES_311）：设计书排除项（Static Python/compiler/
   parallel GC/子解释器/3.12+ 字节码/perf 系）+ 收集期炸弹
   （模块级 import `_static` 或 3.12+ `_testcapi` API 的文件——
   pytest 收集中断会拖垮整个套件）。
3. `--keep-going`（首跑基线需要全量信号，原 fail-fast 保持默认）。

**首跑基线**（取舍后 runner 全量 + 按文件崩溃隔离复核；存档
m9-testwire-tcx-summary2.json / m9-testwire-tcx-perfile.tsv）：

- 全绿：test_frame_evaluator（6）、test_jit_count_calls（4）、
  **test_jit_exception（15/15，M6 域）**、test_oss_quick、
  test_jit_preload（3+6 子测试）、test_coro_extensions（skip）；
- 部分失败（正式燃尽清单）：test_jit_disable 4/15、test_jit_frame
  5/14、test_jitlist 4/12、test_jit_global_cache 1/12、
  test_immortalize 1/3、test_jit_type_annotations 2/5、
  **test_jit_specialization 18/18 全红**（PEP 659 交互域）、
  test_osr 6/19、test_jit_support_instrumentation 13/20（D8 域）；
- **崩溃两案（新发现，套件价值的直接证明）**：①
  `cinderx.jit.pause()` 上下文管理器退出段错误（test_jit_frame::
  test_line_numbers_after_jit_disabled，D8 pause 机制 3.11 缺口）；
  ② test_jit_generators 文件内 SEGV（M9R3 后生成器仍有残余崩溃
  形态，正式 M8 输入）；
- 阻塞（surgery 项）：test_cinderjit / test_jit_attr_cache /
  test_jit_coroutines 模块级 import 链触及 `_static`（设计书"必跑"
  项，需 import 守卫改造）；test_type_cache 用 3.12+
  `_testcapi.type_assign_version`。

## 三、libtest 配置③（JIT-on 双阈值）首跑

**接入坑**：lib_test_runner.py 同样无条件注入
`PYTHONJITLIGHTWEIGHTFRAME=1` → dispatcher 进程 init 即 SEGV（整个
运行零输出 rc=245，定位耗时最长的一坑）；同款版本门修复。曾短暂
误判为 M9 立案的 enum import 崩溃回归——faulthandler 栈恰好停在
enum 类构建——实为 LWF init 崩溃的下游形态，全表面 auto=2 裸复现
（无范围阀 import re/enum/json/dataclasses）三连绿佐证。

**清单**：lib_test_311_jit_on.txt（38 模块：语言核心 26 + gen/协程
4 + 风险区 8；3.11 命名 test_unicode/test_yield_from，3.12 起才更名
test_str/test_pep380）。

**首跑基线**（dispatcher 6 workers；存档
m9-testwire-lt311-t{2,24}-tests_dispatcher.json）：

| 阈值 | 通过 | 失败模块 |
|---|---|---|
| t2 | 31/38 | builtin, coroutines, inspect, scope, sys_settrace, traceback, weakref |
| t24 | 28/38 | builtin, coroutines, descr, exceptions, inspect, scope, sys, traceback, tuple, weakref |

零 worker 崩溃/超时。失败集中在设计书预判的风险区（settrace/
inspect/traceback/weakref）+ t24 特化时序增量（descr/exceptions/
sys/tuple）；六模块双档共同失败（builtin/coroutines/inspect/scope/
traceback/weakref）为燃尽首批。注意与 M2 配置②（26 模块 JIT-off
等价 25/26）口径不同：本表为 JIT-on 的 regrtest 直接失败数。

## 四、suite 骨架与 run_gate 挂接

- 新增 `diffgate_311.toml`（diffgate 主语料 + 基础 libtest 差分；
  M0 遗留的 run_gate 挂接就此销案）、`runtime_311.toml`（3.11 无
  LWF/OSR 拆分档）、`cinderx_local_311.toml`、`libtest_311.toml`
  （双阈值两 job）；基线规范位
  ci_pipeline/diffgate/baselines/cp311.json（当前全绿）。
- run_gate 注册 `pr-311` / `daily-311` 管线（设计书 §4.2 结构；
  ASAN/refleak/热循环追踪仍待接入，daily-311 注释记档）。
- 端到端实证：diffgate_311 经 `run_gate.py --suite diffgate_311`
  完整执行 **2/2 全绿**（主语料 922 + 基础 libtest 差分挂基线）；
  runtime_311/cinderx_local_311 的命令与手工验证流程逐字一致
  （wheel 全流程在 M1 已打通，未重复执行）。
- **基础 libtest 差分（26 模块，auto=24 全表面）首基线**：5 发散
  入册 cp311-libtest-basic.json——test_builtin / test_list /
  test_tuple / test_exceptions 四个 **JIT 侧 SEGV** + test_scope
  FAIL。这是 M9R3 修复后的全表面残余崩溃面（此前 A/B 皆带范围阀，
  测不到），与配置③失败集相互印证，为下一轮燃尽首批目标。
- **环境约束记档**：runtime_311 要求 CINDERX_TEST_PYTHON 带共享
  libpython（toml 注释声明）；容器内已备 /opt/py311shared
  （upstream 3.11.6 --enable-shared）。

## 五、移交清单增量

- RuntimeTests：gtest 崩溃隔离机制；lir_test 收编（版本无关层）；
  HIR 快照 3.11 段（M4）；deopt/gen_asm 层点亮（M3/M6）；
  HIRParser/BuiltinLoadMethodElimination 两处 SEGV 排查（_static
  形态）。
- test_cinderx：`jit.pause()` 3.11 段错误（D8）；
  test_jit_generators 残余 SEGV（正式 M8）；test_jit_specialization
  全红分诊；_static import 守卫改造（test_cinderjit 等三文件，
  设计书必跑项）；coroutine 套件拒编断言改写（D6，本轮未动）。
- libtest：双档共同失败六模块燃尽；config① stock oracle 全量与
  config② 全量等价（M2 正式主体）仍未建。
- 工时：本轮约 2.5 小时（三条腿并行；最费时的坑是 lib_test_runner
  零输出 SEGV 的定位）。
