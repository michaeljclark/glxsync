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

## Implementation Notes

These notes outline implementation details for the following concerns:

- Extended Synchronization
- Non-blocking IO
- Frame Pacing
- Flow Control
- Congestion Control

### Extended Synchronization

The primary issue causing visual artifacts during window resizing is that
window decorations and client buffers are updated asynchronously by separate
agents, with the window manager responsible for updating decorations and the
client application responsible for updating the client buffer. If the rendering
of the decorations and the client area are not synchronized then one will see
uninitialized buffer contents in the client or decoration area.

![configure-sync](/images/configure-sync.png)

This is solved using the extended frame synchronization protocol in which
supporting clients advertise to the compositor by exporting the
`_NET_WM_SYNC_REQUEST` atom in the `_WM_PROTOCOLS` property along with
the IDs for two XSync counters in the `_NET_WM_SYNC_REQUEST_COUNTER` property.

The first counter is used for basic synchronization which is a mechanism for
synchronized updates in response to `ConfigureNotify` events. The second
counter is used for extended synchronization, which in addition to being used
for synchronized updates in response to configuration changes, provides a more
general mechanism that can be used for synchronization of regular frames.

The compositor detects clients that advertise extended synchronization from
the presence of the two counters. The first counter is ignored when extended
synchronization is being used. During window resizes the compositor sends
`_NET_WM_SYNC_REQUEST` messages containing a synchronization serial number
that is used to initiate synchronized rendering. These synchronized render
requests begin three phase updates:

- serial number stored in the extended counter in the `ConfigureNotify` event.
- extended counter incremented to _an odd value mod 4_ in `begin_frame` to
  indicate rendering is in progress.
- extended counter set to to the _initial value plus 4_ in `end_frame` to
  indicating rendering is complete.

The events for a synchronized window update look like this:

- _ClientMessage_
  - _synchronize_ (`message_type == WM_PROTOCOLS &&
                    data.l[0] == _NET_WM_SYNC_REQUEST`)
    - store synchronized rendering serial number:
      - `sync_serial = data.l[2]`
- _ConfigureNotify_
  - _resize_
    - signal synchronized rendering initiated:
      - `sync_counter(dpy, extended_counter, sync_serial + 0)`
- _Expose_
  - _XFlush_
    - make sure last frame has been flushed.
  - _draw_frame_
    - run user code to draw the frame.
  - _begin_frame_
    - signal buffer contents for serial number are rendering (urgent):
      - `sync_counter(dpy, extended_counter, sync_serial + 3)`
  - _swap_buffers_
    - the frame buffer contents are volatile at this point.
  - _end_frame_
    - signal buffer contents for serial number are now complete:
      - `sync_counter(dpy, extended_counter, sync_serial + 4)`
- _ClientMessage_
  - _frame_drawn_ (`message_type == _NET_WM_FRAME_DRAWN`)
    - drawing at this point is *not safe* because frame presentation
      has not been scheduled (buffer has not been copied).
- _ClientMessage_
  - _frame_timings_ (`message_type == _NET_WM_FRAME_TIMINGS`)
    - drawing at this point is *safe* frame because frame presentation
      has been scheduled (buffer has been copied).

Normal and urgent frames are differentiated by incrementing the extended
counter by either _1_ or _3_.

- (counter % 4 == 1) - normal frame rendering in progress.
- (counter % 4 == 3) - urgent frame rendering in progress.

See the _Frame Synchronization_ link in the references for a more complete
description including disambiguation of basic and extended synchronization.

### Non-blocking IO

A prerequisite for extended synchronization with frame pacing is a non-blocking
event loop. A typical X11 application eventloop calls `XNextEvent` to read
events from the event queue but `XNextEvent` will block if the event queue is
empty. The XLib `ConnectionNumber(dpy)` interface exists to retrieve the file
descriptor for the X Server connection to enable implementation of polled IO,
but file descriptor readiness is not sufficient to determine whether calls to
`XNextEvent` will block because the XLib event queue buffers events internally.

One must precede calls to `XNextEvent` with `XEventsQueued(dpy, QueuedAlready)`
to retrieve the number of buffered events without triggering IO. If there are
no buffered events, poll the descriptor for readiness, and when ready call
`XEventsQueued(dpy, QueuedAfterReading)` to prime the event queue buffer.

Ideally Xlib would expose an `XNextEventTimed` interface that takes event
queue buffering into consideration. Note there is potential for erroneous
implementations due to misuse of `XPending`, `XSync` or `XFlush`, which
themselves can trigger IO. There are situations where it is necessary to call
`XSync` or `XFlush` but they are distinct from polling events.

### Frame Pacing

Frame pacing builds on polled IO with timeouts and high resolution timing
interfaces such as `clock_gettime(CLOCK_MONOTONIC)` to target precise moments
in time when frame draws are to be initiated so that frames can be drawn at a
constant frequency in the general case.

![frame-pacing](/images/frame-pacing.png)

Frame pacing can target a specific frequency in a steady state when there
is capacity to maintain rendering at that fixed rate. In support of frame
pacing, there also needs to be a flow control mechanism responding to render
bandwidth issues due to scene complexity and congestion control in response
to collisions between scheduled rendering and user interaction causing
reconfiguration requests that require instantaneous rendering.

The high-level event loop looks like this:

```C
    for (;;)
    {
        /* wait until next frame or next event */
        while (XEventsQueued(d, QueuedAlready) == 0)
        {
            switch (wait_frame_or_event(d, w)) {
            case event_ready: goto next;
            case frame_ready: submit_frame(d, w, frame_normal, frame_rate);
            case wait_retry: continue;
            }
        }
next:
        /* process event queue without blocking */
        while (XEventsQueued(d, QueuedAlready) > 0)
        {
            XNextEvent(d, &e);
            ...
        }
    }

```

Frame draw interval and render times are recorded in two circular buffers:

- _frame_time_buffer[31]_
  - sampled at start of draw_frame and holds the reciprocal of the frame rate.
    - `frame_delta = current_frame_start - last_frame_start`
- _render_time_buffer[31]_
  - sampled at end of swap_buffers and holds the CPU render time per frame.
    - `render_delta = current_frame_end - current_frame_start`

Presently the frame_draw delta is used to estimate the finish time of urgent
frames to schedule the start time for resumption of paced frames. These frames
may of course be further delayed by more urgent render requests. When capacity
is exceeded and synchronization is enabled i.e. timing for the last frame has
not been received, then `submit_draw` will add 2 milliseconds to the scheduled
present time and check back then to see if it is safe to submit a new frame.

### Flow Control

Flow control relates to the mechanism for response to steady state variability
in scene complexity and bandwidth using observed CPU and GPU draw timings
from `draw_frame()` and present timings from `swap_buffers()`. When observed
steady state frame rate deviates from a targeted frame rate a flow control
strategy must be employed to adapt the frame rate.

With extended synchronization, the compositor will send frame drawn messages
(_XClient.message_type_ == `_NET_WM_FRAME_DRAWN`) and frame timing messages
(_XClient.message_type_ == `_NET_WM_FRAME_TIMINGS`) that indicate when a frame
with a specific serial number was drawn and when it is to be presented.

![frame-offset](/images/frame-offset.png)

_glxsync_ employs a simple strategy whereby it will not start drawing a new
frame until it has received timings from the previously submitted frame.
This was observed to be necessary based on testing. Superficially this may
appear to make frame submission synchronous, but a clever driver is still
capable of allowing multiple frames in flight at once by returning promises
i.e. frame timings messages can be sent when the presentation schedule of a
rendered frame is known which can be as soon as when the frame render has
completed. This is a reasonable strategy for OpenGL but Vulkan rendering
will likely need something more sophisticated (`_NET_WM_SYNC_FENCES`).

![xflush-offset](/images/xflush-offset.png)

It was found that `XFlush` is needed to maintain flow and somewhat
counter-intuitively it is called before each new frame is submitted. If one
considers Nagle and flow control combined with frame pacing we can see
regularity of `XFlush` timing when issued before each frame. If we have
capacity to render at a constant frame rate, then an `XFlush(dpy)` marker
placed at the start of the frame draw will occur at a constant time offset,
subject to variable render times due to variable complexity, whereas an
`XFlush(dpy)` marker placed at the end of the frame draw would have
irregular timings needing statistics to recover the time offset.

### Congestion Control

Congestion control relates to the mechanism for handling urgent rendering
in response to floods of instantaneous `ConfigureNotify` and `Expose` events
caused by user initiated move or resize operations that require reshaping and
re-rendering of the framebuffer. These requests need special synchronization
with the window manager and compositor to be drawn at a size that precisely
matches the frame rendered by the client. The congestion control aspect to
this problem is due to these frames displacing regularly scheduled frames.

_glxsync_ employs a simple strategy whereby it will substitute a regularly
scheduled frame at the target frame rate with an urgent frame sent
instantaneously with a new schedule for the next normal frame at the measured
average frame rate over the last N frames. This allows for submission of the
next regularly scheduled frame at a rate that is measured to be sustainable.

![expose-offset](/images/expose-offset.png)

The flow control and congestion control strategies combine such that steady
state frames are issued at a constant rate with a flow control strategy of
delaying frames until timings are acknowledged, and a congestion control
strategy of urgently issuing instantaneous frames in response to configure
events and scheduling the next regular frame at the soonest time that can be
achieved based on measured short term average frame rate.

These two simple strategies were observed to avoid visible tears at the
expense of introducing a slight stutter with a shifting time offset in response
to congestion from instantaneous render requests. A more sophisticated
algorithm that targets vertical blank synchronization might attempt to skew
the time offset over some period to return to a constant rate that is in sync
with the vertical blank offset.

## Build

_glxsync_ depends on the following libraries: _X11, Xext, GLX, GL_.

```
sudo apt-get install libx11-dev libxext-dev libglx-dev libgl-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- --verbose
```

## Test

_glxsync_ frame synchronization can be disabled from the command-line.

```
$ gl2_xsync -h

usage: ./build/gl2_xsync [options]

-h, --help              print this help message
-d, --debug             enable debug messages
-t, --trace             enable trace messages
-n, --no-sync           disable frame synchronization
-f, --frame-rate <fps>  target frame rate (default 59.94)
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
