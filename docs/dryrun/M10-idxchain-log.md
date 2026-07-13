# M10 索引链内联轮:半行内下标快路径补全与三项盲点定性

分支 dryrun/m10-liststore(叠加于下标盲区轮)。源起:PRECALL_NO_KW
候选轮开工前的两步定性(迭代协议面 + hexiom 稳态剖析),证据把轮次
主题改写为索引链。

## 一、三项定性(每项二十分钟档)

1. **迭代协议面**:range/list/tuple/dict 四种循环的最终 HIR 全部
   逐圈发射 `InvokeIterNext`(helper 调用),零类型化下沉——疑点
   属实;但稳态份额证伪其肥度:meteor 1.8%、mdp 0.05%、hexiom
   1.4%。类型化迭代全集上限 ~0.1pp 档,**降级为顺手项不立轮**。
2. **meteor_contest/mdp 定谳 C 主导天花板**:meteor 稳态碎片带
   经符号贴名为 setobject.c 内部(frozenset 棋盘的添加/查找/哈希)
   + list 内部 + RichCompare;mdp 为 `PyObject_Hash` 20.7%(元组键
   哈希)+ dict 查找内部。两者停在 1.0 档不是 JIT 失职,活本来
   在 C 里。与 telco 同族销案。
3. **PRECALL_NO_KW 家族降级**:三个目标用例中两个(meteor/mdp)
   C 主导,hexiom 的全部调用面(JITRT_Vectorcall)仅 1.5%——
   普查候选的预期收益坍缩,暂不立轮。

## 二、hexiom 稳态剖析定罪(轮次主题来源)

| 份额 | 归属 |
|---|---|
| ~8% | 索引链 helper:_PyNumber_Index 2.9% + PyNumber_AsSsize_t 2.0% + PyLong_AsSsize_t 1.5% + JITRT_CheckSequenceBounds 1.75% |
| 10-12% | Ci_EvalFrameDefault_311 解释器驻留 |
| ~10.6% | PyObject_RichCompare(Bool) 泛型比较 |

定型下标快路径实为**半行内**:守卫/下沉齐备,但索引解箱与界检
仍逐次打 helper。LongCompare 侧此前已有紧凑内联(证伪"第三链")。

## 三、实现(commit 8f1ffe405,simplify.cpp 单文件)

1. **simplifyIndexUnbox**:TLongExact 已证输入按 IsCompactLong
   分派——紧凑长整走 CompactLongUnbox 双指令,多数位值保留
   PyNumber_AsSsize_t 慢臂(溢出异常逐字一致)。慢臂 CallStatic
   直呼 helper 而非重发同名指令,避免固定点自循环。
2. **trySimplifySequenceBoundsInline**:TListExact/TTupleExact/
   TUnicodeExact × TCInt64 下标,负值归一与界检内联(无符号单
   比较折叠双端测试),越界慢臂直呼 JITRT_CheckSequenceBounds。
   PyUnicode 依布局双关(PyASCIIObject::length 恰在 ob_size 槽)
   与 helper 的 Py_SIZE 读取一致。

两处均运行时分派,无投机守卫、无 deopt 面、无引用语义变更。

## 四、验证与收益

- list/tuple/str × 9 种下标形态(双端边界/越界/巨整数/布尔)
  差分对拍全等;41 用例矩阵全等;diffgate 0 新增;RCM 8 组与
  前轮参照逐 case 全等;冒烟四件过。
- **hexiom 进程内 1.30→1.45**(B4 −9.2%),索引链四件全部退出
  稳态热点前列;richards 0.0079/nbody 0.1285/scimark 子核同夜
  续降(sor 0.0792、lu 0.0947、monte 0.0389)。
- 驻留探因:调用链 JITRT_Vectorcall→C 内建→PyIter_Next→解释器
  ——小生成器表达式被 sum/min/sorted 族驱动,低于 128 单元
  尺寸门策略性留解释(生成器轮既判:小紧 yield 编译反 +58%)。
  非缺陷,销案;出路归投机内联族(genexpr 内联)。RichCompare
  残余同根(解释态 genexpr + C 容器内部),非独立可作为面。

## 五、v29 引爆潜伏 LIR 缺陷:set_err 悬空坠落(commit 406712253)

v29 全集 docutils B 侧 SIGSEGV(85/86)。取证全程:

1. **崩溃点漂移**(JIT 页 XDecref 垃圾指针 → _Py_CheckFunctionResult
   内 → 桩区 PC)= 延迟性状态污染,炸点不可追,须找源头;
2. **能力二分**:IndexUnbox 单独=好;bounds 内联(哪怕仅 list)=坏;
3. **压力证伪**常规组合(负下标/越界/GC 六万次异常路径全过),
   症状"returned a result with an exception set"提示异常悬挂;
4. **最小复现**:同函数内连续两个 try/except IndexError 包裹的
   下标、越界发生于第二处(docutils get_2D_block 形)——解释器过、
   JIT 泄异常;
5. HIR 语义层穷尽自洽(deopt reason 同为 kUnhandledException、
   ownership 标记、栈单、Incref 下沉全对)→ **落 LIR 后重排 dump
   见真凶**:`kIsNegativeAndErrOccurred` 下沉的 set_err 块
   (旗标减 -1)从未与汇合块建边,零后继块被重排扔到千行外,
   运行时从块尾**坠入无关基本块**。builder 语境下分配序恰好相邻
   而侥幸多年;本轮 simplify 慢臂以该指令收尾的形状首次引爆。

修复:置错块尾 switchBlock→appendBlock 显式建边(五行)。同文件
switchBlock 位点全核,无同族隐患。修后最小复现体/docutils/全部
门禁复绿,hexiom 收益保持(0.00955)。

**教训**:①下沉层"依赖布局相邻的隐式坠落"=潜伏缺陷面,新发射
语境(simplify 慢臂收尾形)会引爆 builder 时代的布局假设——与
pickle 轮"无返回出口块"同族,LIR 块必须显式终结;②崩溃点漂移
+症状多形=状态污染,能力二分→最小复现→逐层(HIR→LIR→布局)
下潜是正确序;③新 HIR 发射形状的验证矩阵须含**同函数内异常
处理器**形(try/except 包裹的慢臂 deopt)。

## 六、v30 全集与同夜因果判决

v30(86/86 无失败)名义几何 1.012,逐项中位 0.9698——宿主漂移
继续叠深(B 侧全域较 v28 夜再慢约 3%,asyncio_tcp_ssl 前夜 +14.8%
本夜 −15.9% 纯抖)。绝对几何跨夜不可比,按判例做同夜修前臂
(HEAD~2 二进制)十项子集 A/B:

| 用例 | 修前(同夜) | v30(修后) | 因果差 |
|---|---|---|---|
| crypto_pyaes | 1.034 | 1.251 | **+21.0%** |
| hexiom | 1.082 | 1.228 | **+13.5%** |
| fannkuch | 1.225 | 1.281 | +4.6% |
| scimark | 1.354 | 1.378 | +1.8% |
| richards | 2.679 | 2.725 | +1.7% |
| sqlalchemy_imperative | 1.072 | 1.084 | +1.1% |
| telco | 0.892 | 0.898 | +0.7% |
| networkx | 0.883 | 0.886 | ~0 |
| asyncio_tcp | 0.960 | 0.965 | ~0 |
| docutils | 0.875 | 0.873 | ~0(修复后无害) |

十项全非负;v30 表观跌幅项在修前臂同夜同样低迷,漂移定谳。
crypto_pyaes(满屏字节表下标)与 hexiom 为索引链内联的最大
受益者。docutils 与全部噪声带项零回归。

## 六、遗留

- PrimitiveUnbox TCInt64(builder array 路径所用)同形紧凑内联
  ——PyLong_AsSsize_t residual 的另一半,下轮顺手项。
- 迭代协议类型化(listiter/rangeiter/tupleiter 三臂)已定形状
  (匹配 InvokeIterNext+CondBranchIterNotDone 对,专用 pass),
  按 ~0.1pp 档排队。
