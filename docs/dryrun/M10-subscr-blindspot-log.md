# M10 下标盲区轮:纯算法 1.0x 异常带的取证与第一刀

分支 dryrun/m10-liststore。上一轮收口后从盲点角度盘点:86 项中
纯算法 Python 用例(理应为 JIT 金矿)窝在 1.0x 档且从未有专项轮:
scimark 1.033、meteor_contest 0.983、mdp 1.009、crypto_pyaes 1.097、
hexiom 1.101(v27 值)。

## 一、取证

### scimark 五子核拆测(进程内 best-of,B4 vs 同二进制巨阈值 B0)

轮前:fft 1.34 / sor 1.18 / lu 1.13 / monte_carlo 1.12 /
**sparse_mat_mult 0.99(JIT 与解释打平)**。聚合报告掩盖了烂子项
(deepcopy_reduce 均值混叠教训重演)。

### 形态定性(读 bench 源码)

- **scimark 全族容器是 `array('d')`/`array('i')`,不是 list**。
  sparse 内环 `x[col[i]]*val[i]` 的 col/row 是 `array('i')`——整型
  typecode 无快路径,逐元素泛型 helper;sor 的 `G[x,y]` 走 Array2D
  自定义 `__getitem__/__setitem__`(元组下标+`_idx` 再调一层),
  是派发链税(内联缺口家族);monte_carlo 是 `nextDouble()` 方法
  调用税 + 体内 array 读写。
- 特化流直方图(dis adaptive):hexiom 5 处/nbody 12 处/fannkuch
  2 处 STORE_SUBSCR_LIST_INT 位点;hexiom/meteor/mdp 的最大公因子
  实为 CALL_ADAPTIVE/PRECALL_PYFUNC(动态调用=内联缺口)与
  PRECALL_NO_KW_LEN/LIST_APPEND(普查缺口),写侧只是次要项。

### 现状缺口(读 JIT 源码)

1. allowlist 只准入 STORE_SUBSCR_DICT,**LIST_INT 写侧从未接入**;
   simplifyStoreSubscr 只有 dict 一支(CallStatic)。读侧
   BINARY_SUBSCR_LIST_INT 早有完整定型下沉(读写不对称)。
2. array 快路径('d' 硬编码)的证据门是纯静态的(容器静态可证或
   下标可证 TLongExact)——**方法体内经属性加载的容器
   (`self.data[idx]`)与链式下标(`x[col[i]]` 的下标是 Phi 汇合
   TObject)两侧全失守**,Array2D 体内的数组存取今天全走泛型。
   'd' 路径实际只服务过"裸局部变量+循环变量直下标"形(fft/nbody)。

## 二、实现(commit 6982a7a7b,13 文件)

1. **STORE_SUBSCR_LIST_INT 消费**:allowlist 放行(3.11 块)+
   builder 发 GuardType(TListExact/TLongExact)+ simplifyStoreSubscr
   行内存储。引用次序遵循 list_ass_item:LoadArrayItem
   borrowed=false 收养旧值、StoreArrayItem 偷取新值、旧值 Decref
   后置于覆写之后。
2. **UseObj 存活锚指令**(新 HIR opcode):无输出、运行时零代码、
   DCE 显式豁免。定罪实录:UseType 锚被紧邻 RefcountInsertion 之前
   的 DCE 删除(无输出、无 store 效应、不在 useful 根集),旧值
   Decref 被插到覆写之前,差分冒烟的析构重入用例抓获(`__del__`
   透过 list 槽看到垂死对象)。上游 Meta 配方即 UseObj,dev 线
   评审记忆中"UseType 可作替代锚"的推断被本树实证否定。
3. **array 整型 typecode**:读写快路径 'd' 单分派→'d'/'i' 双分派。
   'i' 读:LoadArrayItem TCInt32 + PrimitiveConvert 符号扩展 +
   PrimitiveBox;'i' 写:PrimitiveUnbox TCInt64 + int32 范围预检
   (越界路由慢路径保留 OverflowError)+ 截断存储。界检提至
   typecode 分派前共享(界语义与 typecode 无关)。
4. **失败位点证据门** `BytecodeInstruction::isSubscrAdaptiveStuck`:
   3.11 下标特化失败(容器非 list/tuple/dict/getitem 类)的位点
   停留在 *_ADAPTIVE 形。_PyCode_Quicken 将计数器清零,首次执行
   即尝试:成功换 opcode、失败置 backoff≥1。判别式=ADAPTIVE 形
   且 backoff 非零(为零⇔从未执行,排除冷位点)。作为 array
   读写快路径的发射证据,解锁上述双盲位点。发射形是投机分支
   (CondBranchCheckType)而非 deopt 守卫——类型不符仅付一次
   类型测试,无 despec 交互面。
   - 第一版判别式 `backoff > ADAPTIVE_BACKOFF_START(5)` 校准错了
     参照系(stock adaptive_counter_start 的 5 档只用于重臂场景,
     quicken 初值是 0):无循环体的小方法在 auto=4 编译前只失败
     ~3 次,backoff 3-4 够不着 5——HIR dump 抓到 __getitem__ 体内
     仍是泛型 BinaryOp 才定罪。修正为 `!= 0` 后 sor/monte 兑现。

## 三、验证

- 差分冒烟 41 用例编译/解释对拍全等(析构重入序/别名/负下标/
  大整数下标/int32 溢出/混合 typecode/bytes/memoryview/dict)。
- RCM 8 组同臂前后(stash 重建取修前参照)漂移集逐 case 全等。
- 差分门禁 940 例 0 新增(较基线修复 4 例);libtest 26 模块唯一
  新分歧=test_scope testLeaks(已知跟踪项,watcher 强引用,修复在
  旁路分支;本轮冒烟 rc_store/rc_alias_loop 已验存储环引用中性)。
- 冒烟四件全过。

## 四、子核终值(进程内,B4 vs 同二进制 B0)

| 子核 | 轮前 | 轮后 |
|---|---|---|
| sparse_mat_mult | 0.99 | **1.90** |
| fft | 1.34 | **2.01** |
| lu | 1.13 | **1.44** |
| sor | 1.18 | **1.28** |
| monte_carlo | 1.12 | **1.18** |

sor/lu/monte 残余为派发链税(每元素 1-2 次 Python 调用+元组分配),
归投机内联族,本轮不做。轮前 monte B0 0.0605 读数为漂移档,复测
0.044 稳定。

## 五、v28 全集与宿主漂移判决

v28(A 侧取 v19-fresh 档)名义几何 1.042,较 v27 的 1.054 表观
下降,且靶子与面上背离:scimark 1.033→**1.417**(+37%)、nbody
1.237→1.398(+13%),但 coroutines/2to3/python_startup/float/mako/
deepcopy/tornado 等普遍浅跌 4-12%。

按"带外档值必复测"两步裁决:

1. **复测**(同 10 项子集):django_template 弹回 1.092(v28 的
   1.007 为噪声),richards 2.734 与 v27 持平(对照稳),但
   coroutines/startup/float/mako/deepcopy 复测仍停低位——排除
   单次噪声。
2. **同夜 stash A/B**(决定性,修前二进制同夜同宿主重跑同子集):
   修前臂同样低迷(coroutines 0.825、startup 0.248、float 1.200、
   deepcopy 0.845,scimark 基线亦从 1.033 落到 1.008)。**逐项
   因果差全部非负**:coroutines +11%、django +8%、mako +2.8%、
   deepcopy +2.7%、richards +2.3%、float +1.3%、startup +1.6%、
   2to3/tornado 持平、scimark +38.5%。

判决:v28 的表观下降全部为**宿主漂移**(测量夜宿主整体慢于 v27
测量夜,A 侧为固定档案,B 侧绝对时间随宿主平移),本轮改动对
全部疑似回归项因果中性偏正。v28 绝对几何不可与 v27 直接比较;
干净的全集定价待宿主稳定后双侧重跑或 950 复测给出。担心过的
拓宽发射税(str/defaultdict 等 stuck 位点探针)实测不构成回归。

战役口径:v27 1.054 仍为基准参考;本轮增量以同夜因果差与子核
终值计。
