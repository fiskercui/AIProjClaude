#pragma once

#include "types.h"
#include <string>

namespace nanite {

struct RawMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;   // 3 per triangle
    AABB                  bounds;

    uint32_t numTris() const { return (uint32_t)(indices.size() / 3); }
};

// Load a Wavefront OBJ file. Returns false on error.
bool loadOBJ(const std::string& filepath, RawMesh& outMesh);

} // namespace nanite
