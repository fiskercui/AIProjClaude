#include "rasterizer.h"
#include <cstring>

namespace nanite {

// ---------- Framebuffer ----------

void Framebuffer::resize(int w, int h) {
    width = w;
    height = h;
    color.resize(w * h);
    depth.resize(w * h);
    visBuffer.resize(w * h);
    clear();
}

void Framebuffer::clear() {
    // Dark gray background
    std::fill(color.begin(), color.end(), 0xFF1A1A2E); // ABGR
    std::fill(depth.begin(), depth.end(), 1.0f);
    for (auto& vb : visBuffer) { vb.clusterIndex = INVALID_INDEX; vb.triangleIndex = INVALID_INDEX; }
}

void Framebuffer::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    color[y * width + x] = (0xFF << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

void Framebuffer::setPixelAlpha(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    color[y * width + x] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// ---------- Render Mode Names ----------

const char* renderModeName(RenderMode mode) {
    switch (mode) {
        case RenderMode::Solid:         return "Solid";
        case RenderMode::LODColors:     return "LOD Colors";
        case RenderMode::ClusterColors: return "Cluster Colors";
        case RenderMode::Wireframe:     return "Wireframe";
        case RenderMode::VisBuffer:     return "Vis Buffer";
        case RenderMode::Depth:         return "Depth";
        default: return "Unknown";
    }
}

// ---------- Color utilities ----------

// LOD level to color
static void lodColor(int32_t mipLevel, int32_t maxLevel, uint8_t& r, uint8_t& g, uint8_t& b) {
    // Color gradient: blue (finest) -> green -> yellow -> orange -> red (coarsest)
    float t = (maxLevel > 0) ? (float)mipLevel / (float)maxLevel : 0.0f;
    t = std::min(1.0f, std::max(0.0f, t));

    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0; g = (uint8_t)(s * 255); b = (uint8_t)((1.0f - s) * 255);
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = (uint8_t)(s * 255); g = 255; b = 0;
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = 255; g = (uint8_t)((1.0f - s) * 255); b = 0;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 255; g = 0; b = (uint8_t)(s * 128);
    }
}

// Hash-based deterministic color for cluster ID
static void clusterColor(uint32_t id, uint8_t& r, uint8_t& g, uint8_t& b) {
    // Simple hash for variety
    uint32_t h = id * 2654435761u;
    r = (uint8_t)((h >>  0) & 0xFF) | 0x40;
    g = (uint8_t)((h >>  8) & 0xFF) | 0x40;
    b = (uint8_t)((h >> 16) & 0xFF) | 0x40;
}

// ---------- Rasterizer ----------

struct ScreenVertex {
    float x, y, z; // screen-space x, y and depth z in [0, 1]
    glm::vec3 normal;
};

void rasterize(
    const std::vector<Cluster>& clusters,
    const std::vector<VisibleCluster>& visible,
    const PackedView& view,
    Framebuffer& fb,
    RenderMode mode,
    RasterStats& stats,
    int32_t maxMipLevel)
{
    stats = {};

    // Simple directional light for shading
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));

    for (const auto& vc : visible) {
        const Cluster& cluster = clusters[vc.clusterIndex];

        // Transform all cluster vertices to screen space
        std::vector<ScreenVertex> screenVerts(cluster.vertices.size());
        std::vector<bool> vertVisible(cluster.vertices.size(), true);

        for (size_t v = 0; v < cluster.vertices.size(); v++) {
            glm::vec4 clip = view.viewProjMatrix * glm::vec4(cluster.vertices[v].position, 1.0f);

            if (clip.w <= 0.0f) {
                vertVisible[v] = false;
                continue;
            }

            // Perspective divide -> NDC
            float invW = 1.0f / clip.w;
            float ndcX = clip.x * invW;
            float ndcY = clip.y * invW;
            float ndcZ = clip.z * invW;

            // NDC [-1, 1] -> screen [0, width/height]
            screenVerts[v].x = (ndcX * 0.5f + 0.5f) * (float)fb.width;
            screenVerts[v].y = (1.0f - (ndcY * 0.5f + 0.5f)) * (float)fb.height; // flip Y
            screenVerts[v].z = ndcZ * 0.5f + 0.5f; // [0, 1] depth
            screenVerts[v].normal = cluster.vertices[v].normal;
        }

        // Rasterize each triangle
        for (uint32_t t = 0; t < cluster.numTris; t++) {
            uint32_t i0 = cluster.indices[t * 3 + 0];
            uint32_t i1 = cluster.indices[t * 3 + 1];
            uint32_t i2 = cluster.indices[t * 3 + 2];

            if (!vertVisible[i0] || !vertVisible[i1] || !vertVisible[i2]) continue;

            const ScreenVertex& sv0 = screenVerts[i0];
            const ScreenVertex& sv1 = screenVerts[i1];
            const ScreenVertex& sv2 = screenVerts[i2];

            // Signed area (2x) for backface culling
            float signedArea2 = (sv1.x - sv0.x) * (sv2.y - sv0.y)
                              - (sv2.x - sv0.x) * (sv1.y - sv0.y);

            if (signedArea2 >= 0.0f) {
                stats.trianglesBackfaceCulled++;
                continue; // backface
            }

            float invArea = 1.0f / signedArea2;

            // Bounding box (clipped to framebuffer)
            int minX = std::max(0, (int)std::floor(std::min({ sv0.x, sv1.x, sv2.x })));
            int maxX = std::min(fb.width - 1, (int)std::ceil(std::max({ sv0.x, sv1.x, sv2.x })));
            int minY = std::max(0, (int)std::floor(std::min({ sv0.y, sv1.y, sv2.y })));
            int maxY = std::min(fb.height - 1, (int)std::ceil(std::max({ sv0.y, sv1.y, sv2.y })));

            if (minX > maxX || minY > maxY) continue;

            stats.trianglesRasterized++;

            for (int py = minY; py <= maxY; py++) {
                for (int px = minX; px <= maxX; px++) {
                    float x = (float)px + 0.5f;
                    float y = (float)py + 0.5f;

                    // Barycentric coordinates via edge functions
                    float w0 = ((sv1.x - x) * (sv2.y - y) - (sv2.x - x) * (sv1.y - y)) * invArea;
                    float w1 = ((sv2.x - x) * (sv0.y - y) - (sv0.x - x) * (sv2.y - y)) * invArea;
                    float w2 = 1.0f - w0 - w1;

                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                    // Interpolate depth
                    float depth = w0 * sv0.z + w1 * sv1.z + w2 * sv2.z;

                    // Depth test
                    int fbIdx = py * fb.width + px;
                    if (depth >= fb.depth[fbIdx]) continue;

                    fb.depth[fbIdx] = depth;
                    fb.visBuffer[fbIdx] = { vc.clusterIndex, t };
                    stats.pixelsWritten++;

                    // Compute color based on render mode
                    uint8_t r = 128, g = 128, b = 128;

                    switch (mode) {
                    case RenderMode::Solid:
                    case RenderMode::Wireframe: {
                        // Interpolate normal and do basic N.L shading
                        glm::vec3 normal = glm::normalize(
                            w0 * sv0.normal + w1 * sv1.normal + w2 * sv2.normal);
                        float ndotl = std::max(0.0f, glm::dot(normal, lightDir));
                        float ambient = 0.15f;
                        float shade = ambient + (1.0f - ambient) * ndotl;
                        r = g = b = (uint8_t)(shade * 230.0f);

                        if (mode == RenderMode::Wireframe) {
                            float minBary = std::min({ w0, w1, w2 });
                            if (minBary < 0.02f) { r = 0; g = 255; b = 100; }
                        }
                        break;
                    }
                    case RenderMode::LODColors: {
                        lodColor(vc.mipLevel, maxMipLevel, r, g, b);
                        // Apply some shading on top
                        glm::vec3 normal = glm::normalize(
                            w0 * sv0.normal + w1 * sv1.normal + w2 * sv2.normal);
                        float shade = 0.3f + 0.7f * std::max(0.0f, glm::dot(normal, lightDir));
                        r = (uint8_t)(r * shade);
                        g = (uint8_t)(g * shade);
                        b = (uint8_t)(b * shade);
                        break;
                    }
                    case RenderMode::ClusterColors: {
                        clusterColor(vc.clusterIndex, r, g, b);
                        glm::vec3 normal = glm::normalize(
                            w0 * sv0.normal + w1 * sv1.normal + w2 * sv2.normal);
                        float shade = 0.3f + 0.7f * std::max(0.0f, glm::dot(normal, lightDir));
                        r = (uint8_t)(r * shade);
                        g = (uint8_t)(g * shade);
                        b = (uint8_t)(b * shade);
                        break;
                    }
                    case RenderMode::VisBuffer: {
                        // Encode clusterID and triID as color
                        r = (uint8_t)(vc.clusterIndex & 0xFF);
                        g = (uint8_t)((vc.clusterIndex >> 8) & 0xFF);
                        b = (uint8_t)(t & 0xFF);
                        break;
                    }
                    case RenderMode::Depth: {
                        // Map depth [0..1] to grayscale (with gamma for visibility)
                        float d = std::pow(depth, 0.3f);
                        uint8_t val = (uint8_t)(d * 255.0f);
                        r = g = b = val;
                        break;
                    }
                    default: break;
                    }

                    fb.setPixel(px, py, r, g, b);
                }
            }
        }
    }
}

} // namespace nanite
