# M10 生成器体量门轮:同步生成器按码元数分型自动编译

分支 dryrun/m10-sa-kind6 追加提交 46ef06d2b(与残项②③同枝)。

## 一、证据链

落后带验尸稳态 attach 剖面:genshi 在 auto=4 下 37.1% 时间仍在
解释器——模板流水线生成器不入编译面(gen 轮旧判决:恢复仪式使
编译净负)。旗标试探 genshi −10.7%,判决翻案条件成立:七轮调用/
入口/IC 优化已侵蚀恢复仪式代价,大体量生成器编译转净正;但
generators 基准(紧 yield 琐碎体)全编仍 +58% 净负——形态二分,
一刀切两个方向都错。

体量分布可分:generators 基准生成器 56 码元,genshi 热生成器
288-945 码元(21 个中位 288)。阈值取 128。

## 二、实现

ci_autoJit311AllowsCode 的同步生成器闸改体量门:码元数
(Py_SIZE(code))≥ sync_gen_min_units(默认 128)入编译面;
0=关闭体量门(回旧行为);jit-compile-sync-generators=true 维持
研究口径全编。旗标 jit-sync-gen-min-units /
PYTHONJITSYNCGENMINUNITS。

## 三、验证与定价

- 交错三档面板(0/128/64 双轮):generators/nqueens 零回归,
  genshi −6.5%,html5lib −3.5%;64 档仅多 0.5% 但对 56 码元
  琐碎类安全边际减半,取 128;
- genshi 稳态解释器份额 37.1%→19.5%(jitgenSendImplInternal
  2.6% 为新增恢复机器成本,净赚);
- 门禁:冒烟 16/16,libtest 46 模块 0 新增(2 转好),PGO 链
  验收 OK;
- **全集 v25(存档 full113-ab-v25-summary.json):86 项零失败,
  几何 1.046→1.053(+0.7pp),≥1.0 项 59→64**。genshi 0.944→
  **1.071(转正)**,nqueens 1.006→1.043,async_tree_io/
  memoization 各 +3.5pp,generators 1.108→1.124(无害),
  pathlib 0.93→0.959;mako/comprehensions 中性,sqlglot −3pp
  属其历史噪声带。

## 四、附带修正(记忆同步)

roi_backoff_enabled 默认关且不随 AUTO 启用——!80 的异常冻结支线
在交付口径下 inert;argparse_subparsers 短窗 +23% 复盘为 mid_best
窗口被模块编译期污染的假象,pyperf 真口径 0.96 残差归温函数结构
族。教训:含大模块导入编译面的用例不可用短窗 mid_best 定价。

## 五、遗留

sphinx 0.90(温函数结构族,既判)、mako 0.96(时间不在生成器)、
2to3/startup(短命进程税,provider 分支)、la kind-7 残余/per-entry
lm hint(各 ≤0.2pp);RCM roi 漂移老账归因须重审(roi 关态下仍在,
exc-fuse/probation 迁移嫌疑)。
