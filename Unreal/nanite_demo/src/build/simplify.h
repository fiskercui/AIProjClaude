#pragma once

#include "../core/types.h"
#include "cluster.h"

namespace nanite {

// Simplify a cluster's geometry using Garland-Heckbert quadric error metrics.
// Returns the maximum geometric error introduced by the simplification.
//
// Parameters:
//   cluster:            Modified in-place (vertices/indices reduced)
//   targetNumTris:      Desired triangle count after simplification
//   lockBoundaryEdges:  If true, boundary edges are never collapsed
//                       (preserves cluster seams, matching UE5 behavior)
float simplifyCluster(
    Cluster& cluster,
    uint32_t targetNumTris,
    bool lockBoundaryEdges = true
);

} // namespace nanite
