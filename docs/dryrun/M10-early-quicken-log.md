# M10 第十轮：提前 quickening（[P6]）与阈值敏感面首曝

日期：2026-07-08/09　分支：`dryrun/m10-early-quicken`　基线：spectral
验尸轮（#45 合入后）

## 一、对象与机械（[P6]）

spectral 验尸轮遗留三难：低阈值编译读生字节码（失类型情报）、高
阈值失覆盖。本轮实现提前 quickening：stock 的 warmup 计数在
vendored 循环两处驱动（RESUME 每进入一次、JUMP_BACKWARD 每回边
一次，−8 起步步进 1 归零即 _PyCode_Quicken）。[P6] 把步进提为
`Ci_QuickenWarmupStep_311`（initialize() 按 jit-early-quicken /
PYTHONJITEARLYQUICKEN 旗标置 4，默认开；关闭或未初始化时为 1，
与 stock 逐字等价）：第 2 个 warmup 事件即 quicken，**带循环 code
首调内成熟**，run-once 无循环代码永不付税。机械验证：直调两次的
函数 dis(adaptive=True) 已见特化形。

## 二、阈值扫描与特化消费风暴（三连发现）

early ON 三极扫描（普通构建，进程内 best-of，ms）：

| 配置 | spectral | richards | nbody | raytrace |
|---|---|---|---|---|
| t=2 early OFF（旧口径） | 114.7 | 53.0 | 76.7 | 140.3 |
| **t=2 early ON（本轮定稿）** | 113.5 | 53.0 | 76.9 | **133.1** |
| t=4 early ON | 93.6 | 53.2 | 76.8 | **191.8（风暴）** |

t=4 一度呈三极全收，但全集 86 项曝出两类问题：

1. **阈值敏感崩溃族（与 [P6] 无关的存量缺陷，本轮修复）**：
   async_tree/none、memoization 在 auto≥4 SIGSEGV（详见第三节）；
2. **特化消费 deopt 风暴**：raytrace t=4 早熟编译后 −41%（perf 实测
   deopt 机械占 ~9% 周期）。机理：BINARY_OP 等类型守卫来自单次
   观测，int/float 混流受者（Vector(0,0,0) 整数零初始化）守卫连环
   失败；与属性投机同病（单观测无多态证据）。ROI backoff 可灭
   风暴（188→140.7）但记账开销误伤 richards（52→59，预算无关），
   不能默认上。**t=2 下编译钩子先于 RESUME 的 warmup，无循环热
   函数仍从生码编译，天然免疫风暴**——early ON × t=2 实测四极
   全绿且 raytrace +5%（quickening 的解释器侧收益）。

**定稿口径：early quicken 默认开 + auto 维持 2**。auto=4（spectral
类 +18% 的成熟度红利）悬挂于"守卫自适应去特化"（deopt 熔断后去
特化重编，DeoptStat 计数机械现成）；COMPARE_OP 特化形 3.11 摘除
（保守去特化）；OSR 仍为独立机会项。

## 三、阈值敏感崩溃族（存量，全阈值静默越界写）

全集 v4（t=4 口径）曝出 async_tree/none、memoization B 侧 SIGSEGV。
验尸链：pyperf worker core → `jit::finalize() → deoptFuncImpl →
setVectorcall` 写已释放函数对象；PID 级布防/触发日志对账 →
受害者（asyncio gather 闭包）看护布防且触发却仍在注册表 → 真凶
**deopt 自伤**：注册表（CompiledFunction）持有已编译闭包的最后
强引用，removeCompiledFunc 放引即死，随后 setVectorcall 写尸体。
命中与堆布局相关呈概率性——**auto=2 口径下同型写多半落在未归还
的 pymalloc 竞技场，历史上一直是静默越界写**。三件修复：

1. **weakref 死亡看护**（watchFuncDeath311）：3.11 无 function
   watcher（PyFunction_EVENT_DESTROY 为 3.12+），funcDestroyed
   从不触发致注册表悬垂。编译注册时布防 weakref 回调（CPython
   析构顺序中 ClearWeakRefs 先于字段清理，回调期可安全走完整
   注销链）；finalize 撤防归还引用；
2. **finalize 活表推进**：deopt 过程可触发看护回调重入擦表，持
   迭代器遍历失效；改为每轮取 begin() 推进 + 防死循环兜底；
3. **deopt 持引护体**：deoptFuncImpl 全程持强引用，杜绝
   removeCompiledFunc 放最后一引后写尸体。

修复后 async_tree 全变体 × 多阈值 20/20 通过。

**方法论**：①阈值是从未被扫过的敏感面——九轮验证全在 auto=2，
一次全集 × 非默认阈值即曝出崩溃族与风暴族，配置维度的覆盖盲区
比代码路径盲区更隐蔽；②"布防了也触发了却还在表里"类矛盾优先查
同调用内时序（自伤）而非机制缺失；③单跑 PASS/FAIL 不可判概率性
崩溃（堆布局相关），必须重复跑；pyperf -o 复用报错会伪装成 FAIL，
判定脚本输出文件必须唯一化。

## 四、门禁与全集（v5，early ON × t=2 定稿口径）

门禁：交付链相四验收 OK；冒烟七件全过；diffgate 全绿；refcount
六组零漂移；libtest 仅两既有已知项。async_tree 全变体 × 多阈值
20/20 复验通过（崩溃族修复）。

全集 86 项（存档 full113-ab-v5-summary.json；v4 为 t=4 试验口径
存档，两项崩溃即其曝出物）：86/86 零报错，async_tree 0.975 /
async_tree_memoization 0.954 复活；raytrace 0.974→**1.005**（越线）、
telco +7.9pp、float +5.5pp、async_tree_cpu_io_mixed +11.4pp、
python_startup 持平（run-once 零税验证 ✓）。几何 0.9453
（v3 0.9561）：差额主体为画像骰子敏感项本轮落于历史带低端
（generators 0.633∈[0.63,0.81]、django_template 0.756∈[0.76,0.91]、
richards_super 2.16 仍居高位），受控进程内对照下 early ON 对四极
严格不劣。判定：**early ON × t=2 定稿**；成熟度红利（spectral 类
+18%）与 auto=4 一并悬挂于守卫自适应去特化专项。
