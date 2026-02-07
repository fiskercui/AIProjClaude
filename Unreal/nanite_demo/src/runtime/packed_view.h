#pragma once

#include "../core/types.h"

namespace nanite {

struct PackedView {
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;
    glm::mat4 viewProjMatrix;

    glm::vec3 viewOrigin;     // camera world position
    glm::vec3 viewForward;    // camera forward direction
    float     nearPlane = 0.1f;
    float     farPlane  = 1000.0f;

    int   viewWidth  = 1280;
    int   viewHeight = 720;

    float lodScale = 1.0f;            // LOD selection scale factor
    float maxPixelsPerEdge = 1.0f;    // quality control

    // 6 frustum planes: left, right, bottom, top, near, far
    // (a, b, c, d) where ax + by + cz + d >= 0 is inside
    glm::vec4 frustumPlanes[6];

    // Recompute lodScale and frustum planes from current matrices
    void update();

    // Setup from camera parameters
    void setup(
        const glm::vec3& eye,
        const glm::vec3& forward,
        const glm::vec3& up,
        float fovYRadians,
        float aspect,
        float nearP,
        float farP,
        int width,
        int height,
        float maxPxPerEdge
    );
};

} // namespace nanite
