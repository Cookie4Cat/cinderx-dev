// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Vendored-interpreter wrapper TU for CPython 3.11 (design decision D3).
//
// Everything under ceval/ is byte-identical to upstream CPython v3.11.6
// (the openEuler 24.03 anchor per the D7 machine verdict) and is locked by
// the source-hash gate; those files are never edited. Every deviation
// required to build the loop inside CinderX lives HERE, one macro per
// deviation, each with a rationale. This file is the "patch list" that
// review approves.

// [P1] Entry-point rename. CinderX installs its own frame evaluator via
// PEP 523 (Ci_EvalFrame); the vendored loop gets a Ci_ name so calls are
// explicit and cannot be confused with libpython's exported
// _PyEval_EvalFrameDefault. All in-file recursive references follow the
// rename, so once a frame enters the vendored loop it stays in it.
#define _PyEval_EvalFrameDefault Ci_EvalFrameDefault_311

// [P2] PEP 509 dict version counter redirect. The vendored
// STORE_ATTR_INSTANCE_VALUE handler stamps ma_version_tag from
// _pydict_global_version, which static-libpython runtimes (manylinux) do
// not export. Defining our own zero-based counter would alias tags already
// handed out by the runtime's counter and could make version guards see a
// mutated dict as unchanged. Instead the macro redirects to a shadow
// counter that Ci_InitOpcodes seeds to <runtime's current value> + 2^40
// (read back via a probe dict), so the two allocators cannot collide in any
// realistic process lifetime. On the openEuler target (shared libpython,
// symbol exported) the formal port links the real counter directly and
// this patch disappears. (The shadow variable itself is defined in
// cinderx_ceval_shims.c; pycore_dict.h's extern declaration follows the
// rename.)
#define _pydict_global_version ci_pydict_global_version_shadow

// [P3]（源级补丁，ceval.c start_frame 处）auto-JIT 帧压栈计数。3.11 无
// code watcher，计数式 auto 的每 code 调用计数改在解释循环帧压栈点完成
// ——该点同时覆盖 vectorcall 入口与特化 CALL 的内联压栈，是 3.11 上
// 唯一能数全所有解释执行的位置。钩子实现位于 pyjit.cpp（CodeExtra 按需
// 分配 + 阈值到达即编译）。P1/P2 为宏级补丁、基座源逐字不动；P3/P4 为
// 首两个源级补丁：宏无法改写结构体字段访问与 opcode 处理器内部语句，
// 补丁位点以 [P3]/[P4] 注释标记，哈希锁同步更新（正式移植转独立
// .patch 文件走 D3 补丁台账评审）。
struct _ts;
struct _PyInterpreterFrame;
extern void Ci_AutoJitCountFramePush311(
    struct _ts* tstate,
    struct _PyInterpreterFrame* frame);

// [P4]（源级补丁，CALL_PY_EXACT_ARGS / CALL_PY_WITH_DEFAULTS）特化调用
// 内联压栈的守卫由"装了自定义求值器就一刀切 DEOPT"改为按被调方判定：
// 仅当被调方仍为解释器默认入口时内联压栈，编译入口/包装入口经通用调用
// 路径进入 JIT。原 stock 检查服务于任意第三方求值器必须看见每个帧的
// PEP 523 语义；本端口的求值器就是本循环自身，内联压栈的帧同样在本
// 循环内执行，语义等价，而一刀切 DEOPT 使所有解释器间调用退化为通用
// 路径（M9 首轮实测的主要结构税之一）。Ci_StockEntry311 为解释器默认
// vectorcall 入口的 void* 镜像（跨 TU 函数指针类型摩擦规避），由 JIT
// initialize() 赋值；未初始化时为空指针，比较恒不相等，行为退回一刀切
// DEOPT（fail-safe）。
extern void* Ci_StockEntry311;

// [P5]（源级补丁，WITH_EXCEPT_START）traceback 引用持有跨越 __exit__
// 调用。上游取得 traceback 后立即归还新引用、把借用指针放进实参窗口：
// 解释器被调方入帧即 incref 实参，借用窗口极短，stock 安全；JIT 被调方
// 遵循"实参借用自调用方数组"约定全程不 incref，一旦 __exit__ 体内剥离
// exc 的最后一个 traceback 引用（unittest _AssertRaisesContext.__exit__
// 的 with_traceback(None)），借用寄存器当场悬垂，其后任意 deopt 物化会
// 对尸体 incref/decref（M9 全表面 SEGV 四案：test_builtin/list/tuple/
// exceptions 共同根因）。补丁把 Py_XDECREF 移至调用之后，引用持有跨越
// 调用，用户可见语义不变。注意：这是"调用方借用实参窗口 vs JIT 借用
// 约定"这一普遍健全性缺口的定点封堵，普遍解（如物化帧入口 incref
// 实参并计量其调用开销）移交正式 M5/M6 决策。

// [P6]（源级补丁，RESUME / JUMP_BACKWARD）提前 quickening。HIR 前端
// 消费自适应字节码，而 stock 的 QUICKENING_WARMUP_DELAY=8 使 auto-JIT
// 低阈值编译恒读未特化码流（spectral 验尸轮：同函数冷编译 114 ms vs
// 熟后编译 92 ms）。补丁把 warmup 步进从 1 提为 Ci_QuickenWarmupStep_311
//（JIT initialize() 按 jit-early-quicken 旗标置 4，即第 2 个 warmup
// 事件——第 2 次进入或首个回边——即 quicken；未初始化时为 1，行为
// 与 stock 逐字等价）。run-once 无循环代码仍永不付 quicken 税；带
// 循环的 code 至迟在首调内成熟，使 auto=2 也能读到特化码流。
extern int Ci_QuickenWarmupStep_311;
#include "ceval/ceval.c"
