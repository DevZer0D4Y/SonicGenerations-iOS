// sonicgenerations - ReXGlue Recompiled Project (iOS)
//
// Sonic Generations (Xbox 360) on the iOS port of ReXGlue, from Player124413's
// Sonic-Generations-recomp-android-and-pc-edition. This class adds what iOS
// needs: paths inside the app container, the disc image picker, the
// HedgeModManager mod loader (src/mods) and its startup screen, and the
// settings that suit an iPhone GPU through MoltenVK (from the Simpsons, NFSMW,
// MC360 and Superstars Tennis ports of this SDK).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include "frame_rate.h"
#include "mods/mod_loader.h"
#if REX_PLATFORM_IOS
#include "iso_import.h"
#include "mod_manager_ios.h"
#include <rex/chrono/clock.h>
#include <rex/audio/audio_system.h>
#include <rex/input/touch/touch_controls.h>
#include <rex/ui/windowed_app_context_sdl.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if REX_PLATFORM_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <rex/cvar_defaults.h>
#include <rex/filesystem/devices/disc_image_device.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#endif

class SonicGenerationsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SonicGenerationsApp>(new SonicGenerationsApp(ctx, "sonicgenerations", PPCImageConfig));
  }

 protected:
#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
  // macOS: the same port running natively, used to test the iPhone build's
  // code paths (Metal through MoltenVK, arm64, 16 KB pages) on a Mac. The game
  // folder comes from --game_data_root as on desktop.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    SetDefaults();
    mod_root_ = paths.user_data_root;
    mod_cache_ = paths.cache_root / "mods";
  }

  void OnPostSetup() override {
    SetGuestFrameStats([this] { return SampleFrame(); });
  }

  void OnPreLaunchModule() override {
    ConfigureFrameRate();
    LoadMods();
  }
#endif

#if REX_PLATFORM_IOS
  // Before the game starts: the disc image picker if there is no game yet.
  // Mods are managed from the MODS button of the touch controls (OnPostSetup).
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    std::error_code ec;
    if (std::filesystem::is_regular_file(defaults.game_data_root, ec) ||
        HasDefaultXex(defaults.game_data_root)) {
      return defaults;
    }
    SgImportISO([defaults, resume = std::move(resume)](std::filesystem::path image) mutable {
      auto paths = defaults;
      paths.game_data_root = std::move(image);
      resume(std::move(paths));
    });
    return std::nullopt;
  }

  // iOS starts the app with no arguments and its bundle is read-only, so
  // everything lives in <container>/Documents, which the Files app shows
  // (UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace):
  //
  //   Documents/game/          the extracted disc (default.xex, disk/*.cpk, movie/)
  //   Documents/mods/          HedgeModManager mod folders (each with mod.ini)
  //   Documents/ios-mods.ini   which mods are enabled, and their order
  //   Documents/userdata/      saves and the profile
  //   Documents/cache/         shader cache, merged mod archives
  //   Documents/sonicgenerations.toml  settings, editable from Files
  //
  // Defaults go through SetDefaultByName: it ranks below the config file and,
  // unlike SetFlagByName, reaches cvars owned by the GPU plugin, which only
  // registers after this hook runs.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto docs = Documents();
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      paths.game_data_root = docs / "game";
    }
    paths.user_data_root = docs / "userdata";
    paths.cache_root = docs / "cache";
    paths.config_path = docs / "sonicgenerations.toml";
    for (const auto& dir : {paths.game_data_root, paths.user_data_root, paths.cache_root}) {
      std::filesystem::create_directories(dir, ec);
    }
    RunCleanupRequest(docs, paths.game_data_root);
    WriteDefaultConfig(paths.config_path);
    SetDefaults();
    mod_root_ = docs;
    mod_cache_ = paths.cache_root / "mods";
    // Files app import: prefer an explicitly named image over extracted data.
    for (const auto& image : {docs / "game.iso", docs / "game.xiso", docs / "game" / "game.iso"}) {
      if (std::filesystem::is_regular_file(image, ec)) {
        paths.game_data_root = image;
        break;
      }
    }
    REXLOG_INFO("iOS paths: game={} config={}", paths.game_data_root.string(),
                paths.config_path.string());
  }

  void OnPreLaunchModule() override {
    ConfigureFrameRate();
    LoadMods();
  }

  void OnPostSetup() override {
    static_cast<rex::ui::SDLWindowedAppContext&>(app_context()).SetLifecycleCallback(
        [this](bool backgrounded) {
          if (backgrounded) {
            static_cast<rex::audio::AudioSystem*>(runtime()->audio_system())->SetBackgrounded(true);
            rex::chrono::Clock::SetPaused(true);
            runtime()->graphics_system()->SetBackgrounded(true);
            REXLOG_INFO("iOS: gameplay suspended; session retained");
            rex::FlushLogging();
          } else {
            rex::chrono::Clock::SetPaused(false);
            static_cast<rex::audio::AudioSystem*>(runtime()->audio_system())->SetBackgrounded(false);
            runtime()->graphics_system()->SetBackgrounded(false);
            has_last_frame_ = false;
            REXLOG_INFO("iOS: gameplay resumed in the existing session");
          }
        });
    // MODS, next to the touch controls' EDIT: the game is paused while the
    // mod manager is open; the selection applies on the next launch.
    rex::input::touch::SetTouchMenuButton("MODS", [this] {
      rex::chrono::Clock::SetPaused(true);
      static_cast<rex::audio::AudioSystem*>(runtime()->audio_system())->SetBackgrounded(true);
      SgRunModManager(mod_root_, [this] {
        static_cast<rex::audio::AudioSystem*>(runtime()->audio_system())->SetBackgrounded(false);
        rex::chrono::Clock::SetPaused(false);
      }, true);
    });
    // "Guest: N FPS" in the F3 overlay; the SDK only shows it once an app
    // provides frame timing.
    SetGuestFrameStats([this] { return SampleFrame(); });
  }

  void OnShutdown() override {
    static_cast<rex::ui::SDLWindowedAppContext&>(app_context()).SetLifecycleCallback({});
  }
#endif

 private:
  // The VFS is mounted and the game has not opened anything yet.
  void LoadMods() {
    if (mod_root_.empty() || !runtime() || !runtime()->file_system()) {
      return;
    }
    const auto summary = sg::mods::ApplyMods(*runtime()->file_system(), mod_root_, mod_cache_);
    if (summary.mods) {
      REXLOG_INFO("Mods: {} enabled, {} files replaced or added", summary.mods, summary.files);
    }
  }

  std::filesystem::path mod_root_;
  std::filesystem::path mod_cache_;

#if REX_PLATFORM_IOS
  // The disc names it Default.xex, and the phone's file system is
  // case-sensitive; the VFS itself ignores case.
  static bool HasDefaultXex(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      auto name = entry.path().filename().string();
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (name == "default.xex" && entry.is_regular_file(ec)) {
        return true;
      }
    }
    return false;
  }

  static std::filesystem::path Documents() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && home[0] ? home : ".") / "Documents";
  }

  // Documents/cleanup.txt: game data paths to delete, one per line, relative to
  // Documents/game (for example "resource/Movie"). Without USB the phone's
  // files can only be added from a computer, not removed, so a computer places
  // this list and the next launch applies it, once. Nothing outside the game
  // folder can be named.
  static void RunCleanupRequest(const std::filesystem::path& docs,
                                const std::filesystem::path& game_root) {
    const auto request = docs / "cleanup.txt";
    std::error_code ec;
    if (!std::filesystem::exists(request, ec)) {
      return;
    }
    std::ifstream in(request);
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      const std::filesystem::path relative(line);
      if (line.empty() || line[0] == '#' || relative.is_absolute() ||
          std::any_of(relative.begin(), relative.end(),
                      [](const std::filesystem::path& part) { return part == ".."; })) {
        continue;
      }
      const auto target = game_root / relative;
      const auto removed = std::filesystem::remove_all(target, ec);
      REXLOG_INFO("cleanup.txt: removed {} ({} entries){}", target.string(),
                  ec ? 0 : removed, ec ? " - " + ec.message() : "");
    }
    in.close();
    std::filesystem::remove(request, ec);
  }

#endif

#if REX_PLATFORM_MAC
#if REX_PLATFORM_IOS
  static void WriteDefaultConfig(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
      return;
    }
    std::ofstream out(path);
    if (!out) {
      return;
    }
    out << "# Sonic Generations settings. Edit in the Files app; takes effect on relaunch.\n"
           "gpu_plugin = \"xenos\"\n"
           "\n"
           "# The game renders 16:9 and the panel is wider.\n"
           "#   false -> stretch to fill, true -> keep the aspect with side bars\n"
           "present_letterbox = false\n"
           "\n"
           "# On-screen gamepad: \"auto\" shows it while no controller is connected,\n"
           "# \"on\" always, \"off\" never. touch_controls_opacity (0.1 to 1.0) dims it.\n"
           "touch_controls = \"auto\"\n"
           "\n"
           "# \"60\", \"vsync\" (60 locked to the display refresh) or \"original\"\n"
           "# (the console's 30 FPS).\n"
           "sg_frame_rate = \"60\"\n";
  }

#endif

  // The disc has English, Japanese, French, German, Spanish and Italian text
  // and speech: default the console language to the device's own when it is
  // one of them. user_language in sonicgenerations.toml still overrides it.
  static void SetLanguageDefault() {
    // Xbox 360 XC_LANGUAGE_* values.
    static const std::pair<const char*, int> kLanguages[] = {
        {"en", 1}, {"ja", 2}, {"de", 3}, {"fr", 4}, {"es", 5}, {"it", 6},
    };
    CFArrayRef preferred = CFLocaleCopyPreferredLanguages();
    if (!preferred) {
      return;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(preferred); ++i) {
      char code[16] = {};
      auto language = static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferred, i));
      if (!CFStringGetCString(language, code, sizeof(code), kCFStringEncodingUTF8)) {
        continue;
      }
      const std::string two(code, std::min<size_t>(2, std::strlen(code)));
      const auto match = std::find_if(std::begin(kLanguages), std::end(kLanguages),
                                      [&](const auto& entry) { return two == entry.first; });
      if (match != std::end(kLanguages)) {
        rex::cvar::SetDefaultByName("user_language", std::to_string(match->second));
        REXLOG_INFO("Game language: {} (user_language {})", match->first, match->second);
        break;
      }
    }
    CFRelease(preferred);
  }

  static void SetDefaults() {
    using rex::cvar::SetDefaultByName;
    // The Xenos GPU emulation; empty would disable graphics entirely.
    SetDefaultByName("gpu_plugin", "xenos");
    SetDefaultByName("vulkan_log_debug_messages", "false");
    SetDefaultByName("present_letterbox", "false");
    // Some titles ask for the profile on the controller's own port and, with
    // only slot 0 signed in, refuse to save; every slot aliases the one local
    // profile.
    SetDefaultByName("signin_all_user_slots", "true");
    // Upstream's PC build returns from calls through function pointers the
    // recompiler did not see instead of aborting; each target is logged once.
    SetDefaultByName("tolerant_indirect_calls", "true");

    // MoltenVK has no fragment-input coverage mask, and emulating the EDRAM in
    // the pixel shader is slow on this GPU: host render targets let the
    // hardware do coverage and blending.
    SetDefaultByName("fsi_sample_mask_from_sample_id", "true");
    SetDefaultByName("render_target_path_vulkan", "fbo");

    // Performance defaults proven on the MC360 and NFSMW ports of this SDK.
    //
    // Marking every uploaded page stale each frame re-copied all of guest
    // memory the GPU reads, every frame.
    SetDefaultByName("clear_memory_page_state", "false");
    // One queue submission per frame, not one per command buffer end.
    SetDefaultByName("vulkan_submit_on_primary_buffer_end", "false");
    // FIFO only: vsync-locked, evenly paced frames.
    SetDefaultByName("vulkan_allow_present_mode_immediate", "false");
    SetDefaultByName("vulkan_allow_present_mode_mailbox", "false");
    SetDefaultByName("vulkan_allow_present_mode_fifo_relaxed", "false");
    SetDefaultByName("framerate_limit", "60");
#if REX_PLATFORM_IOS
    SetDefaultByName("guest_vblank_rate", "60");
    SetDefaultByName("clock_no_scaling", "false");
#endif
    // Clear rectangles drawn with Direct3D's unbounded 8192 scissor: find their
    // real extent by running the vertex shader on the CPU, as Xenia does.
    SetDefaultByName("execute_unclipped_draw_vs_on_cpu", "true");
    // Apple's tile-based GPU loads and stores the whole render area of every
    // render pass: upload rewritten guest pages between passes, and size the
    // passes to what their draws write.
    SetDefaultByName("vulkan_batch_dirty_uploads", "true");
    SetDefaultByName("vulkan_tight_render_area", "true");
    // Unified memory: write CPU-modified pages straight into the GPU's buffer
    // when no queued work reads them, and move the rest to the start of the
    // submission instead of a copy that ends the render pass (36 -> 25 pass
    // ends per frame in the first level; both from the NFSMW port).
    SetDefaultByName("vulkan_shared_memory_direct_upload", "true");
    SetDefaultByName("vulkan_defer_cpu_uploads", "true");
    // Room for scheduling hiccups on a busy or warm phone.
    SetDefaultByName("audio_maxqframes", "16");
    // A CPU write to memory the GPU reads invalidates only its own 16 KB page,
    // not the surrounding 1 MB: the game's heap mixes CPU data with static
    // vertex buffers and textures, and the widening re-uploaded ~550 MB/s and
    // re-decoded every texture each frame (51 -> 60 FPS on an M1, uploads
    // 550 -> 85 MB/s, the write faults cost ~3% of the game thread).
    SetDefaultByName("shared_memory_invalidation_widen_kb", "16");
    // Clears through 4x MSAA views of single-sampled targets share one host
    // target instead of copying on every switch (Superstars Tennis port).
    SetDefaultByName("vulkan_msaa_as_single_sample", "true");
#if REX_PLATFORM_IOS
    // iOS pulls 1024 samples per audio request; call the game's mixer once per
    // 256-sample frame as the console does instead of four times back to back
    // (choppy audio in the NFSMW port).
    SetDefaultByName("audio_pace_callbacks", "true");
    // MoltenVK otherwise encodes each submission's Metal commands inside
    // vkQueueSubmit on the GPU command thread: 23% of that thread in Green Hill
    // (~5.5 ms a frame), enough to push frames past a vblank (42 FPS).
    SetDefaultByName("vulkan_moltenvk_async_submits", "true");
#endif
    SetLanguageDefault();
  }

  rex::ui::FrameStats SampleFrame() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const double dt_ms = std::chrono::duration<double, std::milli>(now - last_frame_).count();
    // Skip the first interval and any absurd one: after the overlay reopens,
    // "previous" covers the whole time it was closed.
    const bool valid = has_last_frame_ && dt_ms > 0.0 && dt_ms < 1000.0;
    last_frame_ = now;
    has_last_frame_ = true;
    if (valid) {
      smoothed_ms_ = smoothed_ms_ <= 0.0 ? dt_ms : smoothed_ms_ * 0.9 + dt_ms * 0.1;
      stats_.frame_time_ms = smoothed_ms_;
      stats_.fps = smoothed_ms_ > 0.0 ? 1000.0 / smoothed_ms_ : 0.0;
      stats_.frame_count = ++frame_count_;
    }
    return stats_;
  }

  rex::ui::FrameStats stats_{};
  std::chrono::steady_clock::time_point last_frame_{};
  double smoothed_ms_ = 0.0;
  uint64_t frame_count_ = 0;
  bool has_last_frame_ = false;
#endif
};
