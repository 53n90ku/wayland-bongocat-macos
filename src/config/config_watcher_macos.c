#include "core/bongocat.h"

int config_watcher_init(ConfigWatcher *watcher, const char *config_path,
                        void (*callback)(const char *)) {
  if (!watcher) {
    return -1;
  }

  watcher->inotify_fd = -1;
  watcher->watch_fd = -1;
  watcher->watching = false;
  watcher->reload_callback = callback;
  free(watcher->config_path);
  watcher->config_path = config_path ? strdup(config_path) : NULL;
  return 0;
}

void config_watcher_start(ConfigWatcher *watcher) {
  if (watcher) {
    watcher->watching = true;
  }
}

void config_watcher_stop(ConfigWatcher *watcher) {
  if (watcher) {
    watcher->watching = false;
  }
}

void config_watcher_cleanup(ConfigWatcher *watcher) {
  if (!watcher) {
    return;
  }

  watcher->watching = false;
  free(watcher->config_path);
  watcher->config_path = NULL;
}
