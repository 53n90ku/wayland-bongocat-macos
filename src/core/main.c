#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "config/config.h"
#include "core/bongocat.h"
#ifndef __APPLE__
#include "core/multi_monitor.h"
#endif
#include "graphics/animation.h"
#include "platform/input.h"
#ifdef __APPLE__
#include "platform/macos.h"
#else
#include "platform/wayland.h"
#endif
#include "utils/error.h"
#include "utils/memory.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

static volatile sig_atomic_t running = 1;
static config_t g_config;
static ConfigWatcher g_config_watcher = {.inotify_fd = -1, .watch_fd = -1};
static bool g_manage_pid_file = true;
static const char *g_forced_monitor_name = NULL;
static atomic_bool g_reload_pending = false;
static int g_pid_fd = -1;

static const char *get_pid_file_path(void) {
  static char pid_path[PATH_MAX];
  if (pid_path[0] != '\0')
    return pid_path;
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && runtime_dir[0] != '\0') {
    snprintf(pid_path, sizeof(pid_path), "%s/bongocat.pid", runtime_dir);
  } else {
    snprintf(pid_path, sizeof(pid_path), "/tmp/bongocat.pid");
  }
  return pid_path;
}

// =============================================================================
// COMMAND LINE ARGUMENTS STRUCTURE
// =============================================================================

typedef struct {
  const char *config_file;
  const char *monitor_name;  // --monitor override for multi-monitor children
  bool multi_monitor_child;  // Internal flag to skip PID file management
  bool watch_config;
  bool toggle_mode;
  bool show_help;
  bool show_version;
#ifdef __APPLE__
  bool install_launch_agent;
  bool uninstall_launch_agent;
#endif
} cli_args_t;

// =============================================================================
// PROCESS MANAGEMENT MODULE
// =============================================================================

static int process_create_pid_file(void) {
  int fd = open(get_pid_file_path(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW,
                0600);
  if (fd < 0) {
    bongocat_log_error("Failed to create PID file: %s", strerror(errno));
    return -1;
  }

  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    int lock_err = errno;
    close(fd);
    if (lock_err == EWOULDBLOCK) {
      bongocat_log_info("Another instance is already running");
      return -2;  // Already running
    }
    bongocat_log_error("Failed to lock PID file: %s", strerror(lock_err));
    return -1;
  }

  char pid_str[32];
  snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
  if (write(fd, pid_str, strlen(pid_str)) < 0) {
    bongocat_log_error("Failed to write PID to file: %s", strerror(errno));
    close(fd);
    return -1;
  }

  return fd;  // Keep file descriptor open to maintain lock
}

static void process_remove_pid_file(void) {
  unlink(get_pid_file_path());
}

static pid_t process_get_running_pid(void) {
  int fd = open(get_pid_file_path(), O_RDONLY);
  if (fd < 0) {
    return -1;  // No PID file exists
  }

  // Try to get a shared lock to read the file
  if (flock(fd, LOCK_SH | LOCK_NB) < 0) {
    int lock_err = errno;
    close(fd);
    if (lock_err == EWOULDBLOCK) {
      // File is locked by another process, so it's running
      // We need to read the PID anyway, so let's try without lock
      fd = open(get_pid_file_path(), O_RDONLY);
      if (fd < 0)
        return -1;
    } else {
      return -1;
    }
  }

  char pid_str[32];
  ssize_t bytes_read = read(fd, pid_str, sizeof(pid_str) - 1);
  close(fd);

  if (bytes_read <= 0) {
    return -1;
  }

  pid_str[bytes_read] = '\0';

  // Parse PID with full validation (replaces unsafe atoi)
  errno = 0;
  char *endptr;
  long parsed = strtol(pid_str, &endptr, 10);
  if (errno != 0 || endptr == pid_str ||
      (*endptr != '\n' && *endptr != '\0' && *endptr != '\r')) {
    bongocat_log_error("Invalid PID in PID file");
    process_remove_pid_file();
    return -1;
  }
  if (parsed <= 1 || parsed > (long)INT32_MAX) {
    bongocat_log_error("PID value out of safe range: %ld", parsed);
    process_remove_pid_file();
    return -1;
  }
  pid_t pid = (pid_t)parsed;

  // Check if process is actually running
  if (kill(pid, 0) != 0) {
    // Process is not running, remove stale PID file
    process_remove_pid_file();
    return -1;
  }

  // Verify the running process is actually bongocat via /proc/PID/comm
  char proc_path[64];
  snprintf(proc_path, sizeof(proc_path), "/proc/%d/comm", pid);
  FILE *fp = fopen(proc_path, "r");
  if (fp) {
    char comm[64] = {0};
    if (fgets(comm, sizeof(comm), fp)) {
      comm[strcspn(comm, "\n")] = '\0';
      if (strcmp(comm, "bongocat") != 0) {
        fclose(fp);
        bongocat_log_info("PID %d is not bongocat (is %s), removing stale file",
                          pid, comm);
        process_remove_pid_file();
        return -1;
      }
    }
    fclose(fp);
  }

  return pid;  // Process is running and verified
}

static int process_handle_toggle(void) {
  pid_t running_pid = process_get_running_pid();

  if (running_pid > 0) {
    // Process is running, kill it
    bongocat_log_info("Stopping bongocat (PID: %d)", running_pid);
    // Negate running pid to allow targetting process group (multiple monitors)
    if (kill(-running_pid, SIGTERM) == 0) {
      // Wait a bit for graceful shutdown
      for (int i = 0; i < 50; i++) {  // Wait up to 5 seconds
        if (kill(-running_pid, 0) != 0) {
          bongocat_log_info("Bongocat stopped successfully");
          return 0;
        }
        usleep(100000);  // 100ms
      }

      // Force kill if still running
      bongocat_log_warning("Force killing bongocat");
      if (kill(running_pid, SIGKILL) != 0) {
        bongocat_log_error("Failed to force kill bongocat: %s",
                           strerror(errno));
        return 1;
      }
      bongocat_log_info("Bongocat force stopped");
    } else {
      bongocat_log_error("Failed to stop bongocat: %s", strerror(errno));
      return 1;
    }
  } else {
    bongocat_log_info("Bongocat is not running, starting it now");
    return -1;  // Signal to continue with normal startup
  }

  return 0;
}

#ifdef __APPLE__
#define MACOS_LAUNCH_AGENT_LABEL "com.saatvik333.bongocat.overlay"

static bool path_has_slash(const char *path) {
  return path && strchr(path, '/') != NULL;
}

static bool resolve_executable_path(const char *argv0, char *out,
                                    size_t out_size) {
  if (!out || out_size == 0) {
    return false;
  }

  if (path_has_slash(argv0)) {
    if (realpath(argv0, out)) {
      return true;
    }
    return false;
  }

  const char *path_env = getenv("PATH");
  if (!path_env) {
    return false;
  }

  char *path_copy = strdup(path_env);
  if (!path_copy) {
    return false;
  }

  bool found = false;
  char *saveptr = NULL;
  for (char *dir = strtok_r(path_copy, ":", &saveptr); dir;
       dir = strtok_r(NULL, ":", &saveptr)) {
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0);
    if (access(candidate, X_OK) == 0 && realpath(candidate, out)) {
      found = true;
      break;
    }
  }

  free(path_copy);
  return found;
}

static char *resolve_absolute_path_or_copy(const char *path) {
  if (!path) {
    return NULL;
  }

  char resolved[PATH_MAX];
  if (realpath(path, resolved)) {
    return strdup(resolved);
  }

  return strdup(path);
}

static char *macos_launch_agent_path(void) {
  const char *home = getenv("HOME");
  if (!home || home[0] == '\0') {
    return NULL;
  }

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/Library/LaunchAgents/%s.plist", home,
           MACOS_LAUNCH_AGENT_LABEL);
  return strdup(path);
}

static bool macos_ensure_launch_agents_dir(void) {
  const char *home = getenv("HOME");
  if (!home || home[0] == '\0') {
    bongocat_log_error("HOME is not set; cannot install LaunchAgent");
    return false;
  }

  char dir[PATH_MAX];
  int len = snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home);
  if (len < 0 || (size_t)len >= sizeof(dir)) {
    bongocat_log_error("LaunchAgents path is too long");
    return false;
  }

  if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
    bongocat_log_error("Failed to create %s: %s", dir, strerror(errno));
    return false;
  }

  return true;
}

static void xml_write_escaped(FILE *file, const char *value) {
  for (const unsigned char *p = (const unsigned char *)value; p && *p; p++) {
    switch (*p) {
    case '&':
      fputs("&amp;", file);
      break;
    case '<':
      fputs("&lt;", file);
      break;
    case '>':
      fputs("&gt;", file);
      break;
    case '"':
      fputs("&quot;", file);
      break;
    case '\'':
      fputs("&apos;", file);
      break;
    default:
      fputc(*p, file);
      break;
    }
  }
}

static int macos_run_launchctl(char *const args[], bool log_errors) {
  pid_t pid = fork();
  if (pid < 0) {
    if (log_errors) {
      bongocat_log_error("Failed to fork launchctl: %s", strerror(errno));
    }
    return -1;
  }

  if (pid == 0) {
    execvp("launchctl", args);
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    if (log_errors) {
      bongocat_log_error("Failed to wait for launchctl: %s", strerror(errno));
    }
    return -1;
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

static void macos_launch_agent_bootout(void) {
  char service[128];
  snprintf(service, sizeof(service), "gui/%d/%s", getuid(),
           MACOS_LAUNCH_AGENT_LABEL);
  char *args[] = {"launchctl", "bootout", service, NULL};
  macos_run_launchctl(args, false);
}

static int macos_launch_agent_bootstrap(const char *plist_path) {
  char domain[64];
  snprintf(domain, sizeof(domain), "gui/%d", getuid());
  char *args[] = {"launchctl", "bootstrap", domain, (char *)plist_path, NULL};
  return macos_run_launchctl(args, true);
}

static int macos_install_launch_agent(const char *argv0,
                                      const char *config_path) {
  char executable_path[PATH_MAX];
  if (!resolve_executable_path(argv0, executable_path,
                               sizeof(executable_path))) {
    bongocat_log_error("Could not resolve executable path for LaunchAgent");
    return 1;
  }

  if (!macos_ensure_launch_agents_dir()) {
    return 1;
  }

  char *plist_path = macos_launch_agent_path();
  if (!plist_path) {
    bongocat_log_error("Could not build LaunchAgent path");
    return 1;
  }

  char *absolute_config = resolve_absolute_path_or_copy(config_path);
  FILE *file = fopen(plist_path, "w");
  if (!file) {
    bongocat_log_error("Failed to write %s: %s", plist_path, strerror(errno));
    free(absolute_config);
    free(plist_path);
    return 1;
  }

  fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", file);
  fputs("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n",
        file);
  fputs("<plist version=\"1.0\">\n<dict>\n", file);
  fputs("  <key>Label</key>\n  <string>", file);
  xml_write_escaped(file, MACOS_LAUNCH_AGENT_LABEL);
  fputs("</string>\n", file);
  fputs("  <key>ProgramArguments</key>\n  <array>\n", file);
  fputs("    <string>", file);
  xml_write_escaped(file, executable_path);
  fputs("</string>\n", file);
  if (absolute_config) {
    fputs("    <string>-c</string>\n    <string>", file);
    xml_write_escaped(file, absolute_config);
    fputs("</string>\n", file);
  }
  fputs("    <string>-w</string>\n  </array>\n", file);
  fputs("  <key>RunAtLoad</key>\n  <true/>\n", file);
  fputs("  <key>KeepAlive</key>\n  <true/>\n", file);
  fputs("  <key>ProcessType</key>\n  <string>Interactive</string>\n", file);
  fputs("  <key>StandardOutPath</key>\n  "
        "<string>/tmp/bongocat.out.log</string>\n",
        file);
  fputs("  <key>StandardErrorPath</key>\n  "
        "<string>/tmp/bongocat.err.log</string>\n",
        file);
  fputs("</dict>\n</plist>\n", file);
  fclose(file);

  macos_launch_agent_bootout();
  int bootstrap_result = macos_launch_agent_bootstrap(plist_path);
  if (bootstrap_result != 0) {
    bongocat_log_warning("LaunchAgent plist was written, but launchctl "
                         "bootstrap returned %d",
                         bootstrap_result);
  }

  bongocat_log_info("Installed LaunchAgent: %s", plist_path);
  bongocat_log_info("It will start at login. Use Cmd+Shift+0 to hide/show.");

  free(absolute_config);
  free(plist_path);
  return bootstrap_result == 0 ? 0 : 1;
}

static int macos_uninstall_launch_agent(void) {
  char *plist_path = macos_launch_agent_path();
  if (!plist_path) {
    bongocat_log_error("Could not build LaunchAgent path");
    return 1;
  }

  macos_launch_agent_bootout();
  if (unlink(plist_path) < 0 && errno != ENOENT) {
    bongocat_log_error("Failed to remove %s: %s", plist_path, strerror(errno));
    free(plist_path);
    return 1;
  }

  bongocat_log_info("Removed LaunchAgent: %s", plist_path);
  free(plist_path);
  return 0;
}
#endif

// =============================================================================
// SIGNAL HANDLING MODULE
// =============================================================================

static void signal_handler(int sig) {
  // Only async-signal-safe functions allowed here
  switch (sig) {
  case SIGINT:
  case SIGTERM:
  case SIGQUIT:
  case SIGHUP:
    running = 0;
    break;
  case SIGCHLD:
    while (waitpid(-1, NULL, WNOHANG) > 0)
      ;
    break;
  default:
    break;
  }
}

// Crash signal handler - only async-signal-safe operations
static void crash_signal_handler(int sig) {
  // Kill child process directly (async-signal-safe)
  pid_t child = input_get_child_pid();
  if (child > 0) {
    kill(child, SIGTERM);
  }

  // Remove PID file (unlink is async-signal-safe)
  if (g_manage_pid_file) {
    unlink(get_pid_file_path());
  }

  // Reset to default handler and re-raise
  signal(sig, SIG_DFL);
  raise(sig);
}

static bongocat_error_t signal_setup_handlers(void) {
  struct sigaction sa;

  // Setup signal handler
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  if (sigaction(SIGINT, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGINT handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  if (sigaction(SIGTERM, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGTERM handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGCHLD handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  // Handle SIGQUIT (Ctrl+\) and SIGHUP (terminal hangup)
  if (sigaction(SIGQUIT, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGQUIT handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  if (sigaction(SIGHUP, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGHUP handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  // Ignore SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  // Setup crash signal handlers to ensure child cleanup
  struct sigaction crash_sa;
  crash_sa.sa_handler = crash_signal_handler;
  sigemptyset(&crash_sa.sa_mask);
  crash_sa.sa_flags = SA_RESETHAND;  // Reset to default after handling

  sigaction(SIGSEGV, &crash_sa, NULL);
  sigaction(SIGABRT, &crash_sa, NULL);
  sigaction(SIGFPE, &crash_sa, NULL);
  sigaction(SIGILL, &crash_sa, NULL);

  return BONGOCAT_SUCCESS;
}

// =============================================================================
// CONFIGURATION MANAGEMENT MODULE
// =============================================================================

static void config_free_output_selection(config_t *config) {
  if (!config) {
    return;
  }

  if (config->output_name) {
    free(config->output_name);
    config->output_name = NULL;
  }

  if (config->output_names) {
    for (int i = 0; i < config->num_output_names; i++) {
      free(config->output_names[i]);
    }
    free(config->output_names);
    config->output_names = NULL;
  }

  config->num_output_names = 0;
}

static bongocat_error_t config_apply_forced_monitor(config_t *config,
                                                    const char *monitor_name) {
  if (!config || !monitor_name) {
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  config_free_output_selection(config);

  config->output_name = strdup(monitor_name);
  if (!config->output_name) {
    bongocat_log_error("Failed to allocate monitor override '%s'",
                       monitor_name);
    return BONGOCAT_ERROR_MEMORY;
  }

  bongocat_log_info("Using forced monitor output: '%s'", monitor_name);
  return BONGOCAT_SUCCESS;
}

static void config_reload_apply(const char *config_path) {
  bongocat_log_info("Reloading configuration from: %s", config_path);

  // Save old device info before loading so we can detect input-device changes.
  int old_num_devices = g_config.num_keyboard_devices;

  // Copy old device paths so we can compare after reload
  char **old_device_paths = NULL;
  if (old_num_devices > 0 && g_config.keyboard_devices != NULL) {
    old_device_paths = malloc(sizeof(char *) * old_num_devices);
    if (old_device_paths != NULL) {
      for (int i = 0; i < old_num_devices; i++) {
        old_device_paths[i] = g_config.keyboard_devices[i]
                                  ? strdup(g_config.keyboard_devices[i])
                                  : NULL;
      }
    }
  }

  // Create a temporary config to test loading
  config_t temp_config = {0};
  bongocat_error_t result = load_config(&temp_config, config_path);

  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to reload config: %s",
                       bongocat_error_string(result));
    bongocat_log_info("Keeping current configuration");
    config_cleanup_full(&temp_config);
    // Free saved paths
    if (old_device_paths != NULL) {
      for (int i = 0; i < old_num_devices; i++) {
        free(old_device_paths[i]);
      }
      free(old_device_paths);
    }
    return;
  }

  // Check if devices changed (count or any path differs)
  bool devices_changed = (old_num_devices != temp_config.num_keyboard_devices);
  if (!devices_changed && old_device_paths != NULL) {
    // Same count - check if any paths differ
    for (int i = 0; i < old_num_devices; i++) {
      const char *old_path = old_device_paths[i];
      const char *new_path = temp_config.keyboard_devices[i];
      if ((old_path == NULL) != (new_path == NULL) ||
          (old_path != NULL && new_path != NULL &&
           strcmp(old_path, new_path) != 0)) {
        devices_changed = true;
        break;
      }
    }
  }

  // Free saved paths
  if (old_device_paths != NULL) {
    for (int i = 0; i < old_num_devices; i++) {
      free(old_device_paths[i]);
    }
    free(old_device_paths);
  }

  // Swap in new config under animation lock to avoid reader races
  pthread_mutex_lock(&anim_lock);
  config_cleanup_full(&g_config);
  g_config = temp_config;

  if (g_forced_monitor_name) {
    bongocat_error_t force_result =
        config_apply_forced_monitor(&g_config, g_forced_monitor_name);
    if (force_result != BONGOCAT_SUCCESS) {
      bongocat_log_warning("Failed to keep forced monitor '%s' during reload",
                           g_forced_monitor_name);
    }
  }
  pthread_mutex_unlock(&anim_lock);

  // Update the running systems with new config
#ifdef __APPLE__
  macos_app_update_config(&g_config);
#else
  wayland_update_config(&g_config);
#endif

  // Check if input devices changed and restart monitoring if needed
  if (devices_changed) {
    bongocat_log_info("Input devices changed, restarting input monitoring");
    bongocat_error_t input_result = input_restart_monitoring(
        g_config.keyboard_devices, g_config.num_keyboard_devices,
        g_config.keyboard_names, g_config.num_names,
        g_config.hotplug_scan_interval, g_config.enable_debug);
    if (input_result != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to restart input monitoring: %s",
                         bongocat_error_string(input_result));
    } else {
      bongocat_log_info("Input monitoring restarted successfully");
    }
  }

  bongocat_log_info("Configuration reloaded successfully!");
  bongocat_log_info("New screen dimensions: %dx%d", g_config.screen_width,
                    g_config.overlay_height);
}

static void config_reload_callback(const char *config_path) {
  (void)config_path;
  atomic_store(&g_reload_pending, true);
}

static void config_process_pending_reload(void) {
  if (!atomic_exchange(&g_reload_pending, false)) {
    return;
  }

  const char *config_path =
      (g_config_watcher.config_path && g_config_watcher.config_path[0] != '\0')
          ? g_config_watcher.config_path
          : "bongocat.conf";
  config_reload_apply(config_path);
}

static void platform_tick_callback(void) {
  config_process_pending_reload();
}

static bongocat_error_t config_setup_watcher(const char *config_file) {
  const char *watch_path = config_file ? config_file : "bongocat.conf";

  if (config_watcher_init(&g_config_watcher, watch_path,
                          config_reload_callback) == 0) {
    config_watcher_start(&g_config_watcher);
    bongocat_log_info("Config file watching enabled for: %s", watch_path);
    return BONGOCAT_SUCCESS;
  } else {
    bongocat_log_warning(
        "Failed to initialize config watcher, continuing without hot-reload");
    return BONGOCAT_ERROR_CONFIG;
  }
}

// =============================================================================
// SYSTEM INITIALIZATION AND CLEANUP MODULE
// =============================================================================

static bongocat_error_t system_initialize_components(void) {
  bongocat_error_t result;

#ifdef __APPLE__
  result = macos_app_init(&g_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to initialize macOS overlay: %s",
                       bongocat_error_string(result));
    return result;
  }
#else
  // Initialize Wayland
  result = wayland_init(&g_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to initialize Wayland: %s",
                       bongocat_error_string(result));
    return result;
  }
#endif

  // Initialize animation system
  result = animation_init(&g_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to initialize animation system: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Build initial pre-scaled frame cache (images loaded, config set).
  // Rasterize at the compositor's render scale (HiDPI) so the SVG renders
  // pixel-perfect at any output scale; falls back to 1× if no scale event
  // has arrived yet.
  {
#ifdef __APPLE__
    int cat_h_phys = macos_phys_dim(g_config.cat_height);
#else
    int cat_h_phys = wayland_phys_dim(g_config.cat_height);
#endif
    int cat_w_phys = (cat_h_phys * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    animation_cache_frames(cat_w_phys, cat_h_phys, g_config.mirror_x,
                           g_config.mirror_y, g_config.enable_antialiasing);
  }

  // Start input monitoring
  result = input_start_monitoring(
      g_config.keyboard_devices, g_config.num_keyboard_devices,
      g_config.keyboard_names, g_config.num_names,
      g_config.hotplug_scan_interval, g_config.enable_debug);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to start input monitoring: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Start animation thread
  result = animation_start();
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to start animation thread: %s",
                       bongocat_error_string(result));
    return result;
  }

  return BONGOCAT_SUCCESS;
}

_Noreturn static void system_cleanup_and_exit(int exit_code) {
  bongocat_log_info("Performing cleanup...");

  // Remove PID file and release lock
  if (g_manage_pid_file) {
    process_remove_pid_file();
    if (g_pid_fd >= 0) {
      close(g_pid_fd);
      g_pid_fd = -1;
    }
  }

  // Stop config watcher
  config_watcher_cleanup(&g_config_watcher);

  // Stop animation system
  animation_cleanup();

  // Cleanup platform backend
#ifdef __APPLE__
  macos_app_cleanup();
#else
  wayland_cleanup();
#endif

  // Cleanup input system
  input_cleanup();

  // Capture debug flag before cleanup to avoid use-after-free
  bool debug_mode = g_config.enable_debug;

  // Cleanup configuration
  config_cleanup_full(&g_config);
  config_cleanup();

  // Print memory statistics in debug mode
  if (debug_mode) {
    memory_print_stats();
  }

#ifdef DEBUG
  memory_leak_check();
#endif

  bongocat_log_info("Cleanup complete, exiting with code %d", exit_code);
  exit(exit_code);
}

// =============================================================================
// COMMAND LINE PROCESSING MODULE
// =============================================================================

static void cli_show_help(const char *program_name) {
  printf("Bongo Cat Wayland Overlay\n");
  printf("Usage: %s [options]\n", program_name);
  printf("Options:\n");
  printf("  -h, --help            Show this help message\n");
  printf("  -v, --version         Show version information\n");
  printf(
      "  -c, --config          Specify config file (default: auto-detect)\n");
  printf("  -w, --watch-config    Watch config file for changes and reload "
         "automatically\n");
  printf("  -t, --toggle          Toggle bongocat on/off (start if not "
         "running, stop if running)\n");
  printf("  -m, --monitor NAME    Bind to a specific monitor output\n");
#ifdef __APPLE__
  printf("      --install-launch-agent    Start bongocat at login\n");
  printf("      --uninstall-launch-agent  Remove the login LaunchAgent\n");
  printf("      Cmd+Shift+0 toggles the macOS overlay while it is running\n");
#endif
  printf("\nConfiguration search order:\n");
  printf("  1. $XDG_CONFIG_HOME/bongocat/bongocat.conf\n");
  printf("  2. ~/.config/bongocat/bongocat.conf\n");
  printf("  3. ./bongocat.conf\n");
  printf("\nMulti-monitor: set monitor=OUT1,OUT2 in config to show on "
         "multiple monitors.\n");
}

static void cli_show_version(void) {
  printf("Bongo Cat Overlay v" BONGOCAT_VERSION "\n");
  printf("Built with fast optimizations\n");
}

static int cli_parse_arguments(int argc, char *argv[], cli_args_t *args) {
  // Initialize arguments with defaults
  *args = (cli_args_t){.config_file = NULL,
                       .monitor_name = NULL,
                       .multi_monitor_child = false,
                       .watch_config = false,
                       .toggle_mode = false,
                       .show_help = false,
                       .show_version = false
#ifdef __APPLE__
                       ,
                       .install_launch_agent = false,
                       .uninstall_launch_agent = false
#endif
  };

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      args->show_help = true;
    } else if (strcmp(argv[i], "--version") == 0 ||
               strcmp(argv[i], "-v") == 0) {
      args->show_version = true;
    } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
      if (i + 1 < argc) {
        args->config_file = argv[i + 1];
        i++;  // Skip the next argument since it's the config file path
      } else {
        bongocat_log_error("--config option requires a file path");
        return 1;
      }
    } else if (strcmp(argv[i], "--watch-config") == 0 ||
               strcmp(argv[i], "-w") == 0) {
      args->watch_config = true;
    } else if (strcmp(argv[i], "--toggle") == 0 || strcmp(argv[i], "-t") == 0) {
      args->toggle_mode = true;
    } else if (strcmp(argv[i], "--monitor") == 0 ||
               strcmp(argv[i], "-m") == 0) {
      if (i + 1 < argc) {
        args->monitor_name = argv[i + 1];
        i++;
      } else {
        bongocat_log_error("--monitor option requires an output name");
        return 1;
      }
    } else if (strcmp(argv[i], "--multi-monitor-child") == 0) {
      args->multi_monitor_child = true;
#ifdef __APPLE__
    } else if (strcmp(argv[i], "--install-launch-agent") == 0) {
      args->install_launch_agent = true;
    } else if (strcmp(argv[i], "--uninstall-launch-agent") == 0) {
      args->uninstall_launch_agent = true;
#endif
    } else {
      bongocat_log_warning("Unknown argument: %s", argv[i]);
    }
  }

  return 0;
}

// =============================================================================
// MAIN APPLICATION ENTRY POINT
// =============================================================================

int main(int argc, char *argv[]) {
  bongocat_error_t result;

  // Initialize error system early
  bongocat_error_init(1);  // Enable debug initially

  bongocat_log_info("Starting Bongo Cat Overlay v" BONGOCAT_VERSION);

  // Parse command line arguments
  cli_args_t args;
  if (cli_parse_arguments(argc, argv, &args) != 0) {
    return 1;
  }

  g_manage_pid_file = !args.multi_monitor_child;
  g_forced_monitor_name = args.monitor_name;

  if (args.multi_monitor_child && !args.monitor_name) {
    bongocat_log_error("--multi-monitor-child requires --monitor");
    return 1;
  }

  // Handle help and version requests
  if (args.show_help) {
    cli_show_help(argv[0]);
    return 0;
  }

  if (args.show_version) {
    cli_show_version();
    return 0;
  }

#ifdef __APPLE__
  if (args.install_launch_agent && args.uninstall_launch_agent) {
    bongocat_log_error("--install-launch-agent and --uninstall-launch-agent "
                       "cannot be used together");
    return 1;
  }

  if (args.install_launch_agent || args.uninstall_launch_agent) {
    if (args.multi_monitor_child) {
      bongocat_log_error("LaunchAgent commands are not valid in child mode");
      return 1;
    }

    if (args.uninstall_launch_agent) {
      return macos_uninstall_launch_agent();
    }

    char *resolved_config = config_resolve_path(args.config_file);
    int install_result = macos_install_launch_agent(argv[0], resolved_config);
    free(resolved_config);
    return install_result;
  }
#endif

  // Handle toggle mode
  if (args.toggle_mode && g_manage_pid_file) {
    int toggle_result = process_handle_toggle();
    if (toggle_result >= 0) {
      return toggle_result;  // Either successfully toggled off or error
    }
    // toggle_result == -1 means continue with startup
  } else if (args.toggle_mode) {
    bongocat_log_error(
        "--toggle is not valid in internal multi-monitor child mode");
    return 1;
  }

  // Setup signal handlers
  result = signal_setup_handlers();
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to setup signal handlers: %s",
                       bongocat_error_string(result));
    return 1;
  }

  // Create PID file to track this instance
  if (g_manage_pid_file) {
    int pid_fd = process_create_pid_file();
    if (pid_fd == -2) {
      bongocat_log_error("Another instance of bongocat is already running");
      return 1;
    } else if (pid_fd < 0) {
      bongocat_log_error("Failed to create PID file");
      return 1;
    }
    g_pid_fd = pid_fd;
  }

  // Resolve and load configuration
  char *resolved_config = config_resolve_path(args.config_file);
  result = load_config(&g_config, resolved_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to load configuration: %s",
                       bongocat_error_string(result));
    if (g_manage_pid_file) {
      process_remove_pid_file();
    }
    free(resolved_config);
    return 1;
  }

  bongocat_log_info("Screen dimensions: %dx%d", g_config.screen_width,
                    g_config.overlay_height);

  if (g_config.enable_debug) {
    bongocat_log_warning(
        "DEBUG MODE ENABLED: Keystrokes are being logged "
        "to stdout/stderr. Disable in config if not intended.");
  }

  // Handle multi-monitor mode
  if (g_forced_monitor_name) {
    // Child process: override output_name with assigned monitor
    if (config_apply_forced_monitor(&g_config, g_forced_monitor_name) !=
        BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to apply forced monitor '%s'",
                         g_forced_monitor_name);
      free(resolved_config);
      return 1;
    }
  }
#ifndef __APPLE__
  else if (g_config.num_output_names > 1) {
    // Parent process: launch one child per configured monitor
    bongocat_log_info("Multi-monitor mode enabled with %d configured monitors",
                      g_config.num_output_names);

    int mm_result =
        multi_monitor_launch(argc, argv, resolved_config, args.watch_config,
                             g_config.output_names, g_config.num_output_names);

    if (mm_result == -1) {
      // Single monitor after config filtering, fall through
      bongocat_log_info("Falling back to single-monitor mode");
    } else {
      free(resolved_config);
      config_cleanup_full(&g_config);
      config_cleanup();
      return mm_result;
    }
  }
#else
  else if (g_config.num_output_names > 1) {
    bongocat_log_info(
        "macOS backend handles monitors in-process; skipping child fan-out");
  }
#endif

  // Initialize config watcher if requested
  if (args.watch_config) {
    config_setup_watcher(resolved_config);
  }
  free(resolved_config);

  // Initialize all system components
  result = system_initialize_components();
  if (result != BONGOCAT_SUCCESS) {
    system_cleanup_and_exit(1);
  }

  bongocat_log_info("Bongo Cat Overlay started successfully");

  // Main platform event loop with graceful shutdown
#ifdef __APPLE__
  macos_app_set_tick_callback(platform_tick_callback);
  result = macos_app_run(&running);
#else
  wayland_set_tick_callback(platform_tick_callback);
  result = wayland_run(&running);
#endif
  if (result != BONGOCAT_SUCCESS) {
#ifdef __APPLE__
    bongocat_log_error("macOS event loop error: %s",
#else
    bongocat_log_error("Wayland event loop error: %s",
#endif
                       bongocat_error_string(result));
    system_cleanup_and_exit(1);
  }

  bongocat_log_info("Main loop exited, shutting down");
  system_cleanup_and_exit(0);

  return 0;  // Never reached
}
