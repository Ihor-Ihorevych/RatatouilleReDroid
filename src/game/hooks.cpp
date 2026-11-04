// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
#include <cstdint>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

#ifdef __ANDROID__
#include "gpu/vulkan_platform.h"
#endif

namespace game {

// Widescreen at the screen's own shape. The game keeps its display aspect in one float (0x32C0A0), set from two constants:
// 4:3 (0x2F6694) and 16:9 (0x2FC3C4), the second when its widescreen option is on. The camera code multiplies that aspect
// into its horizontal projection and divides it by the frame buffer's own, so a larger aspect is a wider view. Everything
// that reads the 16:9 constant is aspect logic (the setters at 0xED312, 0xED8C5 and 0x221325, the checks at 0x1457C8,
// 0x1897BC and 0x20AE2E), so changing the constant's value changes the aspect and keeps those checks agreeing with it.
// The renderer shows the 640x480 frame stretched over the whole picture, which is the anamorphic frame the game draws.
void Loaded() {
#ifdef __ANDROID__
    constexpr uint32_t kWidescreenAspect = 0x002FC3C4;
    app::Mode screen = app::Output();
    const float aspect = gpu::DisplayAspect(screen.width, screen.height);
    uint32_t bits;
    std::memcpy(&bits, &aspect, sizeof(bits));
    kernel::W32(kWidescreenAspect, bits);
    LOGI("game: widescreen aspect %.3f for a %ux%u screen", aspect, screen.width, screen.height);
#endif
}

}  // namespace game
