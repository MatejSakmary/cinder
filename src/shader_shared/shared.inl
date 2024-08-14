#pragma once

#include "daxa/daxa.inl" // IWYU pragma: export

#if defined(__cplusplus)
#define GLM_FORCE_DEPTH_ZERO_TO_ONE 1
#include <glm/glm.hpp>
#define glmsi32vec2 glm::ivec2
#define glmsf32vec2 glm::vec2
#define glmsf32vec3 glm::vec3
#define glmsf32vec4 glm::vec4
#define glmsf32mat4 glm::mat4
#else
#define glmsi32vec2 daxa_i32vec2
#define glmsf32vec2 daxa_f32vec2
#define glmsf32vec3 daxa_f32vec3
#define glmsf32vec4 daxa_f32vec4
#define glmsf32mat4 daxa_f32mat4x4
#endif

struct CameraData
{
    glmsf32mat4 world_to_view;
    glmsf32mat4 view_to_world;
    glmsf32mat4 view_to_clip;
    glmsf32mat4 clip_to_view;
    glmsf32mat4 world_to_clip;
    glmsf32mat4 clip_to_world;
    glmsf32vec3 position;
    glmsf32vec3 up;
    // vec4 for planes contains normal (xyz) and offset (w).
    glmsf32vec3 near_plane_normal;
    glmsf32vec3 left_plane_normal;
    glmsf32vec3 right_plane_normal;
    glmsf32vec3 top_plane_normal;
    glmsf32vec3 bottom_plane_normal;
    daxa_u32vec2 screen_size;
    daxa_f32vec2 inv_screen_size;
    daxa_f32 near_plane;
    daxa_f32 orthogonal_half_ws_width;
    daxa_b32 is_orthogonal;
};

struct GlobalData
{
    CameraData main_camera_data;
};