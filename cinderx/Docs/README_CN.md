# CinderX 详细文档

本文档包含 CinderX 的详细编译、测试、性能分析和调试指南，面向需要深入使用或贡献 CinderX 的开发者。

## 目录

- [开发者构建说明](#开发者构建说明)
- [性能测试](#性能测试)
- [调试与性能分析](#调试与性能分析)


---

## 开发者构建说明

CinderX 的 CMake 构建会通过 `FetchContent` 获取部分第三方依赖。为了减少构建阶段对 GitHub 网络的依赖，可以通过本地依赖缓存目录复用这些源码。启用后，构建会优先校验缓存中的依赖；依赖缺失、远端不匹配或版本不正确时，会刷新对应依赖子目录。

当前纳入本地缓存的依赖包括：

| 依赖 | 版本 |
|---|---|
| `fmt` | `11.2.0` |
| `parallel-hashmap` | `896f1a03e429c45d9fe9638e892fc1da73befadd` |
| `usdt` | `f4ea2f524efa80d062f4d586d78daafb83dc7d24` |
| `capstone` | `5.0.7`，仅在启用 disassembler 时需要 |
| `googletest` | `v1.17.0`，仅在构建 `runtime_tests` 时需要 |

`zlib` 不纳入本地缓存，保持原有 CMake 查找和获取方式。

### 使用 `setup.py` 构建

直接使用 `setup.py` 时，可以通过 `--local` 指定缓存目录：

```bash
python3.14 setup.py build_ext --local=/path/to/cinderx-local-deps
python3.14 setup.py build --local=/path/to/cinderx-local-deps
```

### 使用 `pip` 构建

`pip` 无法直接传递 `--local`，需要使用环境变量：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.14 -m pip install .
```

### 本地 gate 默认路径

`ci_pipeline` 的本地构建 suite 默认使用源码仓平行路径下的缓存目录：

```text
<repo_path>/../cinderx-local-deps
```

运行下面的本地门禁时会自动使用该路径：

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

---

## 性能测试

推荐使用 [pyperformance](https://github.com/python/pyperformance) 进行基准性能测试。pyperformance 是 Python 官方认可的基准测试套件，覆盖 JSON 序列化、正则匹配、加密运算、迭代器等多种场景，能够全面评估 CinderX 带来的性能提升。
性能测试需要基于自行编译的 CPython 进行，以确保基线一致和访问内部 API正常。CinderX依据自动导入功能， 替换CPython原生解释器，从而可借用pyperformance测试套件进行CinderX的性能测试。


以下步骤从源码构建 CPython 开始，逐步完成测试环境搭建。

### 手动构建 CPython

CinderX 当前支持 CPython 3.14 的部分小版本（3.14.0 - 3.14.3）。

#### 获取 CPython 源码

```bash
git clone https://github.com/python/cpython.git
cd cpython
git checkout v3.14.3   # 或其它的 3.14.x 版本
```

#### 配置构建参数

运行 `configure` 时，建议使用以下参数：

```bash
./configure \
    --enable-optimizations \
    --with-lto \
    --prefix=/usr/local/python3.14
```

关键参数说明：

| 参数 | 作用 |
|---|---|
| `--enable-optimizations` | 启用 PGO（Profile-Guided Optimization）优化，显著提升 CPython 自身性能。构建时间较长，但性能测试中对比基线必不可少 |
| `--with-lto` | 启用 LTO 优化，显著提升 CPython 自身性能 |
| `--prefix` | 指定安装路径，避免覆盖系统自带 Python |

#### 编译与安装

```bash
make -j$(nproc)          # 并行编译，$(nproc) 为 CPU 核心数
sudo make altinstall        # 安装到 --prefix 指定的路径
```

安装完成后，将新构建的 Python 加入 `PATH`：

```bash
export PATH=/usr/local/python3.14/bin:$PATH
python3.14 --version     # 确认版本
```

### 创建虚拟环境

首先安装 pyperformance，并创建一个独立的 pyperformance 虚拟环境用于测试：

```bash
# 安装
python3.14 -m pip install pyperformance==1.13.0
# 当前路径创建虚拟环境
python3.14 -m pyperformance venv create --inherit-environ http_proxy,https_proxy
# 修改配置，启用系统 site-packages（测试 cpython 基线数据则改为 false）
sed -i 's/^include-system-site-packages = false/include-system-site-packages = true/' venv/<venv_name>/pyvenv.cfg
```

### 性能测试

``` bash
// 命令示例
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=2 PYTHONJITLIGHTWEIGHTFRAME=1 python3.14 -m pyperformance run \ 
    --affinity=300 \ 
    --warmup 3 \ 
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO,PYTHONJITLIGHTWEIGHTFRAME \ 
    -o test.json
```
pyperformance可通过`-b`参数指定用例范围，也可通过`--fast`方式快速验证，更多使用方式参考[pyperformance Docs](https://pyperformance.readthedocs.io)

---

## 调试与性能分析

CinderX 提供了丰富的环境变量用于调试和性能分析：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITDEBUG=1` | 启用 JIT 调试日志 |
| `PYTHONJITDUMPHIR=1` | 输出初始 HIR（高级中间表示） |
| `PYTHONJITDUMPHIRPASSES=1` | 输出每个优化 pass 后的 HIR |
| `PYTHONJITDUMPFINALHIR=1` | 输出最终优化后的 HIR |
| `PYTHONJITDUMPLIR=1` | 输出 LIR（低级中间表示） |
| `PYTHONJITDUMPASM=1` | 输出最终生成的汇编代码 |
| `PYTHONJITDUMPSTATS=1` | 在进程退出时输出 JIT 运行时统计 |
| `PYTHONJITLOGFILE=/path/to/log` | 将日志写入指定文件 |
| `PYTHONJITGDBSUPPORT=1` | 启用 GDB 调试支持 |
| `PYTHONJITHUGEPAGES=0` | 禁用大页内存（默认启用） |
| `PYTHONJITLIGHTWEIGHTFRAME=1` | 启用轻量级解释器帧 |
| `PYTHONJITLISTFILE=/path/to/list` | 通过 JIT 列表文件选择性编译指定函数 |
| `PYTHONJITPRELOADDEPENDENTLIMIT=N` | 编译时预加载依赖函数的最大数量（默认 99） |
| `PYTHONJITALLSTATICFUNCTIONS=1` | 预加载并编译所有 Static Python 函数 |
| `CINDERX_DISABLE=1` | 强制禁用整个 CinderX 扩展 |

---

## FAQ

### 1. 如何简单确认pyperformance可正常使能CinderX插件
- 确认`venv/<venv_name>/pyvenv.cfg`中的 `include-system-site-packages = true`

