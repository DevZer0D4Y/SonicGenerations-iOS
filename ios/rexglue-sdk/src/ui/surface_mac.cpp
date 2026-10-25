/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Rien Gupta, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/surface_mac.h>

#include <rex/platform.h>

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_video.h>

#if REX_PLATFORM_IOS
#include <CoreFoundation/CoreFoundation.h>

#include <utility>
#endif

namespace rex {
namespace ui {

#if REX_PLATFORM_IOS
namespace {
// Whether Info.plist allows only landscape orientations.
bool LandscapeOnly() {
  static const bool landscape_only = [] {
    CFBundleRef bundle = CFBundleGetMainBundle();
    auto orientations = bundle ? static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(
                                     bundle, CFSTR("UISupportedInterfaceOrientations")))
                               : nullptr;
    if (!orientations || CFGetTypeID(orientations) != CFArrayGetTypeID() ||
        !CFArrayGetCount(orientations)) {
      return false;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(orientations); ++i) {
      auto orientation = static_cast<CFStringRef>(CFArrayGetValueAtIndex(orientations, i));
      if (CFGetTypeID(orientation) != CFStringGetTypeID() ||
          !CFStringHasPrefix(orientation, CFSTR("UIInterfaceOrientationLandscape"))) {
        return false;
      }
    }
    return true;
  }();
  return landscape_only;
}
}  // namespace
#endif

CAMetalLayerSurface::~CAMetalLayerSurface() {
  if (metal_view_) {
    SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
    metal_view_ = nullptr;
  }
}

bool CAMetalLayerSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)) {
    return false;
  }
#if REX_PLATFORM_IOS
  // While the app is in the background (Notification Center, Control Center,
  // the app switcher, a screenshot's preview) iOS lays the window out in
  // portrait for its snapshot. A swap chain made for that size stayed after
  // returning: the landscape picture squeezed into 1290x2796. A landscape-only
  // app is never really portrait.
  if (LandscapeOnly() && pixel_width < pixel_height) {
    std::swap(pixel_width, pixel_height);
  }
#endif
  width_out = uint32_t(pixel_width);
  height_out = uint32_t(pixel_height);
  return true;
}

}  // namespace ui
}  // namespace rex
