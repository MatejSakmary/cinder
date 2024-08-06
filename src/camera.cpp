#include "camera.hpp"

void CameraController::process_input(Window & window, f32 dt)
{
    f32 speed = window.key_pressed(GLFW_KEY_LEFT_SHIFT) ? translationSpeed * 4.0f : translationSpeed;
    speed = window.key_pressed(GLFW_KEY_LEFT_CONTROL) ? speed * 0.25f : speed;

    if (window.is_focused())
    {
        if (window.key_just_pressed(GLFW_KEY_ESCAPE))
        {
            if (window.is_cursor_captured()) { window.release_cursor(); }
            else { window.capture_cursor(); }
        }
    }
    else if (window.is_cursor_captured()) { window.release_cursor(); }

    auto cameraSwaySpeed = this->cameraSwaySpeed;
    if (window.key_pressed(GLFW_KEY_C))
    {
        cameraSwaySpeed *= 0.25;
        bZoom = true;
    }
    else { bZoom = false; }

    glm::vec3 right = glm::cross(forward, up);
    glm::vec3 fake_up = glm::normalize(glm::cross(right, forward));
    if (window.is_cursor_captured())
    {
        if (window.key_pressed(GLFW_KEY_W)) { position += forward * speed * dt; }
        if (window.key_pressed(GLFW_KEY_S)) { position -= forward * speed * dt; }
        if (window.key_pressed(GLFW_KEY_A)) { position -= glm::normalize(glm::cross(forward, up)) * speed * dt; }
        if (window.key_pressed(GLFW_KEY_D)) { position += glm::normalize(glm::cross(forward, up)) * speed * dt; }
        if (window.key_pressed(GLFW_KEY_SPACE)) { position += fake_up * speed * dt; }
        if (window.key_pressed(GLFW_KEY_LEFT_ALT)) { position -= fake_up * speed * dt; }
        if (window.key_pressed(GLFW_KEY_Q)) { position -= up * speed * dt; }
        if (window.key_pressed(GLFW_KEY_E)) { position += up * speed * dt; }
        pitch += window.get_cursor_change_y() * cameraSwaySpeed;
        pitch = std::clamp(pitch, -85.0f, 85.0f);
        yaw += window.get_cursor_change_x() * cameraSwaySpeed;
    }
    forward.x = -glm::cos(glm::radians(yaw - 90.0f)) * glm::cos(glm::radians(pitch));
    forward.y = glm::sin(glm::radians(yaw - 90.0f)) * glm::cos(glm::radians(pitch));
    forward.z = -glm::sin(glm::radians(pitch));
}

auto CameraController::make_camera_info(u32vec2 const render_target_size) const -> CameraInfo
{
    auto fov = this->fov;
    if (bZoom) { fov *= 0.25f; }
    auto inf_depth_reverse_z_perspective = [](auto fov_rads, auto aspect, auto zNear)
    {
        assert(abs(aspect - std::numeric_limits<f32>::epsilon()) > 0.0f);

        f32 const tanHalfFovy = 1.0f / std::tan(fov_rads * 0.5f);

        glm::mat4x4 ret(0.0f);
        ret[0][0] = tanHalfFovy / aspect;
        ret[1][1] = tanHalfFovy;
        ret[2][2] = 0.0f;
        ret[2][3] = -1.0f;
        ret[3][2] = zNear;
        return ret;
    };
    glm::mat4 prespective =
        inf_depth_reverse_z_perspective(glm::radians(fov), f32(render_target_size.x) / f32(render_target_size.y), near);
    prespective[1][1] *= -1.0f;
    CameraInfo ret = {};
    ret.proj = prespective;
    ret.inv_proj = glm::inverse(prespective);
    ret.view = glm::lookAt(position, position + forward, up);
    ret.inv_view = glm::inverse(ret.view);
    ret.view_proj = ret.proj * ret.view;
    ret.inv_view_proj = glm::inverse(ret.view_proj);
    ret.position = this->position;
    ret.up = this->up;
    glm::vec3 ws_ndc_corners[2][2][2];
    glm::mat4 inv_view_proj = glm::inverse(ret.proj * ret.view);
    for (u32 z = 0; z < 2; ++z)
    {
        for (u32 y = 0; y < 2; ++y)
        {
            for (u32 x = 0; x < 2; ++x)
            {
                glm::vec3 corner = glm::vec3((glm::vec2(x, y) - 0.5f) * 2.0f, 1.0f - z * 0.5f);
                glm::vec4 proj_corner = inv_view_proj * glm::vec4(corner, 1);
                ws_ndc_corners[x][y][z] = glm::vec3(proj_corner) / proj_corner.w;
            }
        }
    }
    ret.is_orthogonal = 0u;
    ret.orthogonal_half_ws_width = 0.0f;
    ret.near_plane_normal = glm::normalize(
        glm::cross(ws_ndc_corners[0][1][0] - ws_ndc_corners[0][0][0], ws_ndc_corners[1][0][0] - ws_ndc_corners[0][0][0]));
    ret.right_plane_normal = glm::normalize(
        glm::cross(ws_ndc_corners[1][1][0] - ws_ndc_corners[1][0][0], ws_ndc_corners[1][0][1] - ws_ndc_corners[1][0][0]));
    ret.left_plane_normal = glm::normalize(
        glm::cross(ws_ndc_corners[0][1][1] - ws_ndc_corners[0][0][1], ws_ndc_corners[0][0][0] - ws_ndc_corners[0][0][1]));
    ret.top_plane_normal = glm::normalize(
        glm::cross(ws_ndc_corners[1][0][0] - ws_ndc_corners[0][0][0], ws_ndc_corners[0][0][1] - ws_ndc_corners[0][0][0]));
    ret.bottom_plane_normal = glm::normalize(
        glm::cross(ws_ndc_corners[0][1][1] - ws_ndc_corners[0][1][0], ws_ndc_corners[1][1][0] - ws_ndc_corners[0][1][0]));
    int i = 0;
    ret.screen_size = { render_target_size.x, render_target_size.y };
    ret.inv_screen_size = {
        1.0f / static_cast<f32>(render_target_size.x),
        1.0f / static_cast<f32>(render_target_size.y),
    };
    ret.near_plane = this->near;
    return ret;
}