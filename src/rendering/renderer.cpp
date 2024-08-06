#include "renderer.hpp"

Renderer::Renderer(CreateRendererInfo const & info) :
    window{info.window},
    context{info.context},
    scene{info.scene}
{
    swapchain_image = daxa::TaskImage{{.swapchain_image = true, .name = "swapchain_image"}};
    main_task_graph = create_main_task_graph();
}

Renderer::~Renderer()
{

}

auto Renderer::create_main_task_graph() -> daxa::TaskGraph
{
    daxa::TaskGraph task_graph = {{
        .device = context->device,
        .swapchain = context->swapchain,
        .staging_memory_pool_size = 2'097'152, // 2MiB
        .name = "Cinder Main Task Graph"
    }};

    task_graph.use_persistent_image(swapchain_image);

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

    task_graph.submit({});
    task_graph.present({});
    task_graph.complete({});
    return task_graph;
}

void Renderer::render_frame()
{
    auto new_swapchain_image = context->swapchain.acquire_next_image();   
    if(new_swapchain_image.is_empty()) { return; }
    swapchain_image.set_images({.images = std::array{new_swapchain_image}});

    main_task_graph.execute({});
}

