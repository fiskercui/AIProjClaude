# Nanite Engine Source Code: Architecture & Mechanism Analysis

## Overview

This codebase contains **~187+ source files** spanning the complete Nanite system. Nanite is UE5's virtualized geometry system that renders billions of triangles by combining **offline mesh preprocessing** with a **GPU-driven runtime pipeline**. The code naturally divides into two major halves: **Build-time** (offline) and **Runtime** (rendering).

---

## Step 1: The Big Picture — Two Pipelines

```
┌─────────────────────────────────────┐     ┌──────────────────────────────────────┐
│         BUILD PIPELINE (Offline)    │     │       RENDER PIPELINE (Runtime)       │
│                                     │     │                                      │
│  Input Mesh                         │     │  FPackedView Setup                   │
│    ↓                                │     │    ↓                                 │
│  Clustering (Cluster.h)             │     │  Instance Culling (GPU)              │
│    ↓                                │     │    ↓                                 │
│  DAG Construction (ClusterDAG.h)    │     │  Hierarchy Traversal (GPU)           │
│    ↓                                │     │    ↓                                 │
│  Graph Partitioning                 │     │  Cluster Culling + HZB (GPU)         │
│    ↓                                │     │    ↓                                 │
│  Encoding & Page Assignment         │     │  Rasterization (HW or SW)            │
│    ↓                                │     │    ↓                                 │
│  Streaming-ready GPU data           │     │  Shading & GBuffer Export            │
│                                     │     │    ↓                                 │
│  Developer/NaniteBuilder/           │     │  Composition                         │
│  Developer/NaniteUtilities/         │     │                                      │
│                                     │     │  Renderer/Private/Nanite/            │
│                                     │     │  Shaders/Private/Nanite/             │
└─────────────────────────────────────┘     └──────────────────────────────────────┘
```

---

## Step 2: Build Pipeline — Offline Mesh Processing

Located in `Engine/Source/Developer/NaniteBuilder/` (40 files) and `Engine/Source/Developer/NaniteUtilities/` (18 files).

### 2.1 Clustering — Cluster.h

The fundamental unit of Nanite is the **Cluster** — a small group of ~128 triangles. When a mesh is imported:

1. The input mesh is split into clusters of triangles
2. Each `FCluster` stores: triangle indices, vertex data (`FVertexArray`), material assignments, bounding boxes, and LOD error metrics
3. `FVertexFormat` defines what each vertex contains (positions, normals, tangents, UVs, colors, bone influences)

**Key file:** `Engine/Source/Developer/NaniteBuilder/Private/Cluster.h`

### 2.2 DAG (Directed Acyclic Graph) — ClusterDAG.h

Clusters are organized into a **hierarchical LOD structure**:

1. The finest clusters (LOD 0) form the leaf nodes
2. Groups of clusters are **simplified** (merged and decimated) to form parent clusters
3. This repeats until a single root cluster remains
4. Each level records an **error metric** — the geometric error introduced by simplification
5. At runtime, the GPU traverses this DAG and selects the appropriate LOD per-cluster based on screen-space error

**Key file:** `Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.h`

### 2.3 Graph Partitioning — GraphPartitioner.h

Determines which triangles belong to which cluster. Uses spatial locality and connectivity to ensure clusters are spatially coherent (important for culling efficiency).

**Key file:** `Engine/Source/Developer/NaniteBuilder/Private/GraphPartitioner.h`

### 2.4 Encoding — `Private/Encode/` (14 files)

After building the DAG, the data is encoded into a GPU-streaming-friendly format:

| File | Purpose |
|------|---------|
| `NaniteEncode.cpp` | Main encoding orchestrator |
| `NaniteEncodeGeometryData.cpp` | Geometry (positions, normals) encoding |
| `NaniteEncodeHierarchy.cpp` | BVH hierarchy encoding |
| `NaniteEncodeMaterial.cpp` | Material slot packing |
| `NaniteEncodePageAssignment.cpp` | Memory page assignment for streaming |
| `NaniteEncodeTriStrip.cpp` | Triangle strip compression |
| `NaniteEncodeSkinning.cpp` | Skeletal mesh skinning data |
| `NaniteEncodeRayTracing.cpp` | Ray tracing acceleration structures |

Data is organized into fixed-size **pages** that can be streamed in/out independently.

**Key directory:** `Engine/Source/Developer/NaniteBuilder/Private/Encode/`

---

## Step 3: Runtime Rendering Pipeline

Located in `Engine/Source/Runtime/Renderer/Private/Nanite/` (33 files) and `Engine/Shaders/Private/Nanite/` (53 shaders).

The runtime pipeline is **entirely GPU-driven**. The CPU's role is minimal — it sets up views and dispatches compute shaders. The GPU does all the work.

### 3.1 View Setup — NaniteShared.h

`FPackedView` is the core data structure passed to the GPU. It contains:

- View/projection matrices (current + previous frame for motion vectors)
- LOD scale parameters (controls when to switch LOD levels)
- HZB (Hierarchical Z-Buffer) test rects for occlusion culling
- Culling origin and distance parameters

`FPackedViewArray` manages multiple views for multi-view rendering (e.g., shadow maps render many views simultaneously).

**Key file:** `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteShared.h`

### 3.2 Instance Culling (GPU)

First pass: cull entire mesh **instances** that are outside the frustum or too small to matter. This is a coarse pre-filter before the expensive per-cluster work.

**Key shader:** `Engine/Shaders/Private/Nanite/NaniteInstanceCulling.usf`

### 3.3 Hierarchy Traversal (GPU)

For surviving instances, the GPU **traverses the cluster DAG**:

1. Start at the root of each mesh's BVH
2. At each node, compute the projected screen-space error
3. If the error is small enough, **use this LOD level** (stop descending)
4. If the error is too large, descend to children
5. This selects the optimal LOD **per-cluster**, not per-mesh — different parts of the same mesh can be at different detail levels

**Key shader:** `Engine/Shaders/Private/Nanite/NaniteHierarchyTraversal.ush`

### 3.4 Cluster Culling + HZB

Selected clusters are further culled:

- **Frustum culling**: discard clusters outside the camera frustum
- **HZB occlusion culling**: test cluster bounds against the Hierarchical Z-Buffer from the previous frame to discard occluded clusters
- This is managed on the CPU side by `NaniteCullRaster.h/cpp`

**Key shaders:**
- `Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf`
- `Engine/Shaders/Private/Nanite/NaniteHZBCull.ush`

**Key CPU file:** `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteCullRaster.h`

### 3.5 Rasterization

Nanite uses a **dual rasterization** strategy:

| Mode | When Used | Implementation |
|------|-----------|----------------|
| **Hardware Rasterizer** | Larger triangles (> ~few pixels) | Standard hardware rasterization pipeline |
| **Software Rasterizer** | Micro-triangles (< ~few pixels) | Custom compute shader rasterizer |

The output is a **Visibility Buffer** (not a traditional GBuffer yet). Each pixel stores:

- **Cluster ID** + **Triangle ID** — a compact reference to the exact triangle visible at each pixel

**Key shaders:**
- `Engine/Shaders/Private/Nanite/NaniteRasterizer.usf`
- `Engine/Shaders/Private/Nanite/NaniteWritePixel.ush`
- `Engine/Shaders/Private/Nanite/NaniteRasterBinning.usf`

### 3.6 Material Shading

After rasterization, shading is **deferred**:

1. Pixels are binned by material
2. Each material bin is shaded in a single draw call
3. Vertex attributes are reconstructed from the Visibility Buffer on-the-fly
4. Results are exported to the GBuffer

Key pipeline structures:

- `FNaniteRasterPipeline` — per-material rasterization settings (displacement, WPO, skinning flags)
- `FNaniteShadingPipeline` — per-material shading settings (base pass, Lumen, two-sided, masked)
- `FNaniteRasterBin` / `FNaniteShadingBin` — bin identifiers for pipeline dispatch

**Key files:**
- `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteMaterials.h`
- `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteShading.h`

**Key shaders:**
- `Engine/Shaders/Private/Nanite/NaniteShadeBinning.usf`
- `Engine/Shaders/Private/Nanite/NaniteExportGBuffer.usf`
- `Engine/Shaders/Private/Nanite/NaniteAttributeDecode.ush`
- `Engine/Shaders/Private/Nanite/NaniteVertexFetch.ush`

### 3.7 Composition — NaniteComposition.h

The final step composites Nanite-rendered geometry with the rest of the scene (non-Nanite geometry, sky, translucent objects, etc.).

**Key file:** `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteComposition.h`

---

## Step 4: Streaming System

Located in `Engine/Source/Runtime/Engine/Private/Nanite/`.

| File | Purpose |
|------|---------|
| `NaniteStreamingPageUploader.h/cpp` | Uploads geometry pages from disk to GPU memory |
| `NaniteReadbackManager.h/cpp` | Reads back GPU feedback about which pages are needed |
| `NaniteStreamingShared.h` | Streaming constants and shared structures |
| `NaniteOrderedScatterUpdater.h/cpp` | Ordered scatter buffer updates |

The streaming loop:

1. GPU rendering produces **feedback** about which clusters/pages were requested but not resident
2. `NaniteReadbackManager` reads this feedback back to CPU
3. CPU decides which pages to stream in (from disk) or evict
4. `NaniteStreamingPageUploader` uploads new pages to GPU memory
5. GPU shaders (`NaniteStreaming.usf`) manage page table updates

---

## Step 5: The Complete Frame Flow

```
Frame N:
  ┌─────────────────────────────────────────────────────┐
  │ CPU: Set up FPackedView[], dispatch GPU work        │
  └──────────────┬──────────────────────────────────────┘
                 ↓
  ┌──────────────────────────────────────────────────────┐
  │ GPU Pass 1: Instance Culling                         │
  │   NaniteInstanceCulling.usf                          │
  │   → Discard entire meshes outside frustum            │
  ├──────────────────────────────────────────────────────┤
  │ GPU Pass 2: Hierarchy Traversal + LOD Selection      │
  │   NaniteHierarchyTraversal.ush                       │
  │   → Walk cluster DAG, select per-cluster LOD         │
  ├──────────────────────────────────────────────────────┤
  │ GPU Pass 3: Cluster Culling (Frustum + HZB)          │
  │   NaniteClusterCulling.usf + NaniteHZBCull.ush       │
  │   → Remove occluded/off-screen clusters              │
  ├──────────────────────────────────────────────────────┤
  │ GPU Pass 4: Rasterization (HW + SW)                  │
  │   NaniteRasterizer.usf + NaniteWritePixel.ush        │
  │   → Produce Visibility Buffer (ClusterID+TriID)      │
  ├──────────────────────────────────────────────────────┤
  │ GPU Pass 5: Shade Binning + Material Evaluation      │
  │   NaniteShadeBinning.usf + NaniteExportGBuffer.usf   │
  │   → Reconstruct attributes, run material shaders     │
  ├──────────────────────────────────────────────────────┤
  │ GPU Pass 6: Composition                              │
  │   → Merge with non-Nanite scene                      │
  ├──────────────────────────────────────────────────────┤
  │ GPU: Streaming Feedback                              │
  │   NaniteStreaming.usf                                │
  │   → Report which pages are needed for next frame     │
  └──────────────────────────────────────────────────────┘
                 ↓
  ┌──────────────────────────────────────────────────────┐
  │ CPU: Read feedback, schedule page streaming          │
  └──────────────────────────────────────────────────────┘
```

---

## Step 6: Advanced Features

### 6.1 Ray Tracing

Nanite builds ray tracing acceleration structures from the selected LOD clusters, integrating with Lumen and hardware RT.

**Key files:**
- `Engine/Source/Runtime/Renderer/Private/Nanite/NaniteRayTracing.h`
- `Engine/Shaders/Private/Nanite/NaniteRayTracing.usf`

### 6.2 Tessellation & Displacement

Runtime tessellation with displacement mapping, supported by the `NaniteDisplacedMesh` plugin.

**Key shaders:**
- `Engine/Shaders/Private/Nanite/NaniteTessellation.ush`
- `Engine/Shaders/Private/Nanite/NaniteDice.ush`

### 6.3 Skeletal Mesh / Skinning

Nanite supports animated skeletal meshes with GPU-side vertex deformation.

**Key files:**
- `Engine/Source/Developer/NaniteBuilder/Private/Encode/NaniteEncodeSkinning.cpp`
- `Engine/Shaders/Private/Nanite/NaniteVertexDeformation.ush`

### 6.4 Voxel Rendering

Voxel-based rendering for certain use cases (auto-voxelization, brick structures).

**Key files:**
- `Engine/Source/Runtime/Renderer/Private/Nanite/Voxel.h`
- `Engine/Shaders/Private/Nanite/Voxel/` (subdirectory)

### 6.5 Niagara Integration

Allows Niagara particle systems to render instances via Nanite for massive particle counts with full geometric detail.

**Key plugin:** `Engine/Plugins/FX/NiagaraNanite/`

---

## Step 7: Directory Structure Summary

```
Engine/
├── Source/
│   ├── Runtime/
│   │   ├── Engine/
│   │   │   ├── Internal/Nanite/          # Internal headers (FFixupChunk)
│   │   │   └── Private/Nanite/           # Streaming system (7 files)
│   │   └── Renderer/
│   │       └── Private/Nanite/           # Core rendering pipeline (33 files)
│   └── Developer/
│       ├── NaniteBuilder/                # Offline build pipeline (40 files)
│       │   └── Private/Encode/           # Data encoding (14 files)
│       └── NaniteUtilities/              # Math & utility libraries (18 files)
├── Shaders/
│   └── Private/Nanite/                   # GPU shaders (53 files)
│       └── Voxel/                        # Voxel shaders
└── Plugins/
    ├── Experimental/
    │   ├── NaniteAssemblyEditorUtils/    # Assembly editor tools (8 files)
    │   ├── NaniteDisplacedMesh/          # Displacement mesh support (13 files)
    │   └── PCGInterops/                  # PCG integration (5 files)
    └── FX/
        └── NiagaraNanite/                # Niagara particle integration (9 files)
```

**Total: ~187+ source files**

---

## Key Data Structures Reference

| Structure | File | Purpose |
|-----------|------|---------|
| `FPackedView` | NaniteShared.h | View matrices, LOD scales, culling params — passed to GPU |
| `FPackedViewArray` | NaniteShared.h | Multi-view management for shadow maps etc. |
| `FRasterContext` | NaniteCullRaster.h | Rasterization state (VisBuffer/DepthOnly, HW/SW mode) |
| `FSharedContext` | NaniteCullRaster.h | Shared rendering context (shaders, feature level) |
| `FNaniteRasterPipeline` | NaniteMaterials.h | Per-material raster settings |
| `FNaniteShadingPipeline` | NaniteMaterials.h | Per-material shading settings |
| `FNaniteRasterBin` | NaniteDrawList.h | Raster bin identifier |
| `FNaniteShadingBin` | NaniteDrawList.h | Shading bin identifier |
| `FNaniteVisibility` | NaniteVisibility.h | Per-view visibility state |
| `FCluster` | Cluster.h | Atomic cluster unit (~128 triangles) |
| `FClusterDAG` | ClusterDAG.h | LOD hierarchy (directed acyclic graph) |
| `FClusterRef` | Cluster.h | Reference to a cluster within the DAG |
| `FVertexFormat` | Cluster.h | Vertex attribute layout |
| `FVertexArray` | Cluster.h | Packed vertex data storage |

---

## Recommended Study Order

1. **Cluster.h** — Understand what a cluster is (the atomic unit)
2. **ClusterDAG.h** — Understand the LOD hierarchy
3. **NaniteShared.h** — View setup and core runtime structures
4. **NaniteCullRaster.h** — CPU-side culling/raster orchestration
5. **NaniteHierarchyTraversal.ush** — GPU LOD selection logic
6. **NaniteClusterCulling.usf** — GPU cluster culling
7. **NaniteRasterizer.usf** — The dual HW/SW rasterizer
8. **NaniteShadeBinning.usf** + **NaniteExportGBuffer.usf** — Deferred material evaluation
9. **Encode/ directory** — How all of the above gets packed for the GPU
10. **Streaming files** — How data flows from disk to GPU on demand
