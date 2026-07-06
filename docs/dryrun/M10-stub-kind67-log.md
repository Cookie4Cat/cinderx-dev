# M10 第四轮：la 桩 kind-6/7 内联与枚举元类型放行（C3）

日期：2026-07-06　分支：`dryrun/m10-ic-fastpath` 续　基线：IC 快路径
轮（MR #39）

## 一、直方图翻案（先于实现的关键修正）

归因轮把 sqlglot 540 万/值 helper 往返记在 kind-7 名下——实为打印
模板只列 kind 2/3/7 的口径盲区。全直方图揭示真相：**kind-6
（kMemberDescr，__slots__ 属性）434 万/值才是大头**，kind-7 仅 41
万，另有 kind-5（数据描述符/property）8.4 万。sqlglot Expression
全 __slots__，args/arg_types 皆成员描述符。
**教训：直方图打印必须全量列桶，选择性打印会把归因带偏一个量级。**

## 二、实现（gen_asm la 桩支线 + C1 门放宽）

kind 门 `b.hi` 由直落 slow_path 改指支线，kind 2/3 主路径指令序
不变（richards 无回退红线）：

- **kind-6 内联**（8 指令）：entry.memberdef → 校验成员类型为
  T_OBJECT_EX（其余成员类型的转换语义留 PyMember_GetOne）→
  offset → 对象体直读；空槽（未赋值 slot）回落 helper 抛
  AttributeError。
- **kind-7 三形**：①非 managed-dict 且 tp_dictoffset==0
  （__slots__ 类读类变量）：无实例字典即无遮蔽可能，直返 descr；
  ②keys_version 快形（名字不在共享键，dk 版本未动即无遮蔽）；
  ③mat_hint 探测形（共享键内，me_key 自验证，values[hint] 非空即
  遮蔽值、空槽返回 descr）。getter 描述符（数据/非数据）一律留
  helper 保完整定序；物化实例回落 helper 带 hint 物化读。
- **C1 元类型门放宽**：带 __getattr__ 的元类型（slot hook，即
  EnumType——枚举成员访问 TokenType.X 形态）命中侧语义可复刻
  （hook 前半即 type_getattro，类 dict 命中不进 __getattr__），
  la 类型受者缓存 miss 天然回落完整协议、无抛错路径，放行填充。

排障实录三案：

**案一（本轮自查）**：kind-7 首版对非 managed-dict 接收者直读预头
-32 字节（helper 侧有 Py_TPFLAGS_MANAGED_DICT 标志门未镜像）——
对象分配块外越界读，普通运行不显形、ASAN 必报。走查即改（标志门 +
无字典直返形一并落位）。**红线重申：凡旗标/形态门内读版本或形态
特定内存布局，镜像 helper 时必须连门一起镜像**（与 M9 幻影帧头案
同根）。

**案二（基线潜伏缺陷，PGO 链暴露）**：C2 探针（MR #39 已合入）的
惰性初始化在"异常传播中途被首次触发"时清掉在途异常——探针建类
执行 Python 代码（PyRun_String），失败路径 PyErr_Clear 无差别清
异常，NULL 无异常上浮成 SystemError，PGO 构建下 import enum 途中
EnumType.__getattr__ 的 AttributeError 传播链首触发即全面炸裂
（冒烟五件、diffgate 26 模块、16 项 A/B 全红），plain 构建则因首
调时机不同全绿——**首调时机敏感的惰性初始化是编译布局的骰子**。
双修：初始化器全程 PyErr_Fetch/Restore 保护在途异常（纵深防御）+
jit::initialize() 期显式预热消灭惰性窗口（根治）。
**红线：任何会执行 Python 代码或触碰异常状态的惰性初始化，要么
挪到确定无在途异常的 init 期，要么全程 Fetch/Restore；"首次调用
点无异常"不是可以依赖的假设。**

**案三（构建期薛定谔：profile 依赖的 PGO-use 相错译）**：案二修复
后同签名复发，逐层排除——IC 缓存全关仍炸（本轮运行时代码无罪）、
单函数 jit-list 复现（只编 EnumType.__getattr__ 即触发编译码 raise
丢异常）、构建配置拆轴（plain/纯 LTO/插桩相全绿，仅 PGO-use 相
炸）、洁净重建两次同流程一红一绿——**坐实训练 profile 内容的非
确定性（pyperf 校准/时序抖动改变 gcda）会诱发相三错译**，真凶为
既有 raise/deopt 路径潜伏 UB 或 GCC-14 PGO×LTO 工具链交互，本轮
代码变更只是扰动了骰子。处置：①构建配方新增**相四产物正确性验收
步**（快速探针，红即归档 gcda+.so 取证并整链报错）——"薛定谔构建"
从此不可能溜进交付物，亦即 SR3 构建体系的必备一环提前落地；②红态
取证与 profile 目录级二分（TU→函数→UB 定位）立工具链交互专项。
**红线：PGO 产物必须过构建后正确性验收，"编译成功"不等于"编译
正确"；训练非确定性使该风险恒在，验收步是唯一系统性防线。**

**案三续（探针漏检 + C3 清白定性）**：相四首版轻探针
（import enum/re/dataclasses）会漏——同一红态构建 enum 探针过、
test_slice 的 raise ValueError 仍炸（错译命中面随 profile 变动，
非固定函数）。据此两点定论：①探针面已扩至 raise 密集 stdlib 模块
（test_slice/exceptions/raise/builtin/int），但**完整正确性仍须
靠交付构建过完整 diffgate+libtest 快速档**，相四只拦最粗红态；
②**决定性对照：纯 LTO（无 PGO）构建下 test_slice SUCCESS、libtest
仅既有基线两项——C3 桩代码与探针加固完全清白，PGO 错译是独立的
工具链缺陷**。C3 的可交付性不依赖 PGO 相修复。

## 三、机械指标（sqlglot_v2 稳态差分，修复链四点）

| 阶段 | la helper 进入/值 | helper 占桩入口比 |
|---|---|---|
| C3 前 | 534 万 | 97% |
| +kind-6 | 100 万 | 18% |
| +kind-7 无字典形 | 62 万 | 11% |
| +枚举元类型放行 | ~62 万（la_slow 回落至本底 8.6 万，EnumType 站点清零） | 11% |

## 四、墙钟判决（诚实记录）

普通构建定向 A/B：sqlglot_v2 0.805（C1/C2 轮同构建 0.799，噪声带
内）、pprint/docutils/deepcopy 持平、守护组无回退。**helper 出线量
削减 97%→11% 未转化为墙钟收益**——与 C1/C2 轮 sqlalchemy 结论同族：
现代乱序核对"预测良好的短调用往返"吞吐几乎免费，**计数占比不能
外推为墙钟占比**（PMP 早有提示：C 叶帧仅 12~13%）。IC 慢路径系列
（C1/C2/C3）至此完成机械清障；这些形态剩余劣化的主体坐实为
T1/T2 解释器侧税与编译产物本体质量，IC 侧不再是一等杠杆。

（终态 PGO 门禁与 A/B 数据回填于下节。）

## 五、门禁与量化

**交付口径 = 纯 LTO 干净构建**（决策：PGO-use 相概率性错译独立立项，
C3 不被工具链问题拖住；见第二节案三与 REPORT 第六节）。

门禁（纯 LTO 构建，md5 8203d550）：冒烟七件全过（新增
smoke_ic_kind7：快形/探测形/后置遮蔽/删除回落/property 与方法留
helper/__slots__ 空槽抛错/多态全边界）；diffgate 923 全绿；refcount
六组零漂移；libtest 相对基线无新增分歧（test_slice SUCCESS——即
PGO 红态的漏检项在纯 LTO 干净构建下正常，反证 C3 清白）；3.14
反向编译 + 两 smoke 过。

定向 A/B（纯 LTO 口径，存档 m10c3-lto-ab-summary.json；注意绝对值
较历史 PGO 口径整体低约 3pp，跨口径不可直接比）：

- sqlglot_v2 0.811 / sqlglot_v2_optimize 0.809 / pprint 0.809 /
  xdsl 0.819 / docutils 0.754 / deepcopy 0.708——与 C1/C2 轮同形态
  在各自口径的噪声带内，**墙钟中性**；
- 守护组无回退：richards 2.147 / richards_super 2.119 / deltablue
  1.263（纯 LTO 平移后正常）/ raytrace 0.899（纯 LTO 漂移带内，
  接近持平线波动大）。

墙钟中性与机械清障（sqlglot helper 出线 97%→11%）的落差结论同
C1/C2 轮：现代乱序核对预测良好的短调用往返近乎免费，计数占比不
外推为墙钟。IC 慢路径系列（C1/C2/C3）自此完成机械清障，剩余劣化
主体坐实 T1/T2 解释器侧税与产物本体质量。

## 六、遗留与判决更新

- C4（lm ia_ 桩路径，unpickle 230 万/值）：鉴于本轮墙钟判决，
  预期收益下调至 1~2pp 级，优先级降后；
- 归因单剩余高价值项重排：T1 vendored 本底构建专项（全组 5pp+
  潜力）> T2 钩子链内联 > sqlalchemy perf record 专项 > C4/C5；
- 站点直方图工具修正：diffstats 打印模板改为全桶列示。
