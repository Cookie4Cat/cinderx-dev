# M10 第九轮：spectral_norm 验尸——早产编译（阈值 vs 字节码成熟度）

日期：2026-07-08　分支：`dryrun/m10-spectral-autopsy`　基线：训练集
轮（#44 合入后，PGO+LTO + train_full 交付口径）

## 一、异常信号与四口径分解

spectral_norm（纯浮点数值）全集 v3 报 0.867，而同族 nbody 1.234
——float unboxing 明明有效，同类分裂即异常信号。进程内四口径
（best-of，ms）：stock 96.95 / auto0 112-117 / 巨阈值 89-93 /
auto=2 全编 110.5。**T1 为正税（vendored 解释器与 stock 相当或
更快），损失纯粹是 T3**；PYTHONJITCOMPACTLONGGUARDS=1 仅 ~1%，
盒装整数假说排除。

## 二、验尸过程（三次反转）

1. jit-list 二分全线 ~110ms 且"巨阈值"独快 89ms——ground truth
   仪器（get_compiled_functions）揭示：**巨阈值口径下 eval_A 实际
   越阈值被编译**（676k 调用/迭代），且 **jit-list 在 auto 开启时
   不设限**（205 函数照编；历史用法不受影响——彼时只需目标在集
   合内）；
2. force_compile 组合矩阵（冷编译）：evalA 114 / parts 127（编了
   更慢）/ 全编 109——与自然越阈值的 89ms 矛盾；入口槽验证排除
   "只编不挂"；自适应字节码计数排除模式间特化开关差异；
3. **热身后编译矩阵定案**：同组合先跑热再 force_compile——
   evalA 91.7（冷 114.3）/ evalA+parts 90.2 / 全编 91.3。唯一
   变量 = 编译时字节码特化成熟度。

## 三、根因

**早产编译**：HIR 前端消费自适应字节码；CPython 3.11 的 code
对象在第 8 次调用才 quicken（QUICKENING_WARMUP_DELAY=8），各站点
随后才特化。交付口径 auto=2 在第 2 次调用即编译——**一切生产
编译都发生在 quickening 之前，HIR 恒读生字节码**，特化携带的
形态信息（BINARY_OP_*_FLOAT/CALL_PY_EXACT_ARGS 等）全部丢失。
阈值扫描（spectral，ms）：t=2 110.5 / **t=16 90.1** / t=64 92.1 /
t=256 91.3 / t=1024 94.9——过 quickening 线即完整收割。
（sqlalchemy 3.14 归因中的"premature specialization"疑点与此同源。）

## 四、阈值面板与二次验尸（特化消费者拆分）

t=16/64 面板（18 项，对照 t=2＝全集 v3 同构建）：spectral
0.867→1.081（t=16）兑现，但 **richards 2.271→1.646、richards_super
2.263→1.651 暴跌**；进程内稳态复核 richards t=2/16/64 =
49.3/73.3/88.7 ms。逐层排除：

1. 覆盖排除：t=16 下 richards 模块 36 函数编成 35（仅 9 次调用的
   外壳 Richards.run 缺席）——回退非覆盖，是**产物质量随输入字节码
   形态而变**；
2. **特化消费者拆分实验**（PYTHONJITSPECIALIZEDOPCODES=0，t=16）：
   richards 72.3→55.0（收回大头）、spectral 91.8→115.7（尽失）——
   同一旗标下两类消费者利益相反：数值/比较族类型守卫为纯收益；
   属性/方法特化形（LOAD_ATTR_INSTANCE_VALUE/
   LOAD_METHOD_WITH_VALUES）的**单观测精确接收者类型投机**在多态
   受者（richards 的 Task 子类族）下守卫连环失败成 deopt 陷阱；
3. nbody 的 t=16 回退（78.6→95.7 ms）与特化消费无关（全关不变）：
   **覆盖损失**——advance 每基准仅调约 2 次、体内两万次循环，永远
   不越阈值。少调次大函数类是阈值提升的硬阻塞（OSR 的用武之地，
   osr_enabled 旗标在树、生产默认关、未验证）。

## 五、处置与落地

**落地**：属性/方法精确类型投机独立成开关
`specialized_attr_speculation`（jit-attr-speculation /
PYTHONJITATTRSPECULATION），**默认关**——修复后构建验证：richards
t=16 53.9 ms（≈ t=2 的 54.9，高阈值回退消灭）、旗标开启复现 82.8、
spectral t=16 收益保留（97.6 vs t=2 117.8）。交付阈值 t=2 下字节码
尚未 quicken，特化形不存在，**交付口径零变化**（面板复测确认）。

**交付阈值维持 auto=2**。阈值三难：t=2 保 richards 类（生码调用/
属性翻译已九轮调优）与 nbody 类（覆盖），失 spectral 类（数值类型
情报）；t=16 反之且 nbody 类覆盖损失无阈值解。**完整解 = 阈值提升
× OSR 环内接管联动**，记入 backlog（spectral 类 +21pp 收益悬挂
其上）；另一路线为成熟度感知的二次编译（quicken 后重编）。

## 六、门禁与面板

（回填）
