# BOOTSTRAP-950:950 真机开发接续自举手册

> 目的:开发环境从预演机(Apple Silicon 容器)整体迁移至 950
> 真机,**仅以 git 仓库为迁移载体**。新会话在 950 上按本手册自举
> 后,应能不依赖任何口头交接接续战役。本文档同时固化预演机侧
> 只存在于会话记忆中的判例层(第五节),迁移后以本文档为准。

## 〇、文档分工

| 文档 | 职责 |
|---|---|
| **本文档** | 950 开发自举步骤、迁移边界、M10 后半程方法论判例增补、战役现状与待办 |
| RUN-950-repro.md | 交付口径复现(构建三相、A/B 跑法、真机观察清单)——自举的构建部分以它为准 |
| RUNBOOK.md | 开发环命令面(增量构建、plainize、sitecustomize、run_ab、门禁入口) |
| REPORT.md 第五、六节 | M1–M9 期方法论红线全表与未决专项(接手前通读) |
| M9-\*/M10-\*-log.md | 逐轮判决实录;归因或重测前先查有无既判力 |

## 一、迁移边界(纯 git 载体的得与失)

**随 git 到达**:全部源码与构建脚本、docs/dryrun 全部手册与轮次
日志、A/B 跑批器(run_ab_full113.py)、refcount 矩阵
(refcount_matrix.py)、冒烟集(smoke/)、diffgate 基线
(m8-diffgate-baseline.json)、**门禁工具与语料(gates/,本次
自 scratch 收编,详见第四节)**、历史 full113 档案
(full113-ab-v\*-summary.json)。

**不随 git、须在 950 重建**:

1. **性能基线档案不可移植**。full113-ab-v\* 档案全部产自预演机
   宿主,其 A 侧(stock 臂)绝对时间与 950 无可比性;950 首夜
   必须双侧重跑自建基线(第三节)。历史档案仅作预演机侧趋势
   参考。
2. 容器与工具链、pyperformance 三方依赖目录(/tmp/pp113、
   /tmp/ppdeps)、构建树(scratch/)。
3. 预演机会话记忆。其中有长期价值的判例已固化至第五节;逐轮
   细节在 M-log 中。

## 二、环境自举

构建与依赖按 RUN-950-repro.md 第二、三节执行(GCC 14 /
gcc-toolset、CMake ≥3.20、pyperformance 1.13.0 装至 /tmp/pp113、
三方依赖装至 /tmp/ppdeps、`CINDERX_LOCAL_DEPS_DIR` 全新
`setup.py build`)。开发态增补:

1. **仓库布局建议 /src**(跑批器与训练脚本的缺省常量按 /src
   书写);远端:`oe` = gitcode qq_16646553/cinderx_oe,开发
   分支自 `dryrun-311-base` 拉出,CLI 用 `gc`(非 gitcode)。
2. **工具链坑**:若 JIT 静默缺席(基准与解释器同速、无编译
   日志),先核 `_cinderx.so` 的动态依赖与 LD_LIBRARY_PATH——
   旧配方容器需设 `/opt/gcc-14/lib64`,gcc-toolset 新配方容器
   **禁止设**,两错皆表现为静默缺席。
3. **开发迭代用普通构建**:PGO-use 态改源重编报
   coverage-mismatch,先
   `cmake -DENABLE_PGO_USE=OFF -DENABLE_LTO=OFF .`(plainize)
   再增量 make;收口时重跑三相。plainize 后首次 make 必须显式
   核对 `error:` 计数与 .so md5 变化——历史上三次静默半成品
   构建均由此抓获。
4. **bind mount 下增量构建以 mtime 为准**:宿主侧编辑后容器内
   `touch` 全部改动文件再 make,否则增量构建静默跳过。
5. sitecustomize 挂载与运行期环境见 RUNBOOK 第三节;交付口径
   运行期仅 `PYTHONJITAUTO=4`。

## 三、950 基线自建与 A/B 口径

1. **首夜双侧全跑**:
   ```bash
   cd /src/docs/dryrun && python3.11 run_ab_full113.py --out /tmp/full113-950-v1
   ```
   归档 summary.json 至
   `docs/dryrun/full113-ab-950-v1-summary.json`,此后 JIT 侧
   迭代以 `--a-from` 复用该档案免跑 A 侧。
2. **跑批器行为**:每完成一项即增量写 summary.json(监视以
   日志 GEOMEAN 行或进程退出为准,勿以文件存在为完成信号);
   子项聚合为均值(scimark/deepcopy 类聚合项排查劣化必先拆
   子项单测——均值混叠判例见 M10-950-twins-log)。
3. **绝对几何跨夜不可比**(预演机两夜实证:B 侧全域随宿主
   状态平移 3-5%,A 侧为固定档案,表观回归可达 -12%)。表观
   回归的裁决序:①同子集复测(排除单次噪声);②**同夜修前臂
   A/B**——`git checkout HEAD~N -- cinderx/` 回退源码、touch
   全量重建、跑同子集、`git checkout HEAD -- cinderx/` 恢复
   重建(工作树文件级回退,禁止 checkout 移动 HEAD);③逐项
   因果差定谳。950 若为共享机,该纪律原样适用。
4. **A/B 期间不得重建 .so**;定向 A/B 首选同味 stash/checkout
   舞步,禁跨构建比较。

## 四、门禁四件套(gates/ 收编版)

diffgate 与 libtest 差分工具原居 scratch/(gitignored),本次
收编至 `docs/dryrun/gates/`,已验证可从新位置直跑:

```bash
# ① 差分门禁(940 例,分钟级)
cd /src/docs/dryrun/gates && python3.11 run_diffgate.py \
  --corpus corpus --out /tmp/dg.json \
  --baseline /src/docs/dryrun/m8-diffgate-baseline.json
# ② refcount 矩阵(8 组,jit/interp 双模)
python3.11 /src/docs/dryrun/refcount_matrix.py \
  /src/docs/dryrun/gates/corpus corpus_<组名> <jit|interp> out.json
# 组名:calls controlflow frames generators hotloops ic_mutation operators unbound
# ③ libtest 差分
cd /src/docs/dryrun/gates && python3.11 run_libtest_diff.py \
  --out /tmp/lt.json --baseline baseline-m2-cp31113-microdrift.json
# ④ 冒烟四件(PYTHONJITAUTO=0 逐个)
smoke_laggards.py smoke_ic_round.py smoke_frame_inline.py smoke_entry_guard.py
```

**已知失败台账**(勿误判为新增,但失败面会漂移、勿沿用旧归因,
每次必看具体条目):

| 门禁 | 已知项 | 性质 |
|---|---|---|
| diffgate | 2 例基线失败(0 新增为过门标准;近两轮较基线净修复 4 例) | 老账 |
| libtest | test_scope testLeaks(JIT 臂) | watcher 强引用跟踪项,修复在旁路分支未入主线 |
| RCM | AUTO 态 22 例 roi/probation 一次性迁移漂移(calls 22/operators 24/controlflow 1/ic_mutation 1 档) | 非泄漏;**判据=同臂修前后漂移集逐 case 相等**,勿用 jit/interp 互比 |

**RCM 触发红线:凡改引用语义(收养/偷取/Incref 配平)必跑**;
凡新增 deopt 面或改发射形,diffgate 必跑。

## 五、方法论判例增补(M10 后半程,REPORT 五、六节之后)

以下判例此前仅存于预演机会话记忆,迁移后以本节为准。

### 5.1 取证与定价

- **分型计数先于实现**:动手前先以计数/直方图证实标的份额
  (IC 直方图、dis(adaptive=True) 特化流直方图、稳态 perf)。
- **B0(解释态)剖面份额 ≠ B4(交付态)可收割份额**:特化
  消费只加速被编译代码,温函数族大半不编译(COMPARE 轮教训)。
- **稳态 perf 用 ready-file 协议 attach**:进程内预热完毕落
  ready 文件,perf 再挂;全进程剖析被预热编译污染(曾产出
  27% 编译器份额假象)。短窗 mid_best 不可用于含大模块导入
  编译面的用例定价。
- 概率性崩溃复现与修复验证 N≥3;监视脚本禁用
  `pgrep -f <关键词>`(自匹配自身命令行永不退出),以日志
  终值行为完成信号。
- 剥符号二进制热点贴名:按 `nm -D` 最近前导出符号 + 加载
  滑移(主二进制 +0x400000)人工贴名,静态函数按链接序推断
  所属 .c(meteor=setobject.c 判例)。

### 5.2 特化消费与 simplify 改造

- **specializedOpcode() 是全部特化消费的单控制点**;新特化形
  必须入其白名单,3.11/3.12+ 名单分属不同预处理器块——嵌错
  版本门会被静默预处理吃掉(COMPARE 轮定罪实录),改后必以
  指针级探针或 HIR dump 验证可达。
- **3.11 quicken 将 adaptive 计数器清零**(非
  adaptive_counter_start 的 backoff=5):首执行即尝试特化,
  失败置 backoff≥1。故"ADAPTIVE 形且 backoff≠0 ⇔ 尝试过且
  失败"(isSubscrAdaptiveStuck 判别式);凡以解释器计数器为
  判别式,必先核实际写入路径初值,勿从头文件常量推断。
- **simplify 慢臂禁止重发同名指令**(驱动器固定点重访导致
  无限自展开),一律 CallStatic 直呼对应 helper;新 HIR 指令
  走全套 opcode 开关清单(hir_ops.h X-macro、instr_effects
  两处、DCE、LIR generator、printer、parser、hir.cpp 两处
  分类)。
- **UseType 锚会被紧邻 RefcountInsertion 之前的 DCE 删除**
  (无输出、无 store 效应、不在 useful 根集):收养引用的
  后置释放锚必须用 UseObj(DCE 显式豁免的无输出零代码指令,
  本仓已有)。收养配方:LoadArrayItem(borrowed=false) 收养
  旧值 + 覆写 + UseObj 锚,释放自然后置。
- 手工 Incref/Decref 依旧全线禁止(RC pass 输入契约)。

### 5.3 LIR 与 deopt

- **LIR 基本块必须显式终结**:依赖布局相邻的隐式坠落
  (switchBlock 收尾而未建边)是潜伏缺陷面——
  kIsNegativeAndErrOccurred 的 set_err 悬空块蛰伏至索引链轮
  才引爆(零后继块被重排扔远,运行时坠入无关块;三种表象:
  垃圾指针 XDecref/结果带异常返回/桩区 PC)。新 lowering 一律
  appendBlock/appendBranch 建边;审计法:grep switchBlock,
  核每处前置是否已建边。
- **崩溃点漂移 + 症状多形 = 延迟性状态污染**,勿追炸点;
  裁决序:能力二分(半区禁用)→ 最小复现 → HIR → LIR(重排
  后)→ 布局逐层下潜。
- **新发射形验证矩阵必含**:同函数内 try/except 包裹的慢臂
  deopt 形(异常经 deopt 入本帧 handler)、析构重入序
  (`__del__` 经容器观察覆写前状态)、别名(值即容器)、负/
  巨整数/布尔下标、混合 typecode、越界异常路径批量压力。
- helper 行内快路径的准入 = 该 helper 全部内部分支(JITRT_Call
  双 NULL 判例);兜底符号引用约定(借用/新引用)与 vanilla
  必须逐一核对。

### 5.4 策略与体检

- 聚合项(scimark 五子核、deepcopy 子项)劣化必先拆项单测。
- 落后项三态分诊(B0/B4/巨阈值)先于深潜;C 主导天花板
  (元组键哈希、set 内建、C 正则)直接销案勿硬啃——已定谳:
  meteor_contest、mdp、telco。
- 小生成器表达式驻留解释器为策略既判(尺寸门
  PYTHONJITSYNCGENMINUNITS=128;小紧 yield 编译反 +58%),
  非缺陷;出路归投机内联族。

## 六、交付口径与旗标清单

交付态 = `dryrun-311-base` HEAD 默认值 + `PYTHONJITAUTO=4`,
包含:提前 quickening、守卫自适应去特化、同步生成器尺寸门、
LGB 守卫式装载、调用直派、入口缓存、fresh-attach(预算 8)、
桩共享、kwnames 绑定缓存、COMPARE INT/FLOAT 融合、下标读写全
家族(list 写侧/array d+i/失败位点证据门)、索引链内联、大页
对齐修复。

**默认关闭且勿开**:属性/方法精确类型投机(三审维持)、
`PYTHONJITCOMPACTLONGGUARDS`(pyperformance 中性偏负)、
`CINDERX_AUTOJIT_ROI_BACKOFF`(未激活,异常率冻结机制随之
惰性)、`PYTHONJITHUGEPAGES=0`(切换整个分配器,与常驻页
假设不兼容,历史性损坏)。

## 七、战役现状与待办(2026-07-13 迁移点)

预演机侧参考弧:v22 1.038 → v27 **1.054**(几何,86 项,
≥1.0 共 62);v28-v30 受宿主漂移污染,绝对值弃用,因果结论
见 M10-subscr-blindspot-log 与 M10-idxchain-log。950 复测已
确认近两轮正面(!83 下标盲区、!84 索引链)。

MR 台账(近程):!79 入口缓存、!80 lm 救援、!81 kind-6+kwbind+
生成器门、!82 大页+COMPARE、!83 下标盲区四件套、!84 索引链+
LIR 悬空块修复。

**下一主项(已定方向):投机内联,分三段**——P1 genexpr/
推导式内联(单调用点静态可知被调方;吃 hexiom 驻留 12%、
nqueens 解释残留 40%);P2 固定被调方小函数直接展开(sor/lu/
monte 的 `_idx`/`nextDouble` 派发链);P3 方法派发投机(IC
反馈选单态目标;richards 族 ~20-25% 可回收)。硬前置 = 多帧
deopt 具象化,先做可行性验证;摸底结论见 M10 投机内联轮①
(任务台账 #112)。早年"静态内联器净负"判决成立于无类型
反馈时代,属前提已变的合法重测。

**平行项**:
- **BOLT@950**(一次性系统杠杆,950 的 glibc 2.34 满足
  LLVM17):`-Wl,--emit-relocs` 重链解释器与 _cinderx.so
  (.rela.text 已验证保留)→ ARM 无 LBR,用插桩模式
  (`llvm-bolt -instrument` → 跑训练负载 → `merge-fdata` →
  `llvm-bolt -o` 优化产物)→ 与未 BOLT 产物 A/B。
- 大页修复在 950 的兑现验证(JIT slab 的 AnonHugePages 归属)。
- 清尾包(合计 0.3-0.5pp 档):PrimitiveUnbox TCInt64 紧凑
  内联(索引链孪生)、类型化迭代三臂(形状见
  M10-idxchain-log)、lm per-entry hint、STR 比较准入判别式。
- 二类盲点 DSO 批定罪(logging/typing_protocols/regex 族/
  json/xml,五分钟档)。
- 诊断债(#114):coverage D8 探针、dulwich/genshi 本底层
  差分、html5lib、pickle_pure_python(0.88,950 观察项)。

## 八、程序性约定

- git 身份:`-c user.name=qq_16646553 -c user.email=guozy42@foxmail.com`;
  提交尾注 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- MR:`gc pr create -R qq_16646553/cinderx_oe --head <分支> --base dryrun-311-base --body-file <文件>`;
  正文尾注 `🤖 Generated with [Claude Code](https://claude.com/claude-code)`;
  文案用正式书面语,禁口语比喻。
- 栈式 MR 合并后不自动落 base:后续 MR 建前核实 base 分支
  已包含前序合入。
- 提交信息经 `git commit -F <文件>` 传递(heredoc/printf 曾
  发生引号吞噬)。

## 九、首日验证清单

1. 构建:全新 `setup.py build` → plainize 增量链路各一次,
   `error:` 计数为零且 .so md5 变化;
2. 冒烟四件 + diffgate(0 新增)+ RCM 8 组(与本手册台账
   对齐)+ libtest(唯一新分歧应为 test_scope);
3. 运行期:任一基准 B4 显著快于巨阈值臂(JIT 在岗);
   `grep AnonHugePages /proc/<pid>/smaps_rollup` 验证 JIT slab
   大页归属(!82 修复的 950 兑现,顺手记录);
4. perf 可用性(容器需 privileged;稳态 attach 按 5.1 协议);
5. 启动 950 首夜双侧基线(第三节),归档并提交;
6. 以上全绿后,从第七节待办择项开工。
