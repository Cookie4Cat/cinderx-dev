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
## 后续 PGO+LTO 性能验证（110605）

修复后PGO+LTO包110605已与修复前PGO+LTO包110603直接比较，JIT关闭。四模型均使用相同源码、输入、固定CPU，三档六种排列重复两遍，共12组。针对性回归及624项调用相关测试再次通过。

|模型|修复前相对原生降幅|修复后相对原生降幅|修复后相对修复前|
|---|---:|---:|---:|
|DLRM|11.06%|11.13%|快0.09%，区间跨零|
|MMOE|13.69%|13.23%|慢0.53%|
|ESMM|13.42%|12.49%|慢1.08%|
|GR 2B|0.11%|-0.84%|慢0.95%|

MMOE、ESMM、GR的12组配对bootstrap参考区间未跨零，线程CPU时间也同方向。主要收益保留，但不能宣称正确性修复完全没有性能代价。这里是两个成品wheel的对比，未做逐项修复消融，不能把全部差异精确归因到字典检查。不是实机FREE。完整汇总与构建元数据见review_evidence/PGO_AFTER_FIXES*.json。
## 第二轮：首次初始化与补丁增量依赖

代码提交8634716d4。保留ModuleState的必要构造，将属性配置快照移到主解释器检查、CPython版本检查通过之后；被拒绝的子解释器首次导入不再锁定主解释器设置。

实际复现：旧110605在“子解释器先失败 → 主解释器首次请求execute”时观察到off；修复后观察到execute，强制编译并执行了一个小函数。主解释器首次选择control也使用了自己的默认值。原有“主解释器已安装 → 子解释器失败”回归继续通过。

四份patch注册为CMAKE_CONFIGURE_DEPENDS。旧配置在只改补丁后直接cmake --build时生成文件不变，回归失败；修复后分别修改四份补丁，均自动重新配置并更新生成源码，且与干净configure逐字一致。测试仅修改临时fixture，不修改仓库真实补丁。

新增test_first_import_order.py与test_patch_dependencies.py。29项特化、43项属性差分、4项字典异常、19项启动检查、31项缓存、第三方接管及624项调用相关测试（跳过1项）通过。验证构建为Release+LTO，未重跑本轮PGO性能；仅首次初始化execute探针实际启用了JIT，其余回归JIT-off。详情见review_evidence/REVIEW_ROUND2_VALIDATION.json。