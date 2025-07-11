#include "Shader.hpp"

#include <cassert>
#include <glslang/Include/Types.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <glslang/MachineIndependent/iomapper.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include "Spv.hpp"
#include "TextureCache.hpp"
#include "Utils/Logging.h"
#include <cstdlib>
#include <memory>
#include <string>
#include "Core/StringHelper.hpp"
#include "Utils/Sha.hpp"
#include "Core/MapSet.hpp"
#include <SPIRV-Reflect/spirv_reflect.h>

using namespace wallpaper;
using namespace wallpaper::vulkan;

#define _VK_FORMAT_1(s, sign, type, x)          VK_FORMAT_##x##s##sign##type;
#define _VK_FORMAT_2(s, sign, type, x, y)       VK_FORMAT_##x##s##y##s##sign##type;
#define _VK_FORMAT_3(s, sign, type, x, y, z)    VK_FORMAT_##x##s##y##s##z##s##sign##type;
#define _VK_FORMAT_4(s, sign, type, x, y, z, w) VK_FORMAT_##x##s##y##s##z##s##w##s##sign##type;

constexpr int ClientInputSemanticsVersion = 100;

namespace
{
inline wallpaper::ShaderType ToGeneType(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return wallpaper::ShaderType::VERTEX;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return wallpaper::ShaderType::FRAGMENT;
    default: assert(false); return wallpaper::ShaderType::VERTEX;
    }
}

inline VkShaderStageFlagBits ToVkType(EShLanguage lan) {
    switch (lan) {
    case EShLangVertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case EShLangFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline VkFormat ToVkType(SpvReflectFormat type) { return static_cast<VkFormat>(type); }

inline VkShaderStageFlagBits ToVkType(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

template<typename VEC, typename FUNC>
bool EnumAllRef(VEC& vec, FUNC&& func) {
    uint count { 0 };
    auto result = func(&count, nullptr);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(count);
    result = func(&count, vec.data());
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    return result == SPV_REFLECT_RESULT_SUCCESS;
}

inline glslang::EShClient getClient(glslang::EShTargetClientVersion ClientVersion) {
    switch (ClientVersion) {
    case glslang::EShTargetVulkan_1_0:
    case glslang::EShTargetVulkan_1_1:
    case glslang::EShTargetVulkan_1_2: return glslang::EShClientVulkan;
    case glslang::EShTargetOpenGL_450: return glslang::EShClientOpenGL;
    default: return glslang::EShClientVulkan;
    }
}
inline glslang::EShTargetLanguageVersion
getTargetVersion(glslang::EShTargetClientVersion ClientVersion) {
    glslang::EShTargetLanguageVersion TargetVersion { glslang::EShTargetSpv_1_0 };
    switch (ClientVersion) {
    case glslang::EShTargetVulkan_1_0: TargetVersion = glslang::EShTargetSpv_1_0; break;
    case glslang::EShTargetVulkan_1_1: TargetVersion = glslang::EShTargetSpv_1_3; break;
    case glslang::EShTargetVulkan_1_2: TargetVersion = glslang::EShTargetSpv_1_5; break;
    case glslang::EShTargetVulkan_1_3: TargetVersion = glslang::EShTargetSpv_1_6; break;
    case glslang::EShTargetOpenGL_450: TargetVersion = glslang::EShTargetSpv_1_0; break;
    default: break;
    }
    return TargetVersion;
}

inline bool parse(const ShaderCompUnit& unit, const ShaderCompOpt& opt, EShMessages emsg,
                  glslang::TShader& shader) {
    auto* data   = unit.src.c_str();
    auto  client = getClient(opt.client_ver);
    shader.setStrings(&data, 1);
    shader.setEnvInput(opt.hlsl ? glslang::EShSourceHlsl : glslang::EShSourceGlsl,
                       EShLanguage::EShLangVertex,
                       client,
                       ClientInputSemanticsVersion);
    shader.setEnvClient(client, opt.client_ver);
    shader.setEnvTarget(glslang::EShTargetLanguage::EShTargetSpv, getTargetVersion(opt.client_ver));
    if (opt.auto_map_locations) shader.setAutoMapLocations(true);
    if (opt.auto_map_bindings) shader.setAutoMapBindings(true);
    if (opt.relaxed_rules_vulkan) {
        shader.setGlobalUniformBinding(opt.global_uniform_binding);
        shader.setEnvInputVulkanRulesRelaxed();
    }
    const int        default_ver = 110; // 100 for es, 110 for desktop
    TBuiltInResource resource    = DefaultTBuiltInResource;
    if (! shader.parse(&resource, default_ver, false, emsg)) {
        std::string tmp_name = logToTmpfileWithSha1(unit.src, "%s", unit.src.c_str());
        LOG_INFO("--- shader compile failed ---");
        LOG_ERROR("shader source is at %s", tmp_name.c_str());
        LOG_ERROR("glslang(parse): %s", shader.getInfoLog());
        LOG_INFO("--- end ---");
        return false;
    }
    return true;
}

inline void SetMessageOptions(const ShaderCompOpt& opt, EShMessages& emsg) {
    emsg = (EShMessages)(EShMsgDefault | EShMsgSpvRules);
    if (opt.relaxed_errors_glsl) emsg = (EShMessages)(emsg | EShMsgRelaxedErrors);
    if (opt.suppress_warnings_glsl) emsg = (EShMessages)(emsg | EShMsgSuppressWarnings);
    if (getClient(opt.client_ver) == glslang::EShClientVulkan)
        emsg = (EShMessages)(emsg | EShMsgVulkanRules);
}

inline i32 GetTypeNum(const glslang::TType* type) {
    i32 num { 1 };
    if (type->isArray()) num *= type->getCumulativeArraySize();
    if (type->isVector()) num *= type->getVectorSize();
    if (type->isMatrix()) num *= type->getMatrixCols() * type->getMatrixRows();
    return num;
}
} // namespace
const TBuiltInResource wallpaper::vulkan::DefaultTBuiltInResource {
    .maxLights =  32,
    .maxClipPlanes =  6,
    .maxTextureUnits =  32,
    .maxTextureCoords =  32,
    .maxVertexAttribs =  64,
    .maxVertexUniformComponents =  4096,
    .maxVaryingFloats =  64,
    .maxVertexTextureImageUnits =  32,
    .maxCombinedTextureImageUnits =  80,
    .maxTextureImageUnits =  32,
    .maxFragmentUniformComponents =  4096,
    .maxDrawBuffers =  32,
    .maxVertexUniformVectors =  128,
    .maxVaryingVectors =  8,
    .maxFragmentUniformVectors =  16,
    .maxVertexOutputVectors =  16,
    .maxFragmentInputVectors =  15,
    .minProgramTexelOffset =  -8,
    .maxProgramTexelOffset =  7,
    .maxClipDistances =  8,
    .maxComputeWorkGroupCountX =  65535,
    .maxComputeWorkGroupCountY =  65535,
    .maxComputeWorkGroupCountZ =  65535,
    .maxComputeWorkGroupSizeX =  1024,
    .maxComputeWorkGroupSizeY =  1024,
    .maxComputeWorkGroupSizeZ =  64,
    .maxComputeUniformComponents =  1024,
    .maxComputeTextureImageUnits =  16,
    .maxComputeImageUniforms =  8,
    .maxComputeAtomicCounters =  8,
    .maxComputeAtomicCounterBuffers =  1,
    .maxVaryingComponents =  60,
    .maxVertexOutputComponents =  64,
    .maxGeometryInputComponents =  64,
    .maxGeometryOutputComponents =  128,
    .maxFragmentInputComponents =  128,
    .maxImageUnits =  8,
    .maxCombinedImageUnitsAndFragmentOutputs =  8,
    .maxCombinedShaderOutputResources =  8,
    .maxImageSamples =  0,
    .maxVertexImageUniforms =  0,
    .maxTessControlImageUniforms =  0,
    .maxTessEvaluationImageUniforms =  0,
    .maxGeometryImageUniforms =  0,
    .maxFragmentImageUniforms =  8,
    .maxCombinedImageUniforms =  8,
    .maxGeometryTextureImageUnits =  16,
    .maxGeometryOutputVertices =  256,
    .maxGeometryTotalOutputComponents =  1024,
    .maxGeometryUniformComponents =  1024,
    .maxGeometryVaryingComponents =  64,
    .maxTessControlInputComponents =  128,
    .maxTessControlOutputComponents =  128,
    .maxTessControlTextureImageUnits =  16,
    .maxTessControlUniformComponents =  1024,
    .maxTessControlTotalOutputComponents =  4096,
    .maxTessEvaluationInputComponents =  128,
    .maxTessEvaluationOutputComponents =  128,
    .maxTessEvaluationTextureImageUnits =  16,
    .maxTessEvaluationUniformComponents =  1024,
    .maxTessPatchComponents =  120,
    .maxPatchVertices =  32,
    .maxTessGenLevel =  64,
    .maxViewports =  16,
    .maxVertexAtomicCounters =  0,
    .maxTessControlAtomicCounters =  0,
    .maxTessEvaluationAtomicCounters =  0,
    .maxGeometryAtomicCounters =  0,
    .maxFragmentAtomicCounters =  8,
    .maxCombinedAtomicCounters =  8,
    .maxAtomicCounterBindings =  1,
    .maxVertexAtomicCounterBuffers =  0,
    .maxTessControlAtomicCounterBuffers =  0,
    .maxTessEvaluationAtomicCounterBuffers =  0,
    .maxGeometryAtomicCounterBuffers =  0,
    .maxFragmentAtomicCounterBuffers =  1,
    .maxCombinedAtomicCounterBuffers =  1,
    .maxAtomicCounterBufferSize =  16384,
    .maxTransformFeedbackBuffers =  4,
    .maxTransformFeedbackInterleavedComponents =  64,
    .maxCullDistances =  8,
    .maxCombinedClipAndCullDistances =  8,
    .maxSamples =  4,
    .maxMeshOutputVerticesNV =  256,
    .maxMeshOutputPrimitivesNV =  512,
    .maxMeshWorkGroupSizeX_NV =  32,
    .maxMeshWorkGroupSizeY_NV =  1,
    .maxMeshWorkGroupSizeZ_NV =  1,
    .maxTaskWorkGroupSizeX_NV =  32,
    .maxTaskWorkGroupSizeY_NV =  1,
    .maxTaskWorkGroupSizeZ_NV =  1,
    .maxMeshViewCountNV =  4,
    .maxDualSourceDrawBuffersEXT =  1,

    .limits = 
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    }
};

/*
static bool GetReflectedInfo(glslang::TProgram& pro, ShaderReflected& ref, const ShaderCompOpt& opt)
{ EShReflectionOptions reflect_opt {EShReflectionDefault}; if(opt.reflect_all_io_var) reflect_opt =
(decltype(reflect_opt))(reflect_opt | EShReflectionAllIOVariables); if(opt.reflect_all_block_var)
        reflect_opt = (decltype(reflect_opt))(reflect_opt | EShReflectionAllBlockVariables);

    if(!pro.buildReflection(reflect_opt)) return false;
    int numBlocks = pro.getNumUniformBlocks();
    int numUnfis = pro.getNumUniformVariables();
    for(int i=0;i<numBlocks;i++) {
        auto& block = pro.getUniformBlock(i);
        ref.blocks.push_back(ShaderReflected::Block{
            .index = i,
            .size = block.size,
            .name = block.name,
            .member_map = {}
        });
        vk::DescriptorSetLayoutBinding binding;
        binding.setBinding(block.getBinding())
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setStageFlags(ToVkType(block.stages));
        ref.binding_map[block.name] = binding;
    }
    for(int i=0;i<numUnfis;i++) {
        auto& unif = pro.getUniform(i);
        auto* type = unif.getType();
        auto basic_type = type->getBasicType();
        if(type->isTexture()) {
            vk::DescriptorSetLayoutBinding binding;
            binding.setBinding(unif.getBinding())
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setStageFlags(ToVkType(unif.stages));
            ref.binding_map[unif.name] = binding;
        } else if(!type->isStruct()) {
            // in block
            if(unif.index >= ref.blocks.size())
                return false;
            auto& block = ref.blocks[unif.index];
            ShaderReflected::BlockedUniform bunif {};
            {
                bunif.num = GetTypeNum(type);
                bunif.type = type->getBasicType();
                bunif.block_index = unif.index;
                bunif.offset = unif.offset;
            }
            block.member_map[unif.name] = bunif;
        }
    }
    int numInputs = pro.getNumPipeInputs();
    for(int i=0;i<numInputs;i++) {
        auto& input = pro.getPipeInput(i);
        auto* type = input.getType();
        auto& qual = type->getQualifier();
        if(wallpaper::sstart_with(input.name, "gl_")) continue;
        if(!qual.hasAnyLocation())  {
            LOG_ERROR("shader input %s no location", input.name.c_str());
            return false;
        }
        ShaderReflected::Input rinput;
        rinput.location = qual.layoutLocation;
        rinput.format = ToVkType(type->getBasicType(), GetTypeNum(type));
        ref.input_location_map[input.name] = rinput;
    }

    return true;
};
*/

bool wallpaper::vulkan::GenReflect(std::span<const std::vector<uint>> codes,
                                   std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& ref) {
    spvs.clear();
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code, SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
            spv->stage        = ::ToGeneType(stage);
            spv->spirv        = code;
            spvs.emplace_back(std::move(spv));
        }
        std::vector<SpvReflectInterfaceVariable*> inputs;
        std::vector<SpvReflectDescriptorBinding*> bindings;

        bool ok = EnumAllRef(bindings, [&](auto&&... args) {
            return spv_ref.EnumerateDescriptorBindings(args...);
        });
        if (! ok) return false;

        VkDescriptorSetLayoutBinding vkbinding {};
        vkbinding.stageFlags = stage;

        for (auto pb : bindings) {
            auto& b = *pb;
            if (! b.accessed) continue;

            auto bind_name = std::string(b.name).empty() && b.type_description->type_name != nullptr
                                 ? b.type_description->type_name
                                 : b.name;

            if (exists(ref.binding_map, bind_name)) {
                auto& bind = ref.binding_map[bind_name];
                bind.stageFlags |= stage;
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                auto& block      = b.block;
                auto  block_name = std::string(block.name).empty() ? bind_name : block.name;
                ref.blocks.push_back(ShaderReflected::Block { //.index = i,
                                                              .size       = block.size,
                                                              .name       = block.name,
                                                              .member_map = {} });
                auto& ref_block = ref.blocks.front();

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (u32 i = 0; i < block.member_count; i++) {
                    auto&                           unif = block.members[i];
                    ShaderReflected::BlockedUniform bunif {};
                    {
                        // bunif.num = GetTypeNum(type);
                        bunif.size   = unif.size;
                        bunif.offset = unif.offset;
                    }
                    ref_block.member_map[unif.name] = bunif;
                }
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else {
                LOG_ERROR("unknown DescriptorBinding %d", b.descriptor_type);
                return false;
            }

            ref.binding_map[bind_name] = vkbinding;
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if (wallpaper::sstart_with(input.name, "gl_")) continue;

                if (input.location == std::numeric_limits<decltype(input.location)>::max()) {
                    LOG_ERROR("shader input %s no location", input.name);
                    return false;
                }
                ShaderReflected::Input rinput;
                rinput.location = input.location;
                rinput.format   = ::ToVkType(input.format);

                ref.input_location_map[input.name] = rinput;
            }
        }
    }
    return true;
}

bool wallpaper::vulkan::CompileAndLinkShaderUnits(std::span<const ShaderCompUnit>  compUnits,
                                                  const ShaderCompOpt&        opt,
                                                  std::vector<Uni_ShaderSpv>& spvs) {
    glslang::TProgram program;
    EShMessages       emsg;
    SetMessageOptions(opt, emsg);
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    for (auto& unit : compUnits) {
        shaders.emplace_back(std::make_unique<glslang::TShader>(unit.stage));
        auto& shader = *(shaders.back());
        if (! parse(unit, opt, emsg, shader)) return false;
        program.addShader(&shader);
    }

    if (! program.link(emsg)) {
        LOG_ERROR("glslang(link): %s\n", program.getInfoLog());
        return false;
    }

    for (auto& unit : compUnits) {
        (void)program.getIntermediate(unit.stage);
    }
    glslang::TIntermediate*         firstIm = program.getIntermediate(compUnits[0].stage);
    glslang::TDefaultGlslIoResolver resolver(*firstIm);
    glslang::TGlslIoMapper          ioMapper;

    if (! (program.mapIO(&resolver, &ioMapper))) {
        LOG_ERROR("glslang(mapIo): %s\n", program.getInfoLog());
        return false;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions spvOptions;
    spvOptions.validate          = true;
    spvOptions.generateDebugInfo = false;

    spvs.clear();
    for (auto& unit : compUnits) {
        Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
        spv->stage        = ::ToGeneType(::ToVkType(unit.stage));
        auto im           = program.getIntermediate(unit.stage);
        im->setOriginUpperLeft();
        glslang::GlslangToSpv(*im, spv->spirv, &logger, &spvOptions);
        spvs.emplace_back(std::move(spv));

        auto messages = logger.getAllMessages();
        if (messages.length() > 0) LOG_ERROR("glslang(spv): %s\n", messages.c_str());
    }

    return true;
}

VkFormat wallpaper::vulkan::ToVkType(glslang::TBasicType type, size_t size) {
#define FORMAT_SWITCH(in, s, sign, type)                       \
    switch (in) {                                              \
    case 1: return _VK_FORMAT_1(s, _##sign, type, R);          \
    case 2: return _VK_FORMAT_2(s, _##sign, type, R, G);       \
    case 3: return _VK_FORMAT_3(s, _##sign, type, R, G, B);    \
    case 4: return _VK_FORMAT_4(s, _##sign, type, R, G, B, A); \
    }                                                          \
    break;

    switch (type) {
    case glslang::TBasicType::EbtFloat: FORMAT_SWITCH(size, 32, S, FLOAT);
    case glslang::TBasicType::EbtInt: FORMAT_SWITCH(size, 32, S, INT);
    case glslang::TBasicType::EbtUint: FORMAT_SWITCH(size, 32, U, INT);
    default: break;
    }
    LOG_ERROR("can't covert glslang type \"%s\" of size %d to vulkan format",
              glslang::TType::getBasicString(type),
              size);
    assert(false);
    return VK_FORMAT_UNDEFINED;
#undef FORMAT_SWITCH
}
