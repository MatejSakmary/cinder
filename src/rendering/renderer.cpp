#include "renderer.hpp"

#include "misc.hpp"
#include "tasks/basic_raytracing.inl"

Renderer::Renderer(CreateRendererInfo const & info) :
    window{info.window},
    gpu_context{info.gpu_context},
    scene{info.scene}
{
    swapchain_image = daxa::TaskImage{{.swapchain_image = true, .name = "swapchain_image"}};
    scene_tlas = daxa::TaskTlas({.name = "task scene tlas"});
    main_task_graph = create_main_task_graph();
}

Renderer::~Renderer()
{
    if(gpu_context->device.is_id_valid(basic_rt_sbt_info.buffer))
    {
        gpu_context->device.destroy_buffer(basic_rt_sbt_info.buffer);
    }
}

void Renderer::compile_pipelines()
{
    std::vector<daxa::RayTracingPipelineCompileInfo> raytracing_pipelines {
        {basic_raytracing_pipeline_compile_info()},
    };

    for (auto const & info : raytracing_pipelines)
    {
        auto compilation_result = gpu_context->pipeline_manager.add_ray_tracing_pipeline(info);
        if (compilation_result.value()->is_valid())
        {
            DEBUG_MESSAGE(fmt::format("[Renderer::compile_pipelines()] SUCCESFULLY compiled pipeline {}", info.name));
        }
        else
        {
            DEBUG_MESSAGE(fmt::format("[Renderer::compile_pipelines()] FAILED to compile pipeline {} with message \n {}", info.name,
                compilation_result.message()));
        }
        gpu_context->raytracing_pipelines[info.name] = compilation_result.value();
        auto [sbt_buffer_id, sbt] = gpu_context->raytracing_pipelines[info.name]->create_default_sbt();
        basic_rt_sbt_info.buffer = sbt_buffer_id;
        basic_rt_sbt_info.sbt = sbt;
    }

    while(!gpu_context->pipeline_manager.all_pipelines_valid())
    {
        auto const result = gpu_context->pipeline_manager.reload_all();
        if (daxa::holds_alternative<daxa::PipelineReloadError>(result))
        {
            MESSAGE(daxa::get<daxa::PipelineReloadError>(result).message);
        }
        using namespace std::literals;
        std::this_thread::sleep_for(30ms);
    }
}

auto Renderer::create_main_task_graph() -> daxa::TaskGraph
{
    daxa::TaskGraph task_graph = {{
        .device = gpu_context->device,
        .swapchain = gpu_context->swapchain,
        .staging_memory_pool_size = 2'097'152, // 2MiB
        .name = "Cinder Main Task Graph"
    }};

    auto globals_gpu_buffer = task_graph.create_transient_buffer({
        .size = sizeof(GlobalData),
        .name = "transient global data",
    });

    global_data_cpu = std::make_unique<GlobalData>();
    task_graph.use_persistent_image(swapchain_image);
    task_graph.use_persistent_tlas(scene_tlas);
    task_graph.use_persistent_buffer(scene->gpu_mesh_group_manifest);
    task_graph.use_persistent_buffer(scene->gpu_mesh_manifest);

    task_graph.add_task({
        .attachments = {daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, globals_gpu_buffer)},
        .task = [this, globals_gpu_buffer](daxa::TaskInterface ti)
        {
            allocate_fill_copy(ti, *global_data_cpu, ti.get(globals_gpu_buffer));
        }
    });

    task_graph.add_task({
        .attachments = {daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, swapchain_image)},
        .task = [this](daxa::TaskInterface ti)
        {
            ti.recorder.clear_image({
                .clear_value = {std::array{0.0f, 0.0f, 1.0f, 0.0f}},
                .dst_image = ti.get(swapchain_image).ids[0]
            });
        }
    });

    task_graph.add_task(BasicRaytraceTask{
        .views = std::array{
            daxa::attachment_view(BasicRaytraceH::AT.globals, globals_gpu_buffer),
            daxa::attachment_view(BasicRaytraceH::AT.scene_tlas, scene_tlas),
            daxa::attachment_view(BasicRaytraceH::AT.gpu_mesh_group_manifest, scene->gpu_mesh_group_manifest),
            daxa::attachment_view(BasicRaytraceH::AT.gpu_mesh_manifest, scene->gpu_mesh_manifest),
            daxa::attachment_view(BasicRaytraceH::AT.swapchain_image, swapchain_image),
        },
        .gpu_context = gpu_context,
        .sbt = &basic_rt_sbt_info.sbt,
    });

    task_graph.submit({});
    task_graph.present({});
    task_graph.complete({});
    return task_graph;
}

void Renderer::render_frame(RenderFrameInfo const & info)
{
    auto new_swapchain_image = gpu_context->swapchain.acquire_next_image();   
    if(new_swapchain_image.is_empty()) { return; }
    swapchain_image.set_images({.images = std::array{new_swapchain_image}});
    scene_tlas.set_tlas({.tlas = std::array{info.scene_tlas}});

    global_data_cpu->main_camera_data = info.camera_info;

    main_task_graph.execute({});
}