#pragma once

#include "../core/types.h"
#include "../runtime/rasterizer.h"
#include "../runtime/dag_traversal.h"

struct GLFWwindow;

namespace nanite {

class Display {
public:
    bool init(int width, int height, const char* title);
    void shutdown();

    // Upload CPU framebuffer to GPU texture and draw fullscreen quad
    void present(const Framebuffer& fb);

    GLFWwindow* getWindow() const { return window; }
    bool shouldClose() const;

private:
    GLFWwindow* window = nullptr;
    uint32_t    textureId = 0;
    uint32_t    quadVAO = 0;
    uint32_t    quadVBO = 0;
    uint32_t    shaderProgram = 0;

    void createFullscreenQuad();
    uint32_t compileShader(uint32_t type, const char* source);
    void createShaderProgram();
};

} // namespace nanite
