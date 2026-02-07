#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "core/mesh_loader.h"
#include "build/cluster_dag.h"
#include "runtime/packed_view.h"
#include "runtime/dag_traversal.h"
#include "runtime/rasterizer.h"
#include "render/display.h"
#include "render/camera.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace nanite;

// --- Globals for GLFW callbacks ---
static Camera    gCamera;
static bool      gMouseCaptured = false;
static float     gLastMouseX = 0.0f, gLastMouseY = 0.0f;
static bool      gFirstMouse = true;
static RenderMode gRenderMode = RenderMode::LODColors;
static float     gMaxPixelsPerEdge = 1.0f;
static bool      gKeysPressed[512] = {};

// --- GLFW Callbacks ---

static void keyCallback(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key < 0 || key >= 512) return;

    if (action == GLFW_PRESS) {
        gKeysPressed[key] = true;

        switch (key) {
        case GLFW_KEY_1: gRenderMode = RenderMode::Solid; break;
        case GLFW_KEY_2: gRenderMode = RenderMode::LODColors; break;
        case GLFW_KEY_3: gRenderMode = RenderMode::ClusterColors; break;
        case GLFW_KEY_4: gRenderMode = RenderMode::Wireframe; break;
        case GLFW_KEY_5: gRenderMode = RenderMode::VisBuffer; break;
        case GLFW_KEY_6: gRenderMode = RenderMode::Depth; break;
        case GLFW_KEY_EQUAL:
            gMaxPixelsPerEdge *= 0.8f;
            if (gMaxPixelsPerEdge < 0.1f) gMaxPixelsPerEdge = 0.1f;
            printf("Quality: maxPixelsPerEdge = %.2f\n", gMaxPixelsPerEdge);
            break;
        case GLFW_KEY_MINUS:
            gMaxPixelsPerEdge *= 1.25f;
            if (gMaxPixelsPerEdge > 50.0f) gMaxPixelsPerEdge = 50.0f;
            printf("Quality: maxPixelsPerEdge = %.2f\n", gMaxPixelsPerEdge);
            break;
        case GLFW_KEY_TAB:
            gMouseCaptured = !gMouseCaptured;
            glfwSetInputMode(w, GLFW_CURSOR,
                gMouseCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            gFirstMouse = true;
            break;
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(w, true);
            break;
        }
    } else if (action == GLFW_RELEASE) {
        gKeysPressed[key] = false;
    }
}

static void mouseCallback(GLFWwindow* /*w*/, double xpos, double ypos) {
    if (!gMouseCaptured) return;

    float x = (float)xpos, y = (float)ypos;
    if (gFirstMouse) {
        gLastMouseX = x;
        gLastMouseY = y;
        gFirstMouse = false;
        return;
    }
    float xOff = x - gLastMouseX;
    float yOff = gLastMouseY - y; // reversed Y
    gLastMouseX = x;
    gLastMouseY = y;
    gCamera.processMouse(xOff, yOff);
}

static void scrollCallback(GLFWwindow* /*w*/, double /*xOff*/, double yOff) {
    gCamera.processScroll((float)yOff);
}

// --- Process continuous keyboard input ---
static void processInput(float deltaTime) {
    if (gKeysPressed[GLFW_KEY_W]) gCamera.processKeyboard(Camera::FORWARD, deltaTime);
    if (gKeysPressed[GLFW_KEY_S]) gCamera.processKeyboard(Camera::BACKWARD, deltaTime);
    if (gKeysPressed[GLFW_KEY_A]) gCamera.processKeyboard(Camera::LEFT, deltaTime);
    if (gKeysPressed[GLFW_KEY_D]) gCamera.processKeyboard(Camera::RIGHT, deltaTime);
    if (gKeysPressed[GLFW_KEY_Q] || gKeysPressed[GLFW_KEY_SPACE])
        gCamera.processKeyboard(Camera::UP, deltaTime);
    if (gKeysPressed[GLFW_KEY_E] || gKeysPressed[GLFW_KEY_LEFT_SHIFT])
        gCamera.processKeyboard(Camera::DOWN, deltaTime);

    // Speed adjustment
    if (gKeysPressed[GLFW_KEY_LEFT_BRACKET])  gCamera.speed = std::max(0.1f, gCamera.speed * 0.95f);
    if (gKeysPressed[GLFW_KEY_RIGHT_BRACKET]) gCamera.speed = std::min(50.0f, gCamera.speed * 1.05f);
}

// --- Main ---

int main(int argc, char** argv) {
    printf("=== Nanite Demo - Simplified Virtualized Geometry ===\n\n");

    // Parse arguments
    std::string meshPath = (argc > 1) ? argv[1] : "assets/bunny.obj";
    int width  = 1280;
    int height = 720;

    // 1. Load mesh
    printf("Loading mesh: %s\n", meshPath.c_str());
    RawMesh mesh;
    if (!loadOBJ(meshPath, mesh)) {
        fprintf(stderr, "Failed to load mesh. Usage: NaniteDemo <path_to.obj>\n");
        return 1;
    }
    printf("Mesh: %zu vertices, %u triangles\n",
           mesh.vertices.size(), mesh.numTris());

    // 2. Build Nanite DAG (offline build pipeline)
    printf("\n--- Building Cluster DAG ---\n");
    ClusterDAG dag;
    auto buildStart = std::chrono::high_resolution_clock::now();
    dag.build(mesh);
    auto buildEnd = std::chrono::high_resolution_clock::now();
    float buildMs = std::chrono::duration<float, std::milli>(buildEnd - buildStart).count();
    printf("Build complete: %.1f ms\n", buildMs);

    int32_t maxMipLevel = dag.getMaxMipLevel();

    // Position camera close to mesh surface so LOD transitions are visible
    glm::vec3 meshCenter = dag.totalBounds.center();
    glm::vec3 meshExtent = dag.totalBounds.extent();
    float meshRadius = glm::length(meshExtent);
    gCamera.position = meshCenter + glm::vec3(0, 0, meshRadius * 1.2f);
    gCamera.front = glm::normalize(meshCenter - gCamera.position);
    gCamera.speed = meshRadius * 0.5f;

    // 3. Init display
    Display display;
    if (!display.init(width, height, "Nanite Demo - Simplified Virtualized Geometry")) {
        return 1;
    }

    // Set callbacks
    GLFWwindow* win = display.getWindow();
    glfwSetKeyCallback(win, keyCallback);
    glfwSetCursorPosCallback(win, mouseCallback);
    glfwSetScrollCallback(win, scrollCallback);

    // 4. Create framebuffer
    Framebuffer fb;
    fb.resize(width, height);

    // 5. Main loop
    printf("\n--- Controls ---\n");
    printf("  Tab:       Toggle mouse capture\n");
    printf("  WASD/QE:   Move camera\n");
    printf("  Mouse:     Look around (when captured)\n");
    printf("  Scroll:    Zoom (FOV)\n");
    printf("  1-6:       Render modes (Solid, LOD, Cluster, Wire, VisBuf, Depth)\n");
    printf("  +/-:       Adjust LOD quality (maxPixelsPerEdge)\n");
    printf("  [ / ]:     Adjust camera speed\n");
    printf("  Esc:       Quit\n\n");

    float lastTime = (float)glfwGetTime();
    int frameCount = 0;
    float statTimer = 0.0f;

    while (!display.shouldClose()) {
        float currentTime = (float)glfwGetTime();
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;
        if (deltaTime > 0.1f) deltaTime = 0.1f; // cap

        glfwPollEvents();
        processInput(deltaTime);

        // Setup view
        PackedView view;
        view.setup(
            gCamera.position, gCamera.front, gCamera.up,
            glm::radians(gCamera.fovY),
            (float)width / (float)height,
            0.01f, meshRadius * 20.0f,
            width, height,
            gMaxPixelsPerEdge
        );

        // Traverse DAG - select visible clusters
        std::vector<VisibleCluster> visible;
        TraversalStats traversalStats;
        traverseDAG(dag, view, visible, traversalStats);

        // Rasterize
        fb.clear();
        RasterStats rasterStats;
        rasterize(dag.clusters, visible, view, fb, gRenderMode, rasterStats, maxMipLevel);

        // Display
        display.present(fb);

        // Print stats periodically
        frameCount++;
        statTimer += deltaTime;
        if (statTimer >= 1.0f) {
            float fps = (float)frameCount / statTimer;
            printf("\r[%s] FPS: %.1f | Clusters: %u/%zu visible | Tris: %u | Culled: %u | PxPerEdge: %.2f   ",
                   renderModeName(gRenderMode),
                   fps,
                   traversalStats.clustersSelected,
                   dag.clusters.size(),
                   traversalStats.totalTriangles,
                   traversalStats.clustersFrustumCulled,
                   gMaxPixelsPerEdge);
            fflush(stdout);
            frameCount = 0;
            statTimer = 0.0f;
        }
    }

    printf("\n\nShutting down...\n");
    display.shutdown();
    return 0;
}
