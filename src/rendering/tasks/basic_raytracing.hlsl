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

[shader("closesthit")]
void closest_hit_shader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    float3 hit_location = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    RayDesc ray;
    ray.Origin = hit_location;
    ray.Direction = normalize(float3(1.0f, 2.0f, 3.0f));
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    RayPayload shadow_payload = {};
    TraceRay(RaytracingAccelerationStructure::get(push.scene_tlas), RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, ~0, 1, 0, 0, ray, shadow_payload);
    float shadow = 1.0f - shadow_payload.color.x;

    let flat_color = float4(1.0f);
    payload.color = float4(flat_color * shadow);
    
    // payload.color = float4(1.0, 0.0, 0.0, 1.0);
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