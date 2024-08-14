#include "basic_raytracing.inl"

#include <daxa/daxa.inl>

[[vk::push_constant]] BasicRaytraceH::AttachmentShaderBlob push;

struct RayPayload
{
    float4 color;
}

[shader("raygeneration")]
void ray_gen()
{
    const uint2 thread_idx = DispatchRaysIndex().xy;
    const uint2 resolution = push.globals.main_camera_data.screen_size;
    const float2 uv = float2(thread_idx) / float2(resolution); 
    if(thread_idx.x >= resolution.x || thread_idx.y >= resolution.y)
    {
        return;
    }

    RayDesc ray = {};
    const float3 clip_space_coord = float3((uv * 2.0f) - 1.0f, 0.0f);
    const float3 ray_direction = mul(push.globals.main_camera_data.clip_to_world, float4(clip_space_coord, 1.0f)).xyz;
    ray.Origin = push.globals.main_camera_data.position;
    ray.Direction = normalize(ray_direction);
    ray.TMax = 10000.0f;
    ray.TMin = 0.001f;

    RayPayload payload = {};
    TraceRay(RaytracingAccelerationStructure::get(push.scene_tlas), RAY_FLAG_NONE, ~0, 0, 0, 0, ray, payload);
    RWTexture2D<float>::get(push.swapchain_image)[thread_idx] = payload.color;
}

float3 hsv2rgb(float3 c) {
    float4 k = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + k.xyz) * 6.0 - k.www);
    return c.z * lerp(k.xxx, clamp(p - k.xxx, 0.0, 1.0), c.y);
}

[shader("closesthit")]
void closest_hit_shader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const float3 hit_location = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    RayDesc ray;
    ray.Origin = hit_location;
    ray.Direction = normalize(float3(1.0f, 2.0f, 3.0f));
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    RayPayload shadow_payload = {};
    TraceRay(RaytracingAccelerationStructure::get(push.scene_tlas), RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, ~0, 1, 0, 0, ray, shadow_payload);
    float shadow = 1.0f - shadow_payload.color.x;

    const uint custom_instance_index = InstanceID();
    const uint geometry_index = GeometryIndex();
    
    const GPUMeshGroup *mesh_group = &push.gpu_mesh_group_manifest[custom_instance_index];
    const uint mesh_index = mesh_group->mesh_indices[geometry_index];
    const GPUMesh *mesh = &push.gpu_mesh_manifest[mesh_index];

    const uint index_buffer_offset = PrimitiveIndex() * 3;

    const uint vertex_index_0 = mesh.indices[index_buffer_offset];
    const uint vertex_index_1 = mesh.indices[index_buffer_offset + 1];
    const uint vertex_index_2 = mesh.indices[index_buffer_offset + 2];

    // const float2 uv_0 = mesh.vertex_uvs[vertex_index_0];
    // const float2 uv_1 = mesh.vertex_uvs[vertex_index_1];
    // const float2 uv_2 = mesh.vertex_uvs[vertex_index_2];
    // const float2 interp_uv = uv_0 + attr.barycentrics.x * (uv_1 - uv_0) + attr.barycentrics.y* (uv_2 - uv_0);

    const float3 flat_normal_0 = mesh.vertex_normals[vertex_index_0];
    const float3 flat_normal_1 = mesh.vertex_normals[vertex_index_1];
    const float3 flat_normal_2 = mesh.vertex_normals[vertex_index_2];
    const float3 normal = flat_normal_0 + attr.barycentrics.x * (flat_normal_1 - flat_normal_0) + attr.barycentrics.y* (flat_normal_2 - flat_normal_0);

    const float ndotl = max(0.0f, shadow * dot(normal, ray.Direction.xyz));
    const float intensity = ndotl * 0.9f + 0.1f;
    const float3 color = float3(1.0f);
    // color.rgb = hsv2rgb(float3(float(custom_instance_index + index_buffer_offset) * 0.1323f, 1, 1));

    payload.color = float4(color * intensity, 1.0f);
}

[shader("closesthit")]
void shadow_ray_hit_shader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.color = float4(1.0);
}

[shader("miss")]
void miss_shader(inout RayPayload payload)
{
    payload.color = float4(0.01f, 0.01f, 0.01f, 1.0f);
}