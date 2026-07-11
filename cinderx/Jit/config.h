// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jit {

// Lifetime diagram of the JIT compiler:
//
//   NotInitialized <---------+
//        |                   |
//        v                   |
//     Running <---> Paused   |
//        |            |      |
//        v            |      |
//    Finalizing <-----+      |
//        |                   |
//        |                   |
//        +-------------------+
enum class State : uint8_t {
  kNotInitialized,
  kRunning,
  kPaused,
  kFinalizing,
};

enum class FrameMode : uint8_t {
  kNormal,
  kLightweight,
};

// List of HIR optimization passes to run.
struct HIROptimizations {
  bool begin_inlined_function_elim{true};
  bool builtin_load_method_elim{true};
  bool clean_cfg{true};
  bool dead_code_elim{true};
  bool dynamic_comparison_elim{true};
  bool float_accumulator_promotion{true};
  bool guard_type_removal{true};
  bool inliner{true};
  bool insert_update_prev_instr{true};
  bool list_prefix_reverse_assign{true};
  bool phi_elim{true};
  bool tree_iter_state_machine{true};
  bool primitive_box_remat{true};
  bool primitive_unbox_cse{true};
  bool float_comparison_simplification{true};
  bool simplify{true};
};

// List of LIR optimization passes to run.
struct LIROptimizations {
  bool inliner{true};
};

struct SimplifierConfig {
  // The maximum number of times the simplifier can process a function's CFG.
  size_t iteration_limit{100};
  // The maximum number of new blocks that can be added by the simplifier to a
  // function.
  size_t new_block_limit{1000};
};

struct GdbOptions {
  // Whether GDB support is enabled.
  bool supported{false};
  // Whether to write generated ELF objects to disk.
  bool write_elf_objects{false};
};

struct JitListOptions {
  // Name of the file loaded in as a JIT list.
  std::string filename;
  // Raise a Python error when a line fails to parse.
  bool error_on_parse{false};
  // Use line numbers or not when checking if a function is on a JIT list.
  bool match_line_numbers{false};
};

struct LogOptions {
  // Log general debug messages from the JIT.
  bool debug{false};
  // Log debug messages in the inlining pass.
  bool debug_inliner{false};
  // Log debug messages in the refcount insertion pass.
  bool debug_refcount{false};
  // Log debug messages in the register allocation pass.
  bool debug_regalloc{false};

  // Log HIR, before any passes are run.
  bool dump_hir_initial{false};
  // Log HIR after every pass is run.
  bool dump_hir_passes{false};
  // Log HIR after all passes have been run.
  bool dump_hir_final{false};

  // Log LIR, across all stages.
  bool dump_lir{false};
  // Show the originating HIR instruction for LIR instruction blocks.
  bool lir_origin{true};

  // Log disassembly of compiled functions.
  bool dump_asm{false};
  // Symbolize functions in disassembled call instructions.
  bool symbolize_funcs{true};

  // Log general JIT stats.
  bool dump_stats{false};

  // The file where to write logs to.
  FILE* output_file{stderr};
};

enum class AsmSyntax : uint8_t {
  ATT,
  Intel,
};

// Collection of configuration values for the JIT.
//
// Note: It's fine to store non-trivially destructible objects like std::string
// in this.  It is *not fine* to store Python objects in this because it has
// process lifetime and outlives the Python runtime.
struct Config {
  // Current lifetime state of the JIT.
  State state{State::kNotInitialized};
  // Ignore other CLI arguments and environment variables, force the JIT
  // to be initialized or uninitialized.  Intended for testing.
  std::optional<bool> force_init;
  FrameMode frame_mode{
// Lightweight frames are only implemented for 3.12+ frame layouts; on 3.11
// the materialized-frame model is the supported default (design decision
// D4: the LWF skeleton stays compiled in, the runtime default stays off).
#if defined(ENABLE_LIGHTWEIGHT_FRAMES) && PY_VERSION_HEX >= 0x030C0000
      FrameMode::kLightweight
#else
      FrameMode::kNormal
#endif
  };
  bool allow_jit_list_wildcards{false};
  bool compile_all_static_functions{false};
  bool multiple_code_sections{false};
  bool multithreaded_compile_test{false};
  bool use_huge_pages{true};
  // Use huge pages for cold code sections as well. Only applicable when
  // multiple_code_sections is enabled. Defaults to false (regular pages).
  bool cold_code_huge_pages{false};
  // Assume that data found in the Python frame is unchanged across function
  // calls.  This includes the code object, and the globals and builtins
  // dictionaries (but not their contents).
  bool stable_frame{true};
  // Use inline caches for attribute accesses.
  bool attr_caches{
#if PY_VERSION_HEX < 0x030B0000
      false
#elif defined(Py_GIL_DISABLED)
      // TODO(T250369692): FT support for inline-caches.
      false
#else
      true
#endif
  };
  // Collect stats information about attribute caches.
  bool collect_attr_cache_stats{false};
  // Use type annotations to create runtime checks.
  bool emit_type_annotation_guards{false};
  // Whether or not to JIT specialized opcodes or to fall back to their generic
  // counterparts.
  bool specialized_opcodes{true};
  // 属性/方法特化形（LOAD_ATTR_INSTANCE_VALUE/LOAD_METHOD_WITH_VALUES）
  // 的精确接收者类型投机。解释器缓存只有单次观测、无多态证据，多态
  // 受者（如 richards 的 Task 子类族）下守卫连环失败成 deopt 陷阱
  // （t=16 实测 richards 49→72 ms），故独立于 specialized_opcodes
  // 默认关；数值/比较/下标族的类型守卫收益（spectral_norm +21pp）
  // 不受本开关影响。
  bool specialized_attr_speculation{false};
  // [P6] 提前 quickening（3.11）：warmup 步进提为 4，第 2 个 warmup
  // 事件即特化，使低阈值 auto-JIT 编译读到成熟字节码（早产编译对策，
  // spectral 验尸轮）。
  bool early_quicken{true};
  // 守卫自适应去特化（3.11）：kGuardFailure 深度 deopt 按 code 计数，
  // 越限即卸载并以去特化输入重编（粘滞一次性）。为单次观测型特化
  // 守卫提供止损线：单态受者放胆投机，多态受者的 deopt 风暴被封顶
  //（raytrace t=4 风暴案）。
  bool adaptive_despec{true};
  size_t despec_deopt_threshold{64};
  // 异常 deopt 熔断:直线型 code 的 UnhandledException deopt 越限
  // 即冻结回解释器(见 code_extra.h exc_deopt_count 注释)。
  bool exc_deopt_fuse{true};
  size_t exc_deopt_fuse_threshold{8};
  // 同步生成器自动编译（3.11）。三态实测其编译净效应为负（interp
  // 86.0 ms vs JIT 96.6 ms，恢复仪式固定成本主导，微小生成器体收益
  // 归零；派发层合并实测墙钟中性——成本在本体与溅射恢复），默认不
  // 自动编译、回退解释器；协程/异步生成器不受此开关影响
  //（协程编译净效应 +13%，且 D6 本就禁编协程本体）。force_compile
  // 不受限。yield 点溅射瘦身落地后可重估。
  bool compile_sync_generators{false};
  // Enable OSR hot-loop detection. OSR is production-off by default and must
  // be explicitly enabled by -X osr-enabled or CINDERX_OSR_ENABLED.
  bool osr_enabled{false};
  // Set only by the early JIT initialization path when backedges can still be
  // routed to JUMP_BACKWARD_JIT consistently.
  bool osr_capable{false};
  // Number of executions of a single backedge before attempting OSR.
  uint32_t osr_backedge_threshold{2000};
  // Compile budget knobs consumed by later OSR feature items.
  uint32_t osr_compile_budget_code_units{1024};
  uint32_t osr_compile_warn_threshold_ms{50};

  // Only emit exact-int guards for specialized numeric opcodes in code objects
  // that contain a loop backedge. Disable to restore the old unconditional
  // guard behavior.
  bool backedge_gated_int_guards{true};

  // Support instrumentation (monitoring/tracing/profiling) by falling back to
  // the interpreter
  bool support_instrumentation{false};

  // Add RefineType instructions for Static Python values before they get
  // typechecked.  Enabled by default as HIR doesn't pass through Static Python
  // types very well right now.  Disable to expose new typing opportunities in
  // HIR.
  //
  // TASK(T195042385): Replace this with actual typing.
  bool refine_static_python{true};
  HIROptimizations hir_opts;
  LIROptimizations lir_opts;
  SimplifierConfig simplifier;
  // Limit on how much the inliner can inline.  The number here is internal to
  // the inliner, doesn't have any specific meaning, and can change as the
  // inliner's algorithm changes.
  size_t inliner_cost_limit{2000};
  // Number of workers to use for batch compilation, like in precompile_all().
  // If this number isn't configured then batch compilation will happen inline
  // on the calling thread.
  size_t batch_compile_workers{0};
  // When a function is being compiled, this is the maximum number of dependent
  // functions called by it that can be compiled along with it.
  size_t preload_dependent_limit{99};
  // Memory threshold after which we stop jitting.
  size_t max_code_size{0};
  // Size (in number of entries) of the LoadAttrCached and StoreAttrCached
  // inline caches used by the JIT.
  uint32_t attr_cache_size{4};
  std::optional<uint32_t> compile_after_n_calls;
  // 新鲜函数对象全簿记挂接的每 code 预算（挂接断链轮）：稳定小实例集
  // 全额受益，海量翻新闭包的挂接税由此封顶。0 关闭挂接。
  size_t fresh_attach_budget{8};
  // 进程级共享 attr 桩（桩共享轮）：la/lm/sa 内联桩由首个发射完整桩体
  // 的函数登记，其后函数以 8 字节级跳板复用——每函数桩开销 2-3KB →
  // 数十字节，宽热面负载的指令缓存足迹随之收敛。
  bool shared_attr_stubs{true};
  // 调用位点入口缓存（被调方行内压栈轮）：VectorCall 位点单态缓存
  // {func_code, vectorcall} 守卫对 + 快目标，命中经被调方直达入口
  // （自带 tracing/递归守卫）跳过 __code__ 校验与参数计数链。
  bool call_entry_cache{true};
  // Enable AutoJIT behavior classification for PYTHONJITAUTO=auto[:N]. Plain
  // numeric PYTHONJITAUTO and Python APIs keep this disabled.
  bool auto_classify{false};
  // Provider-before v1 keeps startup/import deferral disabled. This is only
  // for a later import-depth provider-backed slice.
  bool enable_startup_init_policy{false};
  // Minimal dynamic feedback for code that compiles successfully but then
  // repeatedly deopts. Enabled by default; disable with
  // CINDERX_AUTOJIT_ROI_BACKOFF=0 when isolating A/B or rolling back.
  // 自适应冻结层默认全关（基础优化时代决策）：冻结/卸载把病理形态
  // 从剖析视野中藏起来（witness 轮取证成本的直接来源）、给 A/B 引入
  // 冻结时机方差，且"过滤"不解决根因。deopt 风暴、helper 密度等病灶
  // 应直接暴露给 PMP/计数矩阵驱动根本修复；策略层留作收尾阶段按需
  // 启用的旋钮（CINDERX_AUTOJIT_ROI_BACKOFF / _IC_PRESSURE_RATIO /
  // _PROBATION）。
  bool roi_backoff_enabled{false};

  // 试用期计时判定（研究旋钮，默认关）：0 关闭；>0 为每臂样本数 K。
  // 实测结论：亚微秒级调用的计时噪声与"逐函数贪心 vs 全局混合成本"
  // 的错配使其在贴近均势的负载上净伤害（见 M9 probation 轮日志），
  // 生产判据用下方 IC 压力密度。
  size_t probation_calls{0};
  // 冻结判据余量（百分比）：jit 均时 > interp 均时 × pct/100 即冻结。
  size_t probation_margin_pct{125};

  // IC 压力密度冻结：每 4096 次调用为一窗，窗内 stub 慢路径进入数 /
  // 调用数 超过该比值即卸载冻结（0 关闭，默认关——见上方策略层
  // 决策注释）。判别量依据计数矩阵实测：go 型（天生物化+类默认值
  // 遮蔽）≈11/调用，richards/deltablue ≈1.5-1.7/调用，阈值 4 分离。
  size_t ic_pressure_ratio{0};
  size_t roi_deopt_budget_base{32};
  size_t roi_backoff_max_rounds{1};
  size_t roi_rewarm_factor{64};
  GdbOptions gdb;
  JitListOptions jit_list;
  LogOptions log;
  bool compile_perf_trampoline_prefork{false};
  bool dump_hir_stats{false};

  // The ASM syntax the JIT should use when disassembling.
  AsmSyntax asm_syntax{AsmSyntax::ATT};

  // List of function name patterns for which to capture compilation times.
  std::vector<std::string> capture_compilation_times_for;

  // Option to force compiled functions to be immortalized. By default a
  // CompiledFunction's timetime will be tied to a function via a reference put
  // in the function's __dict__. When we force CompiledFunction's to always be
  // immortalized no such reference will be created and the CompiledFunction
  // will be set to be immortal and never collected.
  bool immortalize_compiled_functions{false};

  // Use stable sentinel pointers in output (for deterministic test output).
  bool use_stable_pointers{false};

  // Delay adaptive specialization until a function has been called enough
  // times.
  bool delay_adaptive_code{false};
  // Number of calls before adaptive specialization kicks in.
  uint64_t adaptive_threshold{80};
};

// The JIT's config object. The accessors defined below are used in very hot
// paths in the JIT and need to be defined in a header to ensure that they are
// inlined reliably without LTO.
extern Config s_jit_config;

// Get the JIT's current config object.
inline const Config& getConfig() {
  return s_jit_config;
}

// Get the JIT's current config object with the intent of modifying it.
inline Config& getMutableConfig() {
  return s_jit_config;
}

// Check that the JIT is initialized.  Though it might be paused and or
// finalizing, it's not necessarily usable.
bool isJitInitialized();

// Check that the JIT is initialized and is currently usable.
bool isJitUsable();

// Check that the JIT is initialized but currently paused and unusable.
bool isJitPaused();

} // namespace jit
