#include "mesh_loader.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstdio>

namespace nanite {

// Hash for vertex deduplication
struct VertexKey {
    uint32_t posIdx;
    uint32_t normIdx;
    bool operator==(const VertexKey& o) const {
        return posIdx == o.posIdx && normIdx == o.normIdx;
    }
};
struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        return std::hash<uint64_t>()(((uint64_t)k.posIdx << 32) | k.normIdx);
    }
};

bool loadOBJ(const std::string& filepath, RawMesh& outMesh) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Cannot open OBJ file '%s'\n", filepath.c_str());
        return false;
    }

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;

    struct FaceVert { uint32_t v, vn; };
    std::vector<std::vector<FaceVert>> faces;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string prefix;
        ss >> prefix;

        if (prefix == "v") {
            glm::vec3 p;
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (prefix == "vn") {
            glm::vec3 n;
            ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (prefix == "f") {
            std::vector<FaceVert> face;
            std::string vertStr;
            while (ss >> vertStr) {
                FaceVert fv = { 0, INVALID_INDEX };
                // Parse formats: v, v/vt, v/vt/vn, v//vn
                int v = 0, vt = 0, vn = 0;
                if (sscanf(vertStr.c_str(), "%d/%d/%d", &v, &vt, &vn) == 3) {
                    fv.v  = v - 1;
                    fv.vn = vn - 1;
                } else if (sscanf(vertStr.c_str(), "%d//%d", &v, &vn) == 2) {
                    fv.v  = v - 1;
                    fv.vn = vn - 1;
                } else if (sscanf(vertStr.c_str(), "%d/%d", &v, &vt) == 2) {
                    fv.v = v - 1;
                } else if (sscanf(vertStr.c_str(), "%d", &v) == 1) {
                    fv.v = v - 1;
                }
                face.push_back(fv);
            }
            if (face.size() >= 3) {
                faces.push_back(std::move(face));
            }
        }
    }

    if (positions.empty() || faces.empty()) {
        fprintf(stderr, "Error: OBJ file '%s' has no geometry\n", filepath.c_str());
        return false;
    }

    // Triangulate faces and deduplicate vertices
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertMap;
    outMesh.vertices.clear();
    outMesh.indices.clear();
    outMesh.bounds = {};

    bool hasNormals = !normals.empty();

    for (auto& face : faces) {
        // Triangulate polygon as a fan
        for (size_t i = 1; i + 1 < face.size(); i++) {
            FaceVert tri[3] = { face[0], face[i], face[i + 1] };
            for (int j = 0; j < 3; j++) {
                uint32_t normIdx = tri[j].vn != INVALID_INDEX ? tri[j].vn : INVALID_INDEX;
                VertexKey key = { tri[j].v, normIdx };
                auto it = vertMap.find(key);
                if (it != vertMap.end()) {
                    outMesh.indices.push_back(it->second);
                } else {
                    Vertex vert;
                    vert.position = positions[tri[j].v];
                    if (hasNormals && normIdx != INVALID_INDEX && normIdx < normals.size()) {
                        vert.normal = normals[normIdx];
                    } else {
                        vert.normal = glm::vec3(0, 1, 0); // placeholder
                    }
                    uint32_t idx = (uint32_t)outMesh.vertices.size();
                    outMesh.vertices.push_back(vert);
                    outMesh.bounds.expand(vert.position);
                    vertMap[key] = idx;
                    outMesh.indices.push_back(idx);
                }
            }
        }
    }

    // Compute face normals if OBJ had none
    if (!hasNormals) {
        // Reset normals to zero
        for (auto& v : outMesh.vertices) v.normal = glm::vec3(0.0f);
        // Accumulate face normals
        for (size_t i = 0; i < outMesh.indices.size(); i += 3) {
            const glm::vec3& p0 = outMesh.vertices[outMesh.indices[i + 0]].position;
            const glm::vec3& p1 = outMesh.vertices[outMesh.indices[i + 1]].position;
            const glm::vec3& p2 = outMesh.vertices[outMesh.indices[i + 2]].position;
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            outMesh.vertices[outMesh.indices[i + 0]].normal += fn;
            outMesh.vertices[outMesh.indices[i + 1]].normal += fn;
            outMesh.vertices[outMesh.indices[i + 2]].normal += fn;
        }
        for (auto& v : outMesh.vertices) {
            float len = glm::length(v.normal);
            if (len > 1e-8f) v.normal /= len;
            else v.normal = glm::vec3(0, 1, 0);
        }
    }

    printf("  OBJ loaded: %zu vertices, %zu triangles\n",
           outMesh.vertices.size(), outMesh.indices.size() / 3);
    return true;
}

} // namespace nanite
