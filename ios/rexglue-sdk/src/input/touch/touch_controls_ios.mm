/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - The on-screen gamepad of the Unleashed Recompiled iOS
 *              port (ui/touch_controls.cpp), whose layout and tuning come
 *              from the XeniOS defaults (github.com/xenios-jp/XeniOS): a
 *              movement stick with a D-pad ring, swipe-to-look anywhere that
 *              isn't a control, pill-shaped buttons, and an EDIT button that
 *              opens a layout editor (move, resize, hide, opacity, reset).
 *              Drawn with UIKit over SDL's view instead of ImGui.
 */

#include <rex/input/touch/touch_controls.h>

#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/input/touch/touch_input_resolver.h>
#include <rex/input/touch/touch_layout.h>
#include <rex/logging.h>
#include <rex/ui/window_sdl.h>

#if !__has_feature(objc_arc)
#error "touch_controls_ios.mm must be compiled with -fobjc-arc"
#endif

namespace touch = rex::input::touch;

namespace {

constexpr float kStickDeadzone = 0.14f;
constexpr float kStickActivationRadius = 0.48f;
constexpr float kStickDpadRingRadius = 0.32f;

// Horizontal margin kept free on wide screens so controls stay clear of the
// notch and rounded corners.
constexpr float kWideScreenSafeMargin = 0.055f;

// Layout editor limits, in layout space.
constexpr float kEditMinSize = 0.04f;
constexpr float kEditMaxSize = 0.45f;
constexpr float kEditSizeStep = 1.1f;
constexpr float kEditOpacityStep = 0.1f;
constexpr float kEditMinOpacity = 0.1f;

NSString* const kLayoutDefaultsKey = @"RexTouchLayout";
NSString* const kOpacityDefaultsKey = @"RexTouchOpacity";

enum class ControlKind { kStick, kButton };

struct ControlDef {
  ControlKind kind;
  const char* id;
  const char* label;
  touch::TouchRect frame;  // Default frame, normalized to the layout space.
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
};

// The XeniOS default layout, with its actions mapped to their gamepad equivalents.
const ControlDef kControls[] = {
    {ControlKind::kStick, "stick", nullptr, {0.055f, 0.560f, 0.190f, 0.315f}, 0, 0, 0},
    {ControlKind::kButton, "back", "BACK", {0.390f, 0.045f, 0.080f, 0.112f}, rex::input::X_INPUT_GAMEPAD_BACK, 0, 0},
    {ControlKind::kButton, "start", "START", {0.495f, 0.045f, 0.085f, 0.112f}, rex::input::X_INPUT_GAMEPAD_START, 0, 0},
    {ControlKind::kButton, "lb", "LB", {0.660f, 0.050f, 0.085f, 0.112f}, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0},
    {ControlKind::kButton, "rb", "RB", {0.765f, 0.050f, 0.085f, 0.112f}, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0},
    {ControlKind::kButton, "lt", "LT", {0.095f, 0.405f, 0.120f, 0.110f}, 0, 0xFF, 0},
    {ControlKind::kButton, "rt", "RT", {0.860f, 0.405f, 0.120f, 0.110f}, 0, 0, 0xFF},
    {ControlKind::kButton, "y", "Y", {0.760f, 0.455f, 0.065f, 0.115f}, rex::input::X_INPUT_GAMEPAD_Y, 0, 0},
    {ControlKind::kButton, "x", "X", {0.700f, 0.585f, 0.065f, 0.115f}, rex::input::X_INPUT_GAMEPAD_X, 0, 0},
    {ControlKind::kButton, "b", "B", {0.820f, 0.585f, 0.065f, 0.115f}, rex::input::X_INPUT_GAMEPAD_B, 0, 0},
    {ControlKind::kButton, "a", "A", {0.760f, 0.715f, 0.065f, 0.115f}, rex::input::X_INPUT_GAMEPAD_A, 0, 0},
};
constexpr size_t kControlCount = std::size(kControls);

// The button that opens the layout editor. It isn't part of the editable layout.
constexpr touch::TouchRect kEditButtonFrame = {0.290f, 0.045f, 0.075f, 0.112f};
// The app's menu button (SetTouchMenuButton), left of EDIT.
constexpr touch::TouchRect kMenuButtonFrame = {0.195f, 0.045f, 0.075f, 0.112f};

enum class EditorAction { kSmaller, kBigger, kToggleHidden, kLessOpaque, kMoreOpaque, kReset, kDone };

struct EditorButton {
  EditorAction action;
  const char* label;
};

const EditorButton kEditorButtons[] = {
    {EditorAction::kSmaller, "SIZE -"},      {EditorAction::kBigger, "SIZE +"},
    {EditorAction::kToggleHidden, "HIDE"},   {EditorAction::kLessOpaque, "ALPHA -"},
    {EditorAction::kMoreOpaque, "ALPHA +"},  {EditorAction::kReset, "RESET"},
    {EditorAction::kDone, "DONE"},
};
constexpr size_t kEditorButtonCount = std::size(kEditorButtons);

// Capture targets besides the layout's controls.
constexpr int kLookZone = -1;
constexpr int kEditButton = -2;
constexpr int kEditorToolbar = -3;
constexpr int kEditorEmpty = -4;
constexpr int kMenuButton = -5;

// Set by SetTouchMenuButton, read by the overlay; both on the UI thread.
std::string g_menu_label;
std::function<void()> g_menu_action;

enum class StickZone { kStick, kDpadUp, kDpadDown, kDpadLeft, kDpadRight };

struct LayoutEntry {
  touch::TouchRect frame;
  bool hidden;
};

struct Capture {
  // Compared, never messaged: the capture is dropped when its touch ends.
  __unsafe_unretained UITouch* touch = nil;
  int control = kLookZone;
  StickZone stick_zone = StickZone::kStick;
  CGPoint anchor = CGPointZero;
  CGPoint current = CGPointZero;
  touch::TouchRect frame_at_anchor{};  // Layout space, used when dragging in the editor.
};

float LookPointsPerFullScale() {
  return std::clamp(static_cast<float>(REXCVAR_GET(touch_look_points_per_full_scale)), 1.0f,
                    64.0f);
}

float LookVerticalScale() {
  return std::clamp(static_cast<float>(REXCVAR_GET(touch_look_vertical_scale)), 0.25f, 4.0f);
}

double LookHoldSeconds() {
  return std::clamp(REXCVAR_GET(touch_look_hold_seconds), 0.016, 0.25);
}

double ButtonTapHoldSeconds() {
  return std::clamp(REXCVAR_GET(touch_button_tap_hold_seconds), 0.016, 0.25);
}

CGPoint Centre(const touch::TouchRect& rect) {
  return CGPointMake(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
}

float Radius(const touch::TouchRect& rect) {
  return std::min(rect.width, rect.height) * 0.5f;
}

bool RectContains(const touch::TouchRect& rect, CGPoint point) {
  return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y &&
         point.y <= rect.y + rect.height;
}

// Buttons are pills: circles, stretched along their longer side.
bool PillContains(const touch::TouchRect& frame, CGPoint point) {
  const float radius = Radius(frame);
  const CGPoint centre = Centre(frame);
  const float half_span_x = std::max(frame.width * 0.5f - radius, 0.0f);
  const float half_span_y = std::max(frame.height * 0.5f - radius, 0.0f);
  const float dx = std::max(float(std::abs(point.x - centre.x)) - half_span_x, 0.0f);
  const float dy = std::max(float(std::abs(point.y - centre.y)) - half_span_y, 0.0f);
  return dx * dx + dy * dy <= radius * radius;
}

CGRect ToCGRect(const touch::TouchRect& rect) {
  return CGRectMake(rect.x, rect.y, rect.width, rect.height);
}

int16_t ToAxis(float value) {
  return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

void MergeAxis(int16_t& target, int16_t value) {
  if (std::abs(int(value)) > std::abs(int(target))) {
    target = value;
  }
}

void ClampToLayout(touch::TouchRect& frame) {
  frame.width = std::clamp(frame.width, kEditMinSize, kEditMaxSize);
  frame.height = std::clamp(frame.height, kEditMinSize, kEditMaxSize);
  frame.x = std::clamp(frame.x, 0.0f, 1.0f - frame.width);
  frame.y = std::clamp(frame.y, 0.0f, 1.0f - frame.height);
}

bool HasConnectedGameController() {
  for (GCController* controller in GCController.controllers) {
    if (controller.extendedGamepad || controller.microGamepad) {
      return true;
    }
  }
  return false;
}

}  // namespace

@interface RexTouchControlsOverlayView : UIView
- (instancetype)initWithRuntimeModel:(touch::TouchRuntimeModel*)runtime_model
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithFrame:(CGRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (void)setGameplayOverlayVisible:(BOOL)visible animated:(BOOL)animated;
// Off while a system dialog is up, so taps outside the controls reach it.
- (void)setLookZoneEnabled:(BOOL)enabled;
- (void)shutdown;
@end

@implementation RexTouchControlsOverlayView {
  touch::TouchRuntimeModel* runtime_model_;
  CADisplayLink* display_link_;
  std::array<LayoutEntry, kControlCount> layout_;
  std::array<double, kControlCount> button_press_times_;
  std::vector<Capture> captures_;
  CGPoint look_vector_;
  double look_time_;
  float opacity_;
  int selected_control_;
  BOOL editing_;
  BOOL gameplay_overlay_active_;
  BOOL look_zone_enabled_;
  touch::TouchResolvedState last_published_state_;
  uint32_t next_packet_number_;
  UIImpactFeedbackGenerator* haptic_press_;
  UIImpactFeedbackGenerator* haptic_press_light_;
}

- (instancetype)initWithRuntimeModel:(touch::TouchRuntimeModel*)runtime_model {
  if (!(self = [super initWithFrame:CGRectZero])) {
    return nil;
  }
  runtime_model_ = runtime_model;
  button_press_times_.fill(-1.0);
  look_time_ = -1.0;
  selected_control_ = -1;
  next_packet_number_ = 1;
  look_zone_enabled_ = YES;

  self.backgroundColor = UIColor.clearColor;
  self.opaque = NO;
  self.contentMode = UIViewContentModeRedraw;
  self.alpha = 0.0;
  self.hidden = YES;
  self.multipleTouchEnabled = YES;

  // Created up front so the first press does not pay generator setup latency.
  haptic_press_ = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
  haptic_press_light_ =
      [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];

  [self loadLayout];
  [self createDisplayLinkIfNeeded];
  return self;
}

#pragma mark Layout

- (void)resetLayout {
  for (size_t i = 0; i < kControlCount; ++i) {
    layout_[i] = {kControls[i].frame, false};
  }
}

- (void)loadLayout {
  [self resetLayout];
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  opacity_ = std::clamp(static_cast<float>(REXCVAR_GET(touch_controls_opacity)), kEditMinOpacity,
                        1.0f);
  if (NSNumber* opacity = [defaults objectForKey:kOpacityDefaultsKey]) {
    opacity_ = std::clamp(opacity.floatValue, kEditMinOpacity, 1.0f);
  }
  NSDictionary* saved = [defaults dictionaryForKey:kLayoutDefaultsKey];
  for (size_t i = 0; i < kControlCount; ++i) {
    NSDictionary* entry = saved[@(kControls[i].id)];
    if (![entry isKindOfClass:NSDictionary.class]) {
      continue;
    }
    auto read = [&](NSString* key, float fallback) {
      NSNumber* value = entry[key];
      return [value isKindOfClass:NSNumber.class] ? value.floatValue : fallback;
    };
    touch::TouchRect& frame = layout_[i].frame;
    frame.x = std::clamp(read(@"x", frame.x), 0.0f, 1.0f);
    frame.y = std::clamp(read(@"y", frame.y), 0.0f, 1.0f);
    frame.width = std::clamp(read(@"width", frame.width), kEditMinSize, kEditMaxSize);
    frame.height = std::clamp(read(@"height", frame.height), kEditMinSize, kEditMaxSize);
    layout_[i].hidden = read(@"hidden", 0.0f) != 0.0f;
  }
}

- (void)saveLayout {
  NSMutableDictionary* saved = [NSMutableDictionary dictionary];
  for (size_t i = 0; i < kControlCount; ++i) {
    const LayoutEntry& entry = layout_[i];
    saved[@(kControls[i].id)] = @{
      @"x" : @(entry.frame.x),
      @"y" : @(entry.frame.y),
      @"width" : @(entry.frame.width),
      @"height" : @(entry.frame.height),
      @"hidden" : @(entry.hidden ? 1 : 0),
    };
  }
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  [defaults setObject:saved forKey:kLayoutDefaultsKey];
  [defaults setObject:@(opacity_) forKey:kOpacityDefaultsKey];
}

- (touch::TouchRect)layoutSpace {
  const CGSize size = self.bounds.size;
  const float margin =
      size.height > 0 && (size.width / size.height) > 1.9f ? size.width * kWideScreenSafeMargin
                                                           : 0.0f;
  return {margin, 0.0f, float(size.width) - margin * 2.0f, float(size.height)};
}

// Resolves a frame in layout space to view points.
- (touch::TouchRect)toViewFrame:(const touch::TouchRect&)frame {
  const touch::TouchRect space = [self layoutSpace];
  return {space.x + frame.x * space.width, space.y + frame.y * space.height,
          frame.width * space.width, frame.height * space.height};
}

- (touch::TouchRect)controlFrame:(size_t)index {
  return [self toViewFrame:layout_[index].frame];
}

// The editor toolbar runs along the bottom of the screen.
- (touch::TouchRect)editorButtonFrame:(size_t)index {
  const touch::TouchRect space = [self layoutSpace];
  const float height = self.bounds.size.height;
  const float spacing = space.width * 0.01f;
  const float width =
      (space.width * 0.9f - spacing * float(kEditorButtonCount - 1)) / float(kEditorButtonCount);
  const float start_x = space.x + space.width * 0.05f;
  return {start_x + float(index) * (width + spacing), height * 0.86f, width, height * 0.11f};
}

- (bool)control:(size_t)index containsPoint:(CGPoint)point {
  const touch::TouchRect frame = [self controlFrame:index];
  return kControls[index].kind == ControlKind::kStick ? RectContains(frame, point)
                                                      : PillContains(frame, point);
}

- (int)controlAtPoint:(CGPoint)point includeHidden:(bool)include_hidden {
  // Buttons take priority over the stick.
  for (ControlKind kind : {ControlKind::kButton, ControlKind::kStick}) {
    for (size_t i = 0; i < kControlCount; ++i) {
      if (kControls[i].kind == kind && (include_hidden || !layout_[i].hidden) &&
          [self control:i containsPoint:point]) {
        return int(i);
      }
    }
  }
  return -1;
}

- (StickZone)stickZoneForFrame:(const touch::TouchRect&)frame point:(CGPoint)point {
  const CGPoint centre = Centre(frame);
  const float stick_radius = std::min(frame.width, frame.height) * kStickDpadRingRadius;
  const float dx = point.x - centre.x;
  const float dy = point.y - centre.y;
  if (dx * dx + dy * dy <= stick_radius * stick_radius) {
    return StickZone::kStick;
  }
  if (std::abs(dx) > std::abs(dy)) {
    return dx > 0.0f ? StickZone::kDpadRight : StickZone::kDpadLeft;
  }
  return dy > 0.0f ? StickZone::kDpadDown : StickZone::kDpadUp;
}

// The finger's offset from where it landed, limited to the stick's travel.
- (CGPoint)stickDeltaForFrame:(const touch::TouchRect&)frame capture:(const Capture&)capture {
  const float outer_radius = std::min(frame.width, frame.height) * kStickActivationRadius;
  CGPoint delta = CGPointMake(capture.current.x - capture.anchor.x,
                              capture.current.y - capture.anchor.y);
  const float distance = std::hypot(delta.x, delta.y);
  if (distance > outer_radius && distance > 0.0f) {
    delta.x *= outer_radius / distance;
    delta.y *= outer_radius / distance;
  }
  return delta;
}

// The stick deflection for a capture, with Y pointing down.
- (CGPoint)stickVectorForFrame:(const touch::TouchRect&)frame capture:(const Capture&)capture {
  const float outer_radius = std::min(frame.width, frame.height) * kStickActivationRadius;
  const CGPoint delta = [self stickDeltaForFrame:frame capture:capture];
  const CGPoint normalized = CGPointMake(delta.x / outer_radius, delta.y / outer_radius);
  const float magnitude = std::hypot(normalized.x, normalized.y);
  if (magnitude < kStickDeadzone || magnitude <= 0.0f) {
    return CGPointZero;
  }
  const float rescaled =
      std::clamp((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), 0.0f, 1.0f);
  return CGPointMake(normalized.x / magnitude * rescaled, normalized.y / magnitude * rescaled);
}

#pragma mark Display link and visibility

- (void)createDisplayLinkIfNeeded {
  if (display_link_) {
    return;
  }
  // Republishes every frame while visible: look swipes decay and tapped
  // buttons release on a timer, with no touch event to drive them.
  display_link_ = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayLinkFired:)];
  display_link_.paused = !gameplay_overlay_active_;
  [display_link_ addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)shutdown {
  // The display link retains its target; breaking it here is what lets the
  // overlay deallocate.
  [display_link_ invalidate];
  display_link_ = nil;
  gameplay_overlay_active_ = NO;
  captures_.clear();
  if (runtime_model_) {
    runtime_model_->StoreResolvedState(touch::TouchResolvedState{});
  }
}

- (void)willMoveToWindow:(UIWindow*)new_window {
  if (!new_window) {
    [display_link_ invalidate];
    display_link_ = nil;
  } else {
    [self createDisplayLinkIfNeeded];
  }
  [super willMoveToWindow:new_window];
}

- (void)displayLinkFired:(CADisplayLink*)display_link {
  (void)display_link;
  [self publishResolvedState];
}

- (void)setGameplayOverlayVisible:(BOOL)visible animated:(BOOL)animated {
  if (visible == gameplay_overlay_active_ && self.hidden == !visible) {
    return;
  }
  gameplay_overlay_active_ = visible;
  if (visible) {
    if (self.hidden) {
      self.alpha = 0.0;
    }
    self.hidden = NO;
    display_link_.paused = NO;
    [self setNeedsDisplay];
    [self publishResolvedState];
    [UIView animateWithDuration:animated ? 0.3 : 0.0
                     animations:^{
                       self.alpha = 1.0;
                     }];
    return;
  }
  // Keep the editor's changes if the overlay goes away mid-edit.
  if (editing_) {
    [self applyEditorAction:EditorAction::kDone];
  }
  display_link_.paused = YES;
  captures_.clear();
  look_time_ = -1.0;
  [self publishResolvedState];
  if (!animated || self.hidden) {
    self.hidden = YES;
    self.alpha = 0.0;
    return;
  }
  __weak RexTouchControlsOverlayView* weak_self = self;
  [UIView animateWithDuration:0.2
      animations:^{
        self.alpha = 0.0;
      }
      completion:^(BOOL finished) {
        (void)finished;
        RexTouchControlsOverlayView* strong_self = weak_self;
        // Shown again before the fade finished: stay visible.
        if (strong_self && !strong_self->gameplay_overlay_active_) {
          strong_self.hidden = YES;
        }
      }];
}

- (void)setLookZoneEnabled:(BOOL)enabled {
  if (look_zone_enabled_ == enabled) {
    return;
  }
  look_zone_enabled_ = enabled;
  if (enabled) {
    return;
  }
  // Drop a swipe in progress so it cannot keep turning the camera behind the dialog.
  captures_.erase(std::remove_if(captures_.begin(), captures_.end(),
                                 [](const Capture& c) { return c.control == kLookZone; }),
                  captures_.end());
  look_time_ = -1.0;
  [self publishResolvedState];
}

#pragma mark Input

- (void)playHaptic:(bool)light {
  if (!REXCVAR_GET(touch_haptics)) {
    return;
  }
  UIImpactFeedbackGenerator* generator = light ? haptic_press_light_ : haptic_press_;
  [generator impactOccurred];
  [generator prepare];
}

- (void)publishResolvedState {
  if (!runtime_model_) {
    return;
  }
  touch::TouchResolvedState state = {};
  state.gameplay_enabled = gameplay_overlay_active_;

  // No game input while the layout is being edited.
  if (gameplay_overlay_active_ && !editing_) {
    const double time = CACurrentMediaTime();
    for (const Capture& capture : captures_) {
      if (capture.control < 0) {
        continue;
      }
      const ControlDef& control = kControls[capture.control];
      if (control.kind == ControlKind::kStick) {
        switch (capture.stick_zone) {
          case StickZone::kDpadUp:
            state.buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
            break;
          case StickZone::kDpadDown:
            state.buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
            break;
          case StickZone::kDpadLeft:
            state.buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
            break;
          case StickZone::kDpadRight:
            state.buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
            break;
          case StickZone::kStick: {
            const CGPoint vector = [self stickVectorForFrame:[self controlFrame:capture.control]
                                                     capture:capture];
            state.thumb_lx = ToAxis(vector.x);
            state.thumb_ly = ToAxis(-vector.y);
            break;
          }
        }
      } else if ([self control:capture.control containsPoint:capture.current]) {
        state.buttons |= control.buttons;
        state.left_trigger = std::max(state.left_trigger, control.left_trigger);
        state.right_trigger = std::max(state.right_trigger, control.right_trigger);
      }
    }

    // Keep quick taps pressed long enough that the game sees them even at low frame rates.
    const double tap_hold = ButtonTapHoldSeconds();
    for (size_t i = 0; i < kControlCount; ++i) {
      if (kControls[i].kind == ControlKind::kButton && button_press_times_[i] >= 0.0 &&
          time - button_press_times_[i] < tap_hold) {
        state.buttons |= kControls[i].buttons;
        state.left_trigger = std::max(state.left_trigger, kControls[i].left_trigger);
        state.right_trigger = std::max(state.right_trigger, kControls[i].right_trigger);
      }
    }

    // Swipes produce a short look impulse that fades out, so the camera stops
    // when the finger does.
    if (look_time_ >= 0.0) {
      const double age = time - look_time_;
      const double hold = LookHoldSeconds();
      if (age < hold) {
        const float decay = float(1.0 - age / hold);
        MergeAxis(state.thumb_rx, ToAxis(look_vector_.x * decay));
        MergeAxis(state.thumb_ry, ToAxis(look_vector_.y * decay));
      } else {
        look_time_ = -1.0;
      }
    }
  }

  // Titles detect input edges by packet number: bump it only on a change.
  if (!touch::TouchStatesEqualIgnoringPacket(state, last_published_state_)) {
    state.packet_number = next_packet_number_++;
    if (next_packet_number_ == 0) {
      next_packet_number_ = 1;
    }
    last_published_state_ = state;
  } else {
    state.packet_number = last_published_state_.packet_number;
  }
  runtime_model_->StoreResolvedState(state);
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  (void)event;
  if (self.hidden || self.alpha <= 0.01 || !gameplay_overlay_active_) {
    return nil;
  }
  if (editing_ || look_zone_enabled_) {
    return self;
  }
  // A dialog is up: only claim touches on the controls, so the rest reach it.
  if (RectContains([self toViewFrame:kEditButtonFrame], point) ||
      (!g_menu_label.empty() && RectContains([self toViewFrame:kMenuButtonFrame], point)) ||
      [self controlAtPoint:point includeHidden:false] >= 0) {
    return self;
  }
  return nil;
}

- (void)applyEditorAction:(EditorAction)action {
  switch (action) {
    case EditorAction::kSmaller:
    case EditorAction::kBigger: {
      if (selected_control_ < 0) {
        break;
      }
      touch::TouchRect& frame = layout_[selected_control_].frame;
      const CGPoint centre = Centre(frame);
      const float scale = action == EditorAction::kBigger ? kEditSizeStep : 1.0f / kEditSizeStep;
      const float new_width = frame.width * scale;
      const float new_height = frame.height * scale;
      // Stop at the limits instead of clamping one side, so the control keeps its shape.
      if (new_width < kEditMinSize || new_height < kEditMinSize || new_width > kEditMaxSize ||
          new_height > kEditMaxSize) {
        break;
      }
      frame = {float(centre.x) - new_width * 0.5f, float(centre.y) - new_height * 0.5f, new_width,
               new_height};
      ClampToLayout(frame);
      break;
    }
    case EditorAction::kToggleHidden:
      if (selected_control_ >= 0) {
        layout_[selected_control_].hidden = !layout_[selected_control_].hidden;
      }
      break;
    case EditorAction::kLessOpaque:
    case EditorAction::kMoreOpaque: {
      const float step = action == EditorAction::kMoreOpaque ? kEditOpacityStep : -kEditOpacityStep;
      opacity_ = std::clamp(opacity_ + step, kEditMinOpacity, 1.0f);
      break;
    }
    case EditorAction::kReset:
      [self resetLayout];
      opacity_ = 1.0f;
      break;
    case EditorAction::kDone:
      [self saveLayout];
      editing_ = NO;
      selected_control_ = -1;
      captures_.clear();
      break;
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (!gameplay_overlay_active_) {
    return;
  }
  for (UITouch* touch_object in touches) {
    const CGPoint point = [touch_object locationInView:self];
    captures_.erase(std::remove_if(captures_.begin(), captures_.end(),
                                   [&](const Capture& c) { return c.touch == touch_object; }),
                    captures_.end());
    Capture capture;
    capture.touch = touch_object;
    capture.anchor = point;
    capture.current = point;
    if (editing_) {
      [self editorFingerDown:capture point:point];
    } else {
      [self gameplayFingerDown:capture point:point];
    }
    // Pressing DONE clears the captures, so don't track that touch any further.
    if (editing_ || capture.control != kEditorToolbar) {
      captures_.push_back(capture);
    }
  }
  [self publishResolvedState];
  [self setNeedsDisplay];
}

- (void)gameplayFingerDown:(Capture&)capture point:(CGPoint)point {
  if (!g_menu_label.empty() && RectContains([self toViewFrame:kMenuButtonFrame], point)) {
    capture.control = kMenuButton;
    // Release whatever the other fingers were holding.
    captures_.clear();
    look_time_ = -1.0;
    [self playHaptic:false];
    // After this touch is handled: the action may present a view controller.
    if (auto action = g_menu_action) {
      dispatch_async(dispatch_get_main_queue(), ^{
        action();
      });
    }
    return;
  }
  if (RectContains([self toViewFrame:kEditButtonFrame], point)) {
    capture.control = kEditButton;
    editing_ = YES;
    selected_control_ = -1;
    // Release whatever the other fingers were holding.
    captures_.clear();
    look_time_ = -1.0;
    [self playHaptic:false];
    return;
  }
  capture.control = [self controlAtPoint:point includeHidden:false];
  if (capture.control < 0) {
    capture.control = look_zone_enabled_ ? kLookZone : kEditorEmpty;
    return;
  }
  if (kControls[capture.control].kind == ControlKind::kStick) {
    capture.stick_zone = [self stickZoneForFrame:[self controlFrame:capture.control] point:point];
    [self playHaptic:true];
  } else {
    button_press_times_[capture.control] = CACurrentMediaTime();
    [self playHaptic:false];
  }
}

- (void)editorFingerDown:(Capture&)capture point:(CGPoint)point {
  for (size_t i = 0; i < kEditorButtonCount; ++i) {
    if (RectContains([self editorButtonFrame:i], point)) {
      capture.control = kEditorToolbar;
      [self playHaptic:false];
      [self applyEditorAction:kEditorButtons[i].action];
      return;
    }
  }
  const int control = [self controlAtPoint:point includeHidden:true];
  if (control >= 0) {
    capture.control = control;
    capture.frame_at_anchor = layout_[control].frame;
    selected_control_ = control;
    [self playHaptic:true];
  } else {
    capture.control = kEditorEmpty;
    selected_control_ = -1;
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  for (UITouch* touch_object in touches) {
    auto capture = std::find_if(captures_.begin(), captures_.end(),
                                [&](const Capture& c) { return c.touch == touch_object; });
    if (capture == captures_.end()) {
      continue;
    }
    const CGPoint point = [touch_object locationInView:self];
    const CGPoint previous = [touch_object previousLocationInView:self];
    capture->current = point;
    if (editing_) {
      if (capture->control >= 0) {
        const touch::TouchRect space = [self layoutSpace];
        touch::TouchRect& frame = layout_[capture->control].frame;
        frame.x = capture->frame_at_anchor.x + float(point.x - capture->anchor.x) / space.width;
        frame.y = capture->frame_at_anchor.y + float(point.y - capture->anchor.y) / space.height;
        ClampToLayout(frame);
      }
    } else if (capture->control == kLookZone) {
      const float points = LookPointsPerFullScale();
      look_vector_ = CGPointMake(
          std::clamp(float(point.x - previous.x) / points, -1.0f, 1.0f),
          std::clamp(-float(point.y - previous.y) / points * LookVerticalScale(), -1.0f, 1.0f));
      look_time_ = CACurrentMediaTime();
    }
  }
  [self publishResolvedState];
  [self setNeedsDisplay];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  [self endTouches:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  [self endTouches:touches];
}

- (void)endTouches:(NSSet<UITouch*>*)touches {
  captures_.erase(std::remove_if(captures_.begin(), captures_.end(),
                                 [&](const Capture& c) { return [touches containsObject:c.touch]; }),
                  captures_.end());
  [self publishResolvedState];
  [self setNeedsDisplay];
}

#pragma mark Drawing

- (void)layoutSubviews {
  [super layoutSubviews];
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect {
  (void)rect;
  CGContextRef context = UIGraphicsGetCurrentContext();
  if (!context) {
    return;
  }
  const CGSize size = self.bounds.size;
  const CGFloat border_width = 1.5;
  const BOOL editing = editing_;

  // White with the given alpha, scaled by the opacity setting unless told otherwise.
  auto white = [&](float alpha, bool apply_opacity = true) {
    const float final_alpha = std::clamp(apply_opacity ? alpha * opacity_ : alpha, 0.0f, 1.0f);
    return [UIColor colorWithWhite:1.0 alpha:final_alpha];
  };
  auto draw_label = [&](const char* text, CGPoint centre, CGFloat font_size, UIColor* colour) {
    NSString* string = @(text);
    NSDictionary* attributes = @{
      NSFontAttributeName : [UIFont systemFontOfSize:font_size weight:UIFontWeightSemibold],
      NSForegroundColorAttributeName : colour,
    };
    const CGSize text_size = [string sizeWithAttributes:attributes];
    [string drawAtPoint:CGPointMake(centre.x - text_size.width * 0.5,
                                    centre.y - text_size.height * 0.5)
         withAttributes:attributes];
  };
  auto draw_pill = [&](const touch::TouchRect& frame, UIColor* fill, UIColor* border) {
    UIBezierPath* path = [UIBezierPath bezierPathWithRoundedRect:ToCGRect(frame)
                                                    cornerRadius:Radius(frame)];
    [fill setFill];
    [path fill];
    path.lineWidth = border_width;
    [border setStroke];
    [path stroke];
  };
  auto circle = [](CGPoint centre, CGFloat radius) {
    return [UIBezierPath bezierPathWithArcCenter:centre
                                          radius:radius
                                      startAngle:0
                                        endAngle:2 * M_PI
                                       clockwise:YES];
  };
  auto is_held = [&](size_t index) {
    return std::any_of(captures_.begin(), captures_.end(),
                       [&](const Capture& c) { return c.control == int(index); });
  };

  if (editing) {
    // Dim the game so it's clear the controls are being edited, not played.
    [[UIColor colorWithWhite:0.0 alpha:0.43] setFill];
    UIRectFillUsingBlendMode(self.bounds, kCGBlendModeNormal);
    draw_label("Drag controls to move them. Tap one to select it.",
               CGPointMake(size.width * 0.5, size.height * 0.79), size.height * 0.045,
               [UIColor colorWithWhite:1.0 alpha:0.9]);
  } else {
    const touch::TouchRect frame = [self toViewFrame:kEditButtonFrame];
    draw_pill(frame, white(0.08f), white(0.32f));
    draw_label("EDIT", Centre(frame), std::min(frame.width, frame.height) * 0.30f, white(0.85f));
    if (!g_menu_label.empty()) {
      const touch::TouchRect menu_frame = [self toViewFrame:kMenuButtonFrame];
      draw_pill(menu_frame, white(0.08f), white(0.32f));
      draw_label(g_menu_label.c_str(), Centre(menu_frame),
                 std::min(menu_frame.width, menu_frame.height) * 0.30f, white(0.85f));
    }
  }

  for (size_t i = 0; i < kControlCount; ++i) {
    const ControlDef& control = kControls[i];
    const LayoutEntry& entry = layout_[i];
    if (entry.hidden && !editing) {
      continue;
    }
    const touch::TouchRect frame = [self controlFrame:i];
    const CGPoint centre = Centre(frame);
    const bool held = !editing && is_held(i);
    const bool selected = editing && int(i) == selected_control_;
    // Hidden controls stay faintly visible in the editor so they can be shown again.
    const float alpha_scale = entry.hidden ? 0.35f : 1.0f;

    if (control.kind == ControlKind::kStick) {
      const CGFloat radius = Radius(frame);
      const CGFloat ring_radius = std::min(frame.width, frame.height) * kStickDpadRingRadius;
      UIBezierPath* outer = circle(centre, radius);
      [white(0.10f * alpha_scale) setFill];
      [outer fill];
      outer.lineWidth = border_width;
      [white(0.38f * alpha_scale) setStroke];
      [outer stroke];
      UIBezierPath* ring = circle(centre, ring_radius);
      ring.lineWidth = border_width;
      [white(0.20f * alpha_scale) setStroke];
      [ring stroke];

      // D-pad arrows on the outer ring.
      const CGFloat arrow_distance = (radius + ring_radius) * 0.5;
      const CGFloat arrow_size = (radius - ring_radius) * 0.3;
      const CGPoint directions[] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
      const StickZone zones[] = {StickZone::kDpadUp, StickZone::kDpadDown, StickZone::kDpadLeft,
                                 StickZone::kDpadRight};
      for (size_t d = 0; d < 4; ++d) {
        const bool pressed =
            !editing && std::any_of(captures_.begin(), captures_.end(), [&](const Capture& c) {
              return c.control == int(i) && c.stick_zone == zones[d];
            });
        const CGPoint dir = directions[d];
        const CGPoint tip = CGPointMake(centre.x + dir.x * (arrow_distance + arrow_size),
                                        centre.y + dir.y * (arrow_distance + arrow_size));
        const CGPoint base = CGPointMake(centre.x + dir.x * (arrow_distance - arrow_size),
                                         centre.y + dir.y * (arrow_distance - arrow_size));
        const CGPoint side = CGPointMake(-dir.y * arrow_size, dir.x * arrow_size);
        UIBezierPath* arrow = [UIBezierPath bezierPath];
        [arrow moveToPoint:tip];
        [arrow addLineToPoint:CGPointMake(base.x + side.x, base.y + side.y)];
        [arrow addLineToPoint:CGPointMake(base.x - side.x, base.y - side.y)];
        [arrow closePath];
        [white((pressed ? 0.85f : 0.45f) * alpha_scale) setFill];
        [arrow fill];
      }

      // The knob follows the finger while the stick is held.
      CGPoint knob = centre;
      for (const Capture& capture : captures_) {
        if (!editing && capture.control == int(i) && capture.stick_zone == StickZone::kStick) {
          const CGPoint delta = [self stickDeltaForFrame:frame capture:capture];
          knob = CGPointMake(centre.x + delta.x, centre.y + delta.y);
        }
      }
      UIBezierPath* knob_path = circle(knob, std::min(frame.width, frame.height) * 0.14);
      [white(0.18f * alpha_scale) setFill];
      [knob_path fill];
      knob_path.lineWidth = border_width;
      [white(0.72f * alpha_scale) setStroke];
      [knob_path stroke];

      if (selected) {
        UIBezierPath* outline = circle(centre, radius + border_width * 3.0);
        outline.lineWidth = border_width * 2.0;
        [[UIColor colorWithRed:1.0 green:0.78 blue:0.16 alpha:1.0] setStroke];
        [outline stroke];
      }
    } else {
      draw_pill(frame, white((held ? 0.35f : 0.08f) * alpha_scale),
                white((held ? 0.80f : 0.32f) * alpha_scale));
      const CGFloat font_size =
          std::min(frame.width, frame.height) * (std::strlen(control.label) > 2 ? 0.30f : 0.42f);
      draw_label(control.label, centre, font_size, white((held ? 1.0f : 0.85f) * alpha_scale));
      if (selected) {
        const float outset = border_width * 3.0f;
        const touch::TouchRect outline_frame = {frame.x - outset, frame.y - outset,
                                                frame.width + outset * 2.0f,
                                                frame.height + outset * 2.0f};
        UIBezierPath* outline = [UIBezierPath bezierPathWithRoundedRect:ToCGRect(outline_frame)
                                                           cornerRadius:Radius(outline_frame)];
        outline.lineWidth = border_width * 2.0;
        [[UIColor colorWithRed:1.0 green:0.78 blue:0.16 alpha:1.0] setStroke];
        [outline stroke];
      }
    }
  }

  if (editing) {
    // The toolbar ignores the opacity setting so it's always readable.
    for (size_t i = 0; i < kEditorButtonCount; ++i) {
      const EditorButton& button = kEditorButtons[i];
      const touch::TouchRect frame = [self editorButtonFrame:i];
      const bool needs_selection = button.action == EditorAction::kSmaller ||
                                   button.action == EditorAction::kBigger ||
                                   button.action == EditorAction::kToggleHidden;
      const bool enabled = !needs_selection || selected_control_ >= 0;
      const bool is_done = button.action == EditorAction::kDone;
      const char* label = button.label;
      if (button.action == EditorAction::kToggleHidden && selected_control_ >= 0 &&
          layout_[selected_control_].hidden) {
        label = "SHOW";
      }
      UIColor* fill = is_done ? [UIColor colorWithRed:1.0 green:0.78 blue:0.16 alpha:0.8]
                              : white(enabled ? 0.25f : 0.08f, false);
      UIColor* text = is_done ? UIColor.blackColor : white(enabled ? 1.0f : 0.35f, false);
      UIBezierPath* path = [UIBezierPath bezierPathWithRoundedRect:ToCGRect(frame)
                                                      cornerRadius:frame.height * 0.25];
      [fill setFill];
      [path fill];
      path.lineWidth = border_width;
      [white(0.6f, false) setStroke];
      [path stroke];
      draw_label(label, Centre(frame), frame.height * 0.32, text);
    }
  }
}

@end

// Keeps the overlay on top of SDL's view and decides when it shows: the
// `touch_controls` setting, hardware controllers coming and going, and
// system dialogs opening and closing.
@interface RexTouchControlsController : NSObject
- (instancetype)initWithWindow:(UIWindow*)window
                         hooks:(touch::TouchControlsHooks)hooks NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
- (void)refresh;
- (void)shutdown;
@end

@implementation RexTouchControlsController {
  __weak UIWindow* window_;
  RexTouchControlsOverlayView* overlay_;
  NSTimer* timer_;
  touch::TouchControlsHooks hooks_;
  id<NSObject> connect_observer_;
  id<NSObject> disconnect_observer_;
  id<NSObject> inactive_observer_;
  id<NSObject> active_observer_;
}

- (instancetype)initWithWindow:(UIWindow*)window hooks:(touch::TouchControlsHooks)hooks {
  if (!(self = [super init])) {
    return nil;
  }
  window_ = window;
  hooks_ = std::move(hooks);
  overlay_ =
      [[RexTouchControlsOverlayView alloc] initWithRuntimeModel:&touch::GetTouchRuntimeModel()];
  overlay_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

  __weak RexTouchControlsController* weak_self = self;
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  connect_observer_ = [center addObserverForName:GCControllerDidConnectNotification
                                          object:nil
                                           queue:NSOperationQueue.mainQueue
                                      usingBlock:^(NSNotification* note) {
                                        (void)note;
                                        [weak_self refresh];
                                      }];
  disconnect_observer_ = [center addObserverForName:GCControllerDidDisconnectNotification
                                             object:nil
                                              queue:NSOperationQueue.mainQueue
                                         usingBlock:^(NSNotification* note) {
                                           (void)note;
                                           [weak_self refresh];
                                         }];
  inactive_observer_ = [center addObserverForName:UIApplicationWillResignActiveNotification
                                           object:nil
                                            queue:NSOperationQueue.mainQueue
                                       usingBlock:^(NSNotification* note) {
                                         (void)note;
                                         RexTouchControlsController* controller = weak_self;
                                         if (controller) {
                                           [controller->overlay_ setGameplayOverlayVisible:NO
                                                                                  animated:NO];
                                         }
                                       }];
  active_observer_ = [center addObserverForName:UIApplicationDidBecomeActiveNotification
                                         object:nil
                                          queue:NSOperationQueue.mainQueue
                                     usingBlock:^(NSNotification* note) {
                                       (void)note;
                                       [weak_self refresh];
                                     }];
  // For what has no notification: SDL replacing its view (a new Metal view
  // would cover the overlay), the setting changing, dialogs opening.
  timer_ = [NSTimer timerWithTimeInterval:0.25
                                  repeats:YES
                                    block:^(NSTimer* timer) {
                                      (void)timer;
                                      [weak_self refresh];
                                    }];
  [NSRunLoop.mainRunLoop addTimer:timer_ forMode:NSRunLoopCommonModes];

  touch::GetTouchRuntimeModel().set_attached(true);
  [self refresh];
  return self;
}

- (void)refresh {
  UIWindow* window = window_;
  UIView* host = window.rootViewController.view;
  if (!host) {
    return;
  }
  if (overlay_.superview != host) {
    [overlay_ removeFromSuperview];
    overlay_.frame = host.bounds;
    [host addSubview:overlay_];
  } else if (host.subviews.lastObject != overlay_) {
    [host bringSubviewToFront:overlay_];
  }

  const std::string mode = REXCVAR_GET(touch_controls);
  const bool active = window.windowScene.activationState == UISceneActivationStateForegroundActive;
  const bool visible =
      active && (mode == "on" || (mode == "auto" && !HasConnectedGameController()));
  [overlay_ setGameplayOverlayVisible:visible animated:YES];
  const bool system_ui_active = hooks_.is_system_ui_active && hooks_.is_system_ui_active();
  [overlay_ setLookZoneEnabled:!system_ui_active];
}

- (void)shutdown {
  [timer_ invalidate];
  timer_ = nil;
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  if (connect_observer_) [center removeObserver:connect_observer_];
  if (disconnect_observer_) [center removeObserver:disconnect_observer_];
  if (inactive_observer_) [center removeObserver:inactive_observer_];
  if (active_observer_) [center removeObserver:active_observer_];
  connect_observer_ = disconnect_observer_ = inactive_observer_ = active_observer_ = nil;
  touch::GetTouchRuntimeModel().set_attached(false);
  [overlay_ shutdown];
  [overlay_ removeFromSuperview];
  overlay_ = nil;
}

@end

namespace {
RexTouchControlsController* g_touch_controls = nil;
}  // namespace

namespace rex::input::touch {

void InstallTouchControls(rex::ui::Window* window, TouchControlsHooks hooks) {
  if (g_touch_controls || !window) {
    return;
  }
  // SDL is the only windowing backend on iOS.
  SDL_Window* sdl_window = static_cast<rex::ui::WindowSDL*>(window)->sdl_window();
  if (!sdl_window) {
    return;
  }
  UIWindow* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(
      SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
  if (!ui_window) {
    REXLOG_WARN("Touch controls: SDL window has no UIKit window");
    return;
  }
  g_touch_controls = [[RexTouchControlsController alloc] initWithWindow:ui_window
                                                                  hooks:std::move(hooks)];
  REXLOG_INFO("Touch controls installed ({} controls, touch_controls = {})", kControlCount,
              REXCVAR_GET(touch_controls));
}

void RemoveTouchControls() {
  [g_touch_controls shutdown];
  g_touch_controls = nil;
}

void SetTouchMenuButton(std::string label, std::function<void()> action) {
  if (NSThread.isMainThread) {
    g_menu_label = std::move(label);
    g_menu_action = std::move(action);
    return;
  }
  __block std::string new_label = std::move(label);
  __block std::function<void()> new_action = std::move(action);
  dispatch_sync(dispatch_get_main_queue(), ^{
    g_menu_label = std::move(new_label);
    g_menu_action = std::move(new_action);
  });
}

}  // namespace rex::input::touch
