#define _DARWIN_C_SOURCE
#include "platform/macos.h"

#include "graphics/animation.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#include <math.h>

static config_t *current_config = NULL;
static NSMutableArray<NSWindow *> *overlay_windows = nil;
static CGFloat render_scale = 1.0;
static volatile sig_atomic_t *running_flag = NULL;
static void (*tick_callback_fn)(void) = NULL;
static bool overlay_visible = true;
static bool smart_avoidance_active = false;
static CGFloat smart_avoidance_target_cat_y = 0.0;
static CGFloat animated_cat_y = 0.0;
static bool animated_cat_y_ready = false;
static EventHotKeyRef toggle_hotkey_ref = NULL;
static EventHandlerRef toggle_hotkey_handler_ref = NULL;

static void mark_overlay_needs_display(void);

@interface BongoCatView : NSView
@end

@interface BongoRunLoopPump : NSObject
- (void)tick:(NSTimer *)timer;
- (void)screensChanged:(NSNotification *)notification;
@end

static BongoRunLoopPump *run_loop_pump = nil;

static EventHotKeyID toggle_hotkey_id(void) {
  EventHotKeyID hotkey_id;
  hotkey_id.signature = ((UInt32)'B' << 24) | ((UInt32)'C' << 16) |
                        ((UInt32)'a' << 8) | (UInt32)'t';
  hotkey_id.id = 1;
  return hotkey_id;
}

static OSStatus toggle_hotkey_handler(EventHandlerCallRef next_handler,
                                      EventRef event, void *user_data) {
  (void)next_handler;
  (void)user_data;

  EventHotKeyID hotkey_id;
  OSStatus status =
      GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                        NULL, sizeof(hotkey_id), NULL, &hotkey_id);
  EventHotKeyID expected = toggle_hotkey_id();
  if (status == noErr && hotkey_id.signature == expected.signature &&
      hotkey_id.id == expected.id) {
    macos_app_toggle_visible();
    return noErr;
  }

  return eventNotHandledErr;
}

static void unregister_toggle_hotkey(void) {
  if (toggle_hotkey_ref) {
    UnregisterEventHotKey(toggle_hotkey_ref);
    toggle_hotkey_ref = NULL;
  }

  if (toggle_hotkey_handler_ref) {
    RemoveEventHandler(toggle_hotkey_handler_ref);
    toggle_hotkey_handler_ref = NULL;
  }
}

static void register_toggle_hotkey(void) {
  if (toggle_hotkey_ref) {
    return;
  }

  EventTypeSpec event_type = {
      .eventClass = kEventClassKeyboard,
      .eventKind = kEventHotKeyPressed,
  };
  OSStatus status =
      InstallApplicationEventHandler(&toggle_hotkey_handler, 1, &event_type,
                                     NULL, &toggle_hotkey_handler_ref);
  if (status != noErr) {
    bongocat_log_warning("Failed to install macOS hotkey handler: %d",
                         (int)status);
    toggle_hotkey_handler_ref = NULL;
    return;
  }

  EventHotKeyID hotkey_id = toggle_hotkey_id();
  status = RegisterEventHotKey(kVK_ANSI_0, cmdKey | shiftKey, hotkey_id,
                               GetApplicationEventTarget(), 0,
                               &toggle_hotkey_ref);
  if (status != noErr) {
    bongocat_log_warning("Failed to register Cmd+Shift+0 hotkey: %d",
                         (int)status);
    unregister_toggle_hotkey();
    return;
  }

  bongocat_log_info("Registered macOS hotkey Cmd+Shift+0");
}

static NSString *screen_display_name(NSScreen *screen) {
  if ([screen respondsToSelector:@selector(localizedName)]) {
    return [screen localizedName];
  }
  NSNumber *screen_id = screen.deviceDescription[@"NSScreenNumber"];
  return [NSString stringWithFormat:@"%@", screen_id ? screen_id : @"unknown"];
}

static bool config_wants_screen(config_t *config, NSScreen *screen) {
  if (!config || config->num_output_names <= 0) {
    return screen == [NSScreen mainScreen];
  }

  NSString *name = screen_display_name(screen);
  const char *utf8_name = [name UTF8String];
  for (int i = 0; i < config->num_output_names; i++) {
    if (config->output_names[i] &&
        strcmp(config->output_names[i], utf8_name) == 0) {
      return true;
    }
  }

  return false;
}

static NSArray<NSScreen *> *selected_screens(config_t *config) {
  NSMutableArray<NSScreen *> *screens = [NSMutableArray array];
  for (NSScreen *screen in [NSScreen screens]) {
    if (config_wants_screen(config, screen)) {
      [screens addObject:screen];
    }
  }

  if ([screens count] == 0 && [NSScreen mainScreen]) {
    if (config && config->output_name) {
      bongocat_log_warning("Could not find macOS screen named '%s'; using main "
                           "screen",
                           config->output_name);
    }
    [screens addObject:[NSScreen mainScreen]];
  }

  return [screens copy];
}

static void update_screen_metrics(config_t *config) {
  NSArray<NSScreen *> *screens = selected_screens(config);
  CGFloat max_scale = 1.0;
  NSScreen *first = [screens firstObject];

  for (NSScreen *screen in screens) {
    if ([screen backingScaleFactor] > max_scale) {
      max_scale = [screen backingScaleFactor];
    }
  }

  render_scale = max_scale > 0 ? max_scale : 1.0;
  if (first && config) {
    config->screen_width = (int)NSWidth([first frame]);
  }
}

int macos_phys_dim(int logical) {
  if (logical <= 0) {
    return 0;
  }
  return (int)ceil((double)logical * (double)render_scale);
}

static NSInteger window_level_for_config(const config_t *config) {
  if (config && config->layer == LAYER_OVERLAY) {
    return NSScreenSaverWindowLevel;
  }
  return NSStatusWindowLevel;
}

static CGFloat default_cat_x_for_bounds(const config_t *config, NSRect bounds,
                                        CGFloat cat_width) {
  switch (config->cat_align) {
  case ALIGN_CENTER:
    return (NSWidth(bounds) - cat_width) / 2.0 +
           (CGFloat)config->cat_x_offset;
  case ALIGN_LEFT:
    return (CGFloat)config->cat_x_offset;
  case ALIGN_RIGHT:
    return NSWidth(bounds) - cat_width - (CGFloat)config->cat_x_offset;
  }
  return (NSWidth(bounds) - cat_width) / 2.0;
}

static CGFloat clamped_cat_x(CGFloat x, CGFloat bounds_width,
                             CGFloat cat_width) {
  CGFloat max_x = bounds_width - cat_width;
  if (max_x < 0.0) {
    return 0.0;
  }
  if (x < 0.0) {
    return 0.0;
  }
  if (x > max_x) {
    return max_x;
  }
  return x;
}

static CGRect cat_screen_rect_for_window(NSWindow *window, CGFloat cat_x,
                                         CGFloat cat_y, CGFloat cat_width,
                                         CGFloat cat_height) {
  NSRect window_frame = [window frame];
  NSRect bounds = [[window contentView] bounds];
  CGFloat screen_y =
      NSMinY(window_frame) + NSHeight(bounds) - cat_y - cat_height;
  return CGRectMake(NSMinX(window_frame) + cat_x, screen_y, cat_width,
                    cat_height);
}

static bool rect_intersects_with_margin(CGRect a, CGRect b, CGFloat margin) {
  CGRect expanded = CGRectInset(b, -margin, -margin);
  if (CGRectIsEmpty(a)) {
    CGPoint point = CGPointMake(CGRectGetMidX(a), CGRectGetMidY(a));
    return CGRectContainsPoint(expanded, point);
  }
  return CGRectIntersectsRect(a, expanded);
}

static CGRect visible_cat_screen_rect_for_window(NSWindow *window, CGFloat cat_x,
                                                 CGFloat cat_y,
                                                 CGFloat cat_width,
                                                 CGFloat cat_height) {
  CGRect fallback =
      cat_screen_rect_for_window(window, cat_x, cat_y, cat_width, cat_height);

  pthread_mutex_lock(&anim_lock);
  int frame_index = anim_index;
  if (frame_index < 0 || frame_index >= NUM_FRAMES) {
    frame_index = BONGOCAT_FRAME_BOTH_UP;
  }

  cached_frame_t frame = anim_cached_frames[frame_index];
  if (!frame.data || frame.width <= 0 || frame.height <= 0) {
    pthread_mutex_unlock(&anim_lock);
    return fallback;
  }

  int min_x = frame.width;
  int min_y = frame.height;
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < frame.height; y++) {
    for (int x = 0; x < frame.width; x++) {
      int idx = (y * frame.width + x) * 4;
      if (frame.data[idx + 3] <= 12) {
        continue;
      }
      if (x < min_x) {
        min_x = x;
      }
      if (y < min_y) {
        min_y = y;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y > max_y) {
        max_y = y;
      }
    }
  }
  pthread_mutex_unlock(&anim_lock);

  if (max_x < min_x || max_y < min_y) {
    return fallback;
  }

  CGFloat scale_x = cat_width / (CGFloat)frame.width;
  CGFloat scale_y = cat_height / (CGFloat)frame.height;
  CGFloat visible_x = cat_x + (CGFloat)min_x * scale_x;
  CGFloat visible_y = cat_y + (CGFloat)min_y * scale_y;
  CGFloat visible_w = (CGFloat)(max_x - min_x + 1) * scale_x;
  CGFloat visible_h = (CGFloat)(max_y - min_y + 1) * scale_y;

  return cat_screen_rect_for_window(window, visible_x, visible_y, visible_w,
                                    visible_h);
}

static CGRect camera_guard_screen_rect_for_window(NSWindow *window) {
  NSRect window_frame = [window frame];
  NSRect bounds = [[window contentView] bounds];
  CGFloat guard_width = fmin(NSWidth(bounds) * 0.24, 360.0);
  guard_width = fmax(220.0, guard_width);
  if (guard_width > NSWidth(bounds)) {
    guard_width = NSWidth(bounds);
  }

  CGFloat base_guard_height =
      current_config ? (CGFloat)current_config->cat_height * 0.68 : 72.0;
  CGFloat guard_height = fmax(64.0, base_guard_height);
  if (guard_height > NSHeight(bounds)) {
    guard_height = NSHeight(bounds);
  }

  CGFloat x = NSMinX(window_frame) + (NSWidth(bounds) - guard_width) / 2.0;
  CGFloat y = NSMaxY(window_frame) - guard_height;
  return CGRectMake(x, y, guard_width, guard_height);
}

static CGFloat cat_tuck_distance(CGFloat cat_height) {
  return fmax(34.0, cat_height * 0.58);
}

static CGFloat minimum_cat_y(CGFloat cat_height) {
  return -cat_tuck_distance(cat_height);
}

static CGFloat clamped_cat_y(CGFloat y, CGFloat bounds_height,
                             CGFloat cat_height) {
  CGFloat min_y = minimum_cat_y(cat_height);
  CGFloat max_y = bounds_height - cat_height;
  if (max_y < min_y) {
    max_y = min_y;
  }
  if (y < min_y) {
    return min_y;
  }
  if (y > max_y) {
    return max_y;
  }
  return y;
}

static CGFloat avoidance_y_for_cursor(CGFloat default_y, CGFloat cat_height) {
  if (current_config && current_config->overlay_position == POSITION_TOP) {
    return default_y - cat_tuck_distance(cat_height);
  }
  return 0.0;
}

static bool update_animated_cat_position(CGFloat default_y,
                                         CGFloat bounds_height,
                                         CGFloat cat_height) {
  CGFloat target = smart_avoidance_active ? smart_avoidance_target_cat_y
                                          : default_y;
  target = clamped_cat_y(target, bounds_height, cat_height);

  if (!animated_cat_y_ready) {
    animated_cat_y = clamped_cat_y(default_y, bounds_height, cat_height);
    animated_cat_y_ready = true;
  }

  CGFloat delta = target - animated_cat_y;
  if (fabs((double)delta) < 0.35) {
    if (animated_cat_y != target) {
      animated_cat_y = target;
      return true;
    }
    return false;
  }

  animated_cat_y += delta * 0.075;
  animated_cat_y = clamped_cat_y(animated_cat_y, bounds_height, cat_height);
  return true;
}

static void update_smart_avoidance(void) {
  if (!current_config || !overlay_visible || !overlay_windows) {
    return;
  }

  NSPoint mouse_location = [NSEvent mouseLocation];
  CGRect mouse_rect =
      CGRectMake(mouse_location.x - 2.0, mouse_location.y - 2.0, 4.0, 4.0);

  for (NSWindow *window in overlay_windows) {
    NSRect bounds = [[window contentView] bounds];
    CGFloat cat_height = (CGFloat)current_config->cat_height;
    CGFloat cat_width =
        cat_height * (CGFloat)CAT_IMAGE_WIDTH / (CGFloat)CAT_IMAGE_HEIGHT;
    CGFloat cat_y = (NSHeight(bounds) - cat_height) / 2.0 +
                    (CGFloat)current_config->cat_y_offset;
    CGFloat cat_x =
        default_cat_x_for_bounds(current_config, bounds, cat_width);
    CGRect cat_rect = visible_cat_screen_rect_for_window(
        window, cat_x, cat_y, cat_width, cat_height);
    CGRect camera_rect = camera_guard_screen_rect_for_window(window);
    bool cursor_hits_cat = rect_intersects_with_margin(mouse_rect, cat_rect, 6.0);
    bool cursor_hits_camera =
        rect_intersects_with_margin(mouse_rect, camera_rect, 8.0);

    if (cursor_hits_cat || cursor_hits_camera) {
      smart_avoidance_target_cat_y = avoidance_y_for_cursor(cat_y, cat_height);
      smart_avoidance_active = true;
      if (update_animated_cat_position(cat_y, NSHeight(bounds), cat_height)) {
        mark_overlay_needs_display();
      }
      return;
    }
  }

  smart_avoidance_active = false;
  NSWindow *first_window = [overlay_windows firstObject];
  if (first_window) {
    NSRect bounds = [[first_window contentView] bounds];
    CGFloat cat_height = (CGFloat)current_config->cat_height;
    CGFloat default_y = (NSHeight(bounds) - cat_height) / 2.0 +
                        (CGFloat)current_config->cat_y_offset;
    if (update_animated_cat_position(default_y, NSHeight(bounds),
                                     cat_height)) {
      mark_overlay_needs_display();
    }
  }
}

static NSRect overlay_frame_for_screen(config_t *config, NSScreen *screen) {
  NSRect screen_frame = [screen frame];
  CGFloat overlay_height =
      config ? (CGFloat)config->overlay_height : DEFAULT_BAR_HEIGHT;
  CGFloat y = NSMinY(screen_frame);

  if (!config || config->overlay_position == POSITION_TOP) {
    y = NSMaxY(screen_frame) - overlay_height;
  }

  return NSMakeRect(NSMinX(screen_frame), y, NSWidth(screen_frame),
                    overlay_height);
}

static void mark_overlay_needs_display(void) {
  for (NSWindow *window in overlay_windows) {
    [[window contentView] setNeedsDisplay:YES];
  }
}

static void rebuild_overlay_windows(void) {
  if (!current_config) {
    return;
  }

  if (!overlay_windows) {
    overlay_windows = [NSMutableArray array];
  }

  for (NSWindow *window in overlay_windows) {
    [window orderOut:nil];
    [window close];
  }
  [overlay_windows removeAllObjects];
  animated_cat_y_ready = false;

  update_screen_metrics(current_config);
  NSArray<NSScreen *> *screens = selected_screens(current_config);
  for (NSScreen *screen in screens) {
    NSRect frame = overlay_frame_for_screen(current_config, screen);
    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskBorderless
                                      backing:NSBackingStoreBuffered
                                        defer:NO
                                       screen:screen];
    [window setReleasedWhenClosed:NO];
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];
    [window setIgnoresMouseEvents:YES];
    [window setLevel:window_level_for_config(current_config)];
    [window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorStationary |
                                  NSWindowCollectionBehaviorIgnoresCycle |
                                  NSWindowCollectionBehaviorFullScreenAuxiliary];

    BongoCatView *view = [[BongoCatView alloc] initWithFrame:NSMakeRect(
                                                       0, 0, NSWidth(frame),
                                                       NSHeight(frame))];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [window setContentView:view];
    if (overlay_visible) {
      [window orderFrontRegardless];
    } else {
      [window orderOut:nil];
    }
    [overlay_windows addObject:window];

    bongocat_log_info("Created macOS overlay on screen '%s' (%dx%d)",
                      [screen_display_name(screen) UTF8String],
                      (int)NSWidth(frame), (int)NSHeight(frame));
  }

  mark_overlay_needs_display();
}

static void recache_frames_for_config(void) {
  if (!current_config) {
    return;
  }

  pthread_mutex_lock(&anim_lock);
  animation_invalidate_cache();
  int cat_h = macos_phys_dim(current_config->cat_height);
  int cat_w = (cat_h * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
  animation_cache_frames(cat_w, cat_h, current_config->mirror_x,
                         current_config->mirror_y,
                         current_config->enable_antialiasing);
  pthread_mutex_unlock(&anim_lock);
}

@implementation BongoCatView

- (BOOL)isFlipped {
  return YES;
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  pthread_mutex_lock(&anim_lock);

  config_t *config = current_config;
  if (!config) {
    pthread_mutex_unlock(&anim_lock);
    return;
  }

  NSRect bounds = [self bounds];
  CGFloat alpha = (CGFloat)config->overlay_opacity / 255.0;
  if (alpha > 0.0) {
    [[NSColor colorWithCalibratedWhite:0.0 alpha:alpha] setFill];
    NSRectFill(bounds);
  }

  int frame_index = anim_index;
  if (frame_index < 0 || frame_index >= NUM_FRAMES) {
    frame_index = BONGOCAT_FRAME_BOTH_UP;
  }

  cached_frame_t frame = anim_cached_frames[frame_index];
  if (!frame.data || frame.width <= 0 || frame.height <= 0) {
    pthread_mutex_unlock(&anim_lock);
    return;
  }

  CGFloat cat_height = (CGFloat)config->cat_height;
  CGFloat cat_width =
      cat_height * (CGFloat)CAT_IMAGE_WIDTH / (CGFloat)CAT_IMAGE_HEIGHT;
  CGFloat cat_y = (NSHeight(bounds) - cat_height) / 2.0 +
                  (CGFloat)config->cat_y_offset;
  CGFloat cat_x = default_cat_x_for_bounds(config, bounds, cat_width);
  if (animated_cat_y_ready) {
    cat_y = animated_cat_y;
  }
  cat_x = clamped_cat_x(cat_x, NSWidth(bounds), cat_width);

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider = CGDataProviderCreateWithData(
      NULL, frame.data, (size_t)frame.width * (size_t)frame.height * 4U, NULL);
  if (!color_space || !provider) {
    if (provider) {
      CGDataProviderRelease(provider);
    }
    if (color_space) {
      CGColorSpaceRelease(color_space);
    }
    pthread_mutex_unlock(&anim_lock);
    return;
  }

  CGBitmapInfo bitmap_info =
      kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst;
  CGImageRef image = CGImageCreate(
      frame.width, frame.height, 8, 32, (size_t)frame.width * 4U, color_space,
      bitmap_info, provider, NULL, true, kCGRenderingIntentDefault);
  if (image) {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx,
                       CGRectMake(cat_x, cat_y, cat_width, cat_height), image);
    CGImageRelease(image);
  }

  CGDataProviderRelease(provider);
  CGColorSpaceRelease(color_space);
  pthread_mutex_unlock(&anim_lock);
}

@end

@implementation BongoRunLoopPump

- (void)tick:(NSTimer *)timer {
  (void)timer;
  if (tick_callback_fn) {
    tick_callback_fn();
  }

  update_smart_avoidance();

  if (running_flag && *running_flag == 0) {
    [NSApp stop:nil];
  }
}

- (void)screensChanged:(NSNotification *)notification {
  (void)notification;
  update_screen_metrics(current_config);
  recache_frames_for_config();
  rebuild_overlay_windows();
}

@end

void macos_app_set_tick_callback(void (*callback)(void)) {
  tick_callback_fn = callback;
}

bongocat_error_t macos_app_init(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  current_config = config;
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    if (!run_loop_pump) {
      run_loop_pump = [[BongoRunLoopPump alloc] init];
      [[NSNotificationCenter defaultCenter]
          addObserver:run_loop_pump
             selector:@selector(screensChanged:)
                 name:NSApplicationDidChangeScreenParametersNotification
               object:nil];
    }

    register_toggle_hotkey();
    update_screen_metrics(config);
    rebuild_overlay_windows();
  }

  bongocat_log_info("macOS overlay initialized at %.2fx scale",
                    (double)render_scale);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t macos_app_run(volatile sig_atomic_t *running) {
  BONGOCAT_CHECK_NULL(running, BONGOCAT_ERROR_INVALID_PARAM);

  running_flag = running;
  @autoreleasepool {
    [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0)
                                     target:run_loop_pump
                                   selector:@selector(tick:)
                                   userInfo:nil
                                    repeats:YES];
    [NSApp run];
  }

  return BONGOCAT_SUCCESS;
}

void macos_app_update_config(config_t *config) {
  if (!config) {
    return;
  }

  current_config = config;
  void (^update_block)(void) = ^{
    update_screen_metrics(config);
    recache_frames_for_config();
    rebuild_overlay_windows();
    mark_overlay_needs_display();
  };

  if ([NSThread isMainThread]) {
    update_block();
  } else {
    dispatch_async(dispatch_get_main_queue(), update_block);
  }
}

void macos_app_toggle_visible(void) {
  dispatch_async(dispatch_get_main_queue(), ^{
    overlay_visible = !overlay_visible;
    for (NSWindow *window in overlay_windows) {
      if (overlay_visible) {
        [window orderFrontRegardless];
      } else {
        [window orderOut:nil];
      }
    }
    bongocat_log_info("macOS overlay %s",
                      overlay_visible ? "shown" : "hidden");
  });
}

void draw_bar(void) {
  if (!overlay_windows || !overlay_visible) {
    return;
  }

  if ([NSThread isMainThread]) {
    mark_overlay_needs_display();
  } else {
    dispatch_async(dispatch_get_main_queue(), ^{
      mark_overlay_needs_display();
    });
  }
}

void macos_app_cleanup(void) {
  @autoreleasepool {
    unregister_toggle_hotkey();

    if (run_loop_pump) {
      [[NSNotificationCenter defaultCenter] removeObserver:run_loop_pump];
      run_loop_pump = nil;
    }

    for (NSWindow *window in overlay_windows) {
      [window orderOut:nil];
      [window close];
    }
    [overlay_windows removeAllObjects];
    overlay_windows = nil;
    current_config = NULL;
    running_flag = NULL;
    tick_callback_fn = NULL;
  }
}
