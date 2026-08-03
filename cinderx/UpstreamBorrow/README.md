# 独立 UpstreamBorrow 生成工具

## 简介

该工具用于从指定版本的 CPython 源码中提取 CinderX 所需的私有实现，并生成或
校验仓库中的 Borrow 缓存。工具只依赖标准 Python 环境、libclang 和
`compile_commands.json`，不依赖 Buck 或外部单仓模块。

CPython 源码与目标解释器必须具有相同的补丁版本，否则工具默认拒绝执行。

## 原理

生成器读取模板中的 `@Borrow` 指令，确定需要处理的 CPython 源文件和符号；
随后探测目标解释器的版本、头文件及编译配置，通过 libclang 解析源码并提取
对应函数、类型或预处理指令，最后将模板和提取结果组合成缓存文件。

未传入 `--compile-commands` 时，工具会根据目标解释器自动生成编译数据库。
`--check` 用于逐字节校验现有缓存，`--write` 用于更新缓存。

## 使用指南

先安装依赖：

```bash
PYTHON=/usr/local/cpython-3.14.3/bin/python3.14

"$PYTHON" -m venv /tmp/cinderx-borrow-venv
/tmp/cinderx-borrow-venv/bin/python -m pip install \
  -r cinderx/UpstreamBorrow/requirements-standalone.txt
```

以下示例使用 CPython 3.11.6 源码校验对应缓存：

```bash
PYTHONPATH="$PWD" /tmp/cinderx-borrow-venv/bin/python \
  -m cinderx.UpstreamBorrow.generate \
  --python /usr/local/cpython-3.11.6/bin/python3.11 \
  --cpython-source /path/to/Python-3.11.6 \
  --template cinderx/UpstreamBorrow/borrowed-3.11.c.template \
  --output cinderx/UpstreamBorrow/borrowed-3.11.gen_cached.c \
  --emit-compile-commands /tmp/borrow-3.11-compile_commands.json \
  --check
```

需要更新缓存时，将 `--check` 改为 `--write`。已有标准编译数据库时，可以使用
`--compile-commands /path/to/compile_commands.json`，并省略
`--emit-compile-commands`。
