#pragma once

#include "../core/types.h"
#include "../core/mesh_loader.h"

namespace nanite {

struct Cluster {
    // --- Geometry ---
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;     // local indices into vertices[]
    uint32_t              numTris = 0;

    // --- Bounds ---
    AABB           bounds;
    BoundingSphere sphereBounds;
    BoundingSphere lodBounds;       // used for projected LOD error test

    // --- LOD metadata ---
    float    lodError    = 0.0f;    // max geometric error from simplification
    float    edgeLength  = 0.0f;    // average edge length
    float    surfaceArea = 0.0f;
    int32_t  mipLevel    = 0;       // 0 = leaf (finest), increases toward root

    // --- DAG linkage ---
    uint32_t groupIndex           = INVALID_INDEX; // parent group
    uint32_t generatingGroupIndex = INVALID_INDEX; // group that generated this cluster

    // --- Boundary edges (for simplification locking) ---
    // Per-edge flag: true = boundary edge (shared with another cluster or open)
    std::vector<bool> boundaryEdges; // size = numTris * 3

    // Recompute bounds, sphereBounds, surfaceArea, edgeLength from geometry
    void computeBoundsAndMetrics();

    // Identify boundary edges (edges with only one adjacent triangle)
    void computeBoundaryEdges();
};

// Morton code for 3D spatial sorting
uint32_t mortonEncode(const glm::vec3& normalizedPos);

// Build leaf clusters from a raw mesh using Morton-code spatial sorting.
// Returns indices of newly created clusters in outClusters.
std::vector<uint32_t> buildLeafClusters(
    const RawMesh& mesh,
    std::vector<Cluster>& outClusters
);

// Merge multiple clusters into one combined cluster (geometry union).
// Does NOT simplify - just concatenates and welds vertices.
Cluster mergeClusters(
    const std::vector<Cluster>& allClusters,
    const std::vector<uint32_t>& clusterIndices
);

// Split a single cluster into multiple clusters of at most CLUSTER_SIZE triangles.
// Uses Morton-code spatial partitioning.
std::vector<Cluster> splitCluster(const Cluster& merged);

} // namespace nanite
