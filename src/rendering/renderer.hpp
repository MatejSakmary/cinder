#pragma once

#include <daxa/utils/imgui.hpp>

#include "../window.hpp"
#include "../scene/scene.hpp"

#include "../gpu_context.hpp"
#include "../cinder.hpp"

#include "../shader_shared/shared.inl"

using namespace cinder::types;

struct CreateRendererInfo
{
    Window * window = {};
    GPUContext * context = {};
    Scene * scene = {};
};

struct RenderFrameInfo
{
    CameraInfo const * camera_info = {};
    f32 const delta_time = {};
    Scene const * scene = {};
};

struct Renderer
{
    Renderer(CreateRendererInfo const & info);
    ~Renderer();

    void compile_pipelines();
    void recreate_framebuffer();
    void window_resized();
    void render_frame();

  private:
    auto create_main_task_graph() -> daxa::TaskGraph;

    Window *window = {};
    GPUContext *context = {};
    Scene *scene = {};

    daxa::TaskImage swapchain_image = {};
    daxa::TaskGraph main_task_graph = {};
};