# M7 预演日志：内联缓存失效正确性（D5 版本号守卫拉式验证）

日期：2026-07-04　分支：`dryrun/m7-ic`　基线：`dryrun-311-base@f0c1517b6`（M6 合入后）

验收语料（既定）：diffgate 终态 9 失败（attr 8 + descriptor 1）+
ic_mutation 确定性崩溃案（`CI_EXC_INJECT=41`，M6 注入 fuzz 产出）。

## 一、考古结论：失效机制的 3.11 现状

kunpeng 的四类 IC 失效全部依赖 3.12+ watcher 回调（`PyType_Watch` /
`PyDict_Watch` → `typeChanged()` 系列）。3.11 的 watcher API 是 M1 期
编译垫层里的**静默成功空桩**（cinderx/python.h，返回 0 不做事）——缓存
注册"成功"但变异事件永不送达，缓存永不失效。stale（attr 8 失败）与
UAF（ic_mutation 崩溃：条目内 `BorrowedRef descr/value` 悬垂）同根。

**已是拉式、无需改造的部分**（考古中确认，修正了任务预设）：
- LOAD_GLOBAL：3.11 走专用路径 `tryEmitLoadGlobalModuleValue311` →
  `JITRT_LoadGlobalModuleValue(globals, name, keys_version, index)`，
  运行时 dk_version 校验 + 值活读 + Guard deopt——del global 后旧值
  不可见（出口③）已由该路径与语料（case_global_shadows_builtin_then_del
  等）覆盖；watcher 推式的 GlobalCache 间接槽在 3.11 不可达。
- 模块属性缓存（LoadModuleAttrCache / LoadModuleMethodCache）：
  `< 0x030E` 分支本就以模块 `__dict__` 版本号拉式验证。

**真正依赖死 watcher 的四处**：AttributeCache（LoadAttrCached /
StoreAttrCached 共用）、LoadMethodCache、LoadTypeAttrCache、
LoadTypeMethodCache。

## 二、D5 落地：tp_version_tag 拉式守卫

3.11 的 `tp_version_tag` 即 stock 特化器的守卫机制：任何类变异经
`PyType_Modified` 清除 `Py_TPFLAGS_VALID_VERSION_TAG`（子类递归），
`_PyType_Lookup` 重新赋号。fill 侧的先决条件
（`Ci_Type_HasValidVersionTag` / `ensureVersionTag`）在 kunpeng 中本已
存在（watcher 方案同样需要）——缺的只是**记录与命中校验**两半：

1. **记录**：`AttributeMutator::set_type`、`LoadMethodCache::fill`、
   `LoadTypeAttrCache::fill`、`LoadTypeMethodCache::fill` 记录填充时的
   `tp_version_tag`（`< 0x030C` 门内新增字段）。
2. **命中校验**：
   - `AttributeMutator::matches(tp)`：类型指针 + VALID_VERSION_TAG 标志
     + 版本号三重校验；Load/Store `doInvoke` 命中循环改经它，**校验通过
     前不解引用条目内任何借引用（D9）**；指针同而版本异的条目（版本号
     单调，不可能再命中）当场清空释放槽位。
   - `LoadMethodCache::lookup`：命中前同款校验，失效条目当场清空。
   - **类型接收者两缓存**（LoadTypeAttrCache / LoadTypeMethodCache）的
     内联 `[type, value]` 槽对无法拉式验证版本，3.11 不发射内联快路径
     （simplify.cpp 两处 `< 0x030C` 直接发射 Fill helper），命中判定
     （含版本校验）移入 `invoke()` / `lookup()` 头部。3.12+ 内联路径
     原样保留。

改动足迹：3 文件 +118 行（inline_cache.h/.cpp、hir/simplify.cpp），
全部在 `< 0x030C` 版本门内。

## 三、验收结果

| 验收项 | 结果 |
|---|---|
| diffgate 918 用例 × 两模式 | **0 失败（基线 9 项全部修复，0 新增，0 infra 错误）——语料自穿刺时代（75 失败）以来首次全绿** |
| ic_mutation 确定性崩溃案（CI_EXC_INJECT=41） | 销案：整模块跑完 rc=0 |
| ic_mutation 全检查点注入扫荡 | 56/56 零崩溃（修复前 3 崩） |
| refcount 矩阵（ic_mutation + calls，interp/jit，N=200） | 四组全部 0 漂移 |
| 类变异可见性冒烟 | 类变量重赋值 / 方法替换 / del 类属性→AttributeError / `__class__` 重绑定，全部立即可见 |
| 3.14 反向回归 | 编译 0 错 + JIT 冒烟通过（3.14 watcher 路径原样） |

预演途中一处返工：simplify.cpp 尾部 `#endif` 编排错误产生重复
`});`——位于 `>= 0x030C` 分支内，3.11 预处理不可见故 3.11 全绿，被
3.14 反向编译抓获后修正。双版本编译门禁的既定价值再次得到验证。

## 四、移交与残余（正式 M7 范围）

- **守卫粒度**：数据描述符缓存（DataDescrMutator）仅守卫接收者类型的
  版本号；描述符**自身类**被变异（如删除 `__set__` 使其降级为非数据
  描述符）不触及接收者版本号，3.12+ 由 ac_descr_watcher 覆盖，3.11
  预演版存在此粒度缺口（与 stock 3.11 特化器同等暴露面）。正式开发
  可对 descr_type 增加第二重版本记录。
- **变异套件**：设计书 M7 要求含 `__eq__`/`__del__` 再入用例的完整
  变异矩阵；预演以 ic_mutation（15 用例）+ 注入扫荡代之，再入维度
  未覆盖。
- **ASAN 门禁**：设计书出口②要求 ASAN 重跑；预演容器未插桩，以
  全检查点注入扫荡 + refcount 矩阵代之。
- 版本号回绕（uint32 全局计数器）在长驻进程中的理论风险照抄 stock
  结论（不处理）。

预演工时：约 1.5 小时。
