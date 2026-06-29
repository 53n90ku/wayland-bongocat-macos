#define _DARWIN_C_SOURCE
#include "platform/input.h"

#include "graphics/animation.h"
#include "platform/macos.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

atomic_int *any_key_pressed;
atomic_int *last_key_code;

static CFMachPortRef event_tap = NULL;
static CFRunLoopSourceRef event_source = NULL;
static int wake_pipe[2] = {-1, -1};
static int input_debug_enabled = 0;

static atomic_int *alloc_local_atomic(void) {
  atomic_int *ptr = calloc(1, sizeof(atomic_int));
  if (!ptr) {
    return NULL;
  }
  atomic_store(ptr, 0);
  return ptr;
}

static void close_wake_pipe(void) {
  if (wake_pipe[0] >= 0) {
    close(wake_pipe[0]);
    wake_pipe[0] = -1;
  }
  if (wake_pipe[1] >= 0) {
    close(wake_pipe[1]);
    wake_pipe[1] = -1;
  }
}

static bool setup_wake_pipe(void) {
  if (wake_pipe[0] >= 0 && wake_pipe[1] >= 0) {
    return true;
  }

  if (pipe(wake_pipe) != 0) {
    bongocat_log_warning("Failed to create macOS wake pipe: %s",
                         strerror(errno));
    wake_pipe[0] = -1;
    wake_pipe[1] = -1;
    return false;
  }

  int read_flags = fcntl(wake_pipe[0], F_GETFL, 0);
  int write_flags = fcntl(wake_pipe[1], F_GETFL, 0);
  if (read_flags >= 0) {
    fcntl(wake_pipe[0], F_SETFL, read_flags | O_NONBLOCK);
  }
  if (write_flags >= 0) {
    fcntl(wake_pipe[1], F_SETFL, write_flags | O_NONBLOCK);
  }

  return true;
}

static bool mac_key_is_left_hand(CGKeyCode keycode) {
  switch (keycode) {
  case kVK_ANSI_A:
  case kVK_ANSI_S:
  case kVK_ANSI_D:
  case kVK_ANSI_F:
  case kVK_ANSI_G:
  case kVK_ANSI_Z:
  case kVK_ANSI_X:
  case kVK_ANSI_C:
  case kVK_ANSI_V:
  case kVK_ANSI_B:
  case kVK_ANSI_Q:
  case kVK_ANSI_W:
  case kVK_ANSI_E:
  case kVK_ANSI_R:
  case kVK_ANSI_T:
  case kVK_ANSI_1:
  case kVK_ANSI_2:
  case kVK_ANSI_3:
  case kVK_ANSI_4:
  case kVK_ANSI_5:
  case kVK_ANSI_6:
  case kVK_Escape:
  case kVK_Tab:
  case kVK_CapsLock:
  case kVK_Shift:
  case kVK_Control:
  case kVK_Option:
  case kVK_Command:
  case kVK_ANSI_Grave:
    return true;
  default:
    return false;
  }
}

static void wake_animation_thread(void) {
  if (wake_pipe[1] < 0) {
    return;
  }

  uint64_t value = 1;
  if (write(wake_pipe[1], &value, sizeof(value)) < 0) {
    // Best effort wake. EAGAIN just means the pipe is already wakeable.
  }
}

static void trigger_key_animation(CGKeyCode keycode) {
  if (last_key_code) {
    atomic_store(last_key_code, mac_key_is_left_hand(keycode) ? 30 : 36);
  }

  if (input_debug_enabled) {
    bongocat_log_debug("macOS key: %d", (int)keycode);
  }

  animation_trigger();
  wake_animation_thread();
}

static CGEventRef keyboard_event_callback(CGEventTapProxy proxy, CGEventType type,
                                          CGEventRef event, void *user_info) {
  (void)proxy;
  (void)user_info;

  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput) {
    if (event_tap) {
      CGEventTapEnable(event_tap, true);
    }
    return event;
  }

  if (type == kCGEventKeyDown || type == kCGEventFlagsChanged) {
    CGKeyCode keycode =
        (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    trigger_key_animation(keycode);
  }

  return event;
}

static bool request_keyboard_monitoring_permission(void) {
#if defined(__MAC_10_15)
  if (@available(macOS 10.15, *)) {
    if (CGPreflightListenEventAccess()) {
      return true;
    }
    return CGRequestListenEventAccess();
  }
#endif

  const void *keys[] = {kAXTrustedCheckOptionPrompt};
  const void *values[] = {kCFBooleanTrue};
  CFDictionaryRef options =
      CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                         &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  bool trusted = AXIsProcessTrustedWithOptions(options);
  if (options) {
    CFRelease(options);
  }
  return trusted;
}

pid_t input_get_child_pid(void) {
  return -1;
}

int input_get_wake_fd(void) {
  return wake_pipe[0];
}

bongocat_error_t input_start_monitoring(char **device_paths, int num_devices,
                                        char **names, int num_names,
                                        int scan_interval, int enable_debug) {
  (void)device_paths;
  (void)num_devices;
  (void)names;
  (void)num_names;
  (void)scan_interval;

  input_debug_enabled = enable_debug;

  if (!any_key_pressed) {
    any_key_pressed = alloc_local_atomic();
    if (!any_key_pressed) {
      return BONGOCAT_ERROR_MEMORY;
    }
  }
  if (!last_key_code) {
    last_key_code = alloc_local_atomic();
    if (!last_key_code) {
      free(any_key_pressed);
      any_key_pressed = NULL;
      return BONGOCAT_ERROR_MEMORY;
    }
  }

  setup_wake_pipe();

  if (event_tap) {
    return BONGOCAT_SUCCESS;
  }

  if (!request_keyboard_monitoring_permission()) {
    bongocat_log_warning("macOS Input Monitoring permission is required for "
                         "keyboard animation. Grant it in System Settings > "
                         "Privacy & Security > Input Monitoring, then "
                         "restart.");
    return BONGOCAT_SUCCESS;
  }

  CGEventMask mask =
      CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventFlagsChanged);
  event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                               kCGEventTapOptionListenOnly, mask,
                               keyboard_event_callback, NULL);
  if (!event_tap) {
    bongocat_log_warning("Failed to create macOS keyboard event tap. Check "
                         "System Settings > Privacy & Security > Input "
                         "Monitoring for bongocat.");
    return BONGOCAT_SUCCESS;
  }

  event_source =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap, 0);
  if (!event_source) {
    bongocat_log_warning("Failed to create event tap run loop source");
    CFRelease(event_tap);
    event_tap = NULL;
    return BONGOCAT_SUCCESS;
  }

  CFRunLoopAddSource(CFRunLoopGetMain(), event_source, kCFRunLoopCommonModes);
  CGEventTapEnable(event_tap, true);
  bongocat_log_info("macOS keyboard monitoring started");
  return BONGOCAT_SUCCESS;
}

bongocat_error_t input_restart_monitoring(char **device_paths, int num_devices,
                                          char **names, int num_names,
                                          int scan_interval,
                                          int enable_debug) {
  input_cleanup();
  return input_start_monitoring(device_paths, num_devices, names, num_names,
                                scan_interval, enable_debug);
}

void input_cleanup(void) {
  if (event_source) {
    CFRunLoopRemoveSource(CFRunLoopGetMain(), event_source,
                          kCFRunLoopCommonModes);
    CFRelease(event_source);
    event_source = NULL;
  }

  if (event_tap) {
    CGEventTapEnable(event_tap, false);
    CFRelease(event_tap);
    event_tap = NULL;
  }

  close_wake_pipe();

  free(any_key_pressed);
  any_key_pressed = NULL;
  free(last_key_code);
  last_key_code = NULL;
}
