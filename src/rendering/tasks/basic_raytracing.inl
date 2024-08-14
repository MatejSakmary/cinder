#pragma once

#define DAXA_RAY_TRACING

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl" // IWYU pragma: export
#include "../../shader_shared/geometry.inl" // IWYU pragma: export

DAXA_DECL_TASK_HEAD_BEGIN(BasicRaytraceH)
DAXA_TH_BUFFER_PTR(RAY_TRACING_SHADER_READ, daxa_BufferPtr(GlobalData), globals)
DAXA_TH_BUFFER_PTR(RAY_TRACING_SHADER_READ, daxa_BufferPtr(GPUMeshGroup), gpu_mesh_group_manifest)
DAXA_TH_BUFFER_PTR(RAY_TRACING_SHADER_READ, daxa_BufferPtr(GPUMesh), gpu_mesh_manifest)
DAXA_TH_TLAS_ID(RAY_TRACING_SHADER_READ, scene_tlas)
DAXA_TH_IMAGE_ID(RAY_TRACING_SHADER_STORAGE_WRITE_ONLY, REGULAR_2D, swapchain_image)
DAXA_DECL_TASK_HEAD_END

#if defined(__cplusplus)
#include "../../gpu_context.hpp"
#include "../misc.hpp" 

inline auto basic_raytracing_pipeline_compile_info() -> daxa::RayTracingPipelineCompileInfo
{
    return {
        .ray_gen_infos = {
            {
                .source = daxa::ShaderFile{"basic_raytracing.hlsl"},
                .compile_options = {.entry_point = "ray_gen"},
            },
        },
        .closest_hit_infos = {
            {
                .source = daxa::ShaderFile{"basic_raytracing.hlsl"},
                .compile_options = {.entry_point = "shadow_ray_hit_shader"},
            },
            {
                .source = daxa::ShaderFile{"basic_raytracing.hlsl"},
                .compile_options = {.entry_point = "closest_hit_shader"},
            },
        },
        .miss_hit_infos = {
            {
                .source = daxa::ShaderFile{"basic_raytracing.hlsl"},
                .compile_options = {.entry_point = "miss_shader"},
            },
        },
        .shader_groups_infos = {
            // Gen Group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::GENERAL,
                .general_shader_index = 0,
            },
            // Miss group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::GENERAL,
                .general_shader_index = 3,
            },
            // Hit group
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::TRIANGLES_HIT_GROUP,
                .closest_hit_shader_index = 2,
            },
            daxa::RayTracingShaderGroupInfo{
                .type = daxa::ShaderGroup::TRIANGLES_HIT_GROUP,
                .closest_hit_shader_index = 1,
            },
        },
        .max_ray_recursion_depth = 2,
        .push_constant_size = sizeof(BasicRaytraceH::AttachmentShaderBlob),
        .name = "basic rt pipeline",
    };
}

struct BasicRaytraceTask : BasicRaytraceH::Task
{
    AttachmentViews views = {};
    GPUContext *gpu_context = {};
    daxa::RayTracingShaderBindingTable * sbt = {};

    void callback(daxa::TaskInterface ti)
    {
        auto const resolution = ti.device.info_image(ti.get(AT.swapchain_image).ids[0]).value().size;
        ti.recorder.set_pipeline(*gpu_context->raytracing_pipelines.at(basic_raytracing_pipeline_compile_info().name));
        BasicRaytraceH::AttachmentShaderBlob push = {};
        assign_blob(push, ti.attachment_shader_blob);
        ti.recorder.push_constant(push);
        ti.recorder.trace_rays({
            .width = resolution.x,
            .height = resolution.y,
            .depth = 1,
            .shader_binding_table = *sbt,
        });
    }
};
#endif // __cplusplus