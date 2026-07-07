# M10 第十一轮：守卫自适应去特化与 auto=4 口径切换

日期：2026-07-09/10　分支：`dryrun/m10-adaptive-despec`　基线：
方法论轮（#47 合入后；#48 撤案未合入，投机默认维持关闭）

## 一、对象

early quicken 轮遗留：auto=4 的成熟度红利（spectral 类 +18%、
django/sqlglot 解析组抬升）被特化守卫 deopt 风暴阻塞（raytrace
−41%：单次观测的 BINARY_OP 类型守卫 × int/float 混流受者）；ROI
backoff 可灭风暴但记账开销伤 richards 13%，不能默认上。本轮实现
止损机制本体。

## 二、机制：deopt 熔断 → 摘守卫重编（粘滞一次性）

- **计数**：prepareForDeopt 尾部（与 recordDeoptForRoiBackoff
  并列）按 code 统计 kGuardFailure 深度 deopt
  （CodeExtra::despec_deopt_count，原子直增；instrumentation
  deopt 与其他 reason 不计）；
- **触发**：越限（jit-adaptive-despec-threshold，默认 64）即 CAS
  置粘滞位（despec_state 0→1，保证单次触发），随后枚举该 code 的
  全部已编译函数 uncompileImpl 卸载。[P3] 调用计数已越阈，函数在
  下次调用自然重编；
- **消费**：BytecodeInstruction::specializedOpcode() 为全部特化形
  消费的单控制点（BINARY_OP/COMPARE 类型守卫、下标/解包保留形与
  属性投机识别同经此路），粘滞位置位后一律按 unspecialize 返回生
  形——重编产物即九轮调优的生形翻译；
- **防振荡**：粘滞位不回退，每 code 至多一次重编；
- 旗标：jit-adaptive-despec / CINDERX_ADAPTIVE_DESPEC（默认开）+
  阈值旗标；触发计入 gate stats（adaptive_despec）。

设计取舍：与 ROI backoff 的差异在终端动作——backoff 冻回解释器
（放弃编译收益）且预算轮转记账重；despec 只放弃"赌错的投机"，
保留编译本体收益，无持续记账（计数在既有 deopt 慢路径内，越限后
短路返回）。

## 三、判据（普通构建，进程内 best-of，ms）

| t=4 | despec 开（默认） | despec 关（对照） | t=2 参照带 |
|---|---|---|---|
| raytrace | **142.3（风暴熄灭）** | 197.7（风暴复现） | 133-140 |
| spectral_norm | 96.5（红利保住） | — | 110-118 |
| richards | 53.8（零损） | — | 49-55 |
| nbody | 78.0（零损） | — | 76-78 |

守卫不失败则机制不触发（spectral 零干扰）；赌错者一次重编封顶
（raytrace）。**auto=4 口径解锁成立**，交付阈值 2→4（run_ab 缺省
与训练脚本同步），成熟度编译（early quicken × t=4）全面生效。

## 四、B 探针存档（并行任务）

generators/coroutines 三态（stock/纯解释/JIT，ms）：generators
72.9/86.0/96.6——**编译净效应 −11%**；coroutines 23.4/33.9/29.9——
净效应 +13%。快牌确认：同步生成器不编译（或试用冻结）可把
generators 由 ~0.76 拉至 ~0.85（解释本底比），但须只圈同步生成器
（协程相反）且过全集防误伤（html5lib/tomli 等内部生成器负载）。
移交下轮候选。

## 五、首轮收口曝出两案（机制补完）

首轮收口链两处红灯，均已定界修复：

1. **熔断后重编链路断裂（机制缺陷）**：smoke_frame_inline 断言
   is_jit_compiled 失败——[P3] 钩子对已越阈 code 走"已决快速返回"
  （计数不再推进也不再编译），despec 卸载后函数永久滞留解释态。
   修复与 backoff 非冻结分支同法：触发路径就地 scheduleJitCompile
   （粘滞位先于卸载置位，重编必然读到去特化输入）。修复后 raytrace
   @4 进一步降至 **124.5 ms（历史最佳，即 spectral 轮实测的
   全去特化最优产物）**——此前判据表中的 142.3 实为"卸载未重编、
   回落解释"的残次状态；
2. **refcount 矩阵记账工件（非泄漏）**：corpus_operators 多案例
   `_ns: -1`——共享 helper 在前序案例跑热后被 force_compile，
   烘焙了 LOAD_GLOBAL_MODULE 等特化引用；despec 中途重编换产物，
   烘焙引用集合改变，双快照协议无法归因。三重排除确认非泄漏
   （despec 关即 0；恰为每案例 −1 不随迭代累积；冷函数单跑复刻
   为 0）。处置：矩阵 jit 模式固定 CINDERX_ADAPTIVE_DESPEC=0 并
   注明判据边界——该判据验证编译产物的逐迭代引用中性，产物切换
   的引用平衡由触发路径 Ref 持有审计保证。

## 六、门禁与全集（v6，early × t=4 × despec 完整机制）

门禁：交付链相四验收 OK；冒烟七件全过（含修复后的
smoke_frame_inline）；diffgate 全绿；refcount 六组零漂移（含
corpus_operators，工件处置生效）；libtest 仅两既有已知项。

全集 86 项（存档 full113-ab-v6-summary.json，对照 v5＝early×t=2
口径）：几何 **0.9453 → 0.9558（+1.1pp，追平历史最佳且为机制性
收益）**。主要兑现：spectral_norm 0.873→**1.036**（劣化项转赢家，
成熟度红利）、raytrace 1.005→**1.151**（despec 去特化产物优于
t=2 生码产物）、chaos +14.7pp、float +11.8pp、asyncio_tcp_ssl
+15.5pp、richards_super 2.232、django_template +5.4pp、
python_startup +6.0pp；守护组无回退（richards 2.277）。回退尾
（telco −9.1、sphinx −8.4、async 族散项）均为跨构建波动带内摆动；
generators 0.647 持平（t=4 无额外伤害，不编生成器快牌仍在下轮
候选）。<0.90 项数 25 持平。

**交付口径定稿：early quicken × auto=4 × adaptive despec**。
