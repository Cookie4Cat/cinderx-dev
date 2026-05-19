# CPython 3.14 Fat Wheel POC

本文记录 `build_cp314_fat_wheel.py` 的来龙去脉、当前验证结论，以及
`my-server` 上保留的运行目录。当前方案仍是 POC，用于证明 CinderX 在
CPython 3.14.0/1/2/3 上做 single fat wheel 的可行性。

## 背景

CinderX 会借用 CPython interpreter/private runtime internals，包括 frame
evaluator、generated cases、opcode metadata、dict/type/runtime state 等。
因此 `cp314-cp314` wheel tag 对 CinderX 来说太粗，不能表达 CPython micro
version 和 CPython build config 的差异。

之前的单 binary 宽版本方案依赖 runtime offset shim，容易变成手写 private
layout offset，并且存在 silent memory corruption 风险。这个 POC 选择把复杂度
移到构建和打包矩阵：

```text
CPython 3.14.0 -> 一个 _cinderx native build
CPython 3.14.1 -> 一个 _cinderx native build
CPython 3.14.2 -> 一个 _cinderx native build
CPython 3.14.3 -> 一个 _cinderx native build
```

然后把四个 native build 合进一个 wheel，import 时按
`sys.version_info.micro` 选择对应 native variant。

## 当前脚本

`build_cp314_fat_wheel.py` 是 repack 脚本，不负责编译 CinderX。

输入是四个已经构建好的 ordinary wheel：

```text
--py3140-wheel
--py3141-wheel
--py3142-wheel
--py3143-wheel
```

脚本会：

```text
1. 校验四个 wheel 的 distribution/version/python tag/abi tag/platform tag 一致。
2. 以 py3143 wheel 作为 Python payload base。
3. 删除 base wheel 顶层 _cinderx*.so。
4. 把四个 native extension 移到 cinderx/_native/py314_x/。
5. 生成顶层 _cinderx.py loader。
6. 更新 METADATA 的 Requires-Python 为 >=3.14,<3.14.4。
7. 生成 cinderx/_native/fat_wheel.json provenance。
8. 重新生成 RECORD。
```

生成后的布局类似：

```text
_cinderx.py
cinderx/_native/fat_wheel.json
cinderx/_native/py314_0/_cinderx_3140*.so
cinderx/_native/py314_1/_cinderx_3141*.so
cinderx/_native/py314_2/_cinderx_3142*.so
cinderx/_native/py314_3/_cinderx_3143*.so
```

loader 当前只按 CPython implementation 和 micro version 选择 native：

```text
3.14.0 -> py314_0
3.14.1 -> py314_1
3.14.2 -> py314_2
3.14.3 -> py314_3
3.14.4+ -> ImportError
非 CPython 3.14 -> ImportError
```

## 服务器验证目录

服务器别名：

```text
ssh my-server
```

本次 POC 的主要目录：

```text
/root/cinderx-fat-wheel-20260519-024339
/tmp/cinderx-fat-wheel-20260519-024339
```

`/root/...` 目录保留了汇总报告和关键产物，`/tmp/...` 目录保留了源码 checkout、
venv、构建脚本、测试脚本和详细日志。

关键文件：

```text
/root/cinderx-fat-wheel-20260519-024339/report.md
/root/cinderx-fat-wheel-20260519-024339/fat-wheel/
/root/cinderx-fat-wheel-20260519-024339/manylinux-fat-wheel/
/root/cinderx-fat-wheel-20260519-024339/*_smoke.tsv
/root/cinderx-fat-wheel-20260519-024339/*_lib_test.tsv
/root/cinderx-fat-wheel-20260519-024339/*test_cinderx_runner*.tsv

/tmp/cinderx-fat-wheel-20260519-024339/build_clean_wheels.sh
/tmp/cinderx-fat-wheel-20260519-024339/build_manylinux_wheels.sh
/tmp/cinderx-fat-wheel-20260519-024339/build_manylinux_wheels_resume.sh
/tmp/cinderx-fat-wheel-20260519-024339/experiment_tailoff_3140.sh
```

## 已验证结果

### Host exact-runtime fat wheel

用目标 runtime 自己分别构建 ordinary wheel：

```text
/root/opt/cpython-3.14.0/bin/python3.14
/root/opt/cpython-3.14.1/bin/python3.14
/root/opt/cpython-3.14.2/bin/python3.14
/root/opt/cpython-3.14.3/bin/python3.14
```

再用本脚本 repack 成：

```text
/root/cinderx-fat-wheel-20260519-024339/fat-wheel/cinderx-2026.5.18.0-cp314-cp314-linux_aarch64.whl
sha256: 4e5dd7f231f75d8980756404d7e06c1751412736394f88787a6534ea1ef82454
```

结果：

```text
3.14.0/1/2/3:
  import/init/JIT smoke PASS
  test_cinderx_runner PASS
  Lib/test PASS

3.14.4:
  normal install rejected by Requires-Python
  forced install then import _cinderx raises ImportError
```

这证明 fat wheel loader/repack 方案本身可行。

### manylinux fat wheel with uv-installed builders

在 `quay.io/pypa/manylinux_2_28_aarch64:latest` 容器中，使用
`uv python install 3.14.0/1/2/3` 得到 builder Python，再构建 ordinary wheel、
auditwheel repair、repack 成：

```text
/root/cinderx-fat-wheel-20260519-024339/manylinux-fat-wheel/cinderx-2026.5.18.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
sha256: 334e586d7aa61aa10b41f9403d1441fe43774b45f209ed2b3c27cffed88cd7e4
```

结果：

```text
3.14.0/1/2/3:
  import/init/JIT smoke PASS
  test_cinderx_runner PASS
  Lib/test FAIL

3.14.4:
  normal install rejected by Requires-Python
  forced install then import _cinderx raises ImportError
```

控制实验显示 manylinux ordinary wheel 自己也会失败，所以问题不是 fat loader。

## Tail-call interpreter 结论

失败根因是 builder CPython 和目标 runtime 的 build config 不一致：

```text
uv/python-build-standalone builder:
  Py_TAIL_CALL_INTERP = 1
  CONFIG_ARGS contains --with-tail-call-interp

target /root/opt/cpython-3.14.x runtime:
  Py_TAIL_CALL_INTERP = 0
  CONFIG_ARGS does not contain --with-tail-call-interp
```

CinderX 的 interpreter loop/generated cases 会按 `Py_TAIL_CALL_INTERP` 在编译期
选择代码形态。这个宏不是运行时开关。`build-time=1/runtime=0` 和
`build-time=0/runtime=1` 都应视为不兼容。

验证实验：

```text
/tmp/cinderx-fat-wheel-20260519-024339/experiment_tailoff_3140.sh
```

该脚本临时把 uv-installed CPython 3.14.0 builder 的 `pyconfig.h` 从：

```c
#define Py_TAIL_CALL_INTERP 1
```

改成：

```c
/* #undef Py_TAIL_CALL_INTERP */
```

然后重新构建 3.14.0 ordinary manylinux wheel。结果：

```text
targeted previous failed modules: PASS
full Lib/test: PASS
423 tests OK
0 failed
```

实验 wheel：

```text
/tmp/cinderx-fat-wheel-20260519-024339/manylinux-tailoff-repaired/py3140-tailoff/cinderx-2026.5.18.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
sha256: 8d872b899d6d981bec94072a7bd77ee63be54190d91d5a44448903bbea36986f
```

## 当前推荐支持边界

当前最小发布形态建议只支持 tail-call off runtime family：

```text
Supported:
  CPython 3.14.0/1/2/3
  Linux aarch64 manylinux
  Py_TAIL_CALL_INTERP=0
  Py_GIL_DISABLED=0
  Py_DEBUG=0
  Py_TRACE_REFS=0

Unsupported:
  CPython 3.14.4+
  Py_TAIL_CALL_INTERP=1
  free-threading builds
  debug/trace-ref builds
```

也就是说，正式构建时需要用 tail-call off 的 3.14.0/1/2/3 builder 产出四个
ordinary wheel，再用本脚本 repack 成 fat wheel。

## 注意事项

POC 构建 ordinary wheel 时曾显式设置：

```text
ENABLE_XXCLASSLOADER=1
```

这是为了让 `test_cinderx` 中依赖 `xxclassloader` 的测试可以运行，不应作为生产
wheel 的默认打包选项。正式发布 wheel 应使用默认值，即不打开
`ENABLE_XXCLASSLOADER`。

## 待产品化事项

当前脚本还没有实现完整 hard guard。后续建议补齐：

```text
1. build preflight:
   - builder Python 必须 Py_TAIL_CALL_INTERP=0
   - builder Python 必须 Py_GIL_DISABLED=0
   - builder Python 必须 Py_DEBUG=0
   - builder Python 必须 Py_TRACE_REFS=0

2. fat loader guard:
   - import _cinderx 前用 sysconfig.get_config_var() 检查 runtime config。

3. native init guard:
   - 在 _cinderx native module 初始化早期检查 runtime config。
   - 防止绕过 Python loader 直接加载 native .so。

4. CI matrix:
   - 同一个 fat wheel 安装到 3.14.0/1/2/3 tail-call off runtime。
   - 每个 runtime 跑 import/init/JIT smoke、test_cinderx、Lib/test。
   - 3.14.4 必须明确 ImportError。
```

如果未来确实要支持 tail-call on runtime，可以扩展成二维 fat wheel：

```text
3.14.0/1/2/3 x Py_TAIL_CALL_INTERP=0/1
```

即 8 个 native variants，由 loader 按 `(micro, Py_TAIL_CALL_INTERP)` 选择。
这需要单独准备 tail-call on 的目标 runtime 并完整验证。
