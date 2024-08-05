#pragma once

#include "daxa/daxa.inl"

struct GPUMaterial
{
    daxa_ImageViewId diffuse_texture_id;
    daxa_ImageViewId opacity_texture_id;
    daxa_ImageViewId normal_texture_id;
    daxa_ImageViewId roughnes_metalness_id;
    daxa_b32 alpha_discard_enabled;  
    daxa_b32 normal_compressed_bc5_rg;
    daxa_f32vec3 base_color;
};

struct GPUMesh
{
    daxa_BufferId mesh_buffer;
    daxa_u32 material_index = {};
    daxa_u32 vertex_count = {};
    daxa_u32 index_count = {};
    daxa_BufferPtr(daxa_f32vec3) vertex_positions;
    daxa_BufferPtr(daxa_f32vec2) vertex_uvs;
    daxa_BufferPtr(daxa_f32vec3) vertex_normals;
    daxa_BufferPtr(daxa_u32) indices;
};

struct GPUMeshGroup
{
    daxa_BufferPtr(daxa_u32) mesh_indices;
    daxa_u32 count;
};

#define INVALID_MANIFEST_INDEX (~0u)