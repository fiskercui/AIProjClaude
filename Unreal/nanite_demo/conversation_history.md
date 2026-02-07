# Nanite Demo - 开发对话历史

## 项目概述

基于 Unreal Engine 5 Nanite 虚拟化几何体系统，用 C++ 实现了一个简化版的独立演示程序。该项目展示了 Nanite 的核心概念：网格聚类（Mesh Clustering）、层次化 LOD DAG 构建（Hierarchical LOD DAG）、屏幕空间误差 LOD 选择、视锥体裁剪（Frustum Culling）以及 CPU 软件光栅化与可见性缓冲区（Visibility Buffer）。

- **渲染方式**: CPU 软件光栅化（非 GPU 计算着色器）
- **窗口/显示**: OpenGL + GLFW
- **数学库**: GLM
- **构建系统**: CMake + FetchContent（自动拉取 GLFW、GLM 依赖）

---

## 开发过程

### 第一阶段：需求分析与架构设计

1. 用户提供了 `Nanite_Architecture_Analysis.md` 分析文档，并要求基于 UE5 Nanite 实现简化版本。
2. 分析了 UE5 源码目录 `e:\AIProjClaude\Unreal\Engine\` 中的核心文件：
   - `Cluster.h`, `ClusterDAG.h`, `GraphPartitioner.h`（构建管线）
   - `NaniteShared.h`, `NaniteCullRaster.h`, `NaniteHierarchyTraversal.ush`（运行时）
   - `NaniteBuilder.cpp`、简化与误差度量相关代码
3. 用户选择了 **CPU-only demo** 和 **OpenGL + GLFW** 方案。
4. 制定了详细的实现计划（9个步骤），用户审批通过。

### 第二阶段：核心代码实现

按照计划依次实现了以下模块：

#### 基础类型 (`src/core/types.h`)
- `AABB`：轴对齐包围盒，支持扩展、求中心、求范围
- `BoundingSphere`：包围球，支持从点集、AABB、多个球体构建和合并
- `Vertex`：顶点（位置+法线）
- 常量：`CLUSTER_SIZE=128`, `MAX_GROUP_SIZE=32`, `MIN_GROUP_SIZE=8`

#### OBJ 加载器 (`src/core/mesh_loader.h/.cpp`)
- 解析 `v`/`vn`/`f` 行
- 支持多种面格式（`v`, `v/vt`, `v/vt/vn`, `v//vn`）
- 顶点去重
- 缺失法线时自动生成

#### 网格聚类 (`src/build/cluster.h/.cpp`)
- `Cluster` 结构体：几何数据、边界、LOD 元数据、DAG 链接、边界边
- `buildLeafClusters()`：Morton 码排序三角形质心，每128个三角形切分为一个簇
- `mergeClusters()`：合并多个簇的几何体，通过量化位置焊接相同顶点
- `splitCluster()`：对简化后的网格重新 Morton 码分割
- `computeBoundaryEdges()`：边哈希表查找仅有一个相邻三角形的边

#### 网格简化 (`src/build/simplify.h/.cpp`)
- Garland-Heckbert 二次误差度量（Quadric Error Metrics）
- `Quadric` 结构体：10元素对称4x4矩阵，`evaluate()` 和 `solveOptimal()` 方法
- 优先队列边折叠（最小堆按代价排序）
- 边界边锁定（无限代价）保护簇接缝
- 面翻转检测：每次折叠前验证
- 返回 `sqrt(maxError)` 作为几何距离

#### DAG 层次构建 (`src/build/cluster_dag.h/.cpp`)
- `ClusterGroup`：包围体、LOD 边界、父级 LOD 误差（单调递增）、子簇列表、父簇列表
- `ClusterDAG::build(mesh)` 主构建流程：
  1. 创建叶子簇
  2. 循环直到只剩1个簇：
     - `groupClusters()`：Morton 码空间分组（8-32个一组）
     - `reduceGroup()`：合并子簇 → 简化到约一半三角形 → 重新分割为父簇
     - `parentLODError = max(parentLODError, simplifyError)`（单调性保证）
  3. 标记最终组为根
- 关键不变量：`parentLODError >= max(childError)` 在每个层级

#### 视图设置 (`src/runtime/packed_view.h/.cpp`)
- `PackedView`：视图矩阵、投影矩阵、视图投影矩阵、LOD 缩放、6个视锥体平面
- LOD 缩放公式（来自 UE5）：`lodScale = (0.5 * projMatrix[1][1] * viewHeight) / maxPixelsPerEdge`
- 视锥体平面提取：Gribb-Hartmann 方法

#### DAG 遍历与 LOD 选择 (`src/runtime/dag_traversal.h/.cpp`)
- 基于栈的自顶向下组遍历
- LOD 判定：`computeProjectedError()` 返回 `pixelsPerUnit * lodError`，与 `maxPixelsPerEdge` 比较
- 如果父级可接受 → 渲染 parentClusters；否则 → 通过 `generatingGroupIndex` 下降到子级
- 每个簇进行 AABB 视锥体裁剪

#### 软件光栅化器 (`src/runtime/rasterizer.h/.cpp`)
- `Framebuffer`：颜色（RGBA8）、深度、可见性缓冲区（每像素 clusterID+triID）
- 6种渲染模式：Solid、LODColors、ClusterColors、Wireframe、VisBuffer、Depth
- 流程：顶点变换 → 透视除法 → 视口变换 → 背面剔除 → 边函数光栅化 → 深度测试 → 可见性缓冲写入

#### 相机 (`src/render/camera.h/.cpp`)
- FPS 相机：WASD/QE 移动、鼠标观察、滚轮缩放 FOV

#### 显示 (`src/render/display.h/.cpp`)
- GLFW 窗口 + GLAD OpenGL 加载
- CPU 帧缓冲通过 `glTexSubImage2D` 上传为 GL 纹理
- 全屏四边形渲染

#### 主程序 (`src/main.cpp`)
- GLFW 回调：键盘（1-6 渲染模式、+/- 质量、Tab 鼠标捕获、Esc 退出）、鼠标、滚轮
- 主循环：轮询事件 → 更新相机 → 设置 PackedView → 遍历 DAG → 光栅化 → 显示
- 相机初始位置：网格半径的 1.2 倍处

### 第三阶段：测试资产生成

- `assets/cube.obj`：简单立方体（8顶点，12三角形）
- `assets/sphere.obj`：通过 Python 脚本 `tools/gen_sphere.py` 生成的二十面体球体（5次细分，10242顶点，20480三角形）

### 第四阶段：编译错误修复

| 错误 | 原因 | 修复 |
|------|------|------|
| `NULL` undeclared in glad.c | MSVC 需要显式包含 | 添加 `#include <stddef.h>` |
| Windows `GL/gl.h` 重定义冲突 | Windows SDK 的 GL/gl.h 将 GL 1.0/1.1 函数声明为 `__declspec(dllimport)` | 在 glad.h 添加 `#define __gl_h_` 等守卫宏 |
| `std::iota` not found | MSVC 需要显式包含 | 在 simplify.cpp 添加 `#include <numeric>` |

### 第五阶段：LOD 选择 Bug 修复

**问题**：运行时只有1个簇可见（总是选择最粗糙的 LOD 级别）。

**根因分析**：
- 原始条件 `projScale * parentLODError <= view.lodScale` 将投影误差（像素）与 `viewToPixels / maxPixelsPerEdge`（约869）比较
- 这个阈值过大，导致几乎所有簇都满足"可接受"条件，不会继续细化

**修复**：
- 重写 `dag_traversal.cpp`：`computeProjectedError()` 返回 `pixelsPerUnit * lodError`
- 直接与 `view.maxPixelsPerEdge` 比较
- 相机位置从 2.5x 半径调整到 1.2x 半径

**修复后结果**：150/321 簇可见，19170 三角形，~10 FPS，正确的多级 LOD 选择。

---

## 最终运行结果

```
=== Nanite Demo - Simplified Virtualized Geometry ===

Loading mesh: assets/sphere.obj
  OBJ loaded: 10242 vertices, 20480 triangles
Mesh: 10242 vertices, 20480 triangles

--- Building Cluster DAG ---
Building leaf clusters...
  Level 0: 160 leaf clusters (20480 triangles)
  Level 1: 5 groups from 160 clusters -> 80 parent clusters
  Level 2: 2 groups from 80 clusters -> 40 parent clusters
  Level 3: 1 groups from 40 clusters -> 20 parent clusters
  Level 4: 1 groups from 20 clusters -> 10 parent clusters
  Level 5: 1 groups from 10 clusters -> 5 parent clusters
  Level 6: 1 groups from 5 clusters -> 3 parent clusters
  Level 7: 1 groups from 3 clusters -> 2 parent clusters
  Level 8: 1 groups from 2 clusters -> 1 parent clusters
DAG Summary: 321 clusters, 13 groups, 9 levels
  Level 0: 160 clusters, 20480 triangles
  Level 1: 80 clusters, 10238 triangles
  Level 2: 40 clusters, 5118 triangles
  Level 3: 20 clusters, 2559 triangles
  Level 4: 10 clusters, 1279 triangles
  Level 5: 5 clusters, 639 triangles
  Level 6: 3 clusters, 319 triangles
  Level 7: 2 clusters, 159 triangles
  Level 8: 1 clusters, 79 triangles
Build complete: 135.7 ms

[LOD Colors] FPS: 10.0 | Clusters: 150/321 visible | Tris: 19170 | Culled: 0 | PxPerEdge: 1.00
```

---

## 项目文件结构

```
e:\AIProjClaude\Unreal\nanite_demo\
├── CMakeLists.txt                          # 构建系统
├── assets/
│   ├── cube.obj                            # 测试立方体
│   └── sphere.obj                          # 测试球体 (20K 三角形)
├── tools/
│   └── gen_sphere.py                       # 球体生成脚本
└── src/
    ├── main.cpp                            # 入口点、主循环
    ├── core/
    │   ├── types.h                         # 基础类型 (AABB, BoundingSphere, Vertex)
    │   ├── mesh_loader.h                   # OBJ 加载器声明
    │   └── mesh_loader.cpp                 # OBJ 加载器实现
    ├── build/
    │   ├── cluster.h                       # 簇结构体、构建函数声明
    │   ├── cluster.cpp                     # 簇构建实现 (Morton码、分割、合并)
    │   ├── cluster_dag.h                   # DAG 层次结构声明
    │   ├── cluster_dag.cpp                 # DAG 构建实现 (分组、归约)
    │   ├── simplify.h                      # 网格简化声明
    │   └── simplify.cpp                    # Garland-Heckbert 二次误差简化
    ├── runtime/
    │   ├── packed_view.h                   # 视图结构体声明
    │   ├── packed_view.cpp                 # 视图设置 (矩阵、视锥体平面)
    │   ├── dag_traversal.h                 # DAG 遍历声明
    │   ├── dag_traversal.cpp               # LOD 选择、视锥体裁剪
    │   ├── rasterizer.h                    # 光栅化器声明
    │   └── rasterizer.cpp                  # CPU 软件光栅化 + 可见性缓冲
    ├── render/
    │   ├── display.h                       # OpenGL 显示声明
    │   ├── display.cpp                     # GLFW 窗口、纹理上传
    │   ├── camera.h                        # FPS 相机声明
    │   └── camera.cpp                      # 相机实现
    └── glad/
        ├── include/
        │   ├── glad/
        │   │   └── glad.h                  # OpenGL 3.3 Core 函数声明
        │   └── KHR/
        │       └── khrplatform.h           # Khronos 平台类型
        └── glad.c                          # OpenGL 函数加载实现
```

---

## 与真实 Nanite 的简化对比

| 方面 | 真实 Nanite (UE5) | 本演示 |
|------|-------------------|--------|
| 计算 | GPU 驱动的计算着色器 | 全部 CPU |
| 聚类 | METIS 图分区 | Morton 码排序 |
| 简化 | 完整二次误差 + UV/材质边界 | 基础 Garland-Heckbert |
| 光栅化 | 双硬件+软件 GPU 光栅化器 | CPU 扫描线光栅化器 |
| 遮挡 | 上一帧的层次深度缓冲 (HZB) | 仅视锥体裁剪 |
| 流送 | 基于页的 GPU 内存流送 | 整个网格在内存中 |
| 材质 | 完整 PBR 延迟着色 | 平面着色 / 调试颜色 |
| 规模 | 数十亿三角形 | ~10万三角形 |

---

## 操作控制

| 按键 | 功能 |
|------|------|
| Tab | 切换鼠标捕获 |
| WASD/QE | 移动相机 |
| 鼠标 | 环视（捕获时） |
| 滚轮 | 缩放（FOV） |
| 1-6 | 渲染模式（Solid, LOD, Cluster, Wire, VisBuf, Depth） |
| +/- | 调整 LOD 质量（maxPixelsPerEdge） |
| [ / ] | 调整相机速度 |
| Esc | 退出 |

---

## 核心算法说明

### LOD 选择（屏幕空间误差）

```
projectedError = pixelsPerUnit * lodError
pixelsPerUnit = (0.5 * proj[1][1] * viewHeight) / depth

if projectedError <= maxPixelsPerEdge:
    使用当前（粗糙）LOD → 渲染父簇
else:
    需要更高精度 → 下降到子簇
```

### DAG 遍历

```
1. 从根组开始，将根组压入栈
2. 从栈中弹出一个组
3. 计算该组的投影误差
4. 如果误差可接受 → 渲染该组的父簇（粗糙 LOD）
5. 如果误差不可接受 → 对子簇：
   a. 视锥体裁剪
   b. 如果子簇有生成组 → 将生成组压入栈
   c. 如果是叶子簇 → 直接渲染
6. 重复直到栈为空
```

### Morton 码空间排序

用于聚类和分组时保持空间局部性。将3D坐标量化后交错比特位，相近的空间位置映射到相近的 Morton 码值，排序后切分可得到空间紧凑的簇。


cd nanite_demo
cmake -B build
cmake --build build --config Release
./build/Release/NaniteDemo.exe assets/sphere.obj