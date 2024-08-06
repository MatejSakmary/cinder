#include "scene.hpp"

#include <fstream>

#include <fastgltf/core.hpp>

#include <fmt/format.h>
#include <glm/gtx/quaternion.hpp>
#include <thread>
#include <chrono>
#include <ktx.h>

#define MAX_MESHES 40000
#define MAX_ENTITIES (1u << 20u)
#define MAX_MATERIALS (1u << 16u)

Scene::Scene(daxa::Device device)
    : _device{std::move(device)}
{
    meshgroup_mutex = std::make_unique<std::mutex>();
    /// TODO: THIS IS TEMPORARY! Make manifest and entity buffers growable!
    gpu_scratch_buffer = cinder::make_task_buffer(_device, scratch_buffer_size, "gpu_scratch_buffer"); 
    gpu_entity_parents = cinder::make_task_buffer(_device, sizeof(RenderEntityId) * MAX_ENTITIES, "_gpu_entity_parents");
    gpu_entity_transforms = cinder::make_task_buffer(_device, sizeof(daxa_f32mat4x3) * MAX_ENTITIES, "_gpu_entity_transforms");
    gpu_entity_combined_transforms = cinder::make_task_buffer(_device, sizeof(daxa_f32mat4x3) * MAX_ENTITIES, "_gpu_entity_combined_transforms");
    gpu_entity_mesh_groups = cinder::make_task_buffer(_device, sizeof(u32) * MAX_ENTITIES, "_gpu_entity_mesh_groups");
    gpu_mesh_manifest = cinder::make_task_buffer(_device, sizeof(GPUMesh) * MAX_MESHES, "_gpu_mesh_manifest");
    gpu_mesh_group_manifest = cinder::make_task_buffer(_device, sizeof(GPUMeshGroup) * MAX_MESHES, "_gpu_mesh_group_manifest");
    gpu_material_manifest = cinder::make_task_buffer(_device, sizeof(GPUMaterial) * MAX_MATERIALS, "_gpu_material_manifest");
}

Scene::~Scene()
{
    if (!gpu_mesh_group_indices_array_buffer.is_empty())
    {
        _device.destroy_buffer(gpu_mesh_group_indices_array_buffer);
    }

    for(auto & meshgroup : mesh_group_manifest)
    {
        if(meshgroup.blas.has_value())
        {
            _device.destroy_blas(meshgroup.blas.value());
        }
    }

    for (auto & mesh : mesh_manifest)
    {
        if (mesh.runtime.has_value())
        {
            _device.destroy_buffer(std::bit_cast<daxa::BufferId>(mesh.runtime.value().mesh_buffer));
        }
    }
    for (auto & texture : material_texture_manifest)
    {
        if (texture.runtime_texture.has_value())
        {
            _device.destroy_image(std::bit_cast<daxa::ImageId>(texture.runtime_texture.value()));
        }
        if (texture.secondary_runtime_texture.has_value())
        {
            _device.destroy_image(std::bit_cast<daxa::ImageId>(texture.secondary_runtime_texture.value()));
        }
    }
}
// TODO: Loading god function.
struct LoadManifestFromFileContext
{
    std::filesystem::path file_path = {};
    fastgltf::Asset asset;
    u32 gltf_asset_manifest_index = {};
    u32 texture_manifest_offset = {};
    u32 material_manifest_offset = {};
    u32 mesh_group_manifest_offset = {};
    u32 mesh_manifest_offset = {};
};
static auto get_load_manifest_data_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info) -> std::variant<LoadManifestFromFileContext, Scene::LoadManifestErrorCode>;
static void update_material_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx);
static void update_texture_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx);
static void update_meshgroup_and_mesh_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx);
static void start_async_loads_of_dirty_meshes(Scene & scene, Scene::LoadManifestInfo const & info);
static void start_async_loads_of_dirty_textures(Scene & scene, Scene::LoadManifestInfo const & info);
// Returns root entity of loaded asset.
static auto update_entities_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & ctx) -> RenderEntityId;

auto Scene::load_manifest_from_gltf(LoadManifestInfo const & info) -> std::variant<RenderEntityId, LoadManifestErrorCode>
{
    RenderEntityId root_r_ent_id = {};
    {
        auto load_result = get_load_manifest_data_from_gltf(*this, info);
        if (std::holds_alternative<LoadManifestErrorCode>(load_result))
        {
            return std::get<LoadManifestErrorCode>(load_result);
        }
        LoadManifestFromFileContext load_ctx = std::get<LoadManifestFromFileContext>(std::move(load_result));
        update_texture_manifest_from_gltf(*this, info, load_ctx);
        update_material_manifest_from_gltf(*this, info, load_ctx);
        update_meshgroup_and_mesh_manifest_from_gltf(*this, info, load_ctx);
        root_r_ent_id = update_entities_from_gltf(*this, info, load_ctx);
        gltf_asset_manifest.push_back(GltfAssetManifestEntry{
            .path = load_ctx.file_path,
            .gltf_asset = std::make_unique<fastgltf::Asset>(std::move(load_ctx.asset)),
            .texture_manifest_offset = load_ctx.texture_manifest_offset,
            .material_manifest_offset = load_ctx.material_manifest_offset,
            .mesh_group_manifest_offset = load_ctx.mesh_group_manifest_offset,
            .mesh_manifest_offset = load_ctx.mesh_manifest_offset,
            .root_render_entity = root_r_ent_id,
        });
    }
    start_async_loads_of_dirty_meshes(*this, info);
    start_async_loads_of_dirty_textures(*this, info);

    return root_r_ent_id;
}

static auto get_load_manifest_data_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info) -> std::variant<LoadManifestFromFileContext, Scene::LoadManifestErrorCode>
{
    auto file_path = info.root_path / info.asset_name;

    fastgltf::Parser parser{fastgltf::Extensions::KHR_texture_basisu};

    constexpr auto gltf_options =
        fastgltf::Options::DontRequireValidAssetMember |
        fastgltf::Options::AllowDouble;

    fastgltf::GltfDataBuffer data;
    bool const worked = data.loadFromFile(file_path);
    if (!worked)
    {
        return Scene::LoadManifestErrorCode::FILE_NOT_FOUND;
    }
    auto type = fastgltf::determineGltfFileType(&data);
    LoadManifestFromFileContext load_ctx;
    switch (type)
    {
        case fastgltf::GltfType::glTF:
        {
            fastgltf::Expected<fastgltf::Asset> result = parser.loadGltf(&data, file_path.parent_path(), gltf_options);
            if (result.error() != fastgltf::Error::None)
            {
                return Scene::LoadManifestErrorCode::COULD_NOT_LOAD_ASSET;
            }
            load_ctx.asset = std::move(result.get());
            break;
        }
        case fastgltf::GltfType::GLB:
        {
            fastgltf::Expected<fastgltf::Asset> result = parser.loadGltfBinary(&data, file_path.parent_path(), gltf_options);
            if (result.error() != fastgltf::Error::None)
            {
                return Scene::LoadManifestErrorCode::COULD_NOT_LOAD_ASSET;
            }
            load_ctx.asset = std::move(result.get());
            break;
        }
        default:
            return Scene::LoadManifestErrorCode::INVALID_GLTF_FILE_TYPE;
    }
    load_ctx.file_path = std::move(file_path);
    load_ctx.gltf_asset_manifest_index = s_cast<u32>(scene.gltf_asset_manifest.size());
    load_ctx.texture_manifest_offset = s_cast<u32>(scene.material_texture_manifest.size());
    load_ctx.material_manifest_offset = s_cast<u32>(scene.material_manifest.size());
    load_ctx.mesh_group_manifest_offset = s_cast<u32>(scene.mesh_group_manifest.size());
    load_ctx.mesh_manifest_offset = s_cast<u32>(scene.mesh_manifest.size());
    return load_ctx;
}

static void update_texture_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx)
{
    auto gltf_texture_to_image_index = [&](u32 const texture_index) -> std::optional<u32>
    {
        fastgltf::Asset const & asset = load_ctx.asset;
        if (asset.textures.at(texture_index).basisuImageIndex.has_value())
        {
            return s_cast<u32>(asset.textures.at(texture_index).basisuImageIndex.value());
        }
        else if (asset.textures.at(texture_index).imageIndex.has_value())
        {
            return s_cast<u32>(asset.textures.at(texture_index).imageIndex.value());
        }
        else
        {
            return std::nullopt;
        }
    };
    /// NOTE: GLTF texture = image + sampler we collapse the sampler into the material itself here we thus only iterate over the images
    //        Later when we load in the materials which reference the textures rather than images we just
    //        translate the textures image index and store that in the material

    for (u32 i = 0; i < s_cast<u32>(load_ctx.asset.textures.size()); ++i)
    {
        u32 const texture_manifest_index = s_cast<u32>(scene.material_texture_manifest.size());
        auto gltf_image_idx_opt = gltf_texture_to_image_index(texture_manifest_index);
        DBG_ASSERT_TRUE_M(
            gltf_image_idx_opt.has_value(),
            fmt::format(
                "[ERROR] Texture \"{}\" has no supported gltf image index!\n",
                load_ctx.asset.textures[i].name.c_str()));
        u32 gltf_image_index = gltf_image_idx_opt.value();
        DEBUG_MSG(
            fmt::format("[INFO] Loading texture meta data into manifest:\n  name: {}\n  asset local index: {}\n  manifest index:  {}",
                load_ctx.asset.images[gltf_image_index].name, i, texture_manifest_index));
        // KTX_TTF_BC7_RGBA
        scene.material_texture_manifest.push_back(TextureManifestEntry{
            .type = TextureMaterialType::NONE, // Set by material manifest.
            .gltf_asset_manifest_index = load_ctx.gltf_asset_manifest_index,
            .asset_local_index = i,
            .asset_local_image_index = gltf_image_index,
            .material_manifest_indices = {}, // Filled when reading in materials
            .runtime_texture = {},           // Filled when the texture data are uploaded to the GPU
            .secondary_runtime_texture = {}, // Filled when the texture data are uploaded to the GPU
            .name = load_ctx.asset.textures[i].name.c_str(),
        });
        scene.new_texture_manifest_entries += 1;
    }
}

static void update_material_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx)
{
    for (u32 material_index = 0; material_index < s_cast<u32>(load_ctx.asset.materials.size()); material_index++)
    {
        auto const & material = load_ctx.asset.materials.at(material_index);
        u32 const material_manifest_index = scene.material_manifest.size();
        bool const has_normal_texture = material.normalTexture.has_value();
        bool const has_diffuse_texture = material.pbrData.baseColorTexture.has_value();
        bool const has_roughness_metalness_texture = material.pbrData.metallicRoughnessTexture.has_value();
        std::optional<MaterialManifestEntry::TextureInfo> diffuse_texture_info = {};
        std::optional<MaterialManifestEntry::TextureInfo> opacity_texture_info = {};
        std::optional<MaterialManifestEntry::TextureInfo> normal_texture_info = {};
        std::optional<MaterialManifestEntry::TextureInfo> roughness_metalness_info = {};
        if (has_diffuse_texture)
        {
            u32 const gltf_texture_index = s_cast<u32>(material.pbrData.baseColorTexture.value().textureIndex);
            diffuse_texture_info = {
                .tex_manifest_index = gltf_texture_index + load_ctx.texture_manifest_offset,
                .sampler_index = {}, // TODO(msakmary) ADD SAMPLERS
            };
            opacity_texture_info = {
                .tex_manifest_index = gltf_texture_index + load_ctx.texture_manifest_offset,
                .sampler_index = {}, // TODO(msakmary) ADD SAMPLERS
            };
            TextureManifestEntry & tmenty = scene.material_texture_manifest.at(diffuse_texture_info->tex_manifest_index);
            if (tmenty.type != TextureMaterialType::DIFFUSE)
            {
                DBG_ASSERT_TRUE_M(tmenty.type == TextureMaterialType::NONE, "ERROR: Found a texture used by different materials as DIFFERENT types!");
                tmenty.type = TextureMaterialType::DIFFUSE;
            }
            tmenty.material_manifest_indices.push_back({
                .material_manifest_index = material_manifest_index,
            });
        }
        if (has_normal_texture)
        {
            u32 const gltf_texture_index = s_cast<u32>(material.normalTexture.value().textureIndex);
            normal_texture_info = {
                .tex_manifest_index = gltf_texture_index + load_ctx.texture_manifest_offset,
                .sampler_index = 0, // TODO(msakmary) ADD SAMPLERS
            };
            TextureManifestEntry & tmenty = scene.material_texture_manifest.at(normal_texture_info->tex_manifest_index);
            if (tmenty.type != TextureMaterialType::NORMAL)
            {
                DBG_ASSERT_TRUE_M(tmenty.type == TextureMaterialType::NONE, "ERROR: Found a texture used by different materials as DIFFERENT types!");
                tmenty.type = TextureMaterialType::NORMAL;
            }
            tmenty.material_manifest_indices.push_back({
                .material_manifest_index = material_manifest_index,
            });
        }
        if (has_roughness_metalness_texture)
        {
            u32 const gltf_texture_index = s_cast<u32>(material.pbrData.metallicRoughnessTexture.value().textureIndex);
            roughness_metalness_info = {
                .tex_manifest_index = gltf_texture_index + load_ctx.texture_manifest_offset,
                .sampler_index = 0, // TODO(msakmary) ADD SAMPLERS
            };
            TextureManifestEntry & tmenty = scene.material_texture_manifest.at(roughness_metalness_info->tex_manifest_index);
            if (tmenty.type != TextureMaterialType::ROUGHNESS_METALNESS)
            {
                DBG_ASSERT_TRUE_M(tmenty.type == TextureMaterialType::NONE, "ERROR: Found a texture used by different materials as DIFFERENT types!");
                tmenty.type = TextureMaterialType::ROUGHNESS_METALNESS;
            }
            tmenty.material_manifest_indices.push_back({
                .material_manifest_index = material_manifest_index,
            });
        }
        scene.material_manifest.push_back(MaterialManifestEntry{
            .diffuse_info = diffuse_texture_info,
            .opacity_mask_info = opacity_texture_info,
            .normal_info = normal_texture_info,
            .roughness_metalness_info = roughness_metalness_info,
            .gltf_asset_manifest_index = load_ctx.gltf_asset_manifest_index,
            .asset_local_index = material_index,
            .alpha_discard_enabled = material.alphaMode == fastgltf::AlphaMode::Mask, // || material.alphaMode == fastgltf::AlphaMode::Blend,
            .base_color = f32vec3(material.pbrData.baseColorFactor[0], material.pbrData.baseColorFactor[1], material.pbrData.baseColorFactor[2]),
            .name = material.name.c_str(),
        });
        scene.new_material_manifest_entries += 1;
        DBG_ASSERT_TRUE_M(scene.new_material_manifest_entries < MAX_MATERIALS, "EXCEEDED MAX_MATERIALS");
    }
}

static void update_meshgroup_and_mesh_manifest_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx)
{
    /// NOTE: fastgltf::Mesh is a MeshGroup
    // std::array<u32, MAX_MESHES_PER_MESHGROUP> mesh_manifest_indices;
    for (u32 mesh_group_index = 0; mesh_group_index < s_cast<u32>(load_ctx.asset.meshes.size()); mesh_group_index++)
    {
        auto const & gltf_mesh = load_ctx.asset.meshes.at(mesh_group_index);
        // Linearly allocate chunk from indices array:
        u32 const mesh_manifest_indices_array_offset = static_cast<u32>(scene.mesh_manifest_indices_new.size());
        scene.mesh_manifest_indices_new.resize(scene.mesh_manifest_indices_new.size() + gltf_mesh.primitives.size());

        u32 const mesh_group_manifest_index = s_cast<u32>(scene.mesh_group_manifest.size());
        /// NOTE: fastgltf::Primitive is Mesh
        for (u32 mesh_index = 0; mesh_index < s_cast<u32>(gltf_mesh.primitives.size()); mesh_index++)
        {
            u32 const mesh_manifest_entry = scene.mesh_manifest.size();
            auto const & gltf_primitive = gltf_mesh.primitives.at(mesh_index);
            scene.mesh_manifest_indices_new.at(mesh_manifest_indices_array_offset + mesh_index) = mesh_manifest_entry;
            std::optional<u32> material_manifest_index =
                gltf_primitive.materialIndex.has_value() ? std::optional{s_cast<u32>(gltf_primitive.materialIndex.value()) + load_ctx.material_manifest_offset} : std::nullopt;
            scene.mesh_manifest.push_back(MeshManifestEntry{
                .gltf_asset_manifest_index = load_ctx.gltf_asset_manifest_index,
                // Gltf calls a meshgroup a mesh because these local indices are only used for loading we use the gltf naming
                .asset_local_mesh_index = mesh_group_index,
                // Same as above Gltf calls a mesh a primitive
                .asset_local_primitive_index = mesh_index,
                .material_index = material_manifest_index,
            });
            scene.new_mesh_manifest_entries += 1;
        }

        scene.mesh_group_manifest.push_back(MeshGroupManifestEntry{
            .mesh_manifest_indices_array_offset = mesh_manifest_indices_array_offset,
            .mesh_count = s_cast<u32>(gltf_mesh.primitives.size()),
            .gltf_asset_manifest_index = load_ctx.gltf_asset_manifest_index,
            .asset_local_index = mesh_group_index,
            .loaded_meshes = 0,
            .name = gltf_mesh.name.c_str(),
        });
        scene.new_mesh_group_manifest_entries += 1;
    }
}

static auto update_entities_from_gltf(Scene & scene, Scene::LoadManifestInfo const & info, LoadManifestFromFileContext & load_ctx) -> RenderEntityId
{
    /// NOTE: fastgltf::Node is Entity
    DBG_ASSERT_TRUE_M(load_ctx.asset.nodes.size() != 0, "[ERROR][load_manifest_from_gltf()] Empty node array - what to do now?");
    std::vector<RenderEntityId> node_index_to_entity_id = {};
    /// NOTE: Here we allocate space for each entity and create a translation table between node index and entity id
    for (u32 node_index = 0; node_index < s_cast<u32>(load_ctx.asset.nodes.size()); node_index++)
    {
        node_index_to_entity_id.push_back(scene._render_entities.create_slot());
        scene._dirty_render_entities.push_back(node_index_to_entity_id.back());
    }
    for (u32 node_index = 0; node_index < s_cast<u32>(load_ctx.asset.nodes.size()); node_index++)
    {
        // TODO: For now store transform as a matrix - later should be changed to something else (TRS: translation, rotor, scale).
        auto fastgltf_to_glm_mat4x3_transform = [](std::variant<fastgltf::TRS, fastgltf::Node::TransformMatrix> const & trans) -> glm::mat4x3
        {
            glm::mat4x3 ret_trans;
            if (auto const * trs = std::get_if<fastgltf::TRS>(&trans))
            {
                auto const scale = glm::scale(glm::identity<glm::mat4x4>(), glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]));
                auto const rotation = glm::toMat4(glm::quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]));
                auto const translation = glm::translate(glm::identity<glm::mat4x4>(), glm::vec3(trs->translation[0], trs->translation[1], trs->translation[2]));
                auto const rotated_scaled = rotation * scale;
                auto const translated_rotated_scaled = translation * rotated_scaled;
                /// NOTE: As the last row is always (0,0,0,1) we dont store it.
                ret_trans = glm::mat4x3(translated_rotated_scaled);
            }
            else if (auto const * trs = std::get_if<fastgltf::Node::TransformMatrix>(&trans))
            {
                // Gltf and glm matrices are column major.
                ret_trans = glm::mat4x3(std::bit_cast<glm::mat4x4>(*trs));
            }
            return ret_trans;
        };

        fastgltf::Node const & node = load_ctx.asset.nodes[node_index];
        RenderEntityId const parent_r_ent_id = node_index_to_entity_id[node_index];
        RenderEntity & r_ent = *scene._render_entities.slot(parent_r_ent_id);
        r_ent.mesh_group_manifest_index = node.meshIndex.has_value() ? std::optional<u32>(s_cast<u32>(node.meshIndex.value()) + load_ctx.mesh_group_manifest_offset) : std::optional<u32>(std::nullopt);
        r_ent.transform = fastgltf_to_glm_mat4x3_transform(node.transform);
        r_ent.name = node.name.c_str();
        if (node.meshIndex.has_value())
        {
            r_ent.type = EntityType::MESHGROUP;
        }
        else if (node.cameraIndex.has_value())
        {
            r_ent.type = EntityType::CAMERA;
        }
        else if (node.lightIndex.has_value())
        {
            r_ent.type = EntityType::LIGHT;
        }
        else if (!node.children.empty())
        {
            r_ent.type = EntityType::TRANSFORM;
        }
        if (!node.children.empty())
        {
            r_ent.first_child = node_index_to_entity_id[node.children[0]];
        }
        for (u32 curr_child_vec_idx = 0; curr_child_vec_idx < node.children.size(); curr_child_vec_idx++)
        {
            u32 const curr_child_node_idx = node.children[curr_child_vec_idx];
            RenderEntityId const curr_child_r_ent_id = node_index_to_entity_id[curr_child_node_idx];
            RenderEntity & curr_child_r_ent = *scene._render_entities.slot(curr_child_r_ent_id);
            curr_child_r_ent.parent = parent_r_ent_id;
            bool const has_next_sibling = curr_child_vec_idx < (node.children.size() - 1ull);
            if (has_next_sibling)
            {
                RenderEntityId const next_r_ent_child_id = node_index_to_entity_id[node.children[curr_child_vec_idx + 1]];
                curr_child_r_ent.next_sibling = next_r_ent_child_id;
            }
        }
    }

    /// NOTE: Find all root render entities (aka render entities that have no parent) and store them as
    //        Child root entites under scene root node
    RenderEntityId root_r_ent_id = scene._render_entities.create_slot({
        .transform = glm::mat4x3(glm::identity<glm::mat4x3>()),
        .first_child = std::nullopt,
        .next_sibling = std::nullopt,
        .parent = std::nullopt,
        .mesh_group_manifest_index = std::nullopt,
        .name = info.asset_name.filename().replace_extension("").string() + "_" + std::to_string(load_ctx.gltf_asset_manifest_index),
    });

    scene._dirty_render_entities.push_back(root_r_ent_id);
    RenderEntity & root_r_ent = *scene._render_entities.slot(root_r_ent_id);
    root_r_ent.type = EntityType::ROOT;
    std::optional<RenderEntityId> root_r_ent_prev_child = {};
    for (u32 node_index = 0; node_index < s_cast<u32>(load_ctx.asset.nodes.size()); node_index++)
    {
        RenderEntityId const r_ent_id = node_index_to_entity_id[node_index];
        RenderEntity & r_ent = *scene._render_entities.slot(r_ent_id);
        if (!r_ent.parent.has_value())
        {
            r_ent.parent = root_r_ent_id;
            if (!root_r_ent_prev_child.has_value()) // First child
            {
                root_r_ent.first_child = r_ent_id;
            }
            else // We have other root children already
            {
                scene._render_entities.slot(root_r_ent_prev_child.value())->next_sibling = r_ent_id;
            }
            root_r_ent_prev_child = r_ent_id;
        }
    }
    return root_r_ent_id;
}

static void start_async_loads_of_dirty_meshes(Scene & scene, Scene::LoadManifestInfo const & info)
{
    struct LoadMeshTask : Task
    {
        struct TaskInfo
        {
            AssetProcessor::LoadMeshInfo load_info = {};
            AssetProcessor * asset_processor = {};
        };

        TaskInfo info = {};
        LoadMeshTask(TaskInfo const & info)
            : info{info}
        {
            chunk_count = 1;
        }

        virtual void callback(u32 chunk_index, u32 thread_index) override
        {
            using namespace std::chrono_literals;
            auto const ret_status = info.asset_processor->load_mesh(info.load_info);
            if (ret_status != AssetProcessor::AssetLoadResultCode::SUCCESS)
            {
                DEBUG_MSG(fmt::format("[ERROR]Failed to load mesh group {} mesh {} - error {}",
                    info.load_info.gltf_mesh_index, info.load_info.gltf_primitive_index, AssetProcessor::to_string(ret_status)));
            }
            else
            {
                // DEBUG_MSG(fmt::format("[SUCCESS] Successfuly loaded mesh group {} mesh {}",
                //     info.load_info.gltf_mesh_index, info.load_info.gltf_primitive_index));
            }
        };
    };

    for (u32 mesh_manifest_index = 0; mesh_manifest_index < scene.new_mesh_manifest_entries; mesh_manifest_index++)
    {
        auto const & curr_asset = scene.gltf_asset_manifest.back();
        auto const & mesh_manifest_entry = scene.mesh_manifest.at(curr_asset.mesh_manifest_offset + mesh_manifest_index);
        auto const meshgroup_manifest_index = curr_asset.mesh_group_manifest_offset + mesh_manifest_entry.asset_local_mesh_index;
        // Launch loading of this mesh
        // TODO: ADD DUMMY MATERIAL INDEX!
        info.thread_pool->async_dispatch(
            std::make_shared<LoadMeshTask>(LoadMeshTask::TaskInfo{
                .load_info = {
                    .asset_path = curr_asset.path,
                    .asset = curr_asset.gltf_asset.get(),
                    .gltf_mesh_index = mesh_manifest_entry.asset_local_mesh_index,
                    .gltf_primitive_index = mesh_manifest_entry.asset_local_primitive_index,
                    .global_material_manifest_offset = curr_asset.material_manifest_offset,
                    .manifest_index = mesh_manifest_index,
                    .material_manifest_index = mesh_manifest_entry.material_index.value_or(INVALID_MANIFEST_INDEX),
                },
                .asset_processor = info.asset_processor.get(),
            }),
            TaskPriority::LOW);
    }
}

static void start_async_loads_of_dirty_textures(Scene & scene, Scene::LoadManifestInfo const & info)
{
    struct LoadTextureTask : Task
    {
        struct TaskInfo
        {
            AssetProcessor::LoadTextureInfo load_info = {};
            AssetProcessor * asset_processor = {};
            u32 manifest_index = {};
        };

        TaskInfo info = {};
        LoadTextureTask(TaskInfo const & info)
            : info{info}
        {
            chunk_count = 1;
        }

        virtual void callback(u32 chunk_index, u32 thread_index) override
        {
            auto const ret_status = info.asset_processor->load_texture(info.load_info);
            auto const texture_name = info.load_info.asset->images.at(info.load_info.gltf_image_index).name;
            if (ret_status != AssetProcessor::AssetLoadResultCode::SUCCESS)
            {
                DEBUG_MSG(fmt::format("[ERROR] Failed to load texture index {} name {} - error {}",
                    info.load_info.gltf_texture_index, texture_name, AssetProcessor::to_string(ret_status)));
            }
            else
            {
                // DEBUG_MSG(fmt::format("[SUCCESS] Successfuly loaded texture index {} name {}",
                //     info.load_info.gltf_texture_index, texture_name));
            }
        };
    };
    auto gltf_texture_to_image_index = [&](u32 const texture_index) -> std::optional<u32>
    {
        std::unique_ptr<fastgltf::Asset> const & asset =
            scene.gltf_asset_manifest.at(scene.material_texture_manifest.at(texture_index).gltf_asset_manifest_index).gltf_asset;
        if (asset->textures.at(texture_index).basisuImageIndex.has_value())
        {
            return s_cast<u32>(asset->textures.at(texture_index).basisuImageIndex.value());
        }
        else if (asset->textures.at(texture_index).imageIndex.has_value())
        {
            return s_cast<u32>(asset->textures.at(texture_index).imageIndex.value());
        }
        else
        {
            return std::nullopt;
        }
    };

    for (u32 gltf_texture_index = 0; gltf_texture_index < scene.new_texture_manifest_entries; gltf_texture_index++)
    {
        auto const & curr_asset = scene.gltf_asset_manifest.back();
        auto const texture_manifest_index = curr_asset.texture_manifest_offset + gltf_texture_index;
        auto const & texture_manifest_entry = scene.material_texture_manifest.at(texture_manifest_index);
        auto gpu_compression_format = KTX_TTF_BC7_RGBA;
        auto gltf_image_idx_opt = gltf_texture_to_image_index(texture_manifest_index);
        DBG_ASSERT_TRUE_M(
            gltf_image_idx_opt.has_value(),
            fmt::format(
                "[ERROR] Texture \"{}\" has no supported gltf image index!\n",
                texture_manifest_entry.name));
        if (!texture_manifest_entry.material_manifest_indices.empty())
        {
            // Launch loading of this texture
            info.thread_pool->async_dispatch(
                std::make_shared<LoadTextureTask>(LoadTextureTask::TaskInfo{
                    .load_info = {
                        .asset_path = curr_asset.path,
                        .asset = curr_asset.gltf_asset.get(),
                        .gltf_texture_index = texture_manifest_entry.asset_local_index,
                        .gltf_image_index = texture_manifest_entry.asset_local_image_index,
                        .texture_manifest_index = texture_manifest_index,
                        .texture_material_type = texture_manifest_entry.type,
                    },
                    .asset_processor = info.asset_processor.get(),
                }),
                TaskPriority::LOW);
        }
        else
        {
            DEBUG_MSG(
                fmt::format("[WARNING] Texture \"{}\" can not be loaded because it is not referenced by any material", texture_manifest_entry.name));
        }
    }
    scene.new_texture_manifest_entries = 0;
}

auto Scene::record_gpu_manifest_update(RecordGPUManifestUpdateInfo const & info) -> daxa::ExecutableCommandList
{
    auto recorder = _device.create_command_recorder({});
    /// TODO: Make buffers resize.

    // Calculate required staging buffer size:
    daxa::BufferId staging_buffer = {};
    usize staging_offset = 0;
    std::byte * host_ptr = {};
    if (_dirty_render_entities.size() > 0 || _modified_render_entities.size() > 0)
    {
        usize required_staging_size = 0;
        required_staging_size += sizeof(daxa_f32mat4x3) * (_dirty_render_entities.size() + _modified_render_entities.size()); // _gpu_entity_transforms
        required_staging_size += sizeof(daxa_f32mat4x3) * (_dirty_render_entities.size() + _modified_render_entities.size()); // _gpu_entity_combined_transforms
        required_staging_size += sizeof(GPUMeshGroup) * (_dirty_render_entities.size() + _modified_render_entities.size());   // _gpu_entity_mesh_groups
        staging_buffer = _device.create_buffer({
            .size = required_staging_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "entities update staging",
        });
        recorder.destroy_buffer_deferred(staging_buffer);
        host_ptr = _device.get_host_address(staging_buffer).value();
    }

    /**
     * TODO:
     * - replace with compute shader
     * - write two arrays, one containing entity ids other containing update data
     * - write compute shader that reads both arrays, they then write the updates from staging to entity arrays
     */
    /// NOTE: Update dirty entities.
    auto update_entity = [&](i32 i, RenderEntity * entity, u32 entity_index) -> glm::mat4
    {
        usize offset = (staging_offset + (sizeof(glm::mat4x3) * 2 + sizeof(u32)) * i);
        glm::mat4 transform4 = glm::mat4(
            glm::vec4(entity->transform[0], 0.0f),
            glm::vec4(entity->transform[1], 0.0f),
            glm::vec4(entity->transform[2], 0.0f),
            glm::vec4(entity->transform[3], 1.0f));
        glm::mat4 combined_transform4 = transform4;
        glm::mat4 combined_parent_transform4 = glm::identity<glm::mat4>();
        std::optional<RenderEntityId> parent = entity->parent;
        while (parent.has_value())
        {
            glm::mat4x3 parent_transform4 = glm::mat4(
                glm::vec4(_render_entities.slot(parent.value())->transform[0], 0.0f),
                glm::vec4(_render_entities.slot(parent.value())->transform[1], 0.0f),
                glm::vec4(_render_entities.slot(parent.value())->transform[2], 0.0f),
                glm::vec4(_render_entities.slot(parent.value())->transform[3], 1.0f));
            combined_transform4 = parent_transform4 * combined_transform4;
            combined_parent_transform4 = parent_transform4 * combined_parent_transform4;
            parent = _render_entities.slot(parent.value())->parent;
        }
        u32 mesh_group_manifest_index = entity->mesh_group_manifest_index.value_or(INVALID_MANIFEST_INDEX);
        struct RenderEntityUpdateStagingMemoryView
        {
            glm::mat4x3 transform;
            glm::mat4x3 combined_transform;
            u32 mesh_group_manifest_index;
        };
        entity->combined_transform = combined_transform4;
        *r_cast<RenderEntityUpdateStagingMemoryView *>(host_ptr + offset) = {
            .transform = transform4,
            .combined_transform = combined_transform4,
            .mesh_group_manifest_index = mesh_group_manifest_index,
        };
        recorder.copy_buffer_to_buffer({
            .src_buffer = staging_buffer,
            .dst_buffer = gpu_entity_transforms.get_state().buffers[0],
            .src_offset = offset + offsetof(RenderEntityUpdateStagingMemoryView, transform),
            .dst_offset = sizeof(glm::mat4x3) * entity_index,
            .size = sizeof(glm::mat4x3),
        });
        recorder.copy_buffer_to_buffer({
            .src_buffer = staging_buffer,
            .dst_buffer = gpu_entity_combined_transforms.get_state().buffers[0],
            .src_offset = offset + offsetof(RenderEntityUpdateStagingMemoryView, combined_transform),
            .dst_offset = sizeof(glm::mat4x3) * entity_index,
            .size = sizeof(glm::mat4x3),
        });
        recorder.copy_buffer_to_buffer({
            .src_buffer = staging_buffer,
            .dst_buffer = gpu_entity_mesh_groups.get_state().buffers[0],
            .src_offset = offset + offsetof(RenderEntityUpdateStagingMemoryView, mesh_group_manifest_index),
            .dst_offset = sizeof(u32) * entity_index,
            .size = sizeof(u32),
        });
        return combined_parent_transform4;
    };
    for (u32 i = 0; i < _dirty_render_entities.size(); ++i)
    {
        u32 entity_index = _dirty_render_entities[i].index;
        auto * entity = _render_entities.slot(_dirty_render_entities[i]);
        update_entity(i, entity, entity_index);
    }

    _dirty_render_entities.clear();
    _modified_render_entities.clear();

    if (new_mesh_group_manifest_entries > 0)
    {
        if (!gpu_mesh_group_indices_array_buffer.is_empty())
        {
            recorder.destroy_buffer_deferred(gpu_mesh_group_indices_array_buffer);
        }
        usize mesh_group_indices_mem_size = sizeof(daxa_u32) * mesh_manifest_indices_new.size();
        gpu_mesh_group_indices_array_buffer = _device.create_buffer({
            .size = mesh_group_indices_mem_size,
            .name = "_gpu_mesh_group_indices_array_buffer",
        });
        daxa::BufferId mesh_groups_indices_staging = _device.create_buffer({
            .size = mesh_group_indices_mem_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "mesh group update staging buffer",
        });
        recorder.destroy_buffer_deferred(mesh_groups_indices_staging);
        u32 * indices_staging_ptr = _device.get_host_address_as<u32>(mesh_groups_indices_staging).value();
        std::memcpy(indices_staging_ptr, mesh_manifest_indices_new.data(), mesh_manifest_indices_new.size());
        recorder.copy_buffer_to_buffer({
            .src_buffer = mesh_groups_indices_staging,
            .dst_buffer = gpu_mesh_group_indices_array_buffer,
            .size = mesh_group_indices_mem_size,
        });
        auto mesh_group_indices_array_addr = _device.get_device_address(gpu_mesh_group_indices_array_buffer).value();

        u32 const mesh_group_staging_buffer_size = sizeof(GPUMeshGroup) * new_mesh_group_manifest_entries;
        daxa::BufferId mesh_group_staging_buffer = _device.create_buffer({
            .size = mesh_group_staging_buffer_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "mesh group update staging buffer",
        });
        recorder.destroy_buffer_deferred(mesh_group_staging_buffer);
        GPUMeshGroup * staging_ptr = _device.get_host_address_as<GPUMeshGroup>(mesh_group_staging_buffer).value();
        u32 const mesh_group_manifest_offset = mesh_group_manifest.size() - new_mesh_group_manifest_entries;
        for (u32 new_mesh_group_idx = 0; new_mesh_group_idx < new_mesh_group_manifest_entries; new_mesh_group_idx++)
        {
            u32 const mesh_group_manifest_idx = mesh_group_manifest_offset + new_mesh_group_idx;
            staging_ptr[new_mesh_group_idx].mesh_indices =
                mesh_group_indices_array_addr +
                sizeof(daxa_u32) * mesh_group_manifest.at(mesh_group_manifest_idx).mesh_manifest_indices_array_offset;
            staging_ptr[new_mesh_group_idx].count = mesh_group_manifest.at(mesh_group_manifest_idx).mesh_count;
        }
        recorder.copy_buffer_to_buffer({
            .src_buffer = mesh_group_staging_buffer,
            .dst_buffer = gpu_mesh_group_manifest.get_state().buffers[0],
            .src_offset = 0,
            .dst_offset = mesh_group_manifest_offset * sizeof(GPUMeshGroup),
            .size = sizeof(GPUMeshGroup) * new_mesh_group_manifest_entries,
        });
    }
    if (new_mesh_manifest_entries > 0)
    {
        u32 const mesh_update_staging_buffer_size = sizeof(GPUMesh) * new_mesh_manifest_entries;
        u32 const mesh_manifest_offset = mesh_manifest.size() - new_mesh_manifest_entries;
        daxa::BufferId mesh_staging_buffer = _device.create_buffer({
            .size = mesh_update_staging_buffer_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "mesh update staging buffer",
        });
        recorder.destroy_buffer_deferred(mesh_staging_buffer);
        GPUMesh * staging_ptr = _device.get_host_address_as<GPUMesh>(mesh_staging_buffer).value();
        std::vector<GPUMesh> tmp_meshes;
        tmp_meshes.resize(new_mesh_manifest_entries, GPUMesh{.mesh_buffer = {.value = {}}});
        std::memcpy(staging_ptr, tmp_meshes.data(), new_mesh_manifest_entries * sizeof(GPUMesh));

        recorder.copy_buffer_to_buffer({
            .src_buffer = mesh_staging_buffer,
            .dst_buffer = gpu_mesh_manifest.get_state().buffers[0],
            .src_offset = 0,
            .dst_offset = mesh_manifest_offset * sizeof(GPUMesh),
            .size = sizeof(GPUMesh) * new_mesh_manifest_entries,
        });
    }
    if (new_material_manifest_entries > 0)
    {
        u32 const material_update_staging_buffer_size = sizeof(GPUMaterial) * new_material_manifest_entries;
        u32 const material_manifest_offset = material_manifest.size() - new_material_manifest_entries;
        daxa::BufferId material_staging_buffer = _device.create_buffer({
            .size = material_update_staging_buffer_size,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "material update staging buffer",
        });
        recorder.destroy_buffer_deferred(material_staging_buffer);
        GPUMaterial * staging_ptr = _device.get_host_address_as<GPUMaterial>(material_staging_buffer).value();
        std::vector<GPUMaterial> tmp_materials(new_material_manifest_entries);
        for (i32 i = 0; i < new_material_manifest_entries; i++)
        {
            tmp_materials.at(i).base_color = std::bit_cast<daxa_f32vec3>(material_manifest.at(i + material_manifest_offset).base_color);
        }
        std::memcpy(staging_ptr, tmp_materials.data(), new_material_manifest_entries * sizeof(GPUMaterial));

        recorder.copy_buffer_to_buffer({
            .src_buffer = material_staging_buffer,
            .dst_buffer = gpu_material_manifest.get_state().buffers[0],
            .src_offset = 0,
            .dst_offset = material_manifest_offset * sizeof(GPUMaterial),
            .size = sizeof(GPUMaterial) * new_material_manifest_entries,
        });
    }

    /// TODO: Taskgraph this shit.
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_access = daxa::AccessConsts::READ_WRITE,
    });
    // updating material & texture manifest
    {
        if (info.uploaded_textures.size() > 0)
        {
            /// NOTE: We need to propagate each loaded texture image ID into the material manifest This will be done in two steps:
            //        1) We update the CPU manifest with the correct values and remember the materials that were updated
            //        2) For each dirty material we generate a copy buffer to buffer comand to update the GPU manifest
            std::vector<u32> dirty_material_entry_indices = {};
            // 1) Update CPU Manifest
            for (AssetProcessor::LoadedTextureInfo const & texture_upload : info.uploaded_textures)
            {
                if (texture_upload.secondary_texture)
                {
                    material_texture_manifest.at(texture_upload.texture_manifest_index).secondary_runtime_texture = texture_upload.dst_image;
                }
                else
                {
                    material_texture_manifest.at(texture_upload.texture_manifest_index).runtime_texture = texture_upload.dst_image;
                }
                TextureManifestEntry const & texture_manifest_entry = material_texture_manifest.at(texture_upload.texture_manifest_index);
                for (auto const material_using_texture_info : texture_manifest_entry.material_manifest_indices)
                {
                    MaterialManifestEntry & material_entry = material_manifest.at(material_using_texture_info.material_manifest_index);
                    switch (texture_manifest_entry.type)
                    {
                        case TextureMaterialType::DIFFUSE:
                        {
                            if (texture_upload.secondary_texture)
                            {
                                material_entry.opacity_mask_info->tex_manifest_index = texture_upload.texture_manifest_index;
                            }
                            else
                            {
                                material_entry.diffuse_info->tex_manifest_index = texture_upload.texture_manifest_index;
                            }
                        }
                        break;
                        case TextureMaterialType::DIFFUSE_OPACITY:
                        {
                            material_entry.diffuse_info->tex_manifest_index = texture_upload.texture_manifest_index;
                        }
                        break;
                        case TextureMaterialType::NORMAL:
                        {
                            material_entry.normal_info->tex_manifest_index = texture_upload.texture_manifest_index;
                            material_entry.normal_compressed_bc5_rg = texture_upload.compressed_bc5_rg;
                        }
                        break;
                        case TextureMaterialType::ROUGHNESS_METALNESS:
                        {
                            material_entry.roughness_metalness_info->tex_manifest_index = texture_upload.texture_manifest_index;
                        }
                        break;
                        default: DBG_ASSERT_TRUE_M(false, "unimplemented"); break;
                    }
                    /// NOTE: Add material index only if it was not added previously
                    if (std::find(
                            dirty_material_entry_indices.begin(),
                            dirty_material_entry_indices.end(),
                            material_using_texture_info.material_manifest_index) ==
                        dirty_material_entry_indices.end())
                    {
                        dirty_material_entry_indices.push_back(material_using_texture_info.material_manifest_index);
                    }
                }
            }
            // // 2) Update GPU manifest
            daxa::BufferId materials_update_staging_buffer = {};
            GPUMaterial * staging_origin_ptr = {};
            if (dirty_material_entry_indices.size())
            {
                materials_update_staging_buffer = _device.create_buffer({
                    .size = sizeof(GPUMaterial) * dirty_material_entry_indices.size(),
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "gpu materials update staging",
                });
                recorder.destroy_buffer_deferred(materials_update_staging_buffer);
                staging_origin_ptr = _device.get_host_address_as<GPUMaterial>(materials_update_staging_buffer).value();
            }
            for (u32 dirty_materials_index = 0; dirty_materials_index < dirty_material_entry_indices.size(); dirty_materials_index++)
            {
                MaterialManifestEntry const & material = material_manifest.at(dirty_material_entry_indices.at(dirty_materials_index));
                daxa::ImageId diffuse_id = {};
                daxa::ImageId opacity_id = {};
                daxa::ImageId normal_id = {};
                daxa::ImageId roughness_metalness_id = {};
                /// NOTE: We check if material even has diffuse info, if it does we need to check if the runtime value of this
                //        info is present - It might be that diffuse texture was uploaded marking this material as dirty, but
                //        the normal texture is not yet present thus we don't yet have the runtime info
                if (material.diffuse_info.has_value())
                {
                    auto const & texture_entry = material_texture_manifest.at(material.diffuse_info.value().tex_manifest_index);
                    diffuse_id = texture_entry.runtime_texture.value_or(daxa::ImageId{});
                }
                if (material.opacity_mask_info.has_value())
                {
                    auto const & texture_entry = material_texture_manifest.at(material.opacity_mask_info.value().tex_manifest_index);
                    opacity_id = texture_entry.secondary_runtime_texture.value_or(daxa::ImageId{});
                }
                if (material.normal_info.has_value())
                {
                    auto const & texture_entry = material_texture_manifest.at(material.normal_info.value().tex_manifest_index);
                    normal_id = texture_entry.runtime_texture.value_or(daxa::ImageId{});
                }
                if (material.roughness_metalness_info.has_value())
                {
                    auto const & texture_entry = material_texture_manifest.at(material.roughness_metalness_info.value().tex_manifest_index);
                    roughness_metalness_id = texture_entry.runtime_texture.value_or(daxa::ImageId{});
                }
                staging_origin_ptr[dirty_materials_index].diffuse_texture_id = diffuse_id.default_view();
                staging_origin_ptr[dirty_materials_index].opacity_texture_id = opacity_id.default_view();
                staging_origin_ptr[dirty_materials_index].normal_texture_id = normal_id.default_view();
                staging_origin_ptr[dirty_materials_index].roughnes_metalness_id = roughness_metalness_id.default_view();
                staging_origin_ptr[dirty_materials_index].alpha_discard_enabled = material.alpha_discard_enabled;
                staging_origin_ptr[dirty_materials_index].normal_compressed_bc5_rg = material.normal_compressed_bc5_rg;
                staging_origin_ptr[dirty_materials_index].base_color = std::bit_cast<daxa_f32vec3>(material.base_color);

                daxa::BufferId gpu_material_manifest_buffer = gpu_material_manifest.get_state().buffers[0];
                recorder.copy_buffer_to_buffer({
                    .src_buffer = materials_update_staging_buffer,
                    .dst_buffer = gpu_material_manifest_buffer,
                    .src_offset = sizeof(GPUMaterial) * dirty_materials_index,
                    .dst_offset = sizeof(GPUMaterial) * dirty_material_entry_indices.at(dirty_materials_index),
                    .size = sizeof(GPUMaterial),
                });
            }
            recorder.pipeline_barrier({
                .src_access = daxa::AccessConsts::TRANSFER_WRITE,
                .dst_access = daxa::AccessConsts::READ,
            });
        }
    }

    // updating mesh manifest
    {
        if (info.uploaded_meshes.size() > 0)
        {
            daxa::BufferId staging_buffer = _device.create_buffer({
                .size = info.uploaded_meshes.size() * sizeof(GPUMesh),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "mesh manifest upload staging buffer",
            });

            recorder.destroy_buffer_deferred(staging_buffer);
            GPUMesh * staging_ptr = _device.get_host_address_as<GPUMesh>(staging_buffer).value();
            for (i32 upload_index = 0; upload_index < info.uploaded_meshes.size(); upload_index++)
            {
                auto const & upload = info.uploaded_meshes[upload_index];

                // Copy the runtime mesh data into the mesh manifest entry
                auto & mesh = mesh_manifest.at(upload.manifest_index);
                mesh.runtime = upload.mesh;
                u32 const & meshgroup_offset = gltf_asset_manifest.at(mesh.gltf_asset_manifest_index).mesh_group_manifest_offset;
                u32 const meshgroup_index = meshgroup_offset + mesh.asset_local_mesh_index;
                auto & meshgroup = mesh_group_manifest.at(meshgroup_index);
                // Increase the meshgroup loaded count and in case the meshgroup is fully loaded add its index to queue
                // of candidates for blas build
                {
                    std::lock_guard<std::mutex>meshgroup_lock(*meshgroup_mutex);
                    meshgroup.loaded_meshes += 1;
                    if(meshgroup.loaded_meshes == meshgroup.mesh_count)
                    {
                        loaded_meshgroup_queue.push_back(meshgroup_index);
                    }
                }

                std::memcpy(staging_ptr + upload_index, &upload.mesh, sizeof(GPUMesh));
                recorder.copy_buffer_to_buffer({
                    .src_buffer = staging_buffer,
                    .dst_buffer = gpu_mesh_manifest.get_state().buffers[0],
                    .src_offset = upload_index * sizeof(GPUMesh),
                    .dst_offset = upload.manifest_index * sizeof(GPUMesh),
                    .size = sizeof(GPUMesh),
                });
            }
        }
    }

    /// TODO: Taskgraph this shit.
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::TRANSFER_WRITE,
        .dst_access = daxa::AccessConsts::READ_WRITE,
    });

    new_material_manifest_entries = 0;
    new_mesh_manifest_entries = 0;
    new_mesh_group_manifest_entries = 0;
    return recorder.complete_current_commands();
}

auto Scene::create_and_record_build_as() -> daxa::ExecutableCommandList
{
    auto get_aligned = [&](u64 to_align, u64 alignment) -> u64
    {
        return ((to_align + (alignment - 1)) & ~(alignment - 1));
    };

    auto const scratch_buffer_offset_alignment =
        _device.properties().acceleration_structure_properties.value().min_acceleration_structure_scratch_offset_alignment;

    u32 current_scratch_buffer_offset = 0;
    auto const scratch_device_address = _device.get_device_address(gpu_scratch_buffer.get_state().buffers[0]).value();
    std::vector<std::vector<daxa::BlasTriangleGeometryInfo>> build_geometries = {};
    std::vector<daxa::BlasBuildInfo> build_infos = {};
    while(!loaded_meshgroup_queue.empty())
    {
        std::unique_lock<std::mutex> meshgroup_queue_lock{*meshgroup_mutex};
        auto const meshgroup_index = loaded_meshgroup_queue.front();
        loaded_meshgroup_queue.pop_front();
        meshgroup_queue_lock.unlock();

        auto & meshgroup = mesh_group_manifest.at(meshgroup_index);

        DBG_ASSERT_TRUE_M(meshgroup.loaded_meshes == meshgroup.mesh_count, 
            "[ERROR][Scene::create_and_record_build_as()] Attempting to build a BLAS on a partially loaded meshgroup");

        auto & geometries = build_geometries.emplace_back();
        geometries.reserve(meshgroup.mesh_count);
        auto const mesh_indices_meshgroup_offset = meshgroup.mesh_manifest_indices_array_offset;
        for(i32 mesh_index = 0; mesh_index < meshgroup.mesh_count; mesh_index++)
        {
            u32 const mesh_manifest_index = mesh_manifest_indices_new.at(mesh_indices_meshgroup_offset + mesh_index);
            auto const & mesh = mesh_manifest.at(mesh_manifest_index);

            geometries.push_back({
                .vertex_data = mesh.runtime->vertex_positions,
                .max_vertex = mesh.runtime->vertex_count - 1,
                .index_data = mesh.runtime->indices,
                .count = static_cast<daxa_u32>(mesh.runtime->index_count / 3)
            });
        }

        auto & blas_build_info = build_infos.emplace_back(daxa::BlasBuildInfo{
            .flags = daxa::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE |
                     daxa::AccelerationStructureBuildFlagBits::ALLOW_DATA_ACCESS,
            .geometries = daxa::Span<const daxa::BlasTriangleGeometryInfo>(
                geometries.data(), geometries.size()),
        });

        auto const build_size_info = _device.get_blas_build_sizes(blas_build_info);
        u64 aligned_scratch_size = get_aligned(build_size_info.build_scratch_size, scratch_buffer_offset_alignment);
        DBG_ASSERT_TRUE_M(aligned_scratch_size < scratch_buffer_size, 
            "[ERROR][Scene::create_and_record_build_as()] Meshgroup too big for the scratch buffer - increase scratchbuffer size");

        if(current_scratch_buffer_offset + aligned_scratch_size > scratch_buffer_size)
        {
            geometries.pop_back();
            build_infos.pop_back();

            meshgroup_queue_lock.lock();
            loaded_meshgroup_queue.push_back(meshgroup_index);
            meshgroup_queue_lock.unlock();
            break;
        }

        blas_build_info.scratch_data = scratch_device_address + current_scratch_buffer_offset;
        current_scratch_buffer_offset += aligned_scratch_size;
        auto const aligned_accel_structure_size = get_aligned(build_size_info.acceleration_structure_size, 256);
        meshgroup.blas = _device.create_blas({
            .size = aligned_accel_structure_size,
            .name = "blas",
        });
        blas_build_info.dst_blas = meshgroup.blas.value();
    }

    auto recorder = _device.create_command_recorder({});
    if(!build_infos.empty())
    {
        DEBUG_MSG(fmt::format("[DEBUG][Scene::create_and_record_build_as()] Building {} blases this frame", build_infos.size()));
        recorder.build_acceleration_structures({.blas_build_infos = {build_infos.data(), build_infos.size()}});
    }
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::ACCELERATION_STRUCTURE_BUILD_WRITE,
        .dst_access = daxa::AccessConsts::ACCELERATION_STRUCTURE_BUILD_READ
    });

    std::vector<daxa_BlasInstanceData> blas_instances = {};
    for (u32 entity_i = 0; entity_i < _render_entities.capacity(); ++entity_i)
    {
        RenderEntity const * r_ent = _render_entities.slot_by_index(entity_i);
        if(r_ent != nullptr && r_ent->mesh_group_manifest_index.has_value())
        {
            MeshGroupManifestEntry const & m_entry = mesh_group_manifest.at(r_ent->mesh_group_manifest_index.value());
            if(!m_entry.blas.has_value())
            {
                continue;
            }

            auto const t = r_ent->combined_transform;
            blas_instances.push_back(daxa_BlasInstanceData{
                .transform = {
                    {t[0][0], t[1][0], t[2][0], t[3][0]},
                    {t[0][1], t[1][1], t[2][1], t[3][1]},
                    {t[0][2], t[1][2], t[2][2], t[3][2]},
                },
                .mask = 0xFF,
                .instance_shader_binding_table_record_offset = 1,
                .blas_device_address = _device.get_device_address(m_entry.blas.value()).value(),
            });
        }
    }

    auto blas_instances_buffer = _device.create_buffer({
        .size = sizeof(daxa_BlasInstanceData) * blas_instances.size(),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "blas instances buffer"
    });
    recorder.destroy_buffer_deferred(blas_instances_buffer);

    std::memcpy(_device.get_host_address_as<daxa_BlasInstanceData>(blas_instances_buffer).value(),
                blas_instances.data(),
                blas_instances.size() * sizeof(daxa_BlasInstanceData));

    auto tlas_blas_instances_info = daxa::TlasInstanceInfo{
            .data = _device.get_device_address(blas_instances_buffer).value(),
            .count = s_cast<u32>(blas_instances.size()),
            .is_data_array_of_pointers = false,
            .flags = daxa::GeometryFlagBits::OPAQUE
    };

    auto tlas_build_info = daxa::TlasBuildInfo{
        .flags = daxa::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE,
        .instances = std::array{tlas_blas_instances_info},
    };

    daxa::AccelerationStructureBuildSizesInfo const tlas_build_sizes = _device.get_tlas_build_sizes(tlas_build_info);

    if(_device.is_id_valid(gpu_tlas)) {
        _device.destroy_tlas(gpu_tlas);
    }

    gpu_tlas = _device.create_tlas({
        .size = tlas_build_sizes.acceleration_structure_size,
        .name = "scene tlas",
    });

    DBG_ASSERT_TRUE_M(tlas_build_sizes.build_scratch_size < scratch_buffer_size, 
        "[ERROR][Scene::create_and_record_build_as] Tlas too big for scratch buffer - create bigger scratch buffer");

    tlas_build_info.dst_tlas = gpu_tlas;
    tlas_build_info.scratch_data = scratch_device_address;

    recorder.build_acceleration_structures({
        .tlas_build_infos = std::array{tlas_build_info}
    });
    recorder.pipeline_barrier({
        .src_access = daxa::AccessConsts::ACCELERATION_STRUCTURE_BUILD_READ_WRITE,
        .dst_access = daxa::AccessConsts::READ_WRITE
    });

    return recorder.complete_current_commands();
}