# CinderX

CinderX 是一个 Python 运行时性能扩展，核心功能是将 Python 字节码即时编译（JIT）为原生机器码，从而显著提升 Python 程序的执行速度。本项目基于上游社区 CinderX 项目，专注于 Kunpeng & ARM 环境下的性能优化。

> English documentation: [README.md](README.md)

## 环境要求

- Python 3.14
- Linux (aarch64)，推荐 openEuler 24.03 (LTS-SP3)

## 兼容性

当前发布的 CinderX whl 包对各平台的兼容情况如下：

| CPython 版本 | aarch64 (ARM64) | x86_64 |
|---|---|---|
| 3.14.0 | 支持 | — |
| 3.14.1 | 支持 | — |
| 3.14.2 | 支持 | — |
| 3.14.3 | 支持（推荐） | — |

> **注意**：
> - 当前仅提供 aarch64 (ARM64) 架构的预编译 whl 包，x86_64 暂未提供预编译包。
> - 不支持 Python 3.13 及更早版本，不支持 Python 3.15 及以上版本。

## 安装

在 [openeuler releases](https://gitcode.com/openeuler/cinderx/releases) 获取最新 cinderx whl 包：

```bash
python3.14 -m pip install --no-index cinderx-*_aarch64.whl
```

## 特性概览

### 自动导入

CinderX 支持零代码侵入的自动导入机制。只需设置一个环境变量，CPython 启动时便会自动加载 CinderX 扩展，已有的 Python 项目无需任何改动即可享受 JIT 带来的性能加速。无论是遗留系统还是大型项目，都能以最低成本完成性能验证。

```bash
export CINDERX_PLUGIN_ENABLE=1
```

### JIT 编译器

**作用**：将 Python 字节码即时编译为原生机器码，显著提升热点函数的执行速度。JIT 会自动追踪函数调用频率，对频繁调用的函数进行编译优化。

**开启方式**：

通过环境变量控制 JIT：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITAUTO=N` | 启用自动 JIT 模式，函数调用 N 次后编译 |
| `PYTHONJITALL=1` | 编译所有函数（调用 0 次即编译） |
| `PYTHONJITDISABLE=1` | 禁用 JIT |
| `PYTHONJITLISTFILE=/path/to/list` | 通过 JIT 列表文件选择性编译指定函数 |

也可以通过修改业务代码使能 JIT 功能：

```python
import cinderx.jit

# 自动模式，自动追踪并编译热点函数
cinderx.jit.auto()

# 指定调用次数阈值，函数被调用 N 次后自动编译
cinderx.jit.compile_after_n_calls(10)

# 手动立即编译某个函数
cinderx.jit.force_compile(your_function)

# 标记某个函数在下次被调用时编译
cinderx.jit.lazy_compile(your_function)

# 暂停 JIT（用于调试或特定场景）
with cinderx.jit.pause():
    ...

# 禁用 JIT
cinderx.jit.disable()

# 重新启用 JIT
cinderx.jit.enable()
```

### 其他特性

以下特性为 Meta 上游社区内部生产环境的深度优化，本社区不做重点展开：

- **Static Python（静态 Python）**：利用类型标注在编译期生成更高效的字节码，配合 JIT 可实现接近 Cython 级别的性能。通过 `import __static__` 开启。
- **Strict Modules（严格模块）**：冻结模块类型与内容为不可变，消除常见开发错误。通过 `import __strict__` 开启。
- **并行垃圾回收（Parallel GC）**：多线程并行 GC，减少暂停时间。通过 `cinderx.enable_parallel_gc()` 或 `PARALLEL_GC_ENABLED=1` 开启。
- **缓存属性（Cached Properties）**：高性能 `cached_property` / `async_cached_property` 实现，通过 `from cinderx import cached_property` 使用。
- **对象不朽化（Immortalize）**：标记对象永不被 GC 回收，通过 `cinderx.immortalize_heap()` 开启。
- **自定义帧求值器（Frame Evaluator）**：替换 CPython 解释器循环以支持 Static Python 字节码，通过 `cinderx.install_frame_evaluator()` 开启。
- **轻量级帧（Lightweight Frames）**：更轻量的解释器帧实现，通过 `PYTHONJITLIGHTWEIGHTFRAME=1` 开启。
- **JIT 列表（JIT List）**：通过外部文件精确控制 JIT 编译范围，通过 `PYTHONJITLISTFILE=/path/to/list` 开启。
- **JIT 预加载（Preloading）**：编译前预先解析全局变量和类型描述符，为 JIT 内置步骤，通过 `PYTHONJITPRELOADDEPENDENTLIMIT` 等调整。
- **调试与性能分析**：提供 `PYTHONJITDEBUG`、`PYTHONJITDUMPHIR`、`PYTHONJITDUMPASM` 等环境变量用于 JIT 调试和汇编输出，详情参见 [cinderx/Docs/README_CN.md](cinderx/Docs/README_CN.md#调试与性能分析)。

## 更多文档

详细的编译指南、性能测试方法等请参阅子目录文档：

- [cinderx/Docs/README_CN.md](cinderx/Docs/README_CN.md) — 手动构建 CPython、性能测试、调试环境变量等详细操作指南
- [cinderx/ci_pipeline/README_CN.md](cinderx/ci_pipeline/README_CN.md) — GitCode 门禁、全量功能测试指南