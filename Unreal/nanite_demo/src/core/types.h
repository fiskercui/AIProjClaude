#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <cstdint>
#include <limits>
#include <algorithm>
#include <cmath>
#include <cassert>

namespace nanite {

// --- Constants (matching UE5 Nanite conventions) ---
constexpr uint32_t CLUSTER_SIZE       = 128;   // Max triangles per cluster
constexpr uint32_t MIN_CLUSTER_SIZE   = 64;    // Min triangles per cluster when splitting
constexpr uint32_t MIN_GROUP_SIZE     = 4;     // Min clusters per group
constexpr uint32_t MAX_GROUP_SIZE     = 32;    // Max clusters per group
constexpr float    MAX_PIXELS_PER_EDGE = 1.0f; // Default screen-space error threshold
constexpr uint32_t INVALID_INDEX      = 0xFFFFFFFF;

// --- Axis-Aligned Bounding Box ---
struct AABB {
    glm::vec3 min = glm::vec3( std::numeric_limits<float>::max());
    glm::vec3 max = glm::vec3(-std::numeric_limits<float>::max());

    void expand(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void expand(const AABB& other) {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }
    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return (max - min) * 0.5f; }
    bool valid() const { return min.x <= max.x; }
};

// --- Bounding Sphere ---
struct BoundingSphere {
    glm::vec3 center = glm::vec3(0.0f);
    float     radius = 0.0f;

    static BoundingSphere fromAABB(const AABB& box) {
        BoundingSphere s;
        s.center = box.center();
        s.radius = glm::length(box.extent());
        return s;
    }

    static BoundingSphere merge(const BoundingSphere& a, const BoundingSphere& b) {
        if (a.radius <= 0.0f) return b;
        if (b.radius <= 0.0f) return a;
        glm::vec3 d = b.center - a.center;
        float dist = glm::length(d);
        if (dist + b.radius <= a.radius) return a; // b inside a
        if (dist + a.radius <= b.radius) return b; // a inside b
        float newRadius = (dist + a.radius + b.radius) * 0.5f;
        BoundingSphere s;
        s.radius = newRadius;
        s.center = a.center + d * ((newRadius - a.radius) / dist);
        return s;
    }

    static BoundingSphere fromSpheres(const BoundingSphere* spheres, uint32_t count) {
        if (count == 0) return {};
        BoundingSphere result = spheres[0];
        for (uint32_t i = 1; i < count; i++) {
            result = merge(result, spheres[i]);
        }
        return result;
    }

    static BoundingSphere fromPoints(const glm::vec3* points, uint32_t count) {
        if (count == 0) return {};
        // Simple bounding sphere: center = centroid, radius = max distance
        glm::vec3 c(0.0f);
        for (uint32_t i = 0; i < count; i++) c += points[i];
        c /= (float)count;
        float r = 0.0f;
        for (uint32_t i = 0; i < count; i++) {
            r = std::max(r, glm::distance(c, points[i]));
        }
        return { c, r };
    }
};

// --- Vertex ---
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
};

// --- Visibility buffer pixel ---
struct VisBufferPixel {
    uint32_t clusterIndex  = INVALID_INDEX;
    uint32_t triangleIndex = INVALID_INDEX;
};

// Forward declarations
struct Cluster;
struct ClusterGroup;
class  ClusterDAG;
struct PackedView;

} // namespace nanite
