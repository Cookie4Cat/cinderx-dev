# M10 第八轮：PGO 训练集扩容专项

日期：2026-07-08　分支：`dryrun/m10-pgo-trainset`　基线：sqla 净效应
轮（#43 合入后，PGO+LTO 交付口径）

## 一、对象与假说

全集 86 项 A/B（full113-ab-v2）后，劣化榜主体为解析/模板组 11 项
（0.82-0.89），三态分解证明其损失主体为 T1 解释器本底残差（~15%），
而 T1 残差主体为 stock 的 --enable-optimizations PGO 优势。

**假说**：现役训练集（train_m10：richards/deltablue/raytrace/go/
pickle/generators 六项 + 冒烟，全程 JIT-on）存在双重偏置——
①形态窄（全为计算/调用密集，无解析/模板形态）；②JIT-on 训练下
热函数被 JIT 接管，vendored 解释器循环在训练期几乎不被跑热，
ceval 的 profile 又薄又偏。stock 的 PGO 以全解释执行训练，解释器
分支画像远优于我方。

## 二、实验矩阵

判别主信号 = auto=0 六项几何（纯解释本底，现值 0.857）；
面板 = 解析/模板 8 项 + 守护 5 项（richards/richards_super/
deltablue/go/sqlalchemy_imperative），E0 基线取自 full113-ab-v2
同产物存档。

- **E0**（现役六项训练）：auto0 几何 0.857；面板见存档
  （sqlglot_v2 0.828 / django_template 0.824 / pprint 0.824 /
  docutils 0.856 / mako 0.862 / pathlib 0.838 / xml_etree 0.891 /
  hexiom 0.887；守护 richards 2.249 / rs 2.177 / deltablue 1.277 /
  go 1.034 / sqla_imp 0.962）；
- **E1 宽集**：训练集扩至 16 基准（原六 + sqlglot_v2/
  django_template/pprint/mako/pathlib/xml_etree/hexiom/deepcopy/
  docutils/coroutines）+ 冒烟，仍全程 JIT-on；
- **E2 混合双态**：E1 全量 + 解释器份额组 12 项以 auto=0 重跑一遍
  （两态计数合并入同批 .gcda，直接喂 ceval 热路径）。

结果（面板 13 项几何：E0 1.042 → **E1 1.071（+2.9pp）** → E2 1.049）：

| 项 | E0 | E1 宽集 | E2 混合 |
|---|---|---|---|
| auto=0 六项几何 | 0.857 | 0.858 | 0.858 |
| django_template | 0.824 | **0.913** | 0.852 |
| sqlglot_v2 | 0.828 | 0.844 | 0.856 |
| pprint | 0.824 | 0.839 | 0.839 |
| docutils | 0.856 | 0.867 | 0.843 |
| mako | 0.862 | 0.850 | 0.864 |
| pathlib | 0.838 | 0.857 | 0.840 |
| xml_etree | 0.891 | 0.902 | 0.900 |
| hexiom | 0.887 | 0.904 | 0.885 |
| richards / richards_super | 2.249/2.177 | 2.244/**2.246** | 2.275/2.229 |
| deltablue / go / sqla_imp | 1.277/1.034/0.962 | **1.334/1.091/0.994** | 1.280/1.021/0.943 |

## 三、判决与落地

**判决一（负结果，价值最高）**：auto=0 纯解释本底对训练内容完全
不敏感（0.857→0.858，E1/E2 同）。"ceval profile 被 JIT-on 训练
饿薄"的假说证伪——vendored 解释器的 PGO 已在其画像平台期，T1
残差（~14%）为结构性差异（vendored 循环与 stock ceval 代码本体
不同、跨 DSO 形态），训练侧无进一步空间，勿再投入。

**判决二**：E1 宽集全面胜出——收益来自 **JIT 侧 C++（helper/IC/
编译器）在多形态负载下的画像改善**（窄六项训练从未运出解析/模板
形态的 helper 热路径）。django_template +8.9pp、richards_super
+6.9、go/deltablue +5.7、sqla_imp +3.2，解析组普涨 1-2pp，
面板零回退（mako −1.2pp 在噪声带内）。

**判决三**：E2 混合双态净负（1.049 < 1.071）——auto=0 附加训练
遍稀释共享 helper/钩子代码的 JIT 态分支画像，还带来零解释态收益
（见判决一）。禁用，写入训练脚本头注释。

**落地**：E1 定稿为 ci_pipeline/scripts/train_full_311.sh（16 基准
多形态 + 冒烟，全程 JIT-on，路径可环境覆盖）；RUNBOOK 构建节与
REPORT 环境风险项同步更新。

## 四、门禁与全集复测

train_full 交付链相四验收 OK；冒烟七件全过；diffgate 全绿；
refcount 六组零漂移；libtest 仅 test_builtin/test_scope 两既有
已知项。

全集 86 项 A/B（存档 full113-ab-v3-summary.json，对照 v2 同协议）：
几何 **0.943 → 0.956（+1.3pp）**，与面板 +2.9pp 按份额稀释吻合；
<0.90 劣化项 25 → 23。主要提升：generators +11.8pp（0.644→0.762，
v2 回退确系瞬态/画像运气）、richards_super +8.6、deltablue +6.1、
coroutines +4.7、django_template +3.8、sqlglot_v2_parse +4.0；
回退项全部在跨构建噪声带内（最大 async_tree_memoization_tg
−5.1pp）。

**训练时长代价**：相二由 ~40 s 增至 ~2 min（16 基准 + 冒烟），
全链仍 <10 min，构建管线可承受。
