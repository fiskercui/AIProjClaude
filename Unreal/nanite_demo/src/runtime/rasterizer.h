#pragma once

#include "../core/types.h"
#include "../build/cluster.h"
#include "packed_view.h"
#include "dag_traversal.h"

namespace nanite {

struct Framebuffer {
    int width = 0, height = 0;
    std::vector<uint32_t>      color;     // RGBA8 packed (ABGR byte order)
    std::vector<float>         depth;     // depth buffer [0..1], 1.0 = far
    std::vector<VisBufferPixel> visBuffer;

    void resize(int w, int h);
    void clear();

    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    void setPixelAlpha(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
};

enum class RenderMode {
    Solid = 0,        // flat shading with directional light
    LODColors,        // color-coded by mipLevel
    ClusterColors,    // unique color per cluster
    Wireframe,        // wireframe overlay on solid
    VisBuffer,        // visibility buffer debug view
    Depth,            // depth buffer visualization
    COUNT
};

const char* renderModeName(RenderMode mode);

struct RasterStats {
    uint32_t trianglesRasterized = 0;
    uint32_t trianglesBackfaceCulled = 0;
    uint32_t pixelsWritten = 0;
};

// Rasterize visible clusters into the framebuffer.
void rasterize(
    const std::vector<Cluster>& clusters,
    const std::vector<VisibleCluster>& visible,
    const PackedView& view,
    Framebuffer& fb,
    RenderMode mode,
    RasterStats& stats,
    int32_t maxMipLevel
);

} // namespace nanite
