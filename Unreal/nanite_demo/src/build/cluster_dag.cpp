#include "cluster_dag.h"
#include "simplify.h"
#include <algorithm>
#include <cstdio>

namespace nanite {

void ClusterDAG::build(const RawMesh& mesh) {
    totalBounds = mesh.bounds;

    printf("Building leaf clusters...\n");
    std::vector<uint32_t> currentLevel = buildLeafClusters(mesh, clusters);
    printf("  Level 0: %zu leaf clusters (%zu triangles)\n",
           currentLevel.size(), mesh.indices.size() / 3);

    int32_t mipLevel = 0;

    // Iteratively build hierarchy
    while (currentLevel.size() > 1) {
        mipLevel++;

        // Step 1: Group clusters at current level
        std::vector<uint32_t> newGroupIndices = groupClusters(currentLevel);
        printf("  Level %d: %zu groups from %zu clusters",
               mipLevel, newGroupIndices.size(), currentLevel.size());

        // Step 2: Reduce each group to produce parent clusters
        std::vector<uint32_t> nextLevel;
        for (uint32_t gi : newGroupIndices) {
            std::vector<uint32_t> parentClusters = reduceGroup(gi);
            for (uint32_t pc : parentClusters) {
                nextLevel.push_back(pc);
            }
        }

        printf(" -> %zu parent clusters\n", nextLevel.size());

        if (nextLevel.empty()) {
            // Cannot reduce further, force remaining as roots
            for (uint32_t ci : currentLevel) {
                ClusterGroup rootGroup;
                rootGroup.children.push_back(ci);
                rootGroup.bounds = clusters[ci].sphereBounds;
                rootGroup.lodBounds = clusters[ci].lodBounds;
                rootGroup.parentLODError = clusters[ci].lodError;
                rootGroup.mipLevel = clusters[ci].mipLevel;
                rootGroup.isRoot = true;
                rootGroup.parentClusters.push_back(ci);
                groups.push_back(std::move(rootGroup));
            }
            break;
        }

        // If only one parent cluster, mark its group as root
        if (nextLevel.size() <= 1) {
            for (uint32_t gi : newGroupIndices) {
                groups[gi].isRoot = true;
            }
            break;
        }

        currentLevel = nextLevel;
    }

    // If we started with just one cluster, mark it as root
    if (currentLevel.size() == 1 && groups.empty()) {
        ClusterGroup rootGroup;
        rootGroup.children.push_back(currentLevel[0]);
        rootGroup.bounds = clusters[currentLevel[0]].sphereBounds;
        rootGroup.lodBounds = clusters[currentLevel[0]].lodBounds;
        rootGroup.parentLODError = clusters[currentLevel[0]].lodError;
        rootGroup.mipLevel = 0;
        rootGroup.isRoot = true;
        rootGroup.parentClusters.push_back(currentLevel[0]);
        groups.push_back(std::move(rootGroup));
    }

    // Print summary
    auto perLevel = getClusterCountPerLevel();
    printf("DAG Summary: %zu clusters, %zu groups, %d levels\n",
           clusters.size(), groups.size(), (int)perLevel.size());
    for (size_t i = 0; i < perLevel.size(); i++) {
        uint32_t tris = 0;
        for (auto& c : clusters) {
            if (c.mipLevel == (int32_t)i) tris += c.numTris;
        }
        printf("  Level %zu: %u clusters, %u triangles\n", i, perLevel[i], tris);
    }
}

std::vector<uint32_t> ClusterDAG::groupClusters(
    const std::vector<uint32_t>& levelClusterIndices)
{
    std::vector<uint32_t> newGroupIndices;
    uint32_t count = (uint32_t)levelClusterIndices.size();

    if (count == 0) return newGroupIndices;

    // If few enough clusters, put them all in one group
    if (count <= MAX_GROUP_SIZE) {
        uint32_t gi = (uint32_t)groups.size();
        ClusterGroup group;
        group.children = levelClusterIndices;
        group.mipLevel = clusters[levelClusterIndices[0]].mipLevel;

        // Compute group bounds
        std::vector<BoundingSphere> childSpheres, childLODSpheres;
        for (uint32_t ci : levelClusterIndices) {
            childSpheres.push_back(clusters[ci].sphereBounds);
            childLODSpheres.push_back(clusters[ci].lodBounds);
            group.parentLODError = std::max(group.parentLODError, clusters[ci].lodError);
        }
        group.bounds = BoundingSphere::fromSpheres(childSpheres.data(), (uint32_t)childSpheres.size());
        group.lodBounds = BoundingSphere::fromSpheres(childLODSpheres.data(), (uint32_t)childLODSpheres.size());

        // Assign group to children
        for (uint32_t ci : levelClusterIndices) {
            clusters[ci].groupIndex = gi;
        }

        groups.push_back(std::move(group));
        newGroupIndices.push_back(gi);
        return newGroupIndices;
    }

    // Sort clusters by Morton code of their centroid
    struct ClusterSort {
        uint32_t clusterIndex;
        uint32_t mortonCode;
    };
    std::vector<ClusterSort> sorted(count);

    glm::vec3 bSize = totalBounds.max - totalBounds.min;
    glm::vec3 bMin  = totalBounds.min;
    for (int i = 0; i < 3; i++) {
        if (bSize[i] < 1e-8f) bSize[i] = 1.0f;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ci = levelClusterIndices[i];
        glm::vec3 center = clusters[ci].bounds.center();
        glm::vec3 norm = (center - bMin) / bSize;
        sorted[i] = { ci, mortonEncode(norm) };
    }
    std::sort(sorted.begin(), sorted.end(),
        [](const ClusterSort& a, const ClusterSort& b) { return a.mortonCode < b.mortonCode; });

    // Cut into groups of MAX_GROUP_SIZE
    uint32_t targetGroupSize = MAX_GROUP_SIZE;
    // Adjust target so we don't get tiny groups at the end
    uint32_t numFullGroups = count / targetGroupSize;
    if (numFullGroups == 0) numFullGroups = 1;
    uint32_t adjustedGroupSize = count / numFullGroups;
    if (adjustedGroupSize < MIN_GROUP_SIZE) adjustedGroupSize = MIN_GROUP_SIZE;

    for (uint32_t start = 0; start < count; ) {
        uint32_t remaining = count - start;
        uint32_t groupSize = adjustedGroupSize;
        // If remaining is small, take them all
        if (remaining <= MAX_GROUP_SIZE || remaining - groupSize < MIN_GROUP_SIZE) {
            groupSize = remaining;
        }

        uint32_t gi = (uint32_t)groups.size();
        ClusterGroup group;
        group.mipLevel = clusters[sorted[start].clusterIndex].mipLevel;

        std::vector<BoundingSphere> childSpheres, childLODSpheres;
        for (uint32_t i = start; i < start + groupSize && i < count; i++) {
            uint32_t ci = sorted[i].clusterIndex;
            group.children.push_back(ci);
            childSpheres.push_back(clusters[ci].sphereBounds);
            childLODSpheres.push_back(clusters[ci].lodBounds);
            group.parentLODError = std::max(group.parentLODError, clusters[ci].lodError);
            clusters[ci].groupIndex = gi;
        }
        group.bounds = BoundingSphere::fromSpheres(childSpheres.data(), (uint32_t)childSpheres.size());
        group.lodBounds = BoundingSphere::fromSpheres(childLODSpheres.data(), (uint32_t)childLODSpheres.size());

        groups.push_back(std::move(group));
        newGroupIndices.push_back(gi);
        start += groupSize;
    }

    return newGroupIndices;
}

std::vector<uint32_t> ClusterDAG::reduceGroup(uint32_t groupIndex) {
    ClusterGroup& group = groups[groupIndex];
    std::vector<uint32_t> result;

    if (group.children.empty()) return result;

    // Count total triangles in children
    uint32_t totalTris = 0;
    for (uint32_t ci : group.children) {
        totalTris += clusters[ci].numTris;
    }

    if (totalTris == 0) return result;

    // Step 1: Merge all children into one cluster
    Cluster merged = mergeClusters(clusters, group.children);

    // Step 2: Determine target triangle count (roughly half)
    uint32_t targetTris = std::max(1u, totalTris / 2);
    // Ensure we have enough tris to fill at least one cluster
    targetTris = std::max(targetTris, (uint32_t)MIN_CLUSTER_SIZE);

    // Step 3: Simplify
    float simplifyError = simplifyCluster(merged, targetTris, true);

    // Ensure monotonically increasing error up the hierarchy
    group.parentLODError = std::max(group.parentLODError, simplifyError);
    // Ensure non-zero error so traversal always has something to compare
    if (group.parentLODError <= 0.0f) {
        group.parentLODError = merged.edgeLength * 0.01f;
        if (group.parentLODError <= 0.0f) group.parentLODError = 1e-6f;
    }

    // Step 4: Split simplified mesh back into clusters
    std::vector<Cluster> parentClusters = splitCluster(merged);

    // Step 5: Assign LOD metadata to parent clusters
    int32_t parentMip = group.mipLevel + 1;
    for (auto& pc : parentClusters) {
        pc.mipLevel = parentMip;
        pc.lodError = group.parentLODError;
        pc.lodBounds = group.lodBounds;
        pc.generatingGroupIndex = groupIndex;

        uint32_t clusterIdx = (uint32_t)clusters.size();
        clusters.push_back(std::move(pc));
        result.push_back(clusterIdx);
        group.parentClusters.push_back(clusterIdx);
    }

    return result;
}

std::vector<uint32_t> ClusterDAG::getRootGroupIndices() const {
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < (uint32_t)groups.size(); i++) {
        if (groups[i].isRoot) roots.push_back(i);
    }
    return roots;
}

std::vector<uint32_t> ClusterDAG::getClusterCountPerLevel() const {
    int32_t maxLevel = 0;
    for (auto& c : clusters) {
        maxLevel = std::max(maxLevel, c.mipLevel);
    }
    std::vector<uint32_t> counts(maxLevel + 1, 0);
    for (auto& c : clusters) {
        counts[c.mipLevel]++;
    }
    return counts;
}

int32_t ClusterDAG::getMaxMipLevel() const {
    int32_t maxLevel = 0;
    for (auto& c : clusters) {
        maxLevel = std::max(maxLevel, c.mipLevel);
    }
    return maxLevel;
}

} // namespace nanite
