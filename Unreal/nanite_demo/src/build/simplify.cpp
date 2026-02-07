#include "simplify.h"
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <numeric>
#include <cstdio>

namespace nanite {

// 4x4 symmetric matrix for quadric error metric
struct Quadric {
    double data[10] = {}; // upper triangle of 4x4 symmetric matrix

    Quadric() = default;

    // Build from plane equation ax + by + cz + d = 0
    Quadric(double a, double b, double c, double d) {
        data[0] = a*a; data[1] = a*b; data[2] = a*c; data[3] = a*d;
                        data[4] = b*b; data[5] = b*c; data[6] = b*d;
                                        data[7] = c*c; data[8] = c*d;
                                                        data[9] = d*d;
    }

    Quadric operator+(const Quadric& o) const {
        Quadric r;
        for (int i = 0; i < 10; i++) r.data[i] = data[i] + o.data[i];
        return r;
    }
    Quadric& operator+=(const Quadric& o) {
        for (int i = 0; i < 10; i++) data[i] += o.data[i];
        return *this;
    }

    // Evaluate error for a point
    double evaluate(const glm::dvec3& v) const {
        //   v^T * Q * v
        //   [x y z 1] * Q * [x y z 1]^T
        double x = v.x, y = v.y, z = v.z;
        return data[0]*x*x + 2.0*data[1]*x*y + 2.0*data[2]*x*z + 2.0*data[3]*x
             + data[4]*y*y + 2.0*data[5]*y*z + 2.0*data[6]*y
             + data[7]*z*z + 2.0*data[8]*z
             + data[9];
    }

    // Try to find optimal point by solving the linear system.
    // Returns false if the matrix is singular.
    bool solveOptimal(glm::dvec3& outPos) const {
        // Solve the 3x3 system:
        //   [a00 a01 a02] [x]   [-a03]
        //   [a01 a04 a05] [y] = [-a06]
        //   [a02 a05 a07] [z]   [-a08]
        double a00 = data[0], a01 = data[1], a02 = data[2], a03 = data[3];
        double a11 = data[4], a12 = data[5], a13 = data[6];
        double a22 = data[7], a23 = data[8];

        double det = a00 * (a11*a22 - a12*a12)
                   - a01 * (a01*a22 - a12*a02)
                   + a02 * (a01*a12 - a11*a02);

        if (std::abs(det) < 1e-12) return false;

        double invDet = 1.0 / det;
        outPos.x = ((-a03) * (a11*a22 - a12*a12) - a01 * ((-a13)*a22 - a12*(-a23)) + a02 * ((-a13)*a12 - a11*(-a23))) * invDet;
        outPos.y = (a00 * ((-a13)*a22 - a12*(-a23)) - (-a03) * (a01*a22 - a12*a02) + a02 * (a01*(-a23) - (-a13)*a02)) * invDet;
        outPos.z = (a00 * (a11*(-a23) - (-a13)*a12) - a01 * (a01*(-a23) - (-a13)*a02) + (-a03) * (a01*a12 - a11*a02)) * invDet;
        return true;
    }
};

struct EdgeCollapse {
    uint32_t v0, v1;     // vertex indices
    double   cost;       // quadric error cost
    glm::dvec3 optimalPos;
    uint32_t generation; // for invalidation tracking

    bool operator>(const EdgeCollapse& o) const { return cost > o.cost; }
};

float simplifyCluster(Cluster& cluster, uint32_t targetNumTris, bool lockBoundaryEdges) {
    if (cluster.numTris <= targetNumTris) return 0.0f;

    uint32_t numVerts = (uint32_t)cluster.vertices.size();
    uint32_t numTris  = cluster.numTris;

    // --- Step 1: Build per-vertex quadrics ---
    std::vector<Quadric> vertexQuadrics(numVerts);

    for (uint32_t t = 0; t < numTris; t++) {
        uint32_t i0 = cluster.indices[t * 3 + 0];
        uint32_t i1 = cluster.indices[t * 3 + 1];
        uint32_t i2 = cluster.indices[t * 3 + 2];
        glm::dvec3 p0(cluster.vertices[i0].position);
        glm::dvec3 p1(cluster.vertices[i1].position);
        glm::dvec3 p2(cluster.vertices[i2].position);

        glm::dvec3 normal = glm::cross(p1 - p0, p2 - p0);
        double len = glm::length(normal);
        if (len < 1e-12) continue;
        normal /= len;

        double d = -glm::dot(normal, p0);
        Quadric q(normal.x, normal.y, normal.z, d);

        // Weight by triangle area
        double area = len * 0.5;
        for (int j = 0; j < 10; j++) q.data[j] *= area;

        vertexQuadrics[i0] += q;
        vertexQuadrics[i1] += q;
        vertexQuadrics[i2] += q;
    }

    // --- Step 2: Track locked vertices (boundary) ---
    std::vector<bool> locked(numVerts, false);
    if (lockBoundaryEdges && !cluster.boundaryEdges.empty()) {
        for (uint32_t t = 0; t < numTris; t++) {
            for (int e = 0; e < 3; e++) {
                if (cluster.boundaryEdges[t * 3 + e]) {
                    locked[cluster.indices[t * 3 + e]] = true;
                    locked[cluster.indices[t * 3 + ((e + 1) % 3)]] = true;
                }
            }
        }
    }

    // --- Step 3: Build collapse candidates ---
    // vertex -> current vertex (union-find for collapsed vertices)
    std::vector<uint32_t> vertexRemap(numVerts);
    std::iota(vertexRemap.begin(), vertexRemap.end(), 0);

    auto findRoot = [&](uint32_t v) -> uint32_t {
        while (vertexRemap[v] != v) {
            vertexRemap[v] = vertexRemap[vertexRemap[v]]; // path compression
            v = vertexRemap[v];
        }
        return v;
    };

    // Per-vertex generation counter for invalidating stale heap entries
    std::vector<uint32_t> vertexGen(numVerts, 0);

    // Track which triangles are alive
    std::vector<bool> triAlive(numTris, true);
    uint32_t currentTriCount = numTris;

    // Track triangles per vertex for face-flip detection
    std::vector<std::vector<uint32_t>> vertTris(numVerts);
    for (uint32_t t = 0; t < numTris; t++) {
        for (int v = 0; v < 3; v++) {
            vertTris[cluster.indices[t * 3 + v]].push_back(t);
        }
    }

    // Edge -> unique key for deduplication
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return ((uint64_t)a << 32) | b;
    };

    // Build edge set and initial priority queue
    std::priority_queue<EdgeCollapse, std::vector<EdgeCollapse>, std::greater<EdgeCollapse>> heap;
    std::unordered_set<uint64_t> edgeSet;

    auto computeCollapse = [&](uint32_t v0, uint32_t v1) -> EdgeCollapse {
        EdgeCollapse ec;
        ec.v0 = v0;
        ec.v1 = v1;
        ec.generation = vertexGen[v0] + vertexGen[v1];

        // Both locked: can't collapse
        if (locked[v0] && locked[v1]) {
            ec.cost = 1e30;
            ec.optimalPos = glm::dvec3(cluster.vertices[v0].position);
            return ec;
        }

        Quadric combined = vertexQuadrics[v0] + vertexQuadrics[v1];

        // Try optimal placement
        if (!locked[v0] && !locked[v1]) {
            if (combined.solveOptimal(ec.optimalPos)) {
                ec.cost = combined.evaluate(ec.optimalPos);
                if (ec.cost < 0.0) ec.cost = 0.0;
                return ec;
            }
        }

        // Fallback: evaluate both endpoints and midpoint, pick best
        glm::dvec3 p0(cluster.vertices[v0].position);
        glm::dvec3 p1(cluster.vertices[v1].position);
        glm::dvec3 mid = (p0 + p1) * 0.5;

        double c0 = combined.evaluate(p0);
        double c1 = combined.evaluate(p1);
        double cm = locked[v0] || locked[v1] ? 1e30 : combined.evaluate(mid);

        // If one is locked, collapse to that one
        if (locked[v0]) { ec.cost = c0; ec.optimalPos = p0; }
        else if (locked[v1]) { ec.cost = c1; ec.optimalPos = p1; }
        else if (c0 <= c1 && c0 <= cm) { ec.cost = c0; ec.optimalPos = p0; }
        else if (c1 <= cm) { ec.cost = c1; ec.optimalPos = p1; }
        else { ec.cost = cm; ec.optimalPos = mid; }

        if (ec.cost < 0.0) ec.cost = 0.0;
        return ec;
    };

    for (uint32_t t = 0; t < numTris; t++) {
        uint32_t i0 = cluster.indices[t * 3 + 0];
        uint32_t i1 = cluster.indices[t * 3 + 1];
        uint32_t i2 = cluster.indices[t * 3 + 2];
        uint64_t edges[3] = { edgeKey(i0, i1), edgeKey(i1, i2), edgeKey(i2, i0) };
        uint32_t verts[3][2] = { {i0,i1}, {i1,i2}, {i2,i0} };

        for (int e = 0; e < 3; e++) {
            if (edgeSet.insert(edges[e]).second) {
                heap.push(computeCollapse(verts[e][0], verts[e][1]));
            }
        }
    }

    // --- Step 4: Collapse edges ---
    double maxError = 0.0;

    while (currentTriCount > targetNumTris && !heap.empty()) {
        EdgeCollapse ec = heap.top();
        heap.pop();

        // Skip if invalid (vertex was already collapsed or generation mismatch)
        uint32_t rv0 = findRoot(ec.v0);
        uint32_t rv1 = findRoot(ec.v1);
        if (rv0 == rv1) continue; // already collapsed together
        if (ec.v0 != rv0 || ec.v1 != rv1) continue; // stale entry
        if (ec.generation != vertexGen[rv0] + vertexGen[rv1]) continue; // stale

        // Skip if cost is too high (both locked)
        if (ec.cost >= 1e29) break;

        // Face-flip check: ensure no triangle normals invert
        bool flipDetected = false;
        for (uint32_t t : vertTris[rv1]) {
            if (!triAlive[t]) continue;
            uint32_t ti0 = findRoot(cluster.indices[t * 3 + 0]);
            uint32_t ti1 = findRoot(cluster.indices[t * 3 + 1]);
            uint32_t ti2 = findRoot(cluster.indices[t * 3 + 2]);
            // Skip degenerate or triangles that will collapse
            if (ti0 == ti1 || ti1 == ti2 || ti0 == ti2) continue;
            if (ti0 == rv0 || ti1 == rv0 || ti2 == rv0) continue;

            // Compute normal before and after
            glm::dvec3 before[3] = {
                glm::dvec3(cluster.vertices[ti0].position),
                glm::dvec3(cluster.vertices[ti1].position),
                glm::dvec3(cluster.vertices[ti2].position)
            };
            glm::dvec3 after[3] = { before[0], before[1], before[2] };
            for (int v = 0; v < 3; v++) {
                uint32_t idx = (v == 0) ? ti0 : (v == 1) ? ti1 : ti2;
                if (idx == rv1) after[v] = ec.optimalPos;
            }
            glm::dvec3 nb = glm::cross(before[1] - before[0], before[2] - before[0]);
            glm::dvec3 na = glm::cross(after[1]  - after[0],  after[2]  - after[0]);
            if (glm::dot(nb, na) < 0.0) { flipDetected = true; break; }
        }
        // Also check rv0's triangles
        if (!flipDetected) {
            for (uint32_t t : vertTris[rv0]) {
                if (!triAlive[t]) continue;
                uint32_t ti0 = findRoot(cluster.indices[t * 3 + 0]);
                uint32_t ti1 = findRoot(cluster.indices[t * 3 + 1]);
                uint32_t ti2 = findRoot(cluster.indices[t * 3 + 2]);
                if (ti0 == ti1 || ti1 == ti2 || ti0 == ti2) continue;
                if (ti0 == rv1 || ti1 == rv1 || ti2 == rv1) continue;

                glm::dvec3 before[3] = {
                    glm::dvec3(cluster.vertices[ti0].position),
                    glm::dvec3(cluster.vertices[ti1].position),
                    glm::dvec3(cluster.vertices[ti2].position)
                };
                glm::dvec3 after[3] = { before[0], before[1], before[2] };
                for (int v = 0; v < 3; v++) {
                    uint32_t idx = (v == 0) ? ti0 : (v == 1) ? ti1 : ti2;
                    if (idx == rv0) after[v] = ec.optimalPos;
                }
                glm::dvec3 nb = glm::cross(before[1] - before[0], before[2] - before[0]);
                glm::dvec3 na = glm::cross(after[1]  - after[0],  after[2]  - after[0]);
                if (glm::dot(nb, na) < 0.0) { flipDetected = true; break; }
            }
        }
        if (flipDetected) continue;

        // --- Perform the collapse: merge v1 into v0 ---
        maxError = std::max(maxError, ec.cost);

        // Update v0 position and normal
        cluster.vertices[rv0].position = glm::vec3(ec.optimalPos);
        cluster.vertices[rv0].normal = glm::normalize(
            cluster.vertices[rv0].normal + cluster.vertices[rv1].normal
        );
        if (locked[rv1]) locked[rv0] = true;

        // Merge quadrics
        vertexQuadrics[rv0] = vertexQuadrics[rv0] + vertexQuadrics[rv1];

        // Point rv1 to rv0
        vertexRemap[rv1] = rv0;
        vertexGen[rv0]++;

        // Transfer rv1's triangles to rv0
        for (uint32_t t : vertTris[rv1]) {
            vertTris[rv0].push_back(t);
        }
        vertTris[rv1].clear();

        // Update index buffer and kill degenerate triangles
        for (uint32_t t : vertTris[rv0]) {
            if (!triAlive[t]) continue;
            for (int v = 0; v < 3; v++) {
                cluster.indices[t * 3 + v] = findRoot(cluster.indices[t * 3 + v]);
            }
            uint32_t ti0 = cluster.indices[t * 3 + 0];
            uint32_t ti1 = cluster.indices[t * 3 + 1];
            uint32_t ti2 = cluster.indices[t * 3 + 2];
            if (ti0 == ti1 || ti1 == ti2 || ti0 == ti2) {
                triAlive[t] = false;
                currentTriCount--;
            }
        }

        // Re-insert edges for rv0
        std::unordered_set<uint32_t> neighbors;
        for (uint32_t t : vertTris[rv0]) {
            if (!triAlive[t]) continue;
            for (int v = 0; v < 3; v++) {
                uint32_t nv = findRoot(cluster.indices[t * 3 + v]);
                if (nv != rv0) neighbors.insert(nv);
            }
        }
        for (uint32_t nv : neighbors) {
            heap.push(computeCollapse(rv0, nv));
        }
    }

    // --- Step 5: Compact mesh ---
    std::vector<Vertex> newVerts;
    std::vector<uint32_t> newIndices;
    std::unordered_map<uint32_t, uint32_t> compactMap;

    for (uint32_t t = 0; t < numTris; t++) {
        if (!triAlive[t]) continue;
        uint32_t tri[3];
        for (int v = 0; v < 3; v++) {
            uint32_t root = findRoot(cluster.indices[t * 3 + v]);
            auto it = compactMap.find(root);
            if (it != compactMap.end()) {
                tri[v] = it->second;
            } else {
                uint32_t newIdx = (uint32_t)newVerts.size();
                newVerts.push_back(cluster.vertices[root]);
                compactMap[root] = newIdx;
                tri[v] = newIdx;
            }
        }
        // Skip degenerate
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
        newIndices.push_back(tri[0]);
        newIndices.push_back(tri[1]);
        newIndices.push_back(tri[2]);
    }

    cluster.vertices = std::move(newVerts);
    cluster.indices  = std::move(newIndices);
    cluster.computeBoundsAndMetrics();
    cluster.computeBoundaryEdges();

    // Convert quadric error to geometric distance
    return (float)std::sqrt(std::max(0.0, maxError));
}

} // namespace nanite
