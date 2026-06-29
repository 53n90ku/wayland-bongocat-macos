# Bongo Cat macOS Overlay

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)
[![Version](https://img.shields.io/badge/version-2.0.0-blue.svg)](https://github.com/saatvik333/wayland-bongocat/releases)

A lightweight macOS overlay that shows an animated bongo cat reacting to your keyboard input.

![Demo](assets/demo.gif)

## Features

- Real-time keyboard animation using macOS Input Monitoring
- Transparent always-on-top overlay
- Hot-reloadable configuration
- Multi-monitor selection by display name
- Idle and scheduled sleep mode
- SVG-based rendering for sharp scaling
- Cmd+Shift+0 overlay hide/show hotkey
- Optional LaunchAgent login startup

## Requirements

- macOS
- Xcode Command Line Tools
- `make`

Install the command line tools if needed:

```bash
xcode-select --install
```

## Quick Start

Build the app:

```bash
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat
make
```

Create a config file:

```bash
mkdir -p ~/.config/bongocat
cp bongocat.conf.example ~/.config/bongocat/bongocat.conf
```

Run it:

```bash
./build/bongocat --watch-config
```

The first run may prompt for keyboard monitoring permission. If the cat does not react to typing, grant permission manually in:

```text
System Settings > Privacy & Security > Input Monitoring
```

Then restart `bongocat`.

## Start at Login

Install the user LaunchAgent:

```bash
./build/bongocat --install-launch-agent
```

Remove it:

```bash
./build/bongocat --uninstall-launch-agent
```

While `bongocat` is running, press `Cmd+Shift+0` to hide or show the overlay.

## Configuration

The default config path is:

```text
~/.config/bongocat/bongocat.conf
```

Example:

```ini
cat_height=110
cat_align=center
cat_x_offset=0
cat_y_offset=0

overlay_height=120
overlay_opacity=0
overlay_position=top
layer=top

fps=60
idle_frame=0
keypress_duration=100
enable_hand_mapping=1

# Optional: show only on named displays.
# Use the display names shown in System Settings > Displays.
# monitor=Built-in Retina Display
# monitor=Built-in Retina Display,Studio Display

# Optional sleep mode.
idle_sleep_timeout=0
enable_scheduled_sleep=0
sleep_begin=22:00
sleep_end=06:00

enable_debug=0
```

On macOS, `keyboard_device` and `keyboard_name` are not needed. The app listens through the macOS event tap after Input Monitoring permission is granted.

### Options Reference

| Option | Values | Default | Description |
| --- | --- | --- | --- |
| `cat_height` | 10-200 | 40 | Cat size in pixels |
| `cat_align` | left/center/right | center | Horizontal alignment |
| `cat_x_offset` | any int | 100 | Horizontal offset from alignment |
| `cat_y_offset` | any int | 10 | Vertical offset from center |
| `overlay_height` | 20-300 | 50 | Overlay bar height in pixels |
| `overlay_opacity` | 0-255 | 150 | Background opacity, 0 is transparent |
| `overlay_position` | top/bottom | top | Screen edge position |
| `layer` | top/overlay | top | Window level behavior |
| `monitor` | comma list | main display | macOS display names to render on |
| `fps` | 1-120 | 60 | Animation frame rate |
| `mirror_x` | 0/1 | 0 | Flip cat horizontally |
| `mirror_y` | 0/1 | 0 | Flip cat vertically |
| `enable_hand_mapping` | 0/1 | 1 | Map keys to left/right hand frames |
| `keypress_duration` | ms | 100 | How long key-down frame is held |
| `idle_frame` | 0-4 | 0 | Frame shown when idle |
| `idle_sleep_timeout` | seconds | 0 | Sleep after idle, 0 disables |
| `enable_scheduled_sleep` | 0/1 | 0 | Enable time-based sleep schedule |
| `sleep_begin` | HH:MM | 00:00 | Sleep schedule start time |
| `sleep_end` | HH:MM | 00:00 | Sleep schedule end time |
| `disable_fullscreen_hide` | 0/1 | 0 | Keep overlay visible in fullscreen |
| `enable_debug` | 0/1 | 0 | Enable debug logging |
| `test_animation_duration` | ms | 200 | Test animation frame duration |
| `test_animation_interval` | ms | 0 | Test animation repeat interval |

## Command Line

```bash
./build/bongocat [OPTIONS]

  -c, --config FILE             Config file path
  -m, --monitor NAME            Force a specific display name
  -w, --watch-config            Auto-reload config changes
  -t, --toggle                  Start/stop toggle
      --install-launch-agent    Start bongocat at login
      --uninstall-launch-agent  Remove the login LaunchAgent
  -h, --help                    Help
  -v, --version                 Version
```

 like if you follow the above steps it should be easily configured and then i just edited this from the main repo, so all credit goes to satvik