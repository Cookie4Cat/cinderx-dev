# 五条检视意见修复记录

代码提交：75cc87835；基线仍是 Kunpeng dev bdba8d75f。

|意见|修复|验证|
|---|---|---|
|1 字典比较抛 AttributeError 后走错类属性|普通 suppress 查询之前检查实例存储。general-key managed dict、dict子类及非managed实例字典布局直接回到原属性API，不重跑已经发生副作用的查询。|旧110603实际返回class-value，回归失败；修复后普通字段/方法、managed/非managed四种情况均返回fallback:x，比较仅执行一次。|
|2 默认开关污染子进程|启动器不再写入派生环境变量。native按捕获的ATTR_PATH_MODE解析默认值，用户显式override仍生效。_verify核对全部六项实际/预期配置。|真实.pth路径的父/子模式矩阵、显式override及状态不一致检测通过。旧包在未请求优化的父进程中写出默认值，测试实际失败。|
|3 显式启用失败仍执行应用|显式模式在统一try内安装、核验，普通Exception转SystemExit，越过site.py的异常吞掉逻辑。|旧包bad route后实际打印APP_STARTED；修复后非法route/inline/specialization、冲突disable均非零退出且没有APP_STARTED。|
|4 非owner移除提前清缓存|先检查installed、线程及owner解释器；只有合法owner才能清缓存，保留第三方求值器所有权保护。|三次子解释器失败导入均不改变主解释器状态；既有第三方接管测试通过。|
|5 sdist缺少必需patch|MANIFEST明确包含四个patch及apply.cmake。实际构建进一步发现既有_cinderx.version、cinderx.pth、_cinderx_auto.py也漏包，一并明确纳入。|检查生成tar内容，并从解包的sdist完成native wheel构建。|

运行验证：CPython3.11.6，ARM，JIT-off，Release+LTO（未开PGO）。29项特化、43项属性差分、4项新增字典异常场景、19项启动检查、31项缓存检查、3次子解释器拒绝、第三方求值器接管、624项CPython调用/生成器/协程/trace/profile测试（跳过1项）通过。DLRM与GR2B的NPU mock smoke输出结构、轨迹对齐，候选JIT计数为零。

这是正确性和源码包交付验证，没有重跑修复后的PGO性能，也没有真实NPU数值回归。历史性能结果仍对应修复前wheel。

新用例位于 tools/attr_path/test_dict_attributeerror.py、test_startup.py、test_subinterpreter_owner.py。test_specga.py也改为使用核验后的实际状态决定特化证明。

子进程只能继承调用者实际提供的设置。例如用户显式设置ATTR_PATH_SPECIALIZE=0时，子进程保留该消融；模式默认值不再伪装成用户override。显式属性模式在native模式解析中选择JIT-off，其他未选择属性模式的dev启动路径保持独立。