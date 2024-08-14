#pragma once

#include "cinder.hpp"
using namespace cinder::types;

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include "window.hpp"

#include "shader_shared/shared.inl"

struct CameraController
{
    void process_input(Window &window, f32 dt);
    auto get_camera_data(u32vec2 const render_target_size) const -> CameraData;

    bool bZoom = false;
    f32 fov = 70.0f;
    f32 near = 0.1f;
    f32 cameraSwaySpeed = 0.05f;
    f32 translationSpeed = 10.0f;
    f32vec3 up = {0.f, 0.f, 1.0f};
    f32vec3 forward = {0.962, -0.25, -0.087};
    f32vec3 position = {-22.f, 4.f, 6.f};
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
};