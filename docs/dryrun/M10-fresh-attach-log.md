# M10 第二十八轮：新鲜函数对象挂接断链修复——战役最大单跳

日期：2026-07-10　分支：`dryrun/m10-fresh-attach`　基线：!75。
起点：中间带体检定罪的产物级缺陷（bpe_tokeniser 断点采样 591/600
集中于每轮新建的 max key lambda，code 已编译而新实例永走解释）。

## 一、断链机理

3.11 无函数创建 watcher，auto-JIT 唯一驱动是 [P3] 帧压栈计数钩子
（Ci_AutoJitCountFramePush311）。其"已达阈"速返分支假设"编译成功
则调用不再经解释入口"——对**新鲜函数对象**不成立：热路径每轮新建
的闭包/lambda/推导式共享已编译的 code，但新实例入口仍为
StockEntry，解释调用方经 [P7] 行内推入直达 start_frame，撞上速返
即被永久困在解释态。**3.11 的推导式本身就是每执行新建函数对象**，
故此缺陷系统性卡死全部高频推导式位点——这解释了修复的全域收益。

## 二、设计三版迭代与一处既有 UAF

1. **直挂全簿记**（每新实例 tryAttachCachedCompiledEntry）：修复
   生效但海量翻新闭包负载崩坏——sqlglot +57%/sympy +25%（每实例
   仅调用数次，finalizeFunc 的哈希插入+func_dict 强引用+注册税
   ≫ 收益）；
2. **守卫包装器轻挂接**（仅指针换装到 recursionGuardedVectorcall）：
   零簿记，崩坏消除，但收益也归零（常驻包装税，一次性闭包无兑现）；
3. **带预算全挂接**（终版）：每 code 前 8 个新鲜实例内全簿记挂接
   （CodeExtra.fresh_attach_count），超出即停——稳定小实例集全额
   收益，翻新负载税硬性封顶。

**既有潜伏 UAF（本轮定罪的第二只鸟）**：tryAttachCachedCompiledEntry
把 co_extra 槽中的**借用指针**裸传 finalizeFunc，其内部分配（建立
func_dict 等）可触发 GC——当 CompiledFunction 的锚点全部悬于濒死
实例（推导式即弃形态）时，回收在 finalize 中途析构 compiled 并
清空槽位，返回后继续使用即 UAF。插桩生命史实证：
`CACHE→ATTACH×5→第 6 次挂接中 CLEAR→SIGSEGV`（psutil
per_cpu_times 推导式）。修复：跨 finalize 持强引用钉住；
scheduleJitCompile 的函数创建挂接同路径同愈。

## 三、判据

- 同味 plain best-of（五项全正）：bpe_tokeniser −2.7%、sqlglot_v2
  −3.2%、sympy −2.8%、nqueens −2.6%、deepcopy_reduce −4%（编译
  复用 178→249）；UAF 复现 0/5；
- **全集 v20（存档 full113-ab-v20-summary.json）：86 项零失败，
  几何 1.034→1.067（+3.2pp，战役最大单跳）；≥1.0 从 52 → 67**；
  `<0.90` 收敛至 5 项（忽略族 4 + sphinx，且 sphinx 0.847→0.862）；
  gc_collect/telco 爬出劣化区；
- 广度：async 全族 +8~21%（协程机器高频新建函数对象）、pathlib
  +8.2%、networkx +7.0%、dulwich_log +6.7%；sqlalchemy_imperative
  1.074→**1.131**；richards 2.860/deltablue 1.606 同涨；
- 带外复核：html5lib 档值 −10.5% 复测 1.016，其历史带 0.99-1.12
  宽幅摆动，非机制回退；pprint 复测 1.262/regex_compile 1.128；
- 门禁：PGO 链、冒烟 14 件（新增 smoke_fresh_attach：预算内挂接
  语义+GC churn 回归+复用实例对照）、diffgate 0 新增、libtest
  26/46 仅既档、RCM 六组双模全等。

## 四、工程教训

- plainize 的 `grep -c error` 吞错第三次触发（本轮编译错致 md5
  不变被误读为"构建成功"）——改用显式 make + `error:` 行 grep；
- 宿主挂载（bind mount）的 mtime 传播延迟会骗过增量构建：改源后
  容器内 touch 再 make；
- 定价驱动（tools 化的 mid_best 形态）：伪 Runner 截获 + loops
  参数 best-of，比 steady_iters 量化精度高一个量级。

## 五、遗留

- 预算上限 8 为首版经验值，未做面板扫描（4/16/32 的敏感性未知）；
- html5lib 宽噪声带值得独立归因（±10%，疑 IO/解析器相位）；
- 体检遗留项复测：nqueens 1.005/sympy 1.006（生成器/恢复仪式
  残余，属 M6）；lm 扫描、kind-6、kwnames 缓存维持记档。
