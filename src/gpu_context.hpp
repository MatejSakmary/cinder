#pragma once

#include "cinder.hpp"
#include "window.hpp"

struct GPUContext
{
    GPUContext(Window const & window);
    GPUContext(GPUContext &&) = default;
    ~GPUContext();

    // common unique:
    daxa::Instance context = {};
    daxa::Device device = {};
    daxa::Swapchain swapchain = {};
    daxa::PipelineManager pipeline_manager = {};

    // Pipelines:
    std::unordered_map<std::string, std::shared_ptr<daxa::RasterPipeline>> raster_pipelines = {};
    std::unordered_map<std::string, std::shared_ptr<daxa::ComputePipeline>> compute_pipelines = {};
    std::unordered_map<std::string, std::shared_ptr<daxa::RayTracingPipeline>> raytracing_pipelines = {};

    u32 counter = {};
    auto dummy_string() -> std::string;
};