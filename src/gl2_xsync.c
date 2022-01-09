/*
 * gl2_xsync
 *
 * PLEASE LICENSE 12/2021, Michael Clark <michaeljclark@mac.com>
 *
 * All rights to this work are granted for all purposes, with exception of
 * author's implied right of copyright to defend the free use of this work.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <libgen.h>

#define __USE_GNU
#include <sys/poll.h>
#include <sys/stat.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/sync.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include "linmath.h"
#include "gl2_util.h"

typedef unsigned long ulong;

#define array_size(arr) ((sizeof(arr)/sizeof(arr[0])))

#define Debug(...) { if (debug) fprintf(stdout, __VA_ARGS__); }
#define Trace(...) { if (trace) fprintf(stdout, __VA_ARGS__); }
#define Panic(...) { fprintf(stderr, __VA_ARGS__); exit(9); }

static int help, debug, trace;

/*
 * extended frame synchronization
 */

static Atom WM_PROTOCOLS;
static Atom _NET_SUPPORTED;
static Atom _NET_WM_MOVERESIZE;
static Atom _NET_WM_SYNC_REQUEST;
static Atom _NET_WM_SYNC_REQUEST_COUNTER;
static Atom _NET_WM_FRAME_DRAWN;
static Atom _NET_WM_FRAME_TIMINGS;
static Atom _NET_WM_PING;

static int request_extended_sync;
static int configure_extended_sync;

static ulong current_sync_serial;
static ulong request_sync_serial;
static ulong configure_sync_serial;
static ulong inflight_sync_serial;
static ulong drawn_sync_serial;
static ulong timing_sync_serial;

static XSyncCounter update_counter;
static XSyncCounter extended_counter;

static Atom *supported_atoms;
static long num_supported_atoms;
static int xsync_event_base;
static int xsync_error_base;

static int have_xsync_extension;
static int have_net_supported;
static int have_wm_moveresize;
static int use_frame_sync = 1;

/*
 * glcube demo
 */

typedef struct model_object {
    GLuint vbo;
    GLuint ibo;
    vertex_buffer vb;
    index_buffer ib;
    mat4x4 m, v;
} model_object_t;

static const char* frag_shader_filename = "shaders/cube.fsh";
static const char* vert_shader_filename = "shaders/cube.vsh";

static float t = 0.f;
static bool animation = 1;
static GLuint program;
static mat4x4 v, p;
static model_object_t mo[1];
static float frame_rate = 29.97;
static ulong frame_number;
static long last_draw_time;
static long next_draw_time;
static long current_time;
static long delta_time;
static long render_time;
static int width = 500, height = 500;
static int current_width, current_height;

typedef struct {
    long sum;
    long count;
    long offset;
    long samples[31];
} circular_buffer;

static circular_buffer frame_time_buffer;
static circular_buffer render_time_buffer;

static void circular_buffer_add(circular_buffer *buffer, long new_value)
{
    long old_value = buffer->samples[buffer->offset];
    buffer->samples[buffer->offset] = new_value;
    buffer->sum += new_value - old_value;
    buffer->count++;
    buffer->offset++;
    if (buffer->offset >= array_size(buffer->samples)) {
        buffer->offset = 0;
    }
}

static long circular_buffer_average(circular_buffer *buffer)
{
    if (buffer->count == 0) {
        return -1;
    } else if (buffer->count < array_size(buffer->samples)) {
        return buffer->sum / buffer->count;
    } else {
        return buffer->sum / array_size(buffer->samples);
    }
}

static void model_object_init(model_object_t *mo)
{
    vertex_buffer_init(&mo->vb);
    index_buffer_init(&mo->ib);
}

static void model_object_freeze(model_object_t *mo)
{
    buffer_object_create(&mo->vbo, GL_ARRAY_BUFFER, &mo->vb);
    buffer_object_create(&mo->ibo, GL_ELEMENT_ARRAY_BUFFER, &mo->ib);
}

static void model_object_cube(model_object_t *mo, float s, vec4f col)
{
    float r = col.r, g = col.g, b = col.b, a = col.a;

    const float f[6][3][3] = {
        /* front */  { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, },
        /* right */  { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, },
        /* top */    { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, },
        /* rear */   { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0,-1 }, },
        /* left */   { { 0, 0,-1 }, { 0, 1, 0 }, { 1, 0, 0 }, },
        /* bottom */ { { 1, 0, 0 }, { 0, 0,-1 }, { 0, 1, 0 }, },
    };

    const vertex t[4] = {
        { { -s,  s,  s }, { 0, 0, 1 }, { 0, 1 }, { r, g, b, a } },
        { { -s, -s,  s }, { 0, 0, 1 }, { 0, 0 }, { r, g, b, a } },
        { {  s, -s,  s }, { 0, 0, 1 }, { 1, 0 }, { r, g, b, a } },
        { {  s,  s,  s }, { 0, 0, 1 }, { 1, 1 }, { r, g, b, a } },
    };

    const float colors[6][4] = {
        { 1.0f, 0.0f, 0.0f, 1 }, /* red */
        { 0.0f, 1.0f, 0.0f, 1 }, /* green */
        { 0.0f, 0.0f, 1.0f, 1 }, /* blue */
        { 0.0f, 0.7f, 0.7f, 1 }, /* cyan */
        { 0.7f, 0.0f, 0.7f, 1 }, /* magenta */
        { 0.7f, 0.7f, 0.0f, 1 }, /* yellow */
    };

    uint idx = vertex_buffer_count(&mo->vb);
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 4; j++) {
            vertex v;
            v.pos.x = f[i][0][0]*t[j].pos.x + f[i][0][1]*t[j].pos.y + f[i][0][2]*t[j].pos.z;
            v.pos.y = f[i][1][0]*t[j].pos.x + f[i][1][1]*t[j].pos.y + f[i][1][2]*t[j].pos.z;
            v.pos.z = f[i][2][0]*t[j].pos.x + f[i][2][1]*t[j].pos.y + f[i][2][2]*t[j].pos.z;
            v.norm.x = f[i][0][0]*t[j].norm.x + f[i][0][1]*t[j].norm.y + f[i][0][2]*t[j].norm.z;
            v.norm.y = f[i][1][0]*t[j].norm.x + f[i][1][1]*t[j].norm.y + f[i][1][2]*t[j].norm.z;
            v.norm.z = f[i][2][0]*t[j].norm.x + f[i][2][1]*t[j].norm.y + f[i][2][2]*t[j].norm.z;
            v.uv.x = t[j].uv.x;
            v.uv.y = t[j].uv.y;
            v.col.r = colors[i][0];
            v.col.g = colors[i][1];
            v.col.b = colors[i][2];
            v.col.a = colors[i][3];
            vertex_buffer_add(&mo->vb, v);
        }
    }
    index_buffer_add_primitves(&mo->ib, primitive_topology_quads, 6, idx);
}

static float degrees_to_radians(float a) { return a * M_PI / 180.0f; }

static void model_matrix_transform(mat4x4 m, vec3 scale, vec3 trans, vec3 rot)
{
    mat4x4_identity(m);
    mat4x4_scale_aniso(m, m, scale[0], scale[1], scale[2]);
    mat4x4_translate_in_place(m, trans[0], trans[1], trans[2]);
    mat4x4_rotate_X(m, m, degrees_to_radians(rot[0]));
    mat4x4_rotate_Y(m, m, degrees_to_radians(rot[1]));
    mat4x4_rotate_Z(m, m, degrees_to_radians(rot[2]));
}

static void model_update_matrices(model_object_t *mo)
{
    uniform_matrix_4fv("u_model", (const GLfloat *)mo[0].m);
    uniform_matrix_4fv("u_view", (const GLfloat *)mo[0].v);
}

static void model_object_draw(model_object_t *mo)
{
    glBindBuffer(GL_ARRAY_BUFFER, mo->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mo->ibo);
    vertex_array_pointer("a_pos", 3, GL_FLOAT, 0, sizeof(vertex), offsetof(vertex,pos));
    vertex_array_pointer("a_normal", 3, GL_FLOAT, 0, sizeof(vertex), offsetof(vertex,norm));
    vertex_array_pointer("a_uv", 2, GL_FLOAT, 0, sizeof(vertex), offsetof(vertex,uv));
    vertex_array_pointer("a_color", 4, GL_FLOAT, 0, sizeof(vertex), offsetof(vertex,col));
    glDrawElements(GL_TRIANGLES, (GLsizei)mo->ib.count, GL_UNSIGNED_INT, (void*)0);
}

void reshape(int width, int height)
{
    float h = (float)height / (float)width;

    glViewport(0, 0, (GLint) width, (GLint) height);
    mat4x4_frustum(p, -1., 1., -h, h, 5.f, 1e9f);
    uniform_matrix_4fv("u_projection", (const GLfloat *)p);
}

static void draw_frame()
{
    if (current_width != width || current_height != height) {
        reshape(width, height);
        current_width = width;
        current_height = height;
    }

    if (animation) {
        /* avoid overflow due to deltas > one second */
        t += (delta_time % 1000000u) * 60.0f / 1e6f;
    }

    glClearColor(0.11f, 0.54f, 0.54f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    vec3 model_scale = { 1.0f, 1.0f, 1.0f };
    vec3 model_trans = { 0.0f, 0.0f, 0.0f };
    vec3 model_rot = { 0.25f * t, 0.5f * t, 0.75f * t };
    vec3 view_scale = { 1.0f, 1.0f, 1.0f };
    vec3 view_trans = { 0.0f, 0.0f, -32.0f };
    vec3 view_rot = { 0.0f, 0.0f, 0.0f };

    model_matrix_transform(mo[0].m, model_scale, model_trans, model_rot);
    model_matrix_transform(mo[0].v, view_scale, view_trans, view_rot);
    model_update_matrices(&mo[0]);
    model_object_draw(&mo[0]);
}

static void init()
{
    GLuint shaders[2];

    /* shader program */
    shaders[0] = compile_shader(GL_VERTEX_SHADER, vert_shader_filename);
    shaders[1] = compile_shader(GL_FRAGMENT_SHADER, frag_shader_filename);
    program = link_program(shaders, 2, NULL);

    /* create cube vertex and index buffers and buffer objects */
    model_object_init(&mo[0]);
    model_object_cube(&mo[0], 3.f, (vec4f){0.3f, 0.3f, 0.3f, 1.f});
    model_object_freeze(&mo[0]);

    /* set light position uniform */
    glUseProgram(program);
    uniform_3f("u_lightpos", 5.f, 5.f, 10.f);

    /* enable OpenGL capabilities */
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

/*
 * X11 events
 */

static const char* xevent_names[] = {
  [KeyPress] = "KeyPress",
  [KeyRelease] = "KeyRelease",
  [ButtonPress] = "ButtonPress",
  [ButtonRelease] = "ButtonRelease",
  [MotionNotify] = "MotionNotify",
  [EnterNotify] = "EnterNotify",
  [LeaveNotify] = "LeaveNotify",
  [FocusIn] = "FocusIn",
  [FocusOut] = "FocusOut",
  [KeymapNotify] = "KeymapNotify",
  [Expose] = "Expose",
  [GraphicsExpose] = "GraphicsExpose",
  [NoExpose] = "NoExpose",
  [VisibilityNotify] = "VisibilityNotify",
  [CreateNotify] = "CreateNotify",
  [DestroyNotify] = "DestroyNotify",
  [UnmapNotify] = "UnmapNotify",
  [MapNotify] = "MapNotify",
  [MapRequest] = "MapRequest",
  [ReparentNotify] = "ReparentNotify",
  [ConfigureNotify] = "ConfigureNotify",
  [ConfigureRequest] = "ConfigureRequest",
  [GravityNotify] = "GravityNotify",
  [ResizeRequest] = "ResizeRequest",
  [CirculateNotify] = "CirculateNotify",
  [CirculateRequest] = "CirculateRequest",
  [PropertyNotify] = "PropertyNotify",
  [SelectionClear] = "SelectionClear",
  [SelectionRequest] = "SelectionRequest",
  [SelectionNotify] = "SelectionNotify",
  [ColormapNotify] = "ColormapNotify",
  [ClientMessage] = "ClientMessage",
  [MappingNotify] = "MappingNotify",
  [GenericEvent] = "GenericEvent"
};

/*
 * GLX Visual
 */

static XVisualInfo* find_glx_visual(Display *d, int s)
{
    int attribs[16];
    int i = 0;

    /* Singleton attributes. */
    attribs[i++] = GLX_RGBA;
    attribs[i++] = GLX_DOUBLEBUFFER;

    /* Key/value attributes. */
    attribs[i++] = GLX_RED_SIZE;
    attribs[i++] = 8;
    attribs[i++] = GLX_GREEN_SIZE;
    attribs[i++] = 8;
    attribs[i++] = GLX_BLUE_SIZE;
    attribs[i++] = 8;
    attribs[i++] = GLX_ALPHA_SIZE;
    attribs[i++] = 8;
    attribs[i++] = GLX_DEPTH_SIZE;
    attribs[i++] = 16;

    attribs[i++] = None;

    return glXChooseVisual(d, s, attribs);
}

/*
 * XSync extended frame synchronization
 */

static int init_atoms(Display *d)
{
    WM_PROTOCOLS = XInternAtom(d, "WM_PROTOCOLS", False);
    _NET_SUPPORTED = XInternAtom(d, "_NET_SUPPORTED", False);
    _NET_WM_MOVERESIZE = XInternAtom(d, "_NET_WM_MOVERESIZE", False);
    _NET_WM_SYNC_REQUEST = XInternAtom(d, "_NET_WM_SYNC_REQUEST", False);
    _NET_WM_SYNC_REQUEST_COUNTER = XInternAtom(d, "_NET_WM_SYNC_REQUEST_COUNTER", False);
    _NET_WM_FRAME_DRAWN = XInternAtom(d, "_NET_WM_FRAME_DRAWN", False);
    _NET_WM_FRAME_TIMINGS = XInternAtom(d, "_NET_WM_FRAME_TIMINGS", False);
    _NET_WM_PING = XInternAtom(d, "_NET_WM_PING", False);

    assert(WM_PROTOCOLS);
    assert(_NET_SUPPORTED);
    assert(_NET_WM_MOVERESIZE);
    assert(_NET_WM_SYNC_REQUEST);
    assert(_NET_WM_SYNC_REQUEST_COUNTER);
    assert(_NET_WM_FRAME_DRAWN);
    assert(_NET_WM_FRAME_TIMINGS);
    assert(_NET_WM_PING);
}

static ulong get_time_microseconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000l + ts.tv_nsec/1000l;
}

static void update_wm_supported(Display *d, int s)
{
    Atom type;
    int format;
    long bytes_after;

    XFree(supported_atoms);
    XGetWindowProperty(d, RootWindow(d, s), _NET_SUPPORTED,
                       0, 64, False, XA_ATOM, &type, &format,
                       &num_supported_atoms, &bytes_after,
                       (unsigned char **)&supported_atoms);

    have_net_supported = (type == XA_ATOM && supported_atoms != NULL &&
                      num_supported_atoms > 0);

    for (int i = 0; i < num_supported_atoms; i++) {
        Trace("Atom: %s (%ld)\n",
            XGetAtomName(d, supported_atoms[i]), supported_atoms[i]);
    }
}

static int check_wm_supported(Atom atom)
{
    if (!have_net_supported) return 0;

    for (int i = 0; i < num_supported_atoms; i++) {
        if (supported_atoms[i] == atom)return 1;
    }
}

static void update_wm_protocols(Display *d, Window w)
{
    Atom protocols[] = { _NET_WM_PING, _NET_WM_SYNC_REQUEST };

    XSetWMProtocols(d, w, protocols, array_size(protocols));
}

static void update_wm_hints(Display *d, Window w)
{
    XWMHints wm_hints;

    wm_hints.flags = StateHint | InputHint;
    wm_hints.input = True;
    wm_hints.initial_state = NormalState;

    XSetWMHints(d, w, &wm_hints);
}

static void sync_init(Display *d, Window w)
{
    XSyncValue value;
    XID counters[2];
    int major, minor;

    have_xsync_extension =
        (XSyncQueryExtension(d, &xsync_event_base, &xsync_error_base) &&
         XSyncInitialize(d, &major, &minor));

    if (!have_xsync_extension) return;

    XSyncIntToValue (&value, 0);
    update_counter = XSyncCreateCounter(d, value);
    extended_counter = XSyncCreateCounter(d, value);

    counters[0] = update_counter;
    counters[1] = extended_counter;

    XChangeProperty(d, w, _NET_WM_SYNC_REQUEST_COUNTER, XA_CARDINAL,
           32, PropModeReplace, (unsigned char*)counters, 2);
}

static void sync_counter(Display *d, XSyncCounter counter, ulong value)
{
    XSyncValue sync_value;

    if (!have_xsync_extension) return;

    XSyncIntsToValue(&sync_value, value & 0xFFFFFFFF, value >> 32);
    XSyncSetCounter(d, counter, sync_value);
}

typedef enum { frame_normal, frame_urgent } frame_disposition;

/*
 * inform the compositor that we are starting to draw a frame
 */
static void begin_frame(Display *d, Window w, frame_disposition disposition)
{
    /* extended synchronization in response to _NET_WM_SYNC_REQUEST */
    if (configure_sync_serial != 0 && configure_extended_sync)
    {
        current_sync_serial = configure_sync_serial;
        configure_sync_serial = 0;
    }
    /* advance frame to next multiple of 4 */
    if ((current_sync_serial & 3) != 0) {
        current_sync_serial = (current_sync_serial + 3) & ~3;
    }
    /* advance frame to odd value, 1 = normal, 3 = urgent */
    inflight_sync_serial = current_sync_serial + 4;
    current_sync_serial += (disposition == frame_urgent ? 3 : 1);
    sync_counter(d, extended_counter, current_sync_serial);
}

/*
 * inform the compositor that we have finished drawing a frame
 */
static void end_frame(Display *d, Window w)
{
    /* extended synchronization */
    if ((current_sync_serial & 3) == 1) {
        current_sync_serial += 3;
        sync_counter(d, extended_counter, current_sync_serial);
    } else if ((current_sync_serial & 3) == 3) {
        current_sync_serial += 1;
        sync_counter(d, extended_counter, current_sync_serial);
    }

    /* basic synchronization */
    if (configure_sync_serial != 0 && configure_extended_sync) {
        sync_counter(d, update_counter, configure_sync_serial);
        configure_sync_serial = 0;
        configure_extended_sync = 0;
    }
}

/*
 * submit frame for rendering
 */
static void submit_frame(Display *d, Window w, frame_disposition disposition,
    float target_frame_rate)
{
    current_time = get_time_microseconds();

    /* tearing may result if frames are submitted before receiving timings
     * for inflight frames submitted in response to synchronization requests */
    if (timing_sync_serial > 0 && timing_sync_serial < inflight_sync_serial)
    {
        XSync(d, False);
        next_draw_time = current_time + 2000;
        Trace("[%lu/%ld] Delay: disposition=%s timing_sync_serial=%lu "
            "inflight_sync_serial=%lu\n", frame_number, current_time,
            disposition == frame_urgent ? "urgent" : "normal",
            timing_sync_serial, inflight_sync_serial);
        return;
    }

    Trace("[%lu/%ld] FrameBegin: delta_time=%ld sync_serial=%lu "
        "frame_avg_time=%ld render_avg_time=%ld\n",
        frame_number, current_time, delta_time, current_sync_serial,
        circular_buffer_average(&frame_time_buffer),
        circular_buffer_average(&render_time_buffer));

    if (last_draw_time) {
        delta_time = current_time - last_draw_time;
        circular_buffer_add(&frame_time_buffer, delta_time);
    }
    last_draw_time = current_time;
    next_draw_time = current_time + (long)(1e6f / target_frame_rate);

    frame_number++;

    XFlush(d);
    draw_frame();
    begin_frame(d, w, frame_normal);
    glXSwapBuffers(d, w);
    end_frame(d, w);

    current_time = get_time_microseconds();
    render_time = current_time - last_draw_time;
    circular_buffer_add(&render_time_buffer, render_time);

    Trace("[%lu/%ld] FrameEnd: delta_time=%ld sync_serial=%lu "
        "frame_avg_time=%ld render_avg_time=%ld\n",
        frame_number, current_time, delta_time, current_sync_serial,
        circular_buffer_average(&frame_time_buffer),
        circular_buffer_average(&render_time_buffer));
}

/*
 * poll X11 event queue with timeout
 */
static int poll_event_queue(Display *d, long timeout)
{
    struct pollfd pfds[1] = {
        { .fd = ConnectionNumber(d), .events = POLLIN }
    };
    struct timespec pts = {
        .tv_sec = timeout / 1000000,
        .tv_nsec = (timeout % 1000000) * 1000
    };

    return ppoll(pfds, array_size(pfds), &pts, NULL);
}

typedef enum { wait_retry, frame_ready, event_ready } wait_status;

/*
 * wait for next frame
 */

wait_status wait_frame_or_event(Display *d, Window w)
{
    current_time = get_time_microseconds();

    while (current_time < next_draw_time) {

        long timeout = next_draw_time - current_time;

        Trace("[%lu/%ld] Poll: timeout=%ld\n",
            frame_number, current_time, timeout);

        int ret = poll_event_queue(d, timeout);
        if (ret < 0 && errno == EINTR) {
            return wait_retry;
        } else if (ret < 0) {
            Panic("poll error: %s\n", strerror(errno));
        } else if (ret == 0) {
            return frame_ready;
        } else if (ret == 1 && XEventsQueued(d, QueuedAfterReading) > 0) {
            return event_ready;
        }

        current_time = get_time_microseconds();
    }

    /* we can't allow XNextEvent to block so we must always check descriptor
     * readiness then prime the in-memory queue if returning 'event_ready' */
    int ret = poll_event_queue(d, 0);
    if (ret == 1 && XEventsQueued(d, QueuedAfterReading) > 0) {
        return event_ready;
    } else {
        return frame_ready;
    }
}

/*
 * process X11 event
 */
void process_event(Display *d, Window w)
{
    XEvent e;
    long *l;

    XNextEvent(d, &e);
    current_time = get_time_microseconds();

    switch (e.type)
    {
        case Expose:
        {
            Trace("[%lu/%ld] Event: Expose serial=%lu count=%d\n",
                frame_number, current_time, e.xexpose.serial, e.xexpose.count);

            /* cap frame rate of expose frames to measured frame rate. */
            long frame_time_avg = circular_buffer_average(&frame_time_buffer);
            float measured_frame_rate = 1e6f / frame_time_avg;
            float cap_frame_rate =
                frame_time_avg > 0 && frame_rate > measured_frame_rate ?
                measured_frame_rate : frame_rate;

            submit_frame(d, w, frame_urgent, cap_frame_rate);
            break;
        }
        case ConfigureNotify:
        {
            width = e.xconfigure.width;
            height = e.xconfigure.height;

            configure_sync_serial = request_sync_serial;
            configure_extended_sync = request_extended_sync;
            request_sync_serial = 0;
            request_extended_sync = 0;
            sync_counter(d, extended_counter, current_sync_serial);

            Trace("[%lu/%ld] Event: ConfigureNotify serial=%lu size=%dx%d "
                "current_sync_serial=%lu request_sync_serial=%lu extended_sync=%d\n",
                frame_number, current_time, e.xconfigure.serial,
                width, height, current_sync_serial, configure_sync_serial,
                configure_extended_sync);
            break;
        }
        case ClientMessage:
        {
            l = e.xclient.data.l;
            if (e.xclient.message_type == WM_PROTOCOLS && l[0] == _NET_WM_PING)
            {
                ulong timestamp = l[1], window = l[2];

                e.xclient.window = DefaultRootWindow(d);
                XSendEvent(d, DefaultRootWindow(d), False,
                    SubstructureRedirectMask | SubstructureNotifyMask, &e);

                Trace("[%lu/%ld] Event: ClientMessage: _NET_WM_PING "
                    "serial=%lu timestamp=%lu window=%lu\n",
                    frame_number, current_time, e.xclient.serial,
                    timestamp, window);
            }
            else if (e.xclient.message_type == WM_PROTOCOLS && l[0] == _NET_WM_SYNC_REQUEST)
            {
                request_sync_serial = l[2] + ((long)l[3] << 32);
                request_extended_sync = l[4] != 0;

                Trace("[%lu/%ld] Event: ClientMessage: _NET_WM_SYNC_REQUEST "
                    "serial=%lu sync_serial=%lu extended_sync=%d\n",
                    frame_number, current_time, e.xclient.serial,
                    request_sync_serial, request_extended_sync);
            }
            else if (e.xclient.message_type == _NET_WM_FRAME_DRAWN)
            {
                long drawn_time =  ((long)l[3] << 32) | l[2];
                ulong sync_serial = ((long)l[1] << 32) | l[0];

                if (sync_serial > drawn_sync_serial) {
                    drawn_sync_serial = sync_serial;
                }

                Trace("[%lu/%ld] Event: ClientMessage: _NET_WM_FRAME_DRAWN "
                    "serial=%lu sync_serial=%lu drawn_time=%ld\n",
                    frame_number, current_time, e.xclient.serial,
                    sync_serial, drawn_time);
            }
            else if (e.xclient.message_type == _NET_WM_FRAME_TIMINGS)
            {
                int presentation_offset = l[2];
                int refresh_interval = l[3];
                int frame_delay = l[4];
                ulong sync_serial = ((long)l[1] << 32) | l[0];

                if (sync_serial > timing_sync_serial) {
                    timing_sync_serial = sync_serial;
                }

                Trace("[%lu/%ld] Event: ClientMessage: _NET_WM_FRAME_TIMINGS "
                    "serial=%lu sync_serial=%lu presentation_offset=%u "
                    "refresh_interval=%u frame_delay=%u\n",
                    frame_number, current_time, e.xclient.serial,
                    sync_serial, presentation_offset,
                    refresh_interval, frame_delay);
            }
            break;
        }
        case PropertyNotify:
        {
            Trace("[%lu/%ld] Event: PropertyNotify: %s\n",
                frame_number, current_time, XGetAtomName(d, e.xproperty.atom));
            break;
        }
        default:
        {
            if (e.type < array_size(xevent_names)) {
                Trace("[%lu/%ld] Event: %s\n",
                    frame_number, current_time, xevent_names[e.type]);
            } else {
                Trace("[%lu/%ld] Event: (unknown-type=%d)\n",
                    frame_number, current_time, e.type);
            }
            break;
        }
    }
}

/*
 * gl2_xsync demo
 */

void app_run(char* argv0)
{
    Display *d;
    Window w;
    int s;
    XVisualInfo *visinfo;
    GLXContext ctx;

    d = XOpenDisplay(NULL);
    if (d == NULL) {
        Panic("Cannot open display\n");
    }

    s = DefaultScreen(d);
    visinfo = find_glx_visual(d, s);
    if (!visinfo) {
        Panic("Cannot get glx visual\n");
    }

    init_atoms(d);

    XSetWindowAttributes wa = { 0 };
    wa.colormap = XCreateColormap(d, RootWindow(d, s), visinfo->visual, AllocNone);
    wa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                    PointerMotionMask | ButtonPressMask | ButtonReleaseMask |
                    ExposureMask | FocusChangeMask | VisibilityChangeMask |
                    EnterWindowMask | LeaveWindowMask | PropertyChangeMask;

    w = XCreateWindow(d, RootWindow(d, s),
                      0, 0,   // Position
                      width, height,
                      0,      // Border width
                      visinfo->depth,  // Color depth
                      InputOutput,
                      visinfo->visual,
                      CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                      &wa);

    ctx = glXCreateContext(d, visinfo, NULL, True);

    if (use_frame_sync) {
        sync_init(d, w);
        update_wm_supported(d, s);
        update_wm_protocols(d, w);
        update_wm_hints(d, w);
        have_wm_moveresize = check_wm_supported(_NET_WM_MOVERESIZE);
    }

    Debug("Capabilities: xsync_extension=%d net_supported=%d wm_moveresize=%d\n",
        have_xsync_extension, have_net_supported, have_wm_moveresize);

    XStoreName(d, w, basename(argv0));
    XMapWindow(d, w);
    glXMakeCurrent(d, w, ctx);
    XSelectInput(d, w, wa.event_mask);

    init();

    /* draw first frame immediately */
    next_draw_time = current_time = get_time_microseconds();

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
            process_event(d, w);
        }
    }

    XFree(supported_atoms);
    glXDestroyContext(d, ctx);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
}

int print_usage_and_exit(const char *argv0)
{
    fprintf(stderr, "\nusage: %s [options]\n\n"
                    "-h, --help              print this help message\n"
                    "-d, --debug             enable debug messages\n"
                    "-t, --trace             enable trace messages\n"
                    "-n, --no-sync           disable frame synchronization\n"
                    "-f, --frame-rate <fps>  target frame rate (default %.2f)\n\n",
        argv0, frame_rate);
    exit(9);
}

static bool match_option(const char *arg, const char *opt, const char *longopt)
{
    return strcmp(arg, opt) == 0 || strcmp(arg, longopt) == 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (match_option(argv[i], "-h", "--help")) {
            help = 1;
        } else if (match_option(argv[i], "-t", "--trace")) {
            debug = trace = 1;
        } else if (match_option(argv[i], "-d", "--debug")) {
            debug = 1;
        } else if (match_option(argv[i], "-n", "--no-sync")) {
            use_frame_sync = 0;
        } else if (match_option(argv[i], "-f", "--frame-rate") && i+1 < argc) {
            frame_rate = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            help = 1;
        }
    }

    if (help) print_usage_and_exit(argv[0]);

    app_run(argv[0]);

    return 0;
}
