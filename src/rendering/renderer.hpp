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
    GPUContext * gpu_context = {};
    Scene * scene = {};
};

struct RenderFrameInfo
{
    CameraData const & camera_info = {};
    f32 const delta_time = {};
    daxa::TlasId const & scene_tlas = {};
};

struct SBTInfo
{
    daxa::BufferId buffer;
    daxa::RayTracingShaderBindingTable sbt;
};

struct Renderer
{
    Renderer(CreateRendererInfo const & info);
    ~Renderer();

    void compile_pipelines();
    void recreate_framebuffer();
    void window_resized();
    void render_frame(RenderFrameInfo const & info);
    auto get_swapchain_resolution() -> u32vec2;

  private:
    auto create_main_task_graph() -> daxa::TaskGraph;

    Window *window = {};
    GPUContext *gpu_context = {};
    Scene *scene = {};

    std::unique_ptr<GlobalData> global_data_cpu;

    SBTInfo basic_rt_sbt_info = {};
    daxa::TaskImage swapchain_image = {};
    daxa::TaskTlas scene_tlas = {};
    daxa::TaskGraph main_task_graph = {};
};