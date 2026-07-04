# M9 优化第五轮：动态真值判定行内快路径（IsTruthyFastPath）

日期：2026-07-05　分支：`dryrun/m9-istruthy`　基线：落后组轮
（几何均值 0.858x；richards 修复后 PMP 榜首 = PyObject_IsTrue ~11%）

## 一、机制

`simplifyIsTruthy` 仅处理编译期已知类型（对象常量/TBool/TLongExact/
定长容器）；动态类型操作数（richards 的全部真值判定——布尔谓词
返回值与标志位属性，经 LoadAttr/调用返回均为 TObject）一律下沉为
`PyObject_IsTrue` C 调用 + kNotNegative 守卫。每次真值判定付
调用序言/返回/守卫检查的全套代价，而 PyObject_IsTrue 本体的前三行
就是 True/False/None 单例比较。

## 二、实现：新 LIR 调用形指令 kIsTruthyFastPath

行内发射 `Py_True`/`Py_False` 单例指针比较（动态真值判定的主体
形态），双失即调用 PyObject_IsTrue（慢臂结果可为 -1，沿用既有
kNotNegative 守卫）；结果统一落 w0 后搬运至分配的输出寄存器。
postalloc 已按调用约定把唯一实参放入 x0，调用形语义保证 x9-x15
无活值可供暂存。发射门 `CINDER_AARCH64 && !GIL_DISABLED && <0x030C`，
3.14 走原路（X-macro 条目存在但不发射，行为逐字不变）。

接线清单（新调用形 LIR 指令，第四处为 IC 计数轮抓获的教训位）：
instruction.h X-macro（与 attr 变体同旗标）→ instruction.cpp
isCallLike → postalloc 进入条件两处 + **操作码保留条件**（漏之即
被改写 kCall 直呼 helper、快路径静默失效）→ autogen translate +
dispatch。本指令无第二输出，postgen 免改。

## 三、量化（进程内稳态，同机同法）

richards per-iter 16.00 → **15.06ms（对 stock 1.46x）**；
deltablue/raytrace/go 噪声区间内持平（真值判定占比低）。

19 基准 A/B（auto=2/w3/p3v5，存档 m9truthy-ab-summary.json +
复测 m9truthy-ab2-rerun.json）：几何均值 0.858→0.859（套件层持平，
单项 6% 收益被协议方差淹没）；richards 首跑 1.213 复测 **1.490**
（与进程内 1.46x 吻合，收益实锤）、richards_super 1.549、deltablue
0.925、raytrace 0.912；generators/pickle 波动均在既往方差带内。
**方法论确认：冻结均衡使 A/B 单项方差可达 ±0.15，受控进程内稳态
测量为单项优化的判据，A/B 只作套件级回归检查。**

语义冒烟：True/False/None/int/str/list/自定义 `__bool__`（含抛异常
路径）双侧一致；3.14 反向编译 + truthy/attr/method 冒烟通过。

## 四、门禁与新立案

diffgate 923 全绿；generators 语料与基础 libtest 差分基线原样；
本轮及前三轮定向冒烟复跑通过。

**refcount 矩阵新立案（预存缺陷，非本轮引入）**：
`corpus_operators/case_sub_index_protocol` 对模块级遗留字符串
`_pname` 出现 -1 漂移，**约 1/2 概率闪烁、仅 jit 模式**；二分证实
合并基线（MR #21 后）同样复现——此前各轮"零漂移"系单跑侥幸。
案件特征：函数内定义类（每调用创建/回收 200 个类型对象）+
`__index__` 协议下标；嫌疑面在类型生命周期与 IC 结构交互
（watcher 表/缓存条目/编译产物引用）。移交下轮专项：矩阵门禁应
改为 ≥3 次重复判稳，并以该案为 refleak 专项首客户。

## 五、遗留与后续杠杆

- 真值判定慢臂的 None 分支未行内（None 真值判定常见于
  `while x:` 链表遍历——richards 已覆盖主体，增益预计有限）；
- x86 侧未实现（预演仅 aarch64；上游候选评估同 method stub）；
- go 专项三件套、generators 帧协议轻量化照旧排队。
