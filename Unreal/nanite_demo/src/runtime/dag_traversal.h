#pragma once

#include "../core/types.h"
#include "../build/cluster_dag.h"
#include "packed_view.h"

namespace nanite {

struct TraversalStats {
    uint32_t totalClustersVisited = 0;
    uint32_t clustersSelected     = 0;
    uint32_t clustersFrustumCulled = 0;
    uint32_t totalTriangles       = 0;
    std::vector<uint32_t> clustersByLevel;  // count per mipLevel
};

struct VisibleCluster {
    uint32_t clusterIndex;
    int32_t  mipLevel;
};

// Traverse the DAG and collect visible clusters for rendering.
// Implements Nanite's core runtime LOD selection:
//   - Start at root groups
//   - Compute projected screen-space error
//   - If error acceptable, render current level; else descend
//   - Apply frustum culling per cluster
void traverseDAG(
    const ClusterDAG& dag,
    const PackedView& view,
    std::vector<VisibleCluster>& outVisible,
    TraversalStats& outStats
);

} // namespace nanite
