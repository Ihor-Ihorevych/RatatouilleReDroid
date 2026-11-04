// Compiles generated HLSL through spirv::Compile: does glslang's HLSL front end take what shaders.cpp writes?
#include <cstdio>
#include <string>
#include "gpu/shaders.h"
#include "gpu/spirv.h"
using namespace gpu;

static int g_fail = 0, g_ok = 0;
static void Try(const char* name, const std::string& hlsl, spirv::Stage st) {
    std::vector<uint32_t> spv;
    std::string err;
    if (spirv::Compile(hlsl, "main", st, spv, err)) {
        g_ok++;
    } else {
        g_fail++;
        std::printf("FAIL %s\n%.1500s\n", name, err.c_str());
        if (g_fail == 1) std::printf("---- source ----\n%s\n", hlsl.c_str());
    }
}

int main() {
    // fixed function vertex shaders: untransformed with position+normal+uv, lit and unlit; pretransformed
    FixedKey lit;
    lit.light = 1 | 2;
    lit.types = 1;
    for (uint16_t regs : {uint16_t(0x1), uint16_t(0x5), uint16_t(0x1 | 0x8 | 0x100), uint16_t(0x1 | 0x2 | 0x8 | 0x100 | 0x200)}) {
        Try("fixed-vs", FixedVertexHlsl(regs, false, 0, {}), spirv::Stage::Vertex);
        Try("fixed-vs-lit", FixedVertexHlsl(regs, false, 0, lit), spirv::Stage::Vertex);
        Try("fixed-vs-pretransformed", FixedVertexHlsl(regs, true, 0, {}), spirv::Stage::Vertex);
        Try("fixed-vs-reversed", ReversedDepthHlsl(FixedVertexHlsl(regs, false, 0, lit)), spirv::Stage::Vertex);
        Try("fixed-vs-quad", QuadHlsl(ReversedDepthHlsl(FixedVertexHlsl(regs, true, 0, {}))), spirv::Stage::Vertex);
    }
    // pixel shaders: fixed function stages with 2D textures of each kind
    for (int kind = 0; kind <= 3; kind++) {
        State s;
        TexInfo tex[4];
        for (auto& t : tex) t.kind = uint8_t(kind);
        s.textures[0] = 0;
        PixelKey k = MakePixelKey(s, tex);
        Try("ps-default", PixelHlsl(k), spirv::Stage::Pixel);
        k.quad = 1;
        Try("ps-quad", PixelHlsl(k), spirv::Stage::Pixel);
        k.quad = 0;
        k.alphatest = 1;
        Try("ps-alphatest", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].flags |= kTexTarget;
        Try("ps-target", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].shadow = 1;
        Try("ps-shadow", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].shadow = 0;
        k.ff[0][0] = 4;  // some colour op
        Try("ps-ff-op4", PixelHlsl(k), spirv::Stage::Pixel);
    }
    std::printf("compiled %d, failed %d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
