# `rng_size` 官方插件技术文档（怪物体型控制）

- **插件标识**：`size`（编译产物 `rng_size.dll`，版本 `2.1.0`）
- **源码位置**：[size_plugin.cpp](size_plugin.cpp)
- **宿主依赖**：`mhw_rng.dll`（Plugin SDK `kMhwRngApiVersion == 3`，仅包含头文件 [mhw_rng_api.h](include/mhw_rng_api.h)）
- **接管调用点**：`RVA 0x169BA15`（位于 `nEmDtTune::cSizeTable::getSizeMultiplier` 内）

本文档说明 `rng_size` 插件所依据的原版体型计算逆向逻辑、边界处理方式、按怪物条件分流实现以及命令用法。本插件也是如何使用 `mhw_rng` Pull 模式同步回调（`RegisterSlotHook`）读取调用栈上下文的官方参考实现。

---

## 一、游戏原版怪物体型计算逻辑

### 1. 调用链与栈帧关系
在 Steam 15.23 版本中，大型怪物生成时抽取体型倍率的调用链固定为：
1. 外层生成函数 `FUN_141ccfb70`（`0x141CCFB70`）在非易失性寄存器 **`RBX`** 中持有当前正在初始化的 **`Monster*`** 实例指针，并读取怪物对象偏移 `+0x773C` 处的体型表编号调用 `FUN_140f5f760`。
2. 中间查找函数 `FUN_140f5f760`（`0x140F5F760`）不修改 `RBX`，从全局指针 `*(uintptr_t*)0x145012908` 中按索引取出对应的 `cSizeTable*` 作为 `RCX`，调用 `cSizeTable::getSizeMultiplier`（返回地址位于 `0x140F5F784`，即 `RVA 0x0F5F784`）。
3. `nEmDtTune::cSizeTable::getSizeMultiplier`（`0x14169BA00`）函数头部执行 `push rbx; sub rsp, 0x20`，随后在 `0x14169BA10` 处调用 `MtRandom::nrand(&MtRandom::sInstance)`，其返回地址为 **`0x14169BA15`（`RVA 0x169BA15`）**。

因此，当 `MtRandom::nrand` 在 `RVA 0x169BA15` 被微内核拦截时，以 `ctx->returnAddrSlot`（记为 `const uintptr_t* slotPtr`）为基准的栈帧布局如下：

| 栈槽下标 | 偏移字节 | 存放内容 | 用途 |
| :-: | :-: | :--- | :--- |
| `slotPtr[0]` | `+0x00` | `exeBase + 0x169BA15` | 返回到 `cSizeTable::getSizeMultiplier` 的地址 |
| `slotPtr[1..4]` | `+0x08 .. +0x20` | `getSizeMultiplier` 的 `0x20` 字节局部栈空间 | Windows x64 Shadow Space |
| `slotPtr[5]` | `+0x28` | 保存的原 `RBX` 值，即 **`Monster*`** | 读取当前刷出怪物的 `MonsterID` 与 `tableIdx` |
| `slotPtr[6]` | `+0x30` | `exeBase + 0x0F5F784` | 返回到 `FUN_140f5f760` 的地址，用作栈帧特征校验 |

### 2. 相关内存结构偏移

| 基对象 / 全局地址 | 偏移 | 类型 | 含义 |
| :--- | :--- | :--- | :--- |
| `Monster*` | `+0x12280` | `uint32_t` | 引擎 `MonsterID` 枚举（有效范围 `0..101`，本尊与亚种/特殊个体各自拥有独立编号，例如 `91 = 金狮子`，`92 = 激昂金狮子`） |
| `Monster*` | `+0x773C` | `uint32_t` | 当前怪物在此任务中绑定的体型表索引 `tableIdx` |
| `*(uintptr_t*)(exeBase + 0x5012908)` | `+0xE8` | `uint32_t` | 已加载的 `cSizeTable` 总数 `tblCount` |
| `*(uintptr_t*)(exeBase + 0x5012908)` | `+0xF8` | `cSizeTable**` | 指向 `cSizeTable*` 指针数组的基址（按 `tableIdx` 索引） |
| `cSizeTable*` | `+0x20 + i * 0x10` | `int32_t` | 第 `i` 档（`i = 0..37`，共 38 档）的整数体型百分比 `sizePct`（常规表依次为 `88` 至 `125`） |
| `cSizeTable*` | `+0x24 + i * 0x10` | `int32_t` | 第 `i` 档的抽取权重 `weight`（常规大小金任务表正权重之和为 `100`；该任务不允许出现的档位 `weight` 为 `0`） |

### 3. 原版抽签公式与首尾边界问题
`0x14169BA00` 反编译后的核心逻辑等价于以下 C 代码：

```c
uint32_t raw = MtRandom::nrand(&MtRandom::sInstance);
float roll = (float)raw * (100.0f / 4294967296.0f); // 常量位于 0x142F173C4 (约 2.3283064e-8f)

uint32_t cumWeight = 0;
for (uint32_t i = 0; i < 38; ++i) {
    cumWeight += table->rows[i].weight;
    if (roll <= (float)cumWeight) {
        return &table->rows[i];
    }
}
return &table->rows[12]; // 偏移 +0xE0，即 100% 标准体型兜底档
```

直接向该函数注入极端原始整数（`0` 或 `0xFFFFFFFF`）会触发两类边界问题：

1. **`raw == 0` 命中前导零权重禁用行（导致非法体型）**：
   原版比较使用的是 **`<=`** 而非 `<`。在部分活动或主线任务中，体型表前若干档的权重被设为 `0`。例如活动任务「黄金奖杯」（激昂金狮子）的体型表合法范围为 **`90% ~ 118%`**，其第 0 行（`88%`）和第 1 行（`89%`）的 `weight` 均为 `0`。
   - 若注入 `raw = 0U`，则 `roll = 0.0f`。循环在第 `i = 0` 行（`88%`）计算 `cumWeight = 0`，判断 `0.0f <= 0.0f` 为真，直接返回了本应概率为 `0%` 的 `88%` 非法体型。
   - 若改为注入 **`kMinSafeFloatU32 = 0x00000001U`**，则 `roll = 2.3283064e-8f > 0.0f`。对于所有前导 `weight == 0` 的行（`cumWeight == 0.0f`），`roll <= 0.0f` 均为假；直到进入第一个 `weight >= 1` 的合法行（如「黄金奖杯」第 2 行 `90%`，此时 `cumWeight >= 1.0f`），`2.3283064e-8f <= 1.0f` 成立并准确返回该任务允许的最小体型。若第 0 行本身合法（`weight >= 1`），同样在第 0 行成立。
2. **`raw >= 0xFFFFFF80U` 的单精度舍入与非 100 权重表溢出**：
   - `uint32_t` 的最大值 `0xFFFFFFFFU` 在转换为 IEEE-754 单精度 `float` 时会向上舍入为 `4294967296.0f`（`2^32`），乘系数后得到精确的 `100.0f`。插件将上界固定为 **`kMaxSafeFloatU32 = 0xFFFFFF00U`**（转换后 `roll = 99.9999924f < 100.0f`），留出完整的最低有效尾数位余量。
   - 若遇到自定义任务或特殊表正权重之和 `totalWeight < 100`，`99.99999f` 会大于最后一行的 `cumWeight`，导致循环跑完 38 行后落入末尾的 `rows[12]`（`100%`）兜底分支。因此插件在读取到 `totalWeight != 100` 时，会将目标比例按 `(0, totalWeight)` 动态缩放后再生成 `uint32_t`。

---

## 二、插件内部架构与工作流程

### 1. 条件编号映射（`CondID`）
插件在初始化时通过 `RegisterSlotHook` 挂载 `0x169BA15` 的同步回调 `OnSizeMultiplierDraw`，并使用以下条件编号约定：
- `CondID = 0`：全局默认策略（`g_globalPolicy`），作用于未单独配置专属规则的所有怪物。
- `CondID = monsterId + 1`（范围 `1..102`）：对应 `MonsterID = 0..101` 的单怪专属策略（`g_monsterPolicies`）。

当任务中同时存在多只大型怪物时，回调从栈上识别当前刷出怪物的 `monsterId`：若该怪物配置了专属策略则选用专属策略，否则回退至全局策略；未配置任何规则的怪物继续使用微内核 `CondID = 0` 的 `MasterDerived` 随机流，不受同场其它怪物锁定规则的影响。

### 2. 三种策略模式（`RuleMode`）
- `RuleMode::kNone`：不干预，返回 `kMhwRngActionUseCondition` 且 `condId = 0`。
- `RuleMode::kCondSlot`（对应 `min`、`max`、`mid` 或 `0~100` 权重比例）：
  在设置命令执行时，插件预先将换算好的安全 `uint32_t` 写入微内核复合槽 `(0x169BA15, condId)` 的 `FixedValue`。抽签时若当前任务表 `totalWeight == 100`，回调直接返回 `kMhwRngActionUseCondition(condId)` 由微内核复合槽出值并统计次数；若 `totalWeight != 100`，则按实际总权重缩放后通过 `kMhwRngActionReturnValue` 返回。
- `RuleMode::kExactPct`（对应 `exact <百分比>` 实时反算）：
  抽签瞬间，回调读取当前怪物绑定的 38 行 `cSizeTable`，跳过所有 `weight <= 0` 的禁用行，查找与目标百分比绝对差最小的合法行 `bestIdx`，计算其累计权重区间中点：
  $$\text{midRoll} = \sum_{i=0}^{\text{bestIdx}-1} \max(0, \text{weight}_i) + 0.5 \times \text{weight}_{\text{bestIdx}}$$
  再通过 `Roll0To100ToSafeU32(midRoll)` 转换为 `uint32_t`，以 `kMhwRngActionReturnValue` 直接返回给游戏。

### 3. 刷怪遥测与线程安全设计
- **刷怪记录缓存**：插件维护一个长度为 4 的固定环形数组 `g_recentSpawns`，在每次 `0x169BA15` 触发时记录怪物 `monsterId`、当前任务表的合法最小/最大百分比以及本次抽取的体型结果（即使在未开启任何锁定的默认状态下，也可通过 `$rng size` 或 `$rng size history` 查看刚刷出怪物的任务表范围与实际抽中体型）。
- **锁顺序纪律**：内部互斥锁 `g_stateMx` 仅用于读写 `g_globalPolicy`、`g_monsterPolicies` 与 `g_recentSpawns`。所有 `g_host->...` 调用（设置内核槽位、查询快照、打印聊天反馈）均严格放在 `g_stateMx` 作用域之外执行，避免与宿主回调产生交叉持锁。

---

## 三、命令参考手册（`$rng size`）

| 命令格式 | 作用说明 |
| :--- | :--- |
| `$rng size` 或 `$rng size status` | 查看当前全局策略、单怪专属规则列表，以及最近一次刷出怪物的名称、任务表允许范围和抽中百分比 |
| `$rng size history` | 查看最近最多 4 只刷出怪物的体型记录与任务表范围 |
| `$rng size min` / `mid` / `max` | 全局设置所有怪物为最小合法体型（小金档）、中等（`50/100`）或最大合法体型（大金档） |
| `$rng size <0~100>` | 全局按权重累计轴比例（`0.0 ~ 100.0`）固定体型抽签位置 |
| `$rng size exact <50~250>` | 全局开启实时查表反算，自动在当前任务表中选取最接近指定百分比的合法体型档位 |
| `$rng size <monster> <min\|max\|mid\|0~100\|exact N>` | 仅为指定怪物设置条件体型规则，不影响同场其它怪物 |
| `$rng size <monster> clear`（或 `$rng size clear <monster>`） | 仅移除指定怪物的专属体型规则 |
| `$rng size clear` / `off` / `reset` | 清空全部全局与单怪体型规则，恢复默认随机流 |
| `$rng size monsters [关键词]` | 查询内置的 71 种大型怪物英文名、简写、内部 `em` 代号与 `MonsterID` |

### 怪物选择器（`<monster>`）支持的写法
1. **标准英文名或简写**：如 `rajang`（`#91` 金狮子）、`furious_rajang` 或 `furious`（`#92` 激昂金狮子）、`zinogre`（`#94` 雷狼龙）、`stygian`（`#95` 狱狼龙）、`nergi`（`#25` 灭尽龙）、`ruiner`（`#70` 歼世灭尽龙）、`velkhana`（`#79` 冰咒龙）、`fatalis`（`#101` 黑龙）。
2. **通配符批量匹配**：如 `rajang*`（同时匹配 `rajang` 与 `furious_rajang`）、`*rathalos*`（匹配火龙、苍火龙、银火龙）。
3. **内部资源代号**：精确个体代号如 `em023_00`（金狮子）、`em023_05`（激昂金狮子），或种族前缀 `em023`（自动匹配全部 `em023_*` 个体）。
4. **显式枚举编号**：如 `#92` 或 `id:92`。

> [!IMPORTANT]
> **本尊与亚种/特殊个体的区分**
> 冰原引擎中亚种和特殊个体使用独立的 `MonsterID`。例如在「黄金奖杯」任务中目标为激昂金狮子，应使用 `$rng size furious_rajang min`（或简写 `$rng size furious min`，或全系通配 `$rng size rajang* min`）；单独设置 `$rng size rajang min` 只会对普通金狮子（`#91`）生效。

---

## 四、独立仓库目录、头文件同步与构建

本插件目录包含独立的 SDK 头文件副本与构建脚本，可直接作为单独的 Git 仓库检出并编译：

- `size_plugin.cpp`：插件主源码
- `include/mhw_rng_api.h`：本地携带的 `mhw_rng` Plugin SDK v3 头文件副本（无需链接 `.lib`）
- `sync_sdk.bat`：从核心框架目录更新 `include\mhw_rng_api.h` 的同步脚本
- `build.bat`：自动查找本机 MSVC x64 工具链并输出 `build\rng_size.dll` 的单目录构建脚本

常用命令：
```bat
:: 1. 若核心框架更新了 mhw_rng_api.h，同步本地头文件（不传参默认查找 ..\..\include 或 %MHW_RNG_SDK_DIR%）
sync_sdk.bat <path-to-mhw-rng>

:: 2. 独立编译至 build\rng_size.dll
build.bat

:: 3. 编译并直接覆盖至 Steam 游戏目录 nativePC\plugins\mhw_rng\rng_size.dll
build.bat --deploy
```

部署完成后（即使游戏正在运行也不会提示文件占用），在游戏聊天框输入 `$rng reload size` 即可完成热更新。
