#include "frame_rate.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>

REXCVAR_DEFINE_STRING(sg_frame_rate, "60", "Sonic Generations",
    "Frame rate: 60, vsync, or original for the console's 30. 60 presents without waiting for "
    "a vblank and framerate_limit caps it at 60; vsync presents on every vblank (the Xenia "
    "\"60 FPS\" game patch), so a frame that runs a little long waits for the next one.")
    .allowed({"60", "vsync", "original"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Negative leaves the game's own presentation interval in r11. Set once on the
// startup thread, before any guest thread exists.
int64_t interval_override = -1;

}  // namespace

void ConfigureFrameRate() {
  const auto mode = rex::cvar::GetFlagByName("sg_frame_rate");
  // Interval 0 presents at once. With interval 1, a frame that takes a little
  // over 16.7 ms waits for the next vblank on the GPU thread while the game's
  // thread waits on the GPU: Green Hill held 42 FPS on an A18 Pro iPhone with
  // both threads idle a third of the time. framerate_limit (60) caps it instead.
  if (mode == "original") {
    interval_override = -1;
  } else {
    interval_override = mode == "vsync" ? 1 : 0;
  }
  REXLOG_INFO("Frame rate: {}", mode == "original" ? "original (30)"
                                : mode == "vsync" ? "60, vsync"
                                                  : "60, capped by framerate_limit");
}

void SgPresentationInterval(PPCRegister& r11) {
  if (interval_override >= 0) {
    r11.u64 = uint64_t(interval_override);
  }
}
