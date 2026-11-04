// The title's Direct3D 8 calls drawn with Vulkan, for Android.
//
// The state to host translation follows d3d12.cpp, and the shaders are the same HLSL (shaders.cpp) compiled to SPIR-V
// by spirv.cpp. This is the first cut: what it leaves out is listed in docs/android-vulkan.md.
#if defined(__ANDROID__)

#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "dxgiformat.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/spirv.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "gpu/vulkan_platform.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

ANativeWindow* g_window = nullptr;
bool g_fill = true;                   // the picture grows with a wide screen (see Present), else it stays 16:9
constexpr float kMaxFillAspect = 2.4f;  // 21:9 phones are 2.33
std::atomic<uint32_t> g_resolution_percent{100};  // of the screen's own pixels, set from the settings

constexpr int kFrames = 2;
constexpr uint64_t kRingSize = 64ull << 20;
constexpr uint64_t kRingReserve = 8ull << 20;
constexpr uint32_t kQuadVertices = 6;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;  // depth is stored reversed, 0 is the far plane
constexpr float kDepthFar = 0;

constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr uint32_t kZUseW = 2;
constexpr uint32_t kTexfLinear = 2, kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1, kTaddressClamp = 3;
constexpr uint32_t kBlendDestColor = 0x306;
constexpr uint32_t kCullCw = 2;
constexpr uint32_t kFillWireframe = 2;
constexpr uint32_t kMClipMin = 0x394, kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;
constexpr size_t kTsView = 2, kTsProjection = 3, kTsTexture0 = 16, kTsWorld = 256;

// ---------------------------------------------------------------------------------------------------------------------
// state translation

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004:
            return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 in Vulkan's order
VkCompareOp Cmp(uint32_t v) {
    static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER,        VK_COMPARE_OP_LESS,
                                       VK_COMPARE_OP_EQUAL,        VK_COMPARE_OP_LESS_OR_EQUAL,
                                       VK_COMPARE_OP_GREATER,      VK_COMPARE_OP_NOT_EQUAL,
                                       VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return ops[v >= 0x200 && v <= 0x207 ? v - 0x200 : 7];
}

// the same compare with depth stored reversed
VkCompareOp ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return Cmp(0x200 + mirror[n]);
}

float CmpIndex(uint32_t v) { return float(v >= 0x200 && v <= 0x207 ? v - 0x200 : 7); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat VertexFormat(uint8_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr uint32_t kFmtD16 = 0x2C, kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// ---------------------------------------------------------------------------------------------------------------------
// draw constants, as d3d12.cpp fills them

struct GuestExtent {
    uint32_t w, h;
};

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += x[r * 4 + k] * y[k * 4 + c];
            o[r * 4 + c] = v;
        }
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[c * 4 + r] = m[r * 4 + c];
}

// the inverse transpose of the upper 3x3 of a row major 4x4, into the upper 3x3 of n
void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? CmpIndex(SimpleState(s, kMAlphaFunc)) : 7.0f;
    c.misc[2] = CmpIndex(s.rs[kRsShadowFunc]);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats
constexpr int kLightFloats = 26, kLightDiffuse = 1, kLightSpecular = 5, kLightAmbient = 9, kLightPosition = 13,
              kLightDirection = 16, kLightRange = 19, kLightFalloff = 20, kLightAttenuation0 = 21,
              kLightAttenuation1 = 22, kLightAttenuation2 = 23, kLightTheta = 24, kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

struct WRange {
    float near_w, far_w;
};

WRange ProjectionWRange(const float* m) {
    float m33 = m[10], m34 = m[11], m43 = m[14], m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// ---------------------------------------------------------------------------------------------------------------------
// DXT decoding for GPUs without BC formats (most phones)

void Rgb565(uint16_t c, uint8_t* o) {  // BGRA
    o[2] = uint8_t((c >> 11 & 31) * 255 / 31);
    o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[0] = uint8_t((c & 31) * 255 / 31);
    o[3] = 255;
}

void DecodeColourBlock(const uint8_t* b, uint8_t out[16][4], bool opaque4) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t p[4][4];
    Rgb565(c0, p[0]);
    Rgb565(c1, p[1]);
    if (c0 > c1 || opaque4) {
        for (int i = 0; i < 3; i++) {
            p[2][i] = uint8_t((2 * p[0][i] + p[1][i]) / 3);
            p[3][i] = uint8_t((p[0][i] + 2 * p[1][i]) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) p[2][i] = uint8_t((p[0][i] + p[1][i]) / 2);
        p[2][3] = 255;
        p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out[i], p[(bits >> (i * 2)) & 3], 4);
}

// kind 1, 2, 3 for BC1, BC2, BC3: block rows of w x h texels as BGRA
void DecodeBc(Host kind, const std::vector<uint8_t>& in, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t block = kind == Host::BC1 ? 8 : 16;
    out.assign(size_t(bw) * 4 * bh * 4 * 4, 0);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t at = (size_t(by) * bw + bx) * block;
            if (at + block > in.size()) return;
            const uint8_t* b = &in[at];
            uint8_t px[16][4];
            DecodeColourBlock(kind == Host::BC1 ? b : b + 8, px, kind != Host::BC1);
            if (kind == Host::BC2) {
                for (int i = 0; i < 16; i++) px[i][3] = uint8_t((b[i / 2] >> ((i & 1) * 4) & 15) * 17);
            } else if (kind == Host::BC3) {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) {
                    for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                } else {
                    for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (i * 3)) & 7];
            }
            for (int y = 0; y < 4; y++)
                std::memcpy(&out[((size_t(by) * 4 + y) * bw * 4 + size_t(bx) * 4) * 4], px[y * 4], 16);
        }
}

// ---------------------------------------------------------------------------------------------------------------------
// the backend

struct VkTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;      // what shaders sample (depth only for a depth target)
    VkImageView att_view = VK_NULL_HANDLE;  // what rendering attaches (depth and stencil for a depth target)
    VkFormat format = kBgra;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false;
    uint32_t w = 0, h = 0;      // the host image: the guest size times the render scale
    uint32_t gw = 0, gh = 0;    // the console surface's size, which the title's rects and viewports are in
    uint32_t data = 0;
};

struct VkTex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    TextureDesc d;
    VkFormat format = kBgra;
    uint32_t levels = 1;
    uint32_t pad_w = 0, pad_h = 0;
    uint64_t sig = 0, frame = ~0ull, palette = 0;
    bool decode_bc = false;
};

struct PsoKey {
    uint64_t vs, ps;
    uint32_t regs, topo, has_rt, has_ds;
    uint32_t elements[16];
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

struct DrawShaders {
    bool prog = false, quad = false, fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};
};

using PerfClock = std::chrono::steady_clock;

// where a second of frames goes, logged by Present
struct Perf {
    double draw_ms = 0, compile_ms = 0, pso_ms = 0, wait_ms = 0, present_ms = 0, tex_ms = 0, bind_ms = 0;
    uint64_t frames = 0, draws = 0, shaders = 0, psos = 0, texups = 0, texup_bytes = 0, flushes = 0, rendering = 0, sets = 0;
};

struct PerfTimer {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    explicit PerfTimer(double& a) : acc(a) {}
    ~PerfTimer() { acc += std::chrono::duration<double, std::milli>(PerfClock::now() - t).count(); }
};

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t, uint32_t) override { LOG_FIRST(1, "vulkan: read-back of a render target is not implemented"); }
    void SyncLate(uint32_t) override {}
    void Flush() override;
    void SetGamma(const uint8_t*) override {}
    // no occlusion queries yet: every counted test reports visible pixels, so what hides behind one stays drawn
    void GetReport(uint32_t report) override;
    void WaitGpuFrame(uint64_t n) override;

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceMemory ring_memory = VK_NULL_HANDLE;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        bool submitted = false;
        std::vector<std::function<void()>> garbage;
        std::vector<VkDescriptorPool> pools;  // one descriptor set per draw; reset with the frame
        size_t pool_next = 0;                 // the pool the next set comes from
    };
    VkDescriptorSet AllocSet();
    void ResetFrame(Frame& f);
    struct Alloc {
        VkBuffer buffer;
        uint64_t offset;
        uint8_t* cpu;
    };

    // setup
    bool CreateInstance();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateFrames();
    bool CreatePipelineLayout();
    bool CreateDummies();
    uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags want) const;
    bool CreateImage(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                     uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage& image,
                     VkDeviceMemory& memory);
    // frame
    Frame& Current() { return frames_[frame_ % kFrames]; }
    Alloc Upload(uint64_t size, uint64_t align);
    void Begin();
    void Submit(bool wait);
    void NextFrame();
    // images
    void Transition(VkImage image, VkImageAspectFlags aspect, VkImageLayout& cur, VkImageLayout want, uint32_t levels = 1,
                    uint32_t layers = 1);
    void StartRendering(VkTarget* rt, VkTarget* ds);
    void EndRendering();
    VkTarget* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(VkTarget& t);
    // draw
    DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) const;
    VkShaderModule Module(const std::string& hlsl, spirv::Stage stage, uint64_t& hash);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) const;
    bool BindTexture(const State& s, int stage, DrawTextures& stages);
    bool UploadTexture(VkTex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest,
                       const uint32_t* palette);
    VkSampler Sampler(const State& s, int stage, const DrawTextures& stages, bool shadow);
    void FillDrawConstants(const State& s, const DrawShaders& sh, const PsoKey& k, GuestExtent size, DrawConstants& c) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkPhysicalDeviceFeatures features_{};
    VkPhysicalDeviceProperties props_{};
    bool bc_ = false;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = kBgra;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageLayout> swap_layouts_;
    std::vector<VkSemaphore> swap_done_;

    std::array<Frame, kFrames> frames_;
    uint64_t frame_ = 0, presents_ = 0;
    bool recording_ = false;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, VkTarget> targets_;
    std::unordered_map<uint64_t, VkTex> textures_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;  // by the hash of the HLSL
    struct ShaderEntry {
        VkShaderModule module;
        uint64_t hash;
    };
    std::unordered_map<uint64_t, ShaderEntry> shader_keys_;  // by the draw's shader key: no HLSL written on a hit
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkTarget* bound_rt_ = nullptr;
    VkTarget* bound_ds_ = nullptr;
    VkTarget* rendering_rt_ = nullptr;
    VkTarget* rendering_ds_ = nullptr;
    bool rendering_ = false;

    // a stage with no texture reads one of these
    struct Dummy {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    } dummy_[4];
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    Assembled asm_;
    Perf perf_;
    PerfClock::time_point perf_start_ = PerfClock::now();
    void LogPerf();
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float sx_ = 1, sy_ = 1;  // host pixels per console pixel, across and down
    void ApplyScale();

    PFN_vkCmdBeginRendering vkCmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRendering vkCmdEndRendering_ = nullptr;
    PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2_ = nullptr;
    PFN_vkCmdBindVertexBuffers2 vkCmdBindVertexBuffers2_ = nullptr;
};

#define VK_CHECK(x, what)                                               \
    do {                                                                \
        VkResult r_ = (x);                                              \
        if (r_ != VK_SUCCESS) {                                         \
            LOGE("vulkan: %s failed (%d)", what, int(r_));              \
            return false;                                               \
        }                                                               \
    } while (0)

VulkanBackend::~VulkanBackend() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    // the rest is released with the process; the device owns it
}

// ---- setup ----------------------------------------------------------------------------------------------------------

bool VulkanBackend::CreateInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Ratatouille";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = g_window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateAndroidSurfaceKHR");
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    for (VkPhysicalDevice g : gpus) {
        vkGetPhysicalDeviceProperties(g, &props_);
        if (props_.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                gpu_ = g;
                family_ = i;
                break;
            }
        }
        if (gpu_) break;
    }
    if (!gpu_) {
        LOGE("vulkan: no Vulkan 1.3 GPU with a graphics queue that can present (the renderer needs dynamic rendering)");
        return false;
    }
    {
        // what the driver offers of what the renderer wants: a missing one is named here instead of failing the device
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> have(en);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, have.data());
        auto has = [&](const char* name) {
            for (const auto& e : have)
                if (!std::strcmp(e.extensionName, name)) return true;
            return false;
        };
        LOGI("vulkan: api %u.%u.%u driver %08X, %u device extensions", VK_VERSION_MAJOR(props_.apiVersion),
             VK_VERSION_MINOR(props_.apiVersion), VK_VERSION_PATCH(props_.apiVersion), props_.driverVersion, en);
        for (const char* name : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                 VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                 VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, "VK_KHR_maintenance1"})
            LOGI("vulkan:   %-40s %s", name, has(name) ? "yes" : "NO");
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memory_);
    vkGetPhysicalDeviceFeatures(gpu_, &features_);
    bc_ = features_.textureCompressionBC;
    LOGI("vulkan: %s, BC textures %s", props_.deviceName, bc_ ? "native" : "decoded on the CPU");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.samplerAnisotropy = features_.samplerAnisotropy;
    f2.features.fillModeNonSolid = features_.fillModeNonSolid;
    f2.features.textureCompressionBC = features_.textureCompressionBC;
    f2.features.depthBiasClamp = features_.depthBiasClamp;

    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(gpu_, &dci, nullptr, &dev_), "vkCreateDevice");
    vkGetDeviceQueue(dev_, family_, 0, &queue_);

    vkCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(dev_, "vkCmdBeginRendering"));
    vkCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(dev_, "vkCmdEndRendering"));
    vkCmdPipelineBarrier2_ =
        reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(dev_, "vkCmdPipelineBarrier2"));
    vkCmdBindVertexBuffers2_ =
        reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(vkGetDeviceProcAddr(dev_, "vkCmdBindVertexBuffers2"));
    if (!vkCmdBeginRendering_ || !vkCmdEndRendering_ || !vkCmdPipelineBarrier2_ || !vkCmdBindVertexBuffers2_) {
        LOGE("vulkan: the driver lacks dynamic rendering, synchronization2 or extended dynamic state");
        return false;
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(dev_, &pci, nullptr, &cache_);
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps), "surface capabilities");
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{kBgra, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats)
        if (f.format == kBgra || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    swap_format_ = chosen.format;
    swap_extent_ = caps.currentExtent;
    if (swap_extent_.width == 0xFFFFFFFFu) swap_extent_ = {guest_w_, guest_h_};

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sc.minImageCount = std::min(sc.minImageCount, caps.maxImageCount);
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = swap_extent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // a landscape game on a portrait panel: identity makes the compositor rotate the picture. currentTransform would
    // promise an image already rotated, which is a rotation the renderer does not do.
    sc.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    LOGI("vulkan: surface %ux%u, current transform %X, supported %X, using %X", caps.currentExtent.width,
         caps.currentExtent.height, unsigned(caps.currentTransform), unsigned(caps.supportedTransforms),
         unsigned(sc.preTransform));
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = swap_;
    VkSwapchainKHR next;
    VK_CHECK(vkCreateSwapchainKHR(dev_, &sc, nullptr, &next), "vkCreateSwapchainKHR");
    if (swap_) {
        vkDeviceWaitIdle(dev_);
        for (VkSemaphore s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
        vkDestroySwapchainKHR(dev_, swap_, nullptr);
    }
    swap_ = next;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, swap_images_.data());
    swap_layouts_.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
    swap_done_.resize(count);
    for (auto& s : swap_done_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev_, &si, nullptr, &s);
    }
    return true;
}

uint32_t VulkanBackend::FindMemory(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

bool VulkanBackend::CreateFrames() {
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family_;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cmd), "command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(dev_, &fi, nullptr, &f.fence), "fence");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &f.acquired), "semaphore");

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kRingSize;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(dev_, &bi, nullptr, &f.ring), "ring buffer");
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev_, f.ring, &mr);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = mr.size;
        mi.memoryTypeIndex = FindMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(dev_, &mi, nullptr, &f.ring_memory), "ring memory");
        vkBindBufferMemory(dev_, f.ring, f.ring_memory, 0);
        void* p = nullptr;
        VK_CHECK(vkMapMemory(dev_, f.ring_memory, 0, VK_WHOLE_SIZE, 0, &p), "map ring");
        f.map = static_cast<uint8_t*>(p);
    }
    return true;
}

bool VulkanBackend::CreatePipelineLayout() {
    std::array<VkDescriptorSetLayoutBinding, 10> b{};
    for (uint32_t i = 0; i < 10; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        if (i < 2) {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        } else if (i < 6) {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        } else {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = uint32_t(b.size());
    li.pBindings = b.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_), "descriptor set layout");
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_), "pipeline layout");
    return true;
}

bool VulkanBackend::CreateImage(VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                                uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage& image,
                                VkDeviceMemory& memory) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = flags;
    ii.imageType = type;
    ii.format = fmt;
    ii.extent = {w, h, depth};
    ii.mipLevels = levels;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev_, &ii, nullptr, &image), "vkCreateImage");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev_, image, &mr);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = FindMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &mi, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(dev_, image, nullptr);
        image = VK_NULL_HANDLE;
        LOGE("vulkan: out of device memory for a %ux%u image", w, h);
        return false;
    }
    vkBindImageMemory(dev_, image, memory, 0);
    return true;
}

bool VulkanBackend::CreateDummies() {
    // 2D, cube and 3D, indexed by TexKind
    struct Spec {
        VkImageType type;
        VkImageViewType view;
        uint32_t layers;
        VkImageCreateFlags flags;
    } specs[4] = {{VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0},
                  {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0},
                  {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
                  {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}};
    Begin();
    for (int k = 0; k < 4; k++) {
        Dummy& d = dummy_[k];
        if (!CreateImage(specs[k].type, kBgra, 1, 1, 1, 1, specs[k].layers,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, specs[k].flags, d.image,
                         d.memory))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = specs[k].view;
        vi.format = kBgra;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, specs[k].layers};
        VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &d.view), "dummy view");
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(d.image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, specs[k].layers);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, specs[k].layers};
        vkCmdClearColorImage(Current().cmd, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        Transition(d.image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                   specs[k].layers);
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev_, &si, nullptr, &dummy_sampler_), "dummy sampler");
    return true;
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    g_window = GetAndroidWindow();
    if (!g_window) {
        LOGE("vulkan: no Android window; gpu::SetAndroidWindow must run before the title starts");
        return false;
    }
    if (!CreateInstance() || !CreateDevice() || !CreateSwapchain() || !CreateFrames() || !CreatePipelineLayout())
        return false;
    // 1x1 images in kTexNone, k2D, k3D, kCube order are indexed by TexKind
    if (!CreateDummies()) return false;
    ApplyScale();
    LOGI("vulkan: ready, swapchain %ux%u", swap_extent_.width, swap_extent_.height);
    return true;
}

// ---- frame ------------------------------------------------------------------------------------------------------------

void VulkanBackend::Begin() {
    if (recording_) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Current().cmd, &bi);
    recording_ = true;
}

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = Current();
    uint64_t at = (f.used + align - 1) & ~(align - 1);
    if (at + size > kRingSize) {
        // past the ring: wait for the GPU and start it over (callers check kRingReserve first, so this is a huge draw)
        Submit(true);
        Begin();
        at = 0;
        LOGE("vulkan: upload ring overflow");
    }
    f.used = at + size;
    return {f.ring, at, f.map + at};
}

void VulkanBackend::Submit(bool wait) {
    if (!recording_) return;
    EndRendering();
    Frame& f = Current();
    vkEndCommandBuffer(f.cmd);
    recording_ = false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    vkResetFences(dev_, 1, &f.fence);
    vkQueueSubmit(queue_, 1, &si, f.fence);
    f.submitted = true;
    if (wait) {
        perf_.flushes++;
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        ResetFrame(f);
    }
}

// the GPU is done with the frame: its garbage goes, its ring and descriptor pools start over
void VulkanBackend::ResetFrame(Frame& f) {
    f.submitted = false;
    f.used = 0;
    for (auto& g : f.garbage) g();
    f.garbage.clear();
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
    f.pool_next = 0;
    vkResetCommandBuffer(f.cmd, 0);
}

// a set for the draw being recorded, from the frame's pools; a full pool makes the next one
VkDescriptorSet VulkanBackend::AllocSet() {
    Frame& f = Current();
    for (;;) {
        if (f.pool_next == f.pools.size()) {
            constexpr uint32_t kSets = 8192;
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets * 2},
                                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * 4},
                                                  {VK_DESCRIPTOR_TYPE_SAMPLER, kSets * 4}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSets;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool pool;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_next];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(dev_, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        f.pool_next++;
    }
}

void VulkanBackend::Flush() {
    Submit(true);
    Begin();
}

void VulkanBackend::NextFrame() {
    frame_++;
    Frame& f = Current();
    if (f.submitted) {
        {
            PerfTimer t(perf_.wait_ms);
            vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        }
        ResetFrame(f);
    }
    f.used = 0;
    Begin();
}

void VulkanBackend::WaitGpuFrame(uint64_t n) {
    // frame slots are reused every kFrames presents; a present the slot still holds is waited for
    if (n + kFrames < presents_) return;
    Frame& f = frames_[n % kFrames];
    if (f.submitted) vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
}

void VulkanBackend::GetReport(uint32_t report) {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    kernel::W32(report + kTime, uint32_t(presents_));
    kernel::W32(report + kTimeHigh, 0);
    kernel::W32(report + kCount, 0x100);
    kernel::W32(report + kStatus, 0);
}

// ---- images and targets ---------------------------------------------------------------------------------------------

void VulkanBackend::Transition(VkImage image, VkImageAspectFlags aspect, VkImageLayout& cur, VkImageLayout want,
                               uint32_t levels, uint32_t layers) {
    if (cur == want) return;
    EndRendering();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = cur;
    b.newLayout = want;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, levels, 0, layers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2_(Current().cmd, &di);
    cur = want;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    vkCmdEndRendering_(Current().cmd);
    rendering_ = false;
    rendering_rt_ = rendering_ds_ = nullptr;
}

void VulkanBackend::StartRendering(VkTarget* rt, VkTarget* ds) {
    if (rendering_ && rt == rendering_rt_ && ds == rendering_ds_) return;
    EndRendering();
    if (rt) Transition(rt->image, VK_IMAGE_ASPECT_COLOR_BIT, rt->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (ds)
        Transition(ds->image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, ds->layout,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.layerCount = 1;
    const VkTarget* any = rt ? rt : ds;
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    if (rt) {
        color.imageView = rt->att_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
    }
    if (ds) {
        depth.imageView = ds->att_view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        stencil = depth;
        ri.pDepthAttachment = &depth;
        ri.pStencilAttachment = &stencil;
    }
    vkCmdBeginRendering_(Current().cmd, &ri);
    rendering_ = true;
    rendering_rt_ = rt;
    rendering_ds_ = ds;
}

void VulkanBackend::ReleaseTarget(VkTarget& t) {
    if (bound_rt_ == &t) bound_rt_ = nullptr;
    if (bound_ds_ == &t) bound_ds_ = nullptr;
    if (rendering_rt_ == &t || rendering_ds_ == &t) EndRendering();
    VkDevice d = dev_;
    VkImageView v = t.view, av = t.att_view;
    VkImage i = t.image;
    VkDeviceMemory m = t.memory;
    Current().garbage.push_back([d, v, av, i, m] {
        vkDestroyImageView(d, v, nullptr);
        if (av != v) vkDestroyImageView(d, av, nullptr);  // a colour target uses one view for both
        vkDestroyImage(d, i, nullptr);
        vkFreeMemory(d, m, nullptr);
    });
}

VkTarget* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    if (!gw || !gh) return nullptr;
    uint64_t key = uint64_t(s.data) << 1 | (depth ? 1 : 0);
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        if (it->second.gw == gw && it->second.gh == gh) return &it->second;
        ReleaseTarget(it->second);
        targets_.erase(it);
    }
    LOGI("vulkan: new %s target %08X %ux%u at %ux%u", depth ? "depth" : "colour", s.data, gw, gh,
         std::max(1u, uint32_t(std::lround(gw * sx_))), std::max(1u, uint32_t(std::lround(gh * sy_))));
    VkTarget t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * sx_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sy_)));
    t.data = s.data;
    t.format = depth ? kDepthFormat : kBgra;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(VK_IMAGE_TYPE_2D, t.format, t.w, t.h, 1, 1, 1, usage, 0, t.image, t.memory)) return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    // a sampled depth view may name the depth aspect only; the attachment view names both depth and stencil
    vi.subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev_, &vi, nullptr, &t.view) != VK_SUCCESS) return nullptr;
    if (depth) {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (vkCreateImageView(dev_, &vi, nullptr, &t.att_view) != VK_SUCCESS) return nullptr;
    } else {
        t.att_view = t.view;
    }
    Begin();
    // new targets start cleared, like the console's memory the title has not drawn in yet
    if (depth) {
        Transition(t.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, t.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue v{kDepthFar, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Current().cmd, t.image, t.layout, &v, 1, &r);
    } else {
        Transition(t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue v{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Current().cmd, t.image, t.layout, &v, 1, &r);
    }
    return &(targets_[key] = t);
}

// ---- shaders and pipelines ------------------------------------------------------------------------------------------

DrawShaders VulkanBackend::ClassifyShaders(const State& s, const Assembled& a, uint32_t count) const {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

constexpr uint64_t kDeclarationSalt = 0x10000, kPretransformedSalt = 0x5A5A, kQuadSalt = 0xC3C30000;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// HLSL to a shader module, once per distinct source
VkShaderModule VulkanBackend::Module(const std::string& hlsl, spirv::Stage stage, uint64_t& hash) {
    hash = Hash(hlsl.data(), hlsl.size(), uint64_t(stage) + 1);
    auto it = modules_.find(hash);
    if (it != modules_.end()) return it->second;
    std::vector<uint32_t> code;
    std::string err;
    VkShaderModule module = VK_NULL_HANDLE;
    PerfTimer compile_timer(perf_.compile_ms);
    perf_.shaders++;
    if (!spirv::Compile(hlsl, "main", stage, code, err)) {
        LOGE("vulkan: shader compile failed: %s", err.c_str());
        LOGE("%s", hlsl.c_str());
    } else {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * 4;
        mi.pCode = code.data();
        if (vkCreateShaderModule(dev_, &mi, nullptr, &module) != VK_SUCCESS) module = VK_NULL_HANDLE;
    }
    modules_[hash] = module;
    return module;
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) const {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology) | (a.strip ? 0x100u : 0u);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? uint32_t(bound_rt_->format) : 0;
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    if (k.has_ds) {
        k.zen = s.rs[kRsZEnable] != 0;
        if (k.zen) {
            k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
            k.zfunc = SimpleState(s, kMZFunc);
        }
        k.sten = s.rs[kRsStencilEnable] != 0;
        if (k.sten) {
            k.sfunc = SimpleState(s, kMStencilFunc);
            k.smask = SimpleState(s, kMStencilMask) & 0xFF;
            k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
            k.sfail = s.rs[kRsStencilFail];
            k.szfail = SimpleState(s, kMStencilZFail);
            k.spass = SimpleState(s, kMStencilPass);
        }
        // toward the camera is up on reversed depth
        if (OffsetsPolygons(s, a.topology)) k.zslope = -Bits(SimpleState(s, kMSlopeScale));
    }
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    std::array<VkVertexInputAttributeDescription, 16> attrs{};
    std::array<VkVertexInputBindingDescription, 16> binds{};
    uint32_t na = 0, nb = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        uint32_t buffer = (e >> 8) & 0xFF;
        attrs[na++] = {r, buffer, VertexFormat(uint8_t(e & 0xFF)), e >> 16};
        nb = std::max(nb, buffer + 1);
    }
    for (uint32_t b = 0; b < nb; b++) binds[b] = {b, 0, VK_VERTEX_INPUT_RATE_VERTEX};  // stride is dynamic
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = nb;
    vin.pVertexBindingDescriptions = binds.data();
    vin.vertexAttributeDescriptionCount = na;
    vin.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    const Topology topo = Topology(k.topo & 0xFF);
    const bool strip = (k.topo & 0x100) != 0;
    ia.topology = topo == Topology::Points  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topo == Topology::Lines ? (strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                                            : (strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe && features_.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias || k.zslope ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = k.zwrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ReversedCmp(k.zfunc);
    ds.stencilTestEnable = k.sten ? VK_TRUE : VK_FALSE;
    VkStencilOpState so{};
    so.failOp = StencilOp(k.sfail);
    so.depthFailOp = StencilOp(k.szfail);
    so.passOp = StencilOp(k.spass);
    so.compareOp = Cmp(k.sfunc);
    so.compareMask = k.smask;
    so.writeMask = k.swmask;
    ds.front = ds.back = so;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = Blend(k.src, false);
    cba.dstColorBlendFactor = Blend(k.dst, false);
    cba.colorBlendOp = BlendOp(k.op);
    cba.srcAlphaBlendFactor = Blend(k.src, true);
    cba.dstAlphaBlendFactor = Blend(k.dst, true);
    cba.alphaBlendOp = BlendOp(k.op);
    cba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &cba;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;

    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    PerfTimer pso_timer(perf_.pso_ms);
    perf_.psos++;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipeline) != VK_SUCCESS) {
        LOGE("vulkan: pipeline creation failed");
        pipeline = VK_NULL_HANDLE;
    }
    psos_[h] = pipeline;
    return pipeline;
}

// ---- textures -------------------------------------------------------------------------------------------------------

constexpr uint32_t kSigWholeMax = 65536, kSigSamples = 64, kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        case Host::R16Unorm: return VK_FORMAT_R16_UNORM;
        default: return kBgra;
    }
}

uint32_t TexelBytes(Host h) { return h == Host::R16Unorm ? 2 : 4; }
uint32_t BlockBytes(Host h) { return h == Host::BC1 ? 8 : 16; }
bool IsBc(Host h) { return h == Host::BC1 || h == Host::BC2 || h == Host::BC3; }

bool VulkanBackend::UploadTexture(VkTex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest,
                                  const uint32_t* palette) {
    PerfTimer tex_timer(perf_.tex_ms);
    perf_.texups++;
    const uint32_t faces = d.cube ? 6 : 1;
    Transition(tx.image, VK_IMAGE_ASPECT_COLOR_BIT, tx.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tx.levels, faces);
    std::vector<uint8_t> px, conv;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, guest, palette, px, pitch);
            const uint32_t w = std::max(1u, tx.pad_w >> l), h = std::max(1u, tx.pad_h >> l);
            const uint32_t z = std::max(1u, d.depth >> l);
            const std::vector<uint8_t>* src = &px;
            uint32_t row_bytes, rows, buf_row = 0, buf_height = 0;
            if (tx.decode_bc) {
                DecodeBc(fi.host, px, w, h, conv);
                src = &conv;
                buf_row = (w + 3) & ~3u;
                buf_height = (h + 3) & ~3u;
                row_bytes = buf_row * 4;
                rows = buf_height;
                pitch = row_bytes;
            } else if (IsBc(fi.host)) {
                row_bytes = ((w + 3) / 4) * BlockBytes(fi.host);
                rows = (h + 3) / 4;
            } else {
                row_bytes = w * TexelBytes(fi.host);
                rows = h;
            }
            const uint64_t bytes = uint64_t(row_bytes) * rows * z;
            Alloc up = Upload(bytes, 64);
            // rows tightly packed; a short or missing source row reads as zero
            const size_t src_rows = pitch ? src->size() / pitch / z : 0;
            for (uint32_t zi = 0; zi < z; zi++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = up.cpu + (size_t(zi) * rows + y) * row_bytes;
                    if (y < src_rows) {
                        std::memcpy(dst, &(*src)[(size_t(zi) * src_rows + y) * pitch], std::min<size_t>(pitch, row_bytes));
                    } else {
                        std::memset(dst, 0, row_bytes);
                    }
                }
            VkBufferImageCopy region{};
            region.bufferOffset = up.offset;
            region.bufferRowLength = buf_row;
            region.bufferImageHeight = buf_height;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
            region.imageExtent = {w, h, z};
            vkCmdCopyBufferToImage(Current().cmd, up.buffer, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
    }
    Transition(tx.image, VK_IMAGE_ASPECT_COLOR_BIT, tx.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tx.levels, faces);
    return true;
}

// the stage's texture: a render target the title drew, or guest memory uploaded as is; a dummy when neither
bool VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.view[stage] = dummy_[kTex2D].view;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return false;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    stages.view[stage] = dummy_[info.kind].view;

    // rendered by the GPU: sample the target itself
    auto tit = targets_.find(uint64_t(d.data) << 1 | (fi.depth ? 1 : 0));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed && tit->second.gw == d.width &&
        tit->second.gh == d.height) {
        VkTarget& t = tit->second;
        if (&t == bound_rt_ || &t == bound_ds_) {
            LOG_FIRST(2, "vulkan: a draw samples the target it renders to; reading a dummy");
            return false;
        }
        if (fi.depth) {
            Transition(t.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, t.layout,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            info.shadow = ShadowDepthKind(d.format);
        } else {
            Transition(t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            info.flags |= kTexTarget;
        }
        stages.view[stage] = t.view;
        return true;
    }
    if (fi.host == Host::None || fi.depth) return false;

    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    VkTex& tx = textures_[key];
    const bool same = tx.image && tx.d.width == d.width && tx.d.height == d.height && tx.d.depth == d.depth &&
                      tx.d.levels == d.levels && tx.d.format == d.format && tx.d.cube == d.cube &&
                      tx.d.linear == d.linear;
    if (!same) {
        if (tx.image) {
            VkDevice dev = dev_;
            VkImageView v = tx.view;
            VkImage i = tx.image;
            VkDeviceMemory m = tx.memory;
            Current().garbage.push_back([dev, v, i, m] {
                vkDestroyImageView(dev, v, nullptr);
                vkDestroyImage(dev, i, nullptr);
                vkFreeMemory(dev, m, nullptr);
            });
            tx = VkTex{};
        }
        tx.d = d;
        tx.decode_bc = IsBc(fi.host) && !bc_;
        tx.format = tx.decode_bc ? kBgra : HostFormat(fi.host);
        tx.pad_w = d.width;
        tx.pad_h = d.height;
        tx.levels = std::max(1u, d.levels);
        // a block compressed level that is not a whole number of blocks is kept as one padded level
        if (IsBc(fi.host) && ((d.width & 3) || (d.height & 3))) {
            tx.pad_w = (d.width + 3) & ~3u;
            tx.pad_h = (d.height + 3) & ~3u;
            tx.levels = 1;
        }
        const uint32_t faces = d.cube ? 6 : 1;
        if (!CreateImage(d.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, tx.format, tx.pad_w, tx.pad_h,
                         std::max(1u, d.depth), tx.levels, faces,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, tx.image, tx.memory)) {
            textures_.erase(key);
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tx.image;
        vi.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = tx.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tx.levels, 0, faces};
        if (vkCreateImageView(dev_, &vi, nullptr, &tx.view) != VK_SUCCESS) {
            textures_.erase(key);
            return false;
        }
        tx.sig = ~0ull;
    }
    // the bytes are checked once a frame
    if (tx.frame != presents_) {
        tx.frame = presents_;
        const uint32_t total = TotalSize(d);
        uint64_t sig = MemSig(d.data, total);
        if (sig != tx.sig || palette.hash != tx.palette) {
            tx.sig = sig;
            tx.palette = palette.hash;
            UploadTexture(tx, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), colours);
        }
    }
    stages.view[stage] = tx.view;
    return true;
}

VkSampler VulkanBackend::Sampler(const State& s, int i, const DrawTextures& stages, bool) {
    const auto& t = s.tss[size_t(i)];
    const uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = Address(t[kTssAddressU]);
    si.addressModeV = Address(t[kTssAddressV]);
    si.addressModeW = Address(t[kTssAddressW]);
    si.mipLodBias = Bits(t[kTssMipLodBias]);
    const bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    if (anisotropic && features_.samplerAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min<float>(float(std::max<uint32_t>(t[kTssMaxAnisotropy], 1)),
                                           props_.limits.maxSamplerAnisotropy);
    }
    si.minLod = float(t[kTssMaxMipLevel]);
    si.maxLod = mip ? VK_LOD_CLAMP_NONE : si.minLod;
    float border[4];
    Color(t[kTssBorderColor], border);
    si.borderColor = border[3] == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : border[0] + border[1] + border[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (stages.info[i].shadow) {
        // the console tests texel OP r; with depth stored reversed (g_reversed_depth) the same OP holds here
        si.compareEnable = VK_TRUE;
        si.compareOp = Cmp(s.rs[kRsShadowFunc]);
        si.anisotropyEnable = VK_FALSE;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    uint64_t key = Hash(&si.magFilter, offsetof(VkSamplerCreateInfo, unnormalizedCoordinates) -
                                           offsetof(VkSamplerCreateInfo, magFilter));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev_, &si, nullptr, &sampler) != VK_SUCCESS) sampler = dummy_sampler_;
    samplers_[key] = sampler;
    return sampler;
}

// ---- draw -----------------------------------------------------------------------------------------------------------

void VulkanBackend::FillDrawConstants(const State& s, const DrawShaders& sh, const PsoKey& k, GuestExtent size,
                                      DrawConstants& c) const {
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    // ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset
    const bool offset = OffsetsPolygons(s, Topology(k.topo & 0xFF));
    const float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
    } else {
        WRange r = DrawWRange(s, sh.prog);
        c.zparams[0] = r.near_w / (r.far_w - r.near_w);
        c.zparams[1] = r.far_w;
        c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
    }
    // render target reads move back the half pixel the XDK's screen space offset adds
    const bool shifted = !sh.prog || ReadsViewportOffset(s.vs->program);
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) {
            c.texsize[i][0] = float(d.width);
            c.texsize[i][1] = float(d.height);
        }
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        c.rtshift[i] = shifted ? -0.5f * 0.5f * (sx_ + sy_) : 0.0f;  // host pixels
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfTimer draw_timer(perf_.draw_ms);
    perf_.draws++;
    Begin();
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (a.topology == Topology::Points) {
        LOG_FIRST(1, "vulkan: point lists are not drawn yet (the console's point-to-pixel expansion needs a geometry shader)");
        return;
    }
    if (Current().used > kRingSize - kRingReserve) Flush();

    VkTarget* rt = EnsureTarget(s.rt, false);
    VkTarget* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    bound_rt_ = rt;
    bound_ds_ = ds;

    // shaders
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog && !s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
    sh.vkey = VertexKey(s, a, sh);
    // the HLSL is only written for a shader this key has not made before: it is kilobytes of text per draw otherwise
    auto vit = shader_keys_.find(sh.vkey);
    if (vit != shader_keys_.end()) {
        sh.vs = vit->second.module;
        sh.vh = vit->second.hash;
    } else {
        std::string src = ReversedDepthHlsl(sh.prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                                                    : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk));
        sh.vs = Module(sh.quad ? QuadHlsl(std::move(src)) : src, spirv::Stage::Vertex, sh.vh);
        shader_keys_[sh.vkey] = {sh.vs, sh.vh};
    }

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    {
        PerfTimer bind_timer(perf_.bind_ms);
        for (int i = 0; i < 4; i++) BindTexture(s, i, stages);
    }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    auto pit = shader_keys_.find(sh.pkey ^ kPixelKeySalt);  // pixel keys share the map, the salt keeps them apart
    if (pit != shader_keys_.end()) {
        sh.ps = pit->second.module;
        sh.ph = pit->second.hash;
    } else {
        sh.ps = Module(PixelHlsl(pk), spirv::Stage::Pixel, sh.ph);
        shader_keys_[sh.pkey ^ kPixelKeySalt] = {sh.ps, sh.ph};
    }
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    VkPipeline pso = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pso) return;

    // constants
    GuestExtent size;
    SurfaceSize(rt ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillDrawConstants(s, sh, k, size, c);
    Alloc vsc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vsc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    std::memcpy(dc.cpu, &c, sizeof(c));

    // geometry
    std::array<VkBuffer, 16> vbs{};
    std::array<VkDeviceSize, 16> offsets{}, sizes{}, strides{};
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbs[i] = vb.buffer;
        offsets[i] = vb.offset;
        sizes[i] = b.bytes;
        strides[i] = b.stride;
    }
    Alloc ib{};
    uint32_t index_bytes = 0;
    if (a.index_count) {
        index_bytes = a.index_count * (a.index16 ? 2 : 4);
        ib = Upload(index_bytes, 4);
        std::memcpy(ib.cpu, a.index_src, index_bytes);
    }

    // record
    StartRendering(rt, ds);
    VkCommandBuffer cmd = Current().cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);

    // the viewport covers the target (flipped: Vulkan's y runs down), the guest viewport is in the shader
    const float W = float(rt ? rt->w : ds->w), H = float(rt ? rt->h : ds->h);
    VkViewport vp{0, H, W, -H, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    int32_t x0 = int32_t(s.viewport.x), y0 = int32_t(s.viewport.y);
    int32_t x1 = int32_t(s.viewport.x + s.viewport.w), y1 = int32_t(s.viewport.y + s.viewport.h);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0]));
        y0 = std::max(y0, int32_t(s.scissors[1]));
        x1 = std::min(x1, int32_t(s.scissors[2]));
        y1 = std::min(y1, int32_t(s.scissors[3]));
    }
    {
        const VkTarget* tg = rt ? rt : ds;  // the rect is in console pixels, the target in host pixels
        const float kx = float(tg->w) / float(tg->gw), ky = float(tg->h) / float(tg->gh);
        x0 = int32_t(x0 * kx);
        y0 = int32_t(y0 * ky);
        x1 = int32_t(std::lround(x1 * kx));
        y1 = int32_t(std::lround(y1 * ky));
    }
    x0 = std::clamp(x0, 0, int32_t(W));
    y0 = std::clamp(y0, 0, int32_t(H));
    x1 = std::clamp(x1, x0, int32_t(W));
    y1 = std::clamp(y1, y0, int32_t(H));
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(cmd, bf);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);

    // descriptors: b0, b1, t0-t3, s0-s3
    VkDescriptorBufferInfo bi[2] = {{vsc.buffer, vsc.offset, sizeof(s.vsc)}, {dc.buffer, dc.offset, sizeof(DrawConstants)}};
    VkDescriptorImageInfo ii[4], si[4];
    VkDescriptorSet set = AllocSet();
    if (!set) {
        LOGE("vulkan: out of descriptor sets");
        return;
    }
    std::array<VkWriteDescriptorSet, 10> w{};
    for (uint32_t i = 0; i < 10; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (int i = 0; i < 2; i++) {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    for (int i = 0; i < 4; i++) {
        const bool depth_view = stages.info[i].shadow != 0;
        ii[i] = {VK_NULL_HANDLE, stages.view[i],
                 depth_view ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[2 + i].pImageInfo = &ii[i];
        si[i] = {Sampler(s, i, stages, false), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[6 + i].pImageInfo = &si[i];
    }
    vkUpdateDescriptorSets(dev_, uint32_t(w.size()), w.data(), 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);

    if (a.buffer_count)
        vkCmdBindVertexBuffers2_(cmd, 0, a.buffer_count, vbs.data(), offsets.data(), sizes.data(), strides.data());
    if (a.index_count) {
        vkCmdBindIndexBuffer(cmd, ib.buffer, ib.offset, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cmd, a.draw_count, 1, 0, 0);
    }
}

// ---- clear, copy, present -------------------------------------------------------------------------------------------

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    Begin();
    VkTarget* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    VkTarget* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;
    if ((flags & kClearTarget) && (flags & kClearTarget) != kClearTarget)
        LOG_FIRST(1, "vulkan: a clear of some colour channels clears all four");
    const VkTarget* any = rt ? rt : ds;
    const float kx = float(any->w) / float(any->gw), ky = float(any->h) / float(any->gh);  // console pixels to host
    std::vector<VkClearRect> rs;
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            r[0] = int32_t(r[0] * kx);
            r[1] = int32_t(r[1] * ky);
            r[2] = int32_t(std::lround(r[2] * kx));
            r[3] = int32_t(std::lround(r[3] * ky));
            int32_t x0 = std::clamp(r[0], 0, int32_t(any->w)), y0 = std::clamp(r[1], 0, int32_t(any->h));
            int32_t x1 = std::clamp(r[2], x0, int32_t(any->w)), y1 = std::clamp(r[3], y0, int32_t(any->h));
            rs.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
        }
    } else {
        const auto& v = s.viewport;
        int32_t x0 = std::clamp(int32_t(v.x * kx), 0, int32_t(any->w)), y0 = std::clamp(int32_t(v.y * ky), 0, int32_t(any->h));
        int32_t x1 = std::clamp(int32_t(std::lround((v.x + v.w) * kx)), x0, int32_t(any->w));
        int32_t y1 = std::clamp(int32_t(std::lround((v.y + v.h) * ky)), y0, int32_t(any->h));
        rs.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    std::vector<VkClearAttachment> at;
    if (rt) {
        VkClearAttachment c{};
        c.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.colorAttachment = 0;
        Color(color, c.clearValue.color.float32);
        at.push_back(c);
    }
    const bool together = rt && ds && rt->w == ds->w && rt->h == ds->h;
    if (ds) {
        VkClearAttachment c{};
        c.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                       ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        c.clearValue.depthStencil = {1 - std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        if (together) at.push_back(c);
    }
    if (together) {
        StartRendering(rt, ds);
        vkCmdClearAttachments(Current().cmd, uint32_t(at.size()), at.data(), uint32_t(rs.size()), rs.data());
        return;
    }
    if (rt) {
        StartRendering(rt, nullptr);
        vkCmdClearAttachments(Current().cmd, 1, &at[0], uint32_t(rs.size()), rs.data());
    }
    if (ds) {
        VkClearAttachment c{};
        c.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                       ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        c.clearValue.depthStencil = {1 - std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        StartRendering(nullptr, ds);
        vkCmdClearAttachments(Current().cmd, 1, &c, uint32_t(rs.size()), rs.data());
    }
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    Begin();
    const bool depth = Format(src.format).depth;
    VkTarget* st = EnsureTarget(src, depth);
    VkTarget* dt = EnsureTarget(dst, depth);
    if (!st || !dt) {
        CopySurfaceMemory(src, dst, count, src_rects, dst_points);
        return;
    }
    if (st == dt) return;
    const VkImageAspectFlags aspect =
        depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    Transition(st->image, aspect, st->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dt->image, aspect, dt->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const uint32_t n = count ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = 0, t = 0, r = int32_t(st->gw), b = int32_t(st->gh), dx = 0, dy = 0;  // console pixels
        if (count && src_rects) {
            uint32_t at = src_rects + i * 16;
            l = int32_t(kernel::R32(at));
            t = int32_t(kernel::R32(at + 4));
            r = int32_t(kernel::R32(at + 8));
            b = int32_t(kernel::R32(at + 12));
            dx = l;
            dy = t;
            if (dst_points) {
                dx = int32_t(kernel::R32(dst_points + i * 8));
                dy = int32_t(kernel::R32(dst_points + i * 8 + 4));
            }
        }
        {
            const float skx = float(st->w) / float(st->gw), sky = float(st->h) / float(st->gh);
            const float dkx = float(dt->w) / float(dt->gw), dky = float(dt->h) / float(dt->gh);
            l = int32_t(l * skx);
            t = int32_t(t * sky);
            r = int32_t(std::lround(r * skx));
            b = int32_t(std::lround(b * sky));
            dx = int32_t(dx * dkx);
            dy = int32_t(dy * dky);
        }
        l = std::clamp(l, 0, int32_t(st->w));
        t = std::clamp(t, 0, int32_t(st->h));
        r = std::clamp(r, l, int32_t(st->w));
        b = std::clamp(b, t, int32_t(st->h));
        dx = std::clamp(dx, 0, int32_t(dt->w));
        dy = std::clamp(dy, 0, int32_t(dt->h));
        const int32_t cw = std::min(r - l, int32_t(dt->w) - dx), ch = std::min(b - t, int32_t(dt->h) - dy);
        if (cw <= 0 || ch <= 0) continue;
        VkImageCopy region{};
        region.srcSubresource = {aspect, 0, 0, 1};
        region.dstSubresource = {aspect, 0, 0, 1};
        region.srcOffset = {l, t, 0};
        region.dstOffset = {dx, dy, 0};
        region.extent = {uint32_t(cw), uint32_t(ch), 1};
        vkCmdCopyImage(Current().cmd, st->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

// The render scale: the picture is drawn at the setting's percent of the screen's own pixels (100 is native), so a console
// pixel is sx_ by sy_ host pixels. A change remakes the targets at the new size; the title draws them again.
void VulkanBackend::ApplyScale() {
    const float aspect = DisplayAspect(swap_extent_.width, swap_extent_.height);
    const float vw = std::min(float(swap_extent_.width), float(swap_extent_.height) * aspect);
    const float vh = vw / aspect;
    const float p = float(g_resolution_percent.load()) / 100.0f;
    const float nx = vw * p / float(guest_w_), ny = vh * p / float(guest_h_);
    if (std::fabs(nx - sx_) < 1e-3f && std::fabs(ny - sy_) < 1e-3f) return;
    LOGI("vulkan: resolution %.0f%%: %.0fx%.0f for the %ux%u frame", p * 100, vw * p, vh * p, guest_w_, guest_h_);
    EndRendering();
    bound_rt_ = bound_ds_ = nullptr;
    for (auto& entry : targets_) ReleaseTarget(entry.second);
    targets_.clear();
    sx_ = nx;
    sy_ = ny;
}

// a second of frames: where the time went, in ms per frame, and what happened how often
void VulkanBackend::LogPerf() {
    const double el = std::chrono::duration<double>(PerfClock::now() - perf_start_).count();
    if (el < 2.0) return;
    const Perf& p = perf_;
    const double n = double(std::max<uint64_t>(p.frames, 1));
    LOGI("perf: %.1f fps | per frame: draw %.2f ms (bind %.2f, tex %.2f) shader %.2f pso %.2f | wait %.2f present %.2f | "
         "%.0f draws, %llu shaders %llu psos %llu texture uploads %llu flushes in %.1f s",
         double(p.frames) / el, p.draw_ms / n, p.bind_ms / n, p.tex_ms / n, p.compile_ms / n, p.pso_ms / n, p.wait_ms / n,
         p.present_ms / n, double(p.draws) / n, (unsigned long long)p.shaders, (unsigned long long)p.psos,
         (unsigned long long)p.texups, (unsigned long long)p.flushes, el);
    perf_ = Perf{};
    perf_start_ = PerfClock::now();
}

void VulkanBackend::Present(const Surface& front) {
    Begin();
    VkTarget* t = EnsureTarget(front, false);
    EndRendering();
    uint32_t img = 0;
    VkResult r;
    {
        PerfTimer t(perf_.wait_ms);  // the swapchain hands an image out at the display's pace
        r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, Current().acquired, VK_NULL_HANDLE, &img);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // the window changed size: the frame is skipped and the swapchain remade for the next one
        Submit(true);
        CreateSwapchain();
        Begin();
        presents_++;
        return;
    }
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkAcquireNextImageKHR failed (%d)", int(r));
        return;
    }
    VkCommandBuffer cmd = Current().cmd;
    VkImage dst = swap_images_[img];
    Transition(dst, VK_IMAGE_ASPECT_COLOR_BIT, swap_layouts_[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (t) {
        Transition(t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        // the console's picture is 16:9 (it reports a widescreen display, as BlitFrame in d3d12.cpp shows). On a wider
        // screen it grows with the screen up to kMaxFillAspect (fill), or stays 16:9 with bars (fit); narrower, bars
        // above and below
        const float aspect = DisplayAspect(swap_extent_.width, swap_extent_.height);
        const float vw = std::min(float(swap_extent_.width), float(swap_extent_.height) * aspect);
        const int32_t bw = int32_t(vw), bh = int32_t(vw / aspect);
        const int32_t ox = (int32_t(swap_extent_.width) - bw) / 2, oy = (int32_t(swap_extent_.height) - bh) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(t->w), int32_t(t->h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {ox, oy, 0};
        blit.dstOffsets[1] = {ox + bw, oy + bh, 1};
        vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
    }
    Transition(dst, VK_IMAGE_ASPECT_COLOR_BIT, swap_layouts_[img], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    Frame& f = Current();
    vkEndCommandBuffer(f.cmd);
    recording_ = false;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &swap_done_[img];
    vkResetFences(dev_, 1, &f.fence);
    vkQueueSubmit(queue_, 1, &si, f.fence);
    f.submitted = true;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &swap_done_[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &img;
    {
        PerfTimer t(perf_.present_ms);
        vkQueuePresentKHR(queue_, &pi);
    }

    presents_++;
    perf_.frames++;
    NextFrame();
    ApplyScale();
    LogPerf();
}

}  // namespace

bool g_vulkan_reversed_depth = (g_reversed_depth = true);

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VulkanBackend>(); }

void SetAndroidWindow(ANativeWindow* window) { g_window = window; }
void SetDisplayFill(bool fill) { g_fill = fill; }
void SetResolutionPercent(uint32_t percent) { g_resolution_percent = std::clamp(percent, 10u, 200u); }
float DisplayAspect(uint32_t width, uint32_t height) {
    const float screen = height ? float(width) / float(height) : 16.0f / 9.0f;
    return g_fill ? std::clamp(screen, 16.0f / 9.0f, kMaxFillAspect) : 16.0f / 9.0f;
}
ANativeWindow* GetAndroidWindow() { return g_window; }

}  // namespace gpu

#endif  // __ANDROID__
