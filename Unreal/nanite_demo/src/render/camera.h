#pragma once

#include "../core/types.h"

namespace nanite {

class Camera {
public:
    glm::vec3 position = glm::vec3(0, 0, 3);
    glm::vec3 front    = glm::vec3(0, 0, -1);
    glm::vec3 up       = glm::vec3(0, 1, 0);
    glm::vec3 right    = glm::vec3(1, 0, 0);

    float yaw   = -90.0f;
    float pitch = 0.0f;
    float speed = 2.0f;
    float sensitivity = 0.15f;
    float fovY  = 45.0f;

    enum Movement { FORWARD, BACKWARD, LEFT, RIGHT, UP, DOWN };

    void processKeyboard(Movement direction, float deltaTime);
    void processMouse(float xOffset, float yOffset);
    void processScroll(float yOffset);

    glm::mat4 getViewMatrix() const;

private:
    void updateVectors();
};

} // namespace nanite
