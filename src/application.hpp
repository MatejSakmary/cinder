#pragma once

#include <chrono>
#include <memory>

#include "cinder.hpp"
using namespace cinder::types;

#include "gpu_context.hpp"
#include "window.hpp"
#include "camera.hpp"

#include "scene/scene.hpp"
#include "scene/asset_processor.hpp"
#include "multithreading/thread_pool.hpp"
#include "rendering/renderer.hpp"

struct Application
{
public:
    Application();
    ~Application();

    auto run() -> i32;
private:
    void update();

    std::unique_ptr<Window> window = {};
    std::unique_ptr<GPUContext> gpu_context = {};
    std::unique_ptr<Scene> scene = {};
    std::unique_ptr<AssetProcessor> asset_processor = {};
    std::unique_ptr<ThreadPool> threadpool = {};
    std::unique_ptr<Renderer> renderer = {};

    CameraController camera_controller = {};

    bool keep_running = true;
    f32 delta_time = 0.016666f;
    std::chrono::time_point<std::chrono::steady_clock> last_time_point = {};
};