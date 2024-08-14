#include "application.hpp"

Application::Application()
{
    threadpool = std::make_unique<ThreadPool>(7);
    window = std::make_unique<Window>(1920, 1080, "Cinder");
    gpu_context = std::make_unique<GPUContext>(*window);
    scene = std::make_unique<Scene>(gpu_context->device);
    asset_processor = std::make_unique<AssetProcessor>(gpu_context->device);
    renderer = std::make_unique<Renderer>(CreateRendererInfo{
        .window = window.get(),
        .gpu_context = gpu_context.get(),
        .scene = scene.get(),
    });

    renderer->compile_pipelines();

    std::filesystem::path const DEFAULT_HARDCODED_PATH = "./assets";
    // std::filesystem::path const DEFAULT_HARDCODED_FILE = "caldera/hotel/hotel.gltf";
    std::filesystem::path const DEFAULT_HARDCODED_FILE = "bistro_compressed/bistro_c.gltf";

    auto const result = scene->load_manifest_from_gltf({
        .root_path = DEFAULT_HARDCODED_PATH,
        .asset_name = DEFAULT_HARDCODED_FILE,
        .thread_pool = threadpool,
        .asset_processor = asset_processor
    });

    if(Scene::LoadManifestErrorCode const * err = std::get_if<Scene::LoadManifestErrorCode>(&result)) {
        DEBUG_MESSAGE(fmt::format("[WARN][Application::Application()] Loading \"{}\" Error {}",
            (DEFAULT_HARDCODED_PATH / DEFAULT_HARDCODED_FILE).string(), Scene::to_string(*err)));
    } 
    else {
        DEBUG_MESSAGE(fmt::format("[Info][Application::Application()] Loading \"{}\" Success",
            (DEFAULT_HARDCODED_PATH / DEFAULT_HARDCODED_FILE).string()));
    }
    last_time_point = std::chrono::steady_clock::now();
}

using FpMili = std::chrono::duration<f32, std::chrono::milliseconds::period>;
auto Application::run() -> i32
{
    while(keep_running)
    {
        auto new_time_point = std::chrono::steady_clock::now();
        delta_time = std::chrono::duration_cast<FpMili>(new_time_point - last_time_point).count() * 0.001f;
        last_time_point = new_time_point;

        window->update(delta_time);
        keep_running &= !s_cast<bool>(glfwWindowShouldClose(window->glfw_handle));

        i32vec2 new_window_size = {};
        glfwGetWindowSize(window->glfw_handle, &new_window_size.x, &new_window_size.y);

        if(window->size != new_window_size)
        {
            window->size = new_window_size;
        }

        if(window->size.x != 0.0f && window->size.y != 0.0f)
        {
            update();
            gpu_context->device.collect_garbage();
        }
    }

    return 0;
}

void Application::update()
{
    auto asset_data_upload_info = asset_processor->record_gpu_load_processing_commands();
    auto manifest_update_commands = scene->record_gpu_manifest_update({
        .uploaded_meshes = asset_data_upload_info.uploaded_meshes,
        .uploaded_textures = asset_data_upload_info.uploaded_textures
    });
    auto build_blas_commands = scene->create_and_record_build_as();

    auto cmd_lists = std::array{
        std::move(asset_data_upload_info.upload_commands),
        std::move(manifest_update_commands),
        std::move(build_blas_commands)
    };

    camera_controller.process_input(*window, delta_time);

    gpu_context->device.submit_commands({.command_lists = cmd_lists});
    auto const swapchain_resolution = u32vec2(
        gpu_context->swapchain.get_surface_extent().x, 
        gpu_context->swapchain.get_surface_extent().y
    );
    renderer->render_frame({
        .camera_info = camera_controller.get_camera_data(swapchain_resolution),
        .delta_time = delta_time,
        .scene_tlas = scene->gpu_tlas,
    });
}

Application::~Application()
{
    threadpool.reset();
    auto asset_data_upload_info = asset_processor->record_gpu_load_processing_commands();
    auto manifest_update_commands = scene->record_gpu_manifest_update({
        .uploaded_meshes = asset_data_upload_info.uploaded_meshes,
        .uploaded_textures = asset_data_upload_info.uploaded_textures,
    });
    auto cmd_lists = std::array{
        std::move(asset_data_upload_info.upload_commands),
        std::move(manifest_update_commands)
    };
    gpu_context->device.submit_commands({.command_lists = cmd_lists});
    gpu_context->device.wait_idle();
    gpu_context->device.collect_garbage();
}