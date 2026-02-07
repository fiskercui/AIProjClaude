#include "packed_view.h"

namespace nanite {

static void extractFrustumPlanes(const glm::mat4& vp, glm::vec4 planes[6]) {
    // Gribb-Hartmann method: extract frustum planes from viewProjection matrix
    // Row-major indexing: vp[col][row]
    // Left:   row3 + row0
    planes[0] = glm::vec4(
        vp[0][3] + vp[0][0],
        vp[1][3] + vp[1][0],
        vp[2][3] + vp[2][0],
        vp[3][3] + vp[3][0]
    );
    // Right:  row3 - row0
    planes[1] = glm::vec4(
        vp[0][3] - vp[0][0],
        vp[1][3] - vp[1][0],
        vp[2][3] - vp[2][0],
        vp[3][3] - vp[3][0]
    );
    // Bottom: row3 + row1
    planes[2] = glm::vec4(
        vp[0][3] + vp[0][1],
        vp[1][3] + vp[1][1],
        vp[2][3] + vp[2][1],
        vp[3][3] + vp[3][1]
    );
    // Top:    row3 - row1
    planes[3] = glm::vec4(
        vp[0][3] - vp[0][1],
        vp[1][3] - vp[1][1],
        vp[2][3] - vp[2][1],
        vp[3][3] - vp[3][1]
    );
    // Near:   row3 + row2
    planes[4] = glm::vec4(
        vp[0][3] + vp[0][2],
        vp[1][3] + vp[1][2],
        vp[2][3] + vp[2][2],
        vp[3][3] + vp[3][2]
    );
    // Far:    row3 - row2
    planes[5] = glm::vec4(
        vp[0][3] - vp[0][2],
        vp[1][3] - vp[1][2],
        vp[2][3] - vp[2][2],
        vp[3][3] - vp[3][2]
    );

    // Normalize planes
    for (int i = 0; i < 6; i++) {
        float len = glm::length(glm::vec3(planes[i]));
        if (len > 1e-8f) planes[i] /= len;
    }
}

void PackedView::update() {
    viewProjMatrix = projMatrix * viewMatrix;

    // LOD Scale: matches UE5 NaniteShared.cpp UpdateLODScales
    // viewToPixels = 0.5 * projMatrix[1][1] * viewHeight
    // lodScale = viewToPixels / maxPixelsPerEdge
    float viewToPixels = 0.5f * projMatrix[1][1] * (float)viewHeight;
    lodScale = viewToPixels / maxPixelsPerEdge;

    extractFrustumPlanes(viewProjMatrix, frustumPlanes);
}

void PackedView::setup(
    const glm::vec3& eye,
    const glm::vec3& forward,
    const glm::vec3& up,
    float fovYRadians,
    float aspect,
    float nearP,
    float farP,
    int width,
    int height,
    float maxPxPerEdge)
{
    viewOrigin = eye;
    viewForward = forward;
    nearPlane = nearP;
    farPlane = farP;
    viewWidth = width;
    viewHeight = height;
    maxPixelsPerEdge = maxPxPerEdge;

    viewMatrix = glm::lookAt(eye, eye + forward, up);
    projMatrix = glm::perspective(fovYRadians, aspect, nearP, farP);

    update();
}

} // namespace nanite
