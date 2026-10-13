#pragma once
// Shaders and materials. Bodies use the ATTRIBUTE/VARYING/TEXTURE2D/FRAG_COLOR macros and get a
// #version header for the running GL at load. Uniforms the batch sets: mvp, u_view_x, u_view_y,
// u_snap, u_inv_texture_size, u_time, texture0.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <lam/core/asset_id.h>
#include <lam/core/result.h>
#include <lam/render/sprite.h>
namespace lam {
// Desktop 3.30, ES 3.00, or ES 1.00 for WebGL 1.
enum class ShaderDialect : std::uint8_t { Glsl330, GlslEs300, GlslEs100 };
enum class ShaderStage : std::uint8_t { Vertex, Fragment };
// Header + "#line 1" + body. Rejects bodies with their own #version/#extension or a BOM.
Result<std::string> build_shader_source(ShaderDialect d, ShaderStage s, std::string_view body,
                                        std::string_view file_name);
struct Shader {                                                 // .shader.json: {"vs": "...", "fs": "..."}
    std::uint32_t gpu_id = 0;                                   // 0 headless
    std::string vs_path, fs_path;                               // "" uses the engine's stage
};
// src/dst factors: Alpha (a, 1-a), Additive (a, 1), Multiply (dst, 0), Premultiplied (1, 1-a),
// Opaque (1, 0).
enum class BlendMode : std::uint8_t { Alpha, Additive, Multiply, Premultiplied, Opaque };
// No shader means the engine one.
struct Material { AssetId<Shader> shader; BlendMode blend = BlendMode::Alpha; };
inline constexpr MaterialId kMaterialAlpha = 0;
inline constexpr MaterialId kMaterialAdditive = 1;              // lights, sparks
inline constexpr MaterialId kMaterialMultiply = 2;              // light composite
class Materials {                                               // App resource
public:
    Materials();                                                // registers ids 0-2
    MaterialId add(const Material &m);                          // up to 32
    // Unknown ids assert and return material 0.
    const Material &get(MaterialId id) const;
    std::size_t size() const;
private:
    std::array<Material, 32> materials_{};                      // kMaxMaterials
    std::size_t count_ = 0;
};
std::string_view shader_header(ShaderDialect d, ShaderStage s);
std::string_view engine_vertex_shader_body();
std::string_view engine_fragment_shader_body();
// Sharp-bilinear upscale: exact nearest at integer scales, one soft texel edge otherwise.
std::string_view upscale_fragment_shader_body();
}
LAM_REFLECT_ENUM(lam::BlendMode, "alpha", "additive", "multiply", "premultiplied", "opaque")
