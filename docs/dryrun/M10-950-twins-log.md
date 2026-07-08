# M10 第十八轮：950 双靶（deepcopy_reduce / sqlalchemy_imperative）
# 与全局装载行内化

日期：2026-07-08　分支：`dryrun/m10-950-twins`　基线：五连栈
（v13 几何 1.030）。950 真机回报两项各差约 5% 达标。

## 一、覆盖面发现：子基准混叠

deepcopy_reduce/deepcopy_memo 是 bm_deepcopy 的**子基准**（pyperf
同 json 内三条名目），而 run_ab_full113 的 mean_of 把全部子项均值
混为"deepcopy"一个数——reduce 从未被单独观测。本机拆测：
deepcopy 0.925 / **deepcopy_reduce 0.890** / deepcopy_memo 1.008
（快协议）——reduce 的缺口为机器无关，可就地验尸。

sqlalchemy_imperative 本机 **1.018**（快协议）——缺口为 950
特有，本机无从验尸，见第四节判别清单。

## 二、deepcopy_reduce 验尸

perfmap 剖面：copy:deepcopy 8.4%（编译体）+ 解释残留 3.5% +
**JITRT_LoadGlobalBuiltinValue311 helper 自身 2.7%** + 缺省补齐
1.8% + CheckFunctionResult 2.0%。头号可动杠杆：LGB 轮的守卫式
装载走的是 CallStatic 进 C helper——检查逻辑与解释器行内特化
同构，但每次装载付 C 往返；builtins 名是最高频装载，税按调用
线性放大。

## 三、修复：全局装载行内化（builtins 形 + module 形同构）

helper 调用降级为纯 HIR 行内序列：

- **守卫**：LoadField(ma_keys) → LoadField(dk_version, u32) →
  PrimitiveCompare(eq, 编译期版本常量) → Guard（kNotZero 语义，
  失败即 deopt 回解释器重执行本条 LOAD_GLOBAL）；builtins 形双
  版本（globals 版本钉"未被模块层遮蔽"+ builtins 版本钉布局），
  module 形单版本；
- **装载**：dk_version 全局单调分配，版本命中钉死键结构与条目
  布局——条目值槽偏移为**编译期常量**，自 ma_keys 裸偏移活读
  （值级替换不改版本，经活读天然可见，与 stock 同构）；builtins
  形值经版本钉定非空免判空，module 形保留非空 Guard（同键覆写
  可置洞）；装载后 Incref 交付新引用；
- 原两个 C helper（JITRT_LoadGlobalModuleValue/
  JITRT_LoadGlobalBuiltinValue311）移除。

判据（plain 同味 stash A/B，进程内 best-of，ms）：

| 基准 | 修前 | 修后 | 增益 |
|---|---|---|---|
| deepcopy_reduce | 3.43-3.60 | 3.25-3.40 | **−3%** |
| pprint | 7.82 | 7.30 | **−7%** |
| richards | 44.1-44.8 | 41.9-42.2 | **−5.5%** |
| deepcopy | 5.71-5.88 | 5.64-5.65 | −2% |

builtins/module 名装载无处不在，修复面全局。语义面：遮蔽/变异
冒烟（smoke_load_global_builtin）原样通过；exc_inject_fuzz 64 发
0 崩（本轮新增两类 Guard，异常面重点）；冒烟九件全过；diffgate
940 例 0 新增（4 转好）。

## 四、sqlalchemy_imperative：950 特有缺口的判别清单

本机 1.018，缺口不在机器无关层。结合旧账（该基准历史归因残余
＝"异常 deopts + 过早特化"，后者已被 despec 化解），950 建议
判别序：

1. `CINDERX_ADAPTIVE_DESPEC=0` 对照——若显著变化，特化守卫
   相关；
2. 巨阈值武装态（PYTHONJITAUTO=100000000）对照——分离解释器面
   与编译净效应；
3. perf record 对照 stock（950 有 PMU，可看 cache-miss/分支
   预测差异——deopt 物化与异常路径在弱乱序核的放大是首要嫌疑）；
4. 本轮全局装载行内化落地后先复测——每装载省一次 C 往返在 950
   上的兑现预计大于本机。

## 五、遗留

deepcopy_reduce 残余：解释残留 3.5%（reduce 协议的 C/解释混合段）
与 CheckFunctionResult 出线（libpython 内部支付）；950 复测积压
（本轮起七轮）。
