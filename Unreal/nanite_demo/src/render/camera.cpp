#include "camera.h"

namespace nanite {

void Camera::processKeyboard(Movement direction, float deltaTime) {
    float velocity = speed * deltaTime;
    switch (direction) {
        case FORWARD:  position += front * velocity; break;
        case BACKWARD: position -= front * velocity; break;
        case LEFT:     position -= right * velocity; break;
        case RIGHT:    position += right * velocity; break;
        case UP:       position += up * velocity; break;
        case DOWN:     position -= up * velocity; break;
    }
}

void Camera::processMouse(float xOffset, float yOffset) {
    yaw   += xOffset * sensitivity;
    pitch += yOffset * sensitivity;
    pitch = std::max(-89.0f, std::min(89.0f, pitch));
    updateVectors();
}

void Camera::processScroll(float yOffset) {
    fovY -= yOffset * 2.0f;
    fovY = std::max(10.0f, std::min(120.0f, fovY));
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

void Camera::updateVectors() {
    float yawRad   = glm::radians(yaw);
    float pitchRad = glm::radians(pitch);
    front.x = std::cos(yawRad) * std::cos(pitchRad);
    front.y = std::sin(pitchRad);
    front.z = std::sin(yawRad) * std::cos(pitchRad);
    front = glm::normalize(front);
    right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));
    up    = glm::normalize(glm::cross(right, front));
}

} // namespace nanite
