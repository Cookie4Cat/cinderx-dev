# M9 正确性专项：refcount 矩阵闪烁漂移归因（MCACHE 名字槽驱逐）

日期：2026-07-05　分支：`dryrun/m9-refcount-flake`　立案：IsTruthy 轮
（`corpus_operators/case_sub_index_protocol` 对 `_pname` 值 -1 漂移，
约 1/2 概率、仅 jit 模式；二分证实预存，此前各轮零漂移系单跑侥幸）

## 一、取证链（零修改原则）

1. **复现器悖论**：任何最小侵入（一次文件写、一个哨兵调用、精简
   快照集）都足以压制闪烁——布局/流量极端敏感，凶手只能在
   **逐字原版 harness** 上抓。PYTHONHASHSEED 定种不消除闪烁
   （seed=1 内部仍 1/3），排除纯哈希决定论。
2. **零修改锚点**：python 二进制无静态符号（sys_getrefcount 不可
   断），改锚 cinderx .so 的 `force_compile`（pyjit.cpp:2373，全
   调试信息）；语料工厂使 case 同名（`_make_case.<locals>.case`），
   以**第 520 个包装器编译**（目标 case 排序位 519）为命中条件；
   从 func_globals 裸走字典（注意 `exec(_SRC, _ns)` 生成的算子
   函数 globals 是 _ns 而非模块字典——首版走错）定位 `_pname`
   值对象，当场挂硬件写看点，进程自然跑完由 harness 自报判决。
3. **首次武装即捕获**：132 次写命中全程回溯。窗口内快照自身的
   平衡触碰（LOAD_FAST/调用参数增减）构成 ±3 周期；**命中 #6：
   `_PyType_Lookup` 将计数 7→6，之后所有周期地板永久低一格**——
   唯一不成对写即凶手。尾部归零链为进程收尾拆解（良性）。

## 二、判决：stock 良性簿记，非引用泄漏

CPython 3.11 类型方法缓存（MCACHE）条目对缓存的**名字持强引用**
（`Py_SETREF(entry->name, Py_NewRef(name))`）。目标字符串曾作为
某次类型查找的名字驻留缓存槽（基线快照如实计入该 +1）；测量窗口
内 `case_sub_index_protocol` 每次调用在函数体内新建类（新
tp_version_tag），类型查找按（版本, 名字哈希）散列，撞上驻留槽
即驱逐旧名——合法释放，净 -1。闪烁性 = 版本流水 × 哈希的撞槽
时机；仅 jit 模式可见是因为 JIT 拉式验证与 IC 填充带来额外且版本
序不同的类型查找，改变缓存流量。无泄漏、无 UAF、无修 JIT 必要。

## 三、修复（矩阵判据确定化）

`refcount_matrix.py` 快照前调用 `sys._clear_type_cache()`：两侧
快照均在 MCACHE 空态下取数，任何曾作查找名的字符串目标不再受
驱逐时机影响。验证：jit 模式连续 10 跑零漂移（修前 ~1/2 闪）；
interp 模式与全五组（calls/operators/hotloops/frames/ic_mutation）
interp vs jit 零差异。原拟"矩阵 ≥3 次判稳"的门禁加固被根因修复
取代。

## 四、可复用方法论

- 布局敏感闪烁案的复现器必须**零修改**；侵入式采集先在原版上
  验证闪烁率不灭；
- 无静态符号的二进制以带调试信息的 .so 符号为锚，配合忽略计数/
  条件回调直达目标程序点；
- 硬件写看点 + 让进程自然完成（不 kill），使采集与 harness 判决
  同运行关联；
- ±1 漂移嫌疑名单：MCACHE 名字槽（本案）、interned 短字符串的
  共享使用、伪不朽单例（矩阵已排除）。
