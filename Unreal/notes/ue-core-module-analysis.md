# UE Core 模块结构分析：Containers / Memory / HAL / Templates

## 一、模块结构总览

所有模块位于 `Engine/Source/Runtime/Core/Public/` 下：

```
Core/Public/
├── Containers/          # 容器（TArray, TMap, TSet, FString 等）
├── HAL/                 # 硬件抽象层（内存、文件、线程、平台）
│   ├── Allocators/      # OS 页缓存分配器
│   └── Memory/          # LinearAllocator, MemoryArena 等
├── Templates/           # 类型萃取、智能指针、函数包装
└── UObject/             # FName 定义（NameTypes.h）
```

---

## 二、Containers 模块关键组件

### 1. TArray vs std::vector

**定义**: `Array.h:669`

```cpp
template<typename InElementType, typename InAllocatorType>
class TArray {
    SizeType ArrayNum;               // 当前元素数
    SizeType ArrayMax;               // 已分配容量
    ElementAllocatorType AllocatorInstance;  // 分配器实例
};
```

**核心差异**：

| 特性 | TArray | std::vector |
|------|--------|-------------|
| **增长因子** | 1.375x（`3/8 + 16`） | 通常 1.5x 或 2x |
| **收缩策略** | 浪费 >16KB 或 >50% 时自动收缩 | 无自动收缩 |
| **内联存储** | `TInlineAllocator<N>` 避免堆分配 | 无（需要 `small_vector`） |
| **元素搬迁** | `RelocateConstructItems` 按位搬迁（跳过构造/析构） | 必须走 move 语义 |
| **Allocator 模型** | 侵入式，控制 SizeType、对齐、边界检查 | 标准 Allocator 概念 |
| **O(1) 删除** | `RemoveAtSwap` 用尾元素填坑 | 无内建支持 |

**增长策略** 见 `ContainerAllocationPolicies.h:168-220`：
- 默认增长 = `NewMax + 3*NewMax/8 + 16`（约 1.375x）
- 激进省内存模式 = `NewMax + NewMax/4`（1.25x）
- 最终调用 `FMemory::QuantizeSize()` 对齐到分配器桶大小，避免浪费

### 2. TMap / TSet — 哈希策略

**TMap** 通过宏生成三种变体 (`Map.h:119-137`)：
- `TMap` → 默认
- `TSparseMap` → 稀疏存储
- `TCompactMap` → 紧凑存储

**底层存储模型** 见 `CompactSet.h.inl:44-55`：

```
 ____________ ___________ ________________ ____________
|            |           |                |            |
| Data Array | Hash Size | Collision List | Hash Table |
|____________|___________|________________|____________|
```

- **Data Array**：紧凑连续存储，无空洞（vs `std::unordered_map` 的 bucket+node 布局）
- **Hash Table**：2 的幂大小的桶数组
- **Collision List**：链表索引数组解决冲突
- **哈希函数**：使用 MurmurHash3 finalization (`HashTable.h:23-75`)
- **异构查找**：支持不创建临时 key 直接查找（避免为查一个 `const char*` 构造整个 `FString`）

| 特性 | TMap/TSet | std::unordered_map/set |
|------|-----------|----------------------|
| **数据布局** | 紧凑连续（cache 友好） | node-based（cache 不友好） |
| **迭代性能** | 线性扫描，快 | 跳跃访问，慢 |
| **异构查找** | 原生支持 | C++20 才支持 |
| **每节点开销** | 无额外 node 分配 | 每个元素一次 heap 分配 |

### 3. FString — 字符串

**定义**: `UnrealString.h.inl:54-70`

```cpp
class FString {
    TArray<TCHAR, TSizedDefaultAllocator<32>> Data;  // 内部就是 TArray<TCHAR>
};
```

- 32 字节内联存储（约 15 个 TCHAR），类似 SSO
- 支持 ANSI/WIDE/UTF8/UCS2/UTF32 多种源字符类型构造
- 增长/收缩继承 TArray 的策略
- 大量 UE 特定 API（`Printf`, `Contains`, `Replace`, 路径操作等）

### 4. FName — 池化不可变标识符

**定义**: `NameTypes.h:68-142`

```cpp
struct FName {
    FNameEntryId Index;   // 4 bytes → 指向全局池中的唯一字符串
    int32 Number;         // 4 bytes → 实例编号（Name_0, Name_1...）
};  // 总共 8 bytes
```

- **O(1) 比较**：只比较 8 字节 ID，不比较字符串内容
- **全局去重**：相同字符串只存一份，`NameTypes.h:278-381` 定义了 `FNameEntry`
- **最大长度 1024**，字符串长度上限 10 bit（1023 字符）
- 用途：属性名、资源名、蓝图变量名等高频比较场景

---

## 三、Memory 模块关键组件

### 1. FMemory — 统一内存 API

**定义**: `UnrealMemory.h:93-285`

```cpp
struct FMemory {
    static void* Malloc(SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT);
    static void* Realloc(void* Original, SIZE_T Count, uint32 Alignment = DEFAULT_ALIGNMENT);
    static void  Free(void* Original);
};
```

委托链：`FMemory::Malloc()` → `FMemory_MallocInline()` → `GMalloc->Malloc()`

- `GMalloc` 是全局 `FMalloc*` 指针，指向具体分配器实现
- 集成 AutoRTFM（事务性内存回滚）
- 集成 LLM（Low Level Memory Tracker）追踪

### 2. FMallocBinned2 — 核心分配器

**定义**: `MallocBinned2.h:70-334`

#### 小块分配（≤32KB）

**51 个固定桶大小** 见 `MallocBinned2.cpp:48-92`：

```
16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208,  // +16 步进
256, 288, 320, 384, 448, 512, 560, 624, 720, 816, 912,       // 优化尺寸
1008, 1168, 1392, 1520, 1680, 1872, 2032, 2256, ...          // 更大的桶
...
32768-16                                                       // 最大桶
```

**O(1) 尺寸→桶映射**：`PoolIndex = MemSizeToPoolIndex[Size >> 3]`

**分配路径**:

```
Fast Path（线程缓存命中，~5-10 cycles，无锁）：
  FMemory::Malloc → GMalloc->Malloc → MallocExternalSmall
    → FPerThreadFreeBlockLists::Malloc [无 mutex]

Slow Path（缓存未命中，~100-200 cycles）：
  → 获取 PoolTable.Mutex → 从 ActivePools 分配
  → 如果 pool 耗尽 → 分配新 64KB 页

Large Path（>32KB）：
  → CachedOSPageAllocator → 直接 OS 页分配
```

#### 线程本地缓存（TLS）

**定义**: `MallocBinnedCommon.h:502-649`

```cpp
struct FPerThreadFreeBlockLists {
    FFreeBlockList FreeLists[51];  // 每个桶一个空闲链表
};
```

- 每个线程持有自己的 partial/full bundle（最多 64 项或 64KB）
- 满了的 bundle 回收到全局 recycler
- 释放时先还给 TLS 缓存，满后批量归还全局池

#### 64KB 页池管理

```
64KB Page
┌─────────────────────────────────────┐
│ FFreeBlock Header (bin info, count) │
│ Bin[0] │ Bin[1] │ ... │ Bin[N-1]   │  ← 例如 256字节桶 = 256个bin
└─────────────────────────────────────┘
```

- FPoolInfo 追踪每个页的状态（已分配数、canary 检测、空闲链表）
- Pool 完全空闲时，64KB 页归还 OS（经 CachedOSPageAllocator 缓存）

---

## 四、HAL 模块 — 硬件抽象层

92 个头文件，覆盖：

| 类别 | 文件数 | 关键抽象 |
|------|--------|----------|
| **内存** | 22+ | FMalloc 接口 + 18 种分配器实现 |
| **文件** | 7 | PlatformFile, FileManager |
| **线程** | 10+ | FThread, CriticalSection, Event, Semaphore |
| **原子操作** | 2 | PlatformAtomics |
| **时间** | 1 | PlatformTime |
| **进程** | 1 | PlatformProcess |

每个 `Platform*.h` 在编译时通过 `COMPILED_PLATFORM_HEADER` 宏替换为具体平台实现（Windows/Linux/Mac/iOS/Android）。

---

## 五、Templates 模块 — 模板工具库

83 个头文件，覆盖：

- **类型萃取**（35 个）：`IsArithmetic`, `IsTriviallyCopyConstructible`, `IsPODType` 等
- **类型变换**（9 个）：`RemoveCV`, `Decay`, `MakeSigned` 等
- **智能指针**（6 个）：`TSharedPtr`, `TSharedRef`, `TUniquePtr`
- **函数包装**（5 个）：`TFunction`, `TFunctionRef`, `TUniqueFunction`
- **内存操作**（4 个）：`MemoryOps.h` — `DefaultConstructItems`, `DestructItems`, `RelocateConstructItems`

`RelocateConstructItems` 是关键性能路径 — 对 trivially relocatable 类型做 `memcpy` 代替逐个 move+destruct。

---

## 六、UE 为什么不用 STL？

### 1. 内存控制粒度不够

STL 的 `std::allocator` 模型是"外挂式"的，分配器是容器的模板参数但不侵入存储布局。UE 的 `AllocatorType` 深度侵入容器：
- 控制 `SizeType`（`int32` vs `int64`）
- 控制是否做边界检查（`RequireRangeCheck`）
- 控制内联存储（`TInlineAllocator<N>` 把小数组放栈上）
- 控制 slack 追踪（调试模式追踪内存浪费）

STL 做不到这种程度的定制。

### 2. Trivial Relocatability（按位搬迁）

UE 大量使用 `RelocateConstructItems` — 对 trivially relocatable 类型直接 `memcpy` 整块内存，跳过 move 构造 + 析构。这是 TArray 在 insert/remove/resize 时的关键优化。C++ 标准到 2025 年仍未标准化 trivial relocatability（P1144 提案），STL 容器无法利用这一点。

### 3. 容器布局不适合游戏场景

- `std::unordered_map` 是 node-based，每个元素一次堆分配，cache 极度不友好。UE 的 TMap 用紧凑数组 + 分离哈希表，迭代时是线性扫描。
- `std::vector` 没有内建的 `RemoveAtSwap`（O(1) 无序删除）、没有 slack 管理、没有 `Shrink` 策略。
- `std::string` 没有 FName 这种全局池化 + O(1) 比较的设计。

### 4. 跨平台一致性

STL 实现因编译器而异（MSVC vs libc++ vs libstdc++），行为和性能特征不统一。UE 需要在 Windows/Linux/Mac/iOS/Android/主机平台上保证完全一致的行为，自己实现容器是唯一可靠方案。

### 5. 序列化和反射

UE 的容器深度集成了蓝图反射系统（`ScriptArray.h`、`NameTypes.h`）。TArray/TMap/TSet/FString/FName 都可以直接暴露给蓝图、被 UPROPERTY 标记、参与 GC 追踪。STL 容器无法做到这一点。

### 6. 历史原因

UE 的容器库始于 1998 年（UE1 时代），当时 STL 实现质量参差不齐，在主机平台上更是不可靠。一旦建立了自己的生态，迁移成本极高。

---

## 七、UE 为什么要自己做分配器？

### 1. 系统 malloc 对游戏场景太慢

游戏每帧可能有数万次小块分配/释放。系统 `malloc` 的通用设计（锁竞争、碎片管理）无法满足实时性要求。FMallocBinned2 的设计：
- **线程本地缓存**：fast path 无锁，~5-10 cycles
- **O(1) 桶映射**：`MemSizeToPoolIndex[]` 直接查表
- **批量预取**：一次从全局池取 32 个 bin 填充 TLS 缓存

### 2. 碎片控制

51 个精心选择的桶大小（`MallocBinned2.cpp:48-92`）：
- 每个桶大小精确整除 64KB 页，零页内浪费
- 小于 208 字节的分配按 16 字节步进（覆盖最高频的小对象分配）
- 超过 32KB 直接走 OS 页分配，避免大块碎片化小块池

游戏运行数小时不能因碎片导致 OOM，这在主机上（固定内存）尤为关键。

### 3. 全链路内存追踪

- **LLM（Low Level Memory Tracker）**：集成在 FMemory 层，追踪每类资源的内存占用
- **Slack Tracking**：调试模式追踪每个 TArray 的内存浪费
- **Frame Profiler**：每帧分配统计
- **Callstack Handler**：记录每次分配的调用栈

系统 malloc 的 debug 工具（Valgrind、ASan）是外部的，无法提供这种引擎级的实时追踪。

### 4. 平台特化

不同平台的内存模型差异巨大：
- **主机**：固定内存，必须精确控制（没有虚拟内存 swap）
- **移动端**：内存压力下 OS 会杀进程
- **Fork 支持**：Linux 服务器 fork 模式下需要 COW 感知（FMallocBinned2 有 PreFork/PostFork canary 机制）
- **GPU 内存**：`FMallocBinnedGPU` 专门管理显存

HAL 的分层设计（`MemoryBase.h` 定义 FMalloc 接口 → 18 种实现）允许每个平台选最优分配器。

### 5. 调试分配器矩阵

UE 提供了一整套可插拔的调试分配器，生产代码和调试代码用同一个接口：

| 分配器 | 用途 |
|--------|------|
| `FMallocBinned2/3` | 生产环境 |
| `FMallocStomp` | 检测 buffer overrun（每次分配单独一页 + guard page） |
| `FMallocDoubleFreeFinder` | 检测 double free |
| `FMallocLeakDetection` | 内存泄漏检测 |
| `FMallocFrameProfiler` | 帧级分配 profiling |
| `FMallocPoisonProxy` | 释放后填充 poison 值 |
| `FMallocAnsi` | 退回系统 malloc（对比基准） |

通过启动参数切换分配器（`-stompmalloc`, `-ansimalloc` 等），无需重新编译。

### 6. QuantizeSize — 零浪费对齐

`FMemory::QuantizeSize()` 告诉容器"你要 100 字节，但分配器实际会给你 112 字节（桶大小），你可以用满"。TArray 的增长策略调用此函数来填满分配器桶，消除 slack。STL 的 `std::allocator` 没有这种反向通信机制。
