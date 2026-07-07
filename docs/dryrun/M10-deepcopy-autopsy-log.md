# M10 第十五轮：deepcopy 产物验尸与 LOAD_GLOBAL_BUILTIN 守卫式装载

日期：2026-07-07　分支：`dryrun/m10-deepcopy-autopsy`　基线：
试用冻结阴性轮（!53 合入后，全集 v10 几何 0.982）

## 一、对象

温函数家族出路收敛为产物质量后，取 deepcopy（0.729，家族最劣）
做逐指令验尸：产物到底输给 PEP 659 什么。附带 attr 投机三审
（despec 上线后的复审——旧默认关决策是 pre-despec 的）。

## 二、attr 投机三审：维持默认关（新证据）

2×2 面板（PGO 交付构建）：deepcopy/pprint 完全持平（成熟字节码
+t=4 下投机仍无家族收益），richards 48.3→63.3（**−31%，且
adaptive despec 未熔断该风暴**）。两个结论：①默认关三审维持；
②despec 的保险面不覆盖 attr 投机风暴（其触发计数面/deopt 形态
与 BINARY_OP 族不同），despec"去特化重编"对该形态的失效机理
待正式版查明——在此之前 attr 投机不具备任何启用前提。

## 三、产物验尸（perfmap 符号化 + 优化后 HIR）

perf（jit@4，符号化）：编译体自身仅 copy:deepcopy 6.0% +
_deepcopy_dict 1.2% + _deepcopy_atomic 1.1%，其余全在协议机械——
调用协议合奏 ≈7.7%（JITRT_Vectorcall 3.2 + _Py_CheckFunctionResult
3.3+0.8(纯 PLT 存根) + JITRT_Call 1.5 + CompilationKey 哈希 0.9）、
默认参数补齐 helper（JITRT_CallWithIncorrectArgcount）1.5%、
JITRT_LoadGlobal 1.0%、PyDict_GetItemWithError 4.5%（memo/dispatch
真实工作 + 全局装载查找混合）。

copy:deepcopy 优化后 HIR 指令形清单：

| 损失源 | 数量 | 解释器对应形 | 产物现状 |
|---|---|---|---|
| VectorCall/CallMethod/CallEx | 19 | CALL_PY_EXACT_ARGS 行内压栈等 | 全部泛型 helper |
| LoadGlobal（泛型） | 9 | LOAD_GLOBAL_BUILTIN 行内特化 | 每次执行双字典查找 |
| UpdatePrevInstr | 33 | — | 帧簿记税（既档 ARM 项） |
| LoadAttr/LoadMethodCached | 3 | — | IC 轮战果，已缓存 |

关键发现：属性侧已被 IC 三轮修好；**输的是调用协议（19 处）与
全局装载（9 处）**。而 9 处泛型 LoadGlobal 的成因是既有 3.11
快路径（tryEmitLoadGlobalModuleValue311）**拒收 builtins 名**——
`globals[name]` 未命中即放弃，而 id/type/getattr/isinstance/len
这些最高频名字全部住在 builtins；解释器同位点走
LOAD_GLOBAL_BUILTIN 双版本守卫行内特化。

## 四、修复：LOAD_GLOBAL_BUILTIN 守卫式装载（3.11）

镜像 stock LOAD_GLOBAL_BUILTIN：模块层未命中时改钉双
keys_version——globals 版本钉"名字仍未被模块层遮蔽"（3.11 字典
键结构任何变更使 dk_version 失效），builtins 版本钉条目布局，
索引直读当前值（值级替换如 builtins.len=… 经活读天然可见，
与 stock 同构；编译期读全宽 dk_version，比 stock 缓存的 u16
截断更严）。实现三件：JITRT_LoadGlobalBuiltinValue311 helper +
builder 发射（tryEmitLoadGlobalBuiltinValue311，挂在模块形
未命中支路）+ 遮蔽/变异语义冒烟（smoke_load_global_builtin.py，
入冒烟套件）。

发射验证：copy:deepcopy 泛型 LoadGlobal 9→0（全部转为守卫式
builtin 装载）。同味 stash A/B（plain，进程内 best-of，ms）：

| 基准 | 修前 | 修后 | 增益 |
|---|---|---|---|
| pprint | 9.90-10.04 | 7.90-8.01 | **−20%** |
| deepcopy | 6.69-6.90 | 5.92-6.06 | **−11%** |
| richards（守护组） | 53.4-53.9 | 50.2-50.9 | **−6%（守护组同赢）** |

builtins 名无处不在，修复面全局——温函数家族与赢家组同吃。

## 五、门禁与全集（v11，B-only，与 v9/v10 同 A 侧参照）

门禁：PGO 交付链相四验收 OK；冒烟八件全过（含新增
smoke_load_global_builtin）；diffgate 全绿；refcount 六组零漂移；
libtest 仅 test_builtin/test_scope 两既有项。

全集 86 项（存档 full113-ab-v11-summary.json，崩溃零）：几何
**0.982 → 1.020（+3.8pp，战役首次越过持平线）**，<0.90 项
18→**7**（其中 4 项为既定忽略的短命进程族，实余 deepcopy 0.855 /
sphinx 0.855 / gc_collect 0.886），≥1.0 项 48。

增益精确落在 builtins 密集位点，温函数家族与赢家组同吃：

- 家族：pprint 0.838→**1.125（+28.7pp，转赢家）**、
  django_template →**1.009**、sqlglot_v2 →**1.022**、
  sqlglot_optimize →**1.005**、pickle_pure +13.2pp、
  unpickle +11.9pp、deepcopy +12.6pp、mako +10.0pp、
  docutils/sphinx 亦升；
- 赢家组：richards 2.264→**2.481（+21.7pp）**、richards_super
  2.377、hexiom 0.909→**1.083**、regex_compile →1.062、
  chaos →1.226、bpe_tokeniser →0.997；
- 唯一显著回退 asyncio_tcp_ssl −15.2pp（1.077）在其历史波动带
  （1.07-1.23）内且仍 >1.0。

## 六、遗留（M6 产物质量战役的输入清单）

- **调用协议 19 处泛型 VectorCall**：产物侧 CALL 行内化（镜像
  CALL_PY_EXACT_ARGS/WITH_DEFAULTS 的被调方守卫式压栈）为下一个
  产物级大项；CheckFunctionResult PLT 出线与 CompilationKey
  哈希查找为其中小件；
- 默认参数补齐 helper（JITRT_CallWithIncorrectArgcount）；
- UpdatePrevInstr 每指令簿记（既档 ARM 项）；
- despec 不覆盖 attr 投机风暴的机理；
- 950 真机复测积压（五轮）。
