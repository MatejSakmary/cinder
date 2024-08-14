#include "gpu_context.hpp"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
using HWND = void *;
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

// Not needed, this is set by cmake.
// Intellisense doesnt get it, so this prevents it from complaining.
#if !defined(DAXA_SHADER_INCLUDE_DIR)
#define DAXA_SHADER_INCLUDE_DIR "."
#endif

GPUContext::GPUContext(Window const & window)
    : context{daxa::create_instance({})},
        device{[&](){
            auto device_info = daxa::DeviceInfo2 {
                .explicit_features = daxa::ExplicitFeatureFlagBits::BUFFER_DEVICE_ADDRESS_CAPTURE_REPLAY,
                .max_allowed_images = 100'000,
                .max_allowed_buffers = 100'000,
                .name = "Cinder Device",
            };
            auto const implicit_flags = 
                daxa::ImplicitFeatureFlagBits::RAY_TRACING_PIPELINE |
                daxa::ImplicitFeatureFlagBits::BASIC_RAY_TRACING |
                daxa::ImplicitFeatureFlagBits::MESH_SHADER;
            context.choose_device(implicit_flags, device_info);
            return context.create_device_2(device_info);
        }()
      },
      swapchain{this->device.create_swapchain({
          .native_window = glfwGetWin32Window(window.glfw_handle),
          .native_window_platform = daxa::NativeWindowPlatform::WIN32_API,
          .surface_format_selector = [](daxa::Format format) -> i32
          {
              switch (format)
              {
                  case daxa::Format::R8G8B8A8_UNORM: return 80;
                  case daxa::Format::B8G8R8A8_UNORM: return 60;
                  default:                           return 0;
              }
          },
          .present_mode = daxa::PresentMode::FIFO,
          .image_usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST,
          .name = "Cinder Swapchain",
      })},
      pipeline_manager{daxa::PipelineManager{{
          .device = this->device,
          .shader_compile_options =
              []()
          {
              // msvc time!
              return daxa::ShaderCompileOptions{
                  .root_paths =
                      {
                          "./src",
                          "./src/rendering/tasks",
                          DAXA_SHADER_INCLUDE_DIR,
                      },
                  .write_out_preprocessed_code = "./preproc",
                  .write_out_shader_binary = "./spv_raw",
                //   .spirv_cache_folder = "spv",
                  .language = daxa::ShaderLanguage::SLANG,
                  .enable_debug_info = true,
              };
          }(),
          .register_null_pipelines_when_first_compile_fails = true,
          .name = "Cinder PipelineCompiler",
      }}}
{
}

auto GPUContext::dummy_string() -> std::string
{
    return std::string(" - ") + std::to_string(counter++);
}

GPUContext::~GPUContext()
{
}