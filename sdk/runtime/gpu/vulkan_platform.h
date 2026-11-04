#pragma once
// What the Android app hands the Vulkan backend before the game boots.
#include <cstdint>

struct ANativeWindow;

namespace gpu {

// the surface the backend presents to; call before the title starts, and again with null when it goes away
void SetAndroidWindow(ANativeWindow* window);
ANativeWindow* GetAndroidWindow();

// true: the picture fills a wide screen (16:9 up to about 21:9, stretched sideways as far as the screen needs); false: it
// stays 16:9 with bars at the sides
void SetDisplayFill(bool fill);

// the aspect ratio of the picture on a screen of this size: 16:9, or in fill mode the screen's own up to about 21:9. The renderer
// shapes the picture by it and the game is told the same (src/game/hooks.cpp), so the picture is drawn at that aspect
float DisplayAspect(uint32_t width, uint32_t height);

// the resolution the picture is rendered at, as a percent of the screen's own pixels: 100 is native, less is faster; takes
// effect within a frame
void SetResolutionPercent(uint32_t percent);

}  // namespace gpu
