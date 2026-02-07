#include "cluster.h"
#include <unordered_map>
#include <numeric>

namespace nanite {

// ---------- Morton Code ----------

static uint32_t expandBits(uint32_t v) {
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v <<  8)) & 0x0300F00F;
    v = (v | (v <<  4)) & 0x030C30C3;
    v = (v | (v <<  2)) & 0x09249249;
    return v;
}

uint32_t mortonEncode(const glm::vec3& normalizedPos) {
    uint32_t x = (uint32_t)std::min(1023.0f, std::max(0.0f, normalizedPos.x * 1023.0f));
    uint32_t y = (uint32_t)std::min(1023.0f, std::max(0.0f, normalizedPos.y * 1023.0f));
    uint32_t z = (uint32_t)std::min(1023.0f, std::max(0.0f, normalizedPos.z * 1023.0f));
    return expandBits(x) | (expandBits(y) << 1) | (expandBits(z) << 2);
}

// ---------- Cluster Methods ----------

void Cluster::computeBoundsAndMetrics() {
    bounds = {};
    surfaceArea = 0.0f;
    edgeLength = 0.0f;
    numTris = (uint32_t)(indices.size() / 3);

    if (vertices.empty() || indices.empty()) return;

    for (auto& v : vertices) {
        bounds.expand(v.position);
    }
    sphereBounds = BoundingSphere::fromAABB(bounds);

    float totalEdgeLen = 0.0f;
    uint32_t edgeCount = 0;
    for (uint32_t i = 0; i < numTris; i++) {
        const glm::vec3& p0 = vertices[indices[i * 3 + 0]].position;
        const glm::vec3& p1 = vertices[indices[i * 3 + 1]].position;
        const glm::vec3& p2 = vertices[indices[i * 3 + 2]].position;

        glm::vec3 cross = glm::cross(p1 - p0, p2 - p0);
        surfaceArea += glm::length(cross) * 0.5f;

        totalEdgeLen += glm::distance(p0, p1);
        totalEdgeLen += glm::distance(p1, p2);
        totalEdgeLen += glm::distance(p2, p0);
        edgeCount += 3;
    }
    edgeLength = (edgeCount > 0) ? (totalEdgeLen / (float)edgeCount) : 0.0f;

    // lodBounds defaults to sphereBounds at leaf level
    if (lodBounds.radius <= 0.0f) {
        lodBounds = sphereBounds;
    }
}

void Cluster::computeBoundaryEdges() {
    boundaryEdges.resize(numTris * 3, false);

    // Build edge -> triangle count map
    // Edge key: sorted pair of vertex positions (quantized)
    struct EdgeKey {
        uint64_t a, b;
        bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
    };
    struct EdgeHash {
        size_t operator()(const EdgeKey& k) const {
            return std::hash<uint64_t>()(k.a) ^ (std::hash<uint64_t>()(k.b) * 2654435761u);
        }
    };

    auto posToKey = [](const glm::vec3& p) -> uint64_t {
        // Quantize to avoid floating point issues
        int32_t x = (int32_t)(p.x * 10000.0f);
        int32_t y = (int32_t)(p.y * 10000.0f);
        int32_t z = (int32_t)(p.z * 10000.0f);
        return ((uint64_t)(uint32_t)x << 40) | ((uint64_t)(uint32_t)y << 20) | (uint64_t)(uint32_t)z;
    };

    auto makeEdgeKey = [&](uint32_t i0, uint32_t i1) -> EdgeKey {
        uint64_t a = posToKey(vertices[i0].position);
        uint64_t b = posToKey(vertices[i1].position);
        if (a > b) std::swap(a, b);
        return { a, b };
    };

    std::unordered_map<EdgeKey, uint32_t, EdgeHash> edgeCount;
    for (uint32_t t = 0; t < numTris; t++) {
        for (int e = 0; e < 3; e++) {
            uint32_t i0 = indices[t * 3 + e];
            uint32_t i1 = indices[t * 3 + ((e + 1) % 3)];
            edgeCount[makeEdgeKey(i0, i1)]++;
        }
    }

    for (uint32_t t = 0; t < numTris; t++) {
        for (int e = 0; e < 3; e++) {
            uint32_t i0 = indices[t * 3 + e];
            uint32_t i1 = indices[t * 3 + ((e + 1) % 3)];
            EdgeKey key = makeEdgeKey(i0, i1);
            auto it = edgeCount.find(key);
            // Boundary: edge has only one adjacent triangle
            boundaryEdges[t * 3 + e] = (it == edgeCount.end() || it->second < 2);
        }
    }
}

// ---------- Build Leaf Clusters ----------

std::vector<uint32_t> buildLeafClusters(
    const RawMesh& mesh,
    std::vector<Cluster>& outClusters)
{
    uint32_t numTris = mesh.numTris();
    if (numTris == 0) return {};

    // Compute triangle centroids and Morton codes
    struct TriInfo {
        uint32_t triIndex;
        uint32_t mortonCode;
    };
    std::vector<TriInfo> triInfos(numTris);
    glm::vec3 boundsSize = mesh.bounds.max - mesh.bounds.min;
    glm::vec3 boundsMin  = mesh.bounds.min;
    // Avoid division by zero
    for (int i = 0; i < 3; i++) {
        if (boundsSize[i] < 1e-8f) boundsSize[i] = 1.0f;
    }

    for (uint32_t t = 0; t < numTris; t++) {
        const glm::vec3& p0 = mesh.vertices[mesh.indices[t * 3 + 0]].position;
        const glm::vec3& p1 = mesh.vertices[mesh.indices[t * 3 + 1]].position;
        const glm::vec3& p2 = mesh.vertices[mesh.indices[t * 3 + 2]].position;
        glm::vec3 centroid = (p0 + p1 + p2) / 3.0f;
        glm::vec3 normalized = (centroid - boundsMin) / boundsSize;
        triInfos[t] = { t, mortonEncode(normalized) };
    }

    // Sort by Morton code for spatial locality
    std::sort(triInfos.begin(), triInfos.end(),
        [](const TriInfo& a, const TriInfo& b) { return a.mortonCode < b.mortonCode; });

    // Cut into clusters of CLUSTER_SIZE
    std::vector<uint32_t> newClusterIndices;
    uint32_t baseCluster = (uint32_t)outClusters.size();

    for (uint32_t start = 0; start < numTris; start += CLUSTER_SIZE) {
        uint32_t end = std::min(start + CLUSTER_SIZE, numTris);
        uint32_t clusterTriCount = end - start;

        Cluster cluster;

        // Gather unique vertices for this cluster
        std::unordered_map<uint32_t, uint32_t> globalToLocal;
        for (uint32_t i = start; i < end; i++) {
            uint32_t origTri = triInfos[i].triIndex;
            for (int v = 0; v < 3; v++) {
                uint32_t globalIdx = mesh.indices[origTri * 3 + v];
                if (globalToLocal.find(globalIdx) == globalToLocal.end()) {
                    uint32_t localIdx = (uint32_t)cluster.vertices.size();
                    cluster.vertices.push_back(mesh.vertices[globalIdx]);
                    globalToLocal[globalIdx] = localIdx;
                }
                cluster.indices.push_back(globalToLocal[globalIdx]);
            }
        }

        cluster.mipLevel = 0;
        cluster.lodError = 0.0f;
        cluster.computeBoundsAndMetrics();
        cluster.computeBoundaryEdges();
        cluster.edgeLength = -cluster.edgeLength; // negative = leaf marker

        uint32_t clusterIdx = (uint32_t)outClusters.size();
        outClusters.push_back(std::move(cluster));
        newClusterIndices.push_back(clusterIdx);
    }

    return newClusterIndices;
}

// ---------- Merge Clusters ----------

Cluster mergeClusters(
    const std::vector<Cluster>& allClusters,
    const std::vector<uint32_t>& clusterIndices)
{
    Cluster merged;

    // Vertex welding: merge by quantized position
    struct PosKey {
        int32_t x, y, z;
        bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const {
            return std::hash<int32_t>()(k.x) ^ (std::hash<int32_t>()(k.y) << 10) ^ (std::hash<int32_t>()(k.z) << 20);
        }
    };
    std::unordered_map<PosKey, uint32_t, PosKeyHash> weldMap;

    auto toPosKey = [](const glm::vec3& p) -> PosKey {
        return { (int32_t)(p.x * 100000.0f), (int32_t)(p.y * 100000.0f), (int32_t)(p.z * 100000.0f) };
    };

    for (uint32_t ci : clusterIndices) {
        const Cluster& src = allClusters[ci];
        std::vector<uint32_t> remap(src.vertices.size());

        for (uint32_t v = 0; v < (uint32_t)src.vertices.size(); v++) {
            PosKey key = toPosKey(src.vertices[v].position);
            auto it = weldMap.find(key);
            if (it != weldMap.end()) {
                remap[v] = it->second;
                // Average normals for welded vertices
                merged.vertices[it->second].normal += src.vertices[v].normal;
            } else {
                uint32_t newIdx = (uint32_t)merged.vertices.size();
                merged.vertices.push_back(src.vertices[v]);
                weldMap[key] = newIdx;
                remap[v] = newIdx;
            }
        }

        for (uint32_t idx : src.indices) {
            merged.indices.push_back(remap[idx]);
        }
    }

    // Normalize welded normals
    for (auto& v : merged.vertices) {
        float len = glm::length(v.normal);
        if (len > 1e-8f) v.normal /= len;
    }

    merged.computeBoundsAndMetrics();
    merged.computeBoundaryEdges();
    return merged;
}

// ---------- Split Cluster ----------

std::vector<Cluster> splitCluster(const Cluster& merged) {
    uint32_t numTris = merged.numTris;
    if (numTris <= CLUSTER_SIZE) {
        return { merged };
    }

    // Sort triangles by Morton code of centroid
    struct TriInfo {
        uint32_t triIndex;
        uint32_t mortonCode;
    };
    std::vector<TriInfo> triInfos(numTris);
    glm::vec3 bSize = merged.bounds.max - merged.bounds.min;
    glm::vec3 bMin  = merged.bounds.min;
    for (int i = 0; i < 3; i++) {
        if (bSize[i] < 1e-8f) bSize[i] = 1.0f;
    }

    for (uint32_t t = 0; t < numTris; t++) {
        const glm::vec3& p0 = merged.vertices[merged.indices[t * 3 + 0]].position;
        const glm::vec3& p1 = merged.vertices[merged.indices[t * 3 + 1]].position;
        const glm::vec3& p2 = merged.vertices[merged.indices[t * 3 + 2]].position;
        glm::vec3 c = (p0 + p1 + p2) / 3.0f;
        glm::vec3 norm = (c - bMin) / bSize;
        triInfos[t] = { t, mortonEncode(norm) };
    }
    std::sort(triInfos.begin(), triInfos.end(),
        [](const TriInfo& a, const TriInfo& b) { return a.mortonCode < b.mortonCode; });

    std::vector<Cluster> result;
    for (uint32_t start = 0; start < numTris; start += CLUSTER_SIZE) {
        uint32_t end = std::min(start + CLUSTER_SIZE, numTris);

        Cluster cluster;
        std::unordered_map<uint32_t, uint32_t> remap;

        for (uint32_t i = start; i < end; i++) {
            uint32_t origTri = triInfos[i].triIndex;
            for (int v = 0; v < 3; v++) {
                uint32_t srcIdx = merged.indices[origTri * 3 + v];
                auto it = remap.find(srcIdx);
                if (it != remap.end()) {
                    cluster.indices.push_back(it->second);
                } else {
                    uint32_t localIdx = (uint32_t)cluster.vertices.size();
                    cluster.vertices.push_back(merged.vertices[srcIdx]);
                    remap[srcIdx] = localIdx;
                    cluster.indices.push_back(localIdx);
                }
            }
        }

        cluster.computeBoundsAndMetrics();
        cluster.computeBoundaryEdges();
        result.push_back(std::move(cluster));
    }

    return result;
}

} // namespace nanite
