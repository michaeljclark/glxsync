# glxsync

_glxsync_ demonstrates extended frame synchronization for OpenGL, GLX
and Xlib apps targetting Xorg or Wayland.

![glxsync](/images/glxsync.png)

## Introduction

_glxsync_ is an X Windows OpenGL application using GLX and XSync extended
frame synchronization.

This demo responds to synchronization requests from the compositor in
response to configuration changes, and updates extended synchronization
counters before and after frames to signal to the compositor that rendering
is in progress so that buffers read by the compositor are complete and
match the size in configuration change events.

This allows the compositor to synchronize rendering of window decorations
with rendering of the client area to eliminate tearing during resize.
This demo also receives frame timings and pings from the compositor and
uses poll timeouts and non-blocking IO on the XEvent queue to adjust inter
frame delay to target a specific frame rate.

The demo depends on the following X11 window property atoms:

- _NET_WM_SYNC_REQUEST
- _NET_WM_SYNC_REQUEST_COUNTER
- _NET_WM_FRAME_DRAWN
- _NET_WM_FRAME_TIMINGS
- _NET_WM_PING

## Build

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- --verbose
```

## Profile

```
CPUPROFILE=prof.out LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so ./build/gl2_xsync
google-pprof --text build/gl2_xsync prof.out
```

## References

Frame Synchronization
- https://fishsoup.net/misc/wm-spec-synchronization.html

X Synchronization Extension
- https://www.x.org/releases/X11R7.7/doc/libXext/synclib.html

Window Manager Protocols
- https://specifications.freedesktop.org/wm-spec/1.4/ar01s04.html
- https://specifications.freedesktop.org/wm-spec/1.4/ar01s06.html

Root Window Properties
- https://specifications.freedesktop.org/wm-spec/1.4/ar01s03.html
