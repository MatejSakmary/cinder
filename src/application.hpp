#pragma once

// Standart headers:
#include <chrono>
#include <memory>
// Library headers:
// Project headers:
#include "cinder.hpp"
using namespace cinder::types;

#include "window.hpp"

#include "scene/scene.hpp"
#include "scene/asset_processor.hpp"
#include "multithreading/thread_pool.hpp"
#include "gpu_context.hpp"

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

    bool keep_running = true;
    f32 delta_time = 0.016666f;
    std::chrono::time_point<std::chrono::steady_clock> last_time_point = {};
};