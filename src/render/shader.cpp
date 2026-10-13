// Shader source assembly (from the old gfx/shader_source) and the material table. Bodies use
// these macros so one file works on GL 3.3 and GLES:
//   ATTRIBUTE, VARYING, TEXTURE2D, FRAG_COLOR, FRAG_OUTPUT1 (second output; SHADER_HAS_MRT)
#include <cstddef>
#include <string>
#include <string_view>

#include <lam/core/assert.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>

namespace lam {
namespace {

constexpr std::string_view kVertex330 = "#version 330\n"
                                        "#define SHADER_VERTEX 1\n"
                                        "#define ATTRIBUTE in\n"
                                        "#define VARYING out\n"
                                        "#define TEXTURE2D texture\n";

constexpr std::string_view kFragment330 = "#version 330\n"
                                          "#define SHADER_FRAGMENT 1\n"
                                          "#define SHADER_HAS_DERIVATIVES 1\n"
                                          "#define VARYING in\n"
                                          "#define TEXTURE2D texture\n"
                                          "#define FRAG_COLOR finalColor\n"
                                          "#define SHADER_HAS_MRT 1\n"
                                          "#define FRAG_OUTPUT1 layout(location = 1) out vec4\n"
                                          "layout(location = 0) out vec4 finalColor;\n";

constexpr std::string_view kVertexEs300 = "#version 300 es\n"
                                          "#define SHADER_VERTEX 1\n"
                                          "#define ATTRIBUTE in\n"
                                          "#define VARYING out\n"
                                          "#define TEXTURE2D texture\n";

// ES 3.00 wants explicit locations once there's more than one output, so output 0 always has one.
// Fragment highp matches the vertex stage so shared uniforms still link.
constexpr std::string_view kFragmentEs300 = "#version 300 es\n"
                                            "#define SHADER_FRAGMENT 1\n"
                                            "#define SHADER_HAS_DERIVATIVES 1\n"
                                            "precision highp float;\n"
                                            "precision highp int;\n"
                                            "#define VARYING in\n"
                                            "#define TEXTURE2D texture\n"
                                            "#define FRAG_COLOR finalColor\n"
                                            "#define SHADER_HAS_MRT 1\n"
                                            "#define FRAG_OUTPUT1 layout(location = 1) out vec4\n"
                                            "layout(location = 0) out vec4 finalColor;\n";

constexpr std::string_view kVertexEs100 = "#version 100\n"
                                          "#define SHADER_VERTEX 1\n"
                                          "#define ATTRIBUTE attribute\n"
                                          "#define VARYING varying\n"
                                          "#define TEXTURE2D texture2D\n";

// In ES 1.00 fragment highp and derivatives are both optional.
constexpr std::string_view kFragmentEs100 = "#version 100\n"
                                            "#define SHADER_FRAGMENT 1\n"
                                            "#ifdef GL_OES_standard_derivatives\n"
                                            "#extension GL_OES_standard_derivatives : enable\n"
                                            "#define SHADER_HAS_DERIVATIVES 1\n"
                                            "#endif\n"
                                            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                                            "precision highp float;\n"
                                            "precision highp int;\n"
                                            "#else\n"
                                            "precision mediump float;\n"
                                            "#endif\n"
                                            "#define VARYING varying\n"
                                            "#define TEXTURE2D texture2D\n"
                                            "#define FRAG_COLOR gl_FragColor\n"
                                            "#define FRAG_OUTPUT1 vec4\n";

constexpr std::string_view kEngineVertex =
    "ATTRIBUTE vec2 vertexPosition;\n"
    "ATTRIBUTE vec2 vertexTexCoord;\n"
    "ATTRIBUTE vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform vec4 u_view_x;\n"
    "uniform vec4 u_view_y;\n"
    "uniform float u_snap;\n"
    "uniform vec2 u_inv_texture_size;\n"
    "VARYING vec2 fragTexCoord;\n"
    "VARYING vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 p = vec2(dot(u_view_x.xy, vertexPosition) + u_view_x.z,\n"
    "                  dot(u_view_y.xy, vertexPosition) + u_view_y.z);\n"
    "    p = mix(p, floor(p + 0.5), u_snap);\n"
    "    fragTexCoord = vertexTexCoord * u_inv_texture_size;\n"
    "    fragColor = vertexColor;\n"
    "    gl_Position = mvp * vec4(p, 0.0, 1.0);\n"
    "}\n";

constexpr std::string_view kEngineFragment =
    "VARYING vec2 fragTexCoord;\n"
    "VARYING vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "void main() {\n"
    "    FRAG_COLOR = TEXTURE2D(texture0, fragTexCoord) * fragColor;\n"
    "}\n";

// Sharp bilinear: sample texel centres, blend only within half an output pixel of an edge.
constexpr std::string_view kUpscaleFragment =
    "VARYING vec2 fragTexCoord;\n"
    "VARYING vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec2 u_source_size;\n"
    "uniform vec2 u_scale;\n"
    "void main() {\n"
    "    vec2 scale = max(u_scale, vec2(1.0));\n"
    "    vec2 texel = fragTexCoord * u_source_size;\n"
    "    vec2 base = floor(texel);\n"
    "    vec2 f = texel - base - 0.5;\n"
    "    vec2 region = 0.5 - 0.5 / scale;\n"
    "    vec2 offset = (f - clamp(f, -region, region)) * scale + 0.5;\n"
    "    vec2 uv = (base + offset) / u_source_size;\n"
    "    FRAG_COLOR = vec4(TEXTURE2D(texture0, uv).rgb, 1.0) * fragColor;\n"
    "}\n";

// '#' must be the first non-blank on the line, so a mention in a comment doesn't count.
bool has_directive(std::string_view body, std::string_view name) {
    std::size_t line_start = 0;
    while (line_start <= body.size()) {
        std::size_t line_end = body.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = body.size();
        std::string_view line = body.substr(line_start, line_end - line_start);
        const auto skip_blank = [&line] {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
        };
        skip_blank();
        if (!line.empty() && line.front() == '#') {
            line.remove_prefix(1);
            skip_blank();
            if (line.substr(0, name.size()) == name) return true;
        }
        line_start = line_end + 1;
    }
    return false;
}

} // namespace

std::string_view shader_header(ShaderDialect d, ShaderStage s) {
    const bool vertex = s == ShaderStage::Vertex;
    switch (d) {
    case ShaderDialect::Glsl330:
        return vertex ? kVertex330 : kFragment330;
    case ShaderDialect::GlslEs300:
        return vertex ? kVertexEs300 : kFragmentEs300;
    case ShaderDialect::GlslEs100:
        return vertex ? kVertexEs100 : kFragmentEs100;
    }
    return {};
}

Result<std::string> build_shader_source(ShaderDialect d, ShaderStage s, std::string_view body,
                                        std::string_view file_name) {
    const auto reject = [&](std::string_view why) -> Result<std::string> {
        return make_error(ErrorCode::Parse, std::string(file_name) + ": " + std::string(why));
    };
    if (body.substr(0, 3) == "\xEF\xBB\xBF")
        return reject("starts with a UTF-8 byte-order mark; save it without one");
    if (has_directive(body, "version"))
        return reject("must not contain #version; the loader adds it for each platform");
    if (has_directive(body, "extension"))
        return reject("must not contain #extension; extensions belong in the loader's header");
    std::string text(shader_header(d, s));
    // So driver errors point at lines in the body file.
    text += "#line 1\n";
    text += body;
    return text;
}

std::string_view engine_vertex_shader_body() {
    return kEngineVertex;
}

std::string_view engine_fragment_shader_body() {
    return kEngineFragment;
}

std::string_view upscale_fragment_shader_body() {
    return kUpscaleFragment;
}

// ---- Materials ---------------------------------------------------------------------------------

static_assert(kMaxMaterials == 32, "Materials storage is sized for kMaxMaterials");

Materials::Materials() {
    materials_[0] = Material{{}, BlendMode::Alpha};
    materials_[1] = Material{{}, BlendMode::Additive};
    materials_[2] = Material{{}, BlendMode::Multiply};
    count_ = 3;
}

MaterialId Materials::add(const Material &m) {
    LAM_CHECK(count_ < kMaxMaterials, "Materials::add: at most 32 materials");
    materials_[count_] = m;
    return static_cast<MaterialId>(count_++);
}

const Material &Materials::get(MaterialId id) const {
    LAM_ASSERT(id < count_, "Materials::get: unknown material id");
    return id < count_ ? materials_[id] : materials_[0];
}

std::size_t Materials::size() const {
    return count_;
}

} // namespace lam
