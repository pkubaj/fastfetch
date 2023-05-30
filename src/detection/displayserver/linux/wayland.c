#include "displayserver_linux.h"

#include <stdlib.h>
#include <string.h>

#ifdef FF_HAVE_WAYLAND
#include "common/library.h"
#include "common/io/io.h"
#include "common/thread.h"
#include "3rdparty/fractional_scale_v1_client_protocol.h"

#include <wayland-client.h>
#include <sys/socket.h>

typedef struct WaylandData
{
    FFDisplayServerResult* result;
    FF_LIBRARY_SYMBOL(wl_proxy_marshal_constructor_versioned)
    FF_LIBRARY_SYMBOL(wl_proxy_marshal_flags)
    FF_LIBRARY_SYMBOL(wl_proxy_add_listener)
    FF_LIBRARY_SYMBOL(wl_proxy_destroy)
    FF_LIBRARY_SYMBOL(wl_display_roundtrip)
    struct wl_display* display;
    const struct wl_interface* ffwl_output_interface;
    const struct wl_interface* ffwl_surface_interface;
    bool detectName;
} WaylandData;

typedef struct WaylandDisplay
{
    int32_t width;
    int32_t height;
    int32_t refreshRate;
    int32_t scale;
    enum wl_output_transform transform;
    FFDisplayType type;
    FFstrbuf name;
    bool detectName;
} WaylandDisplay;

#ifndef __FreeBSD__
static void waylandDetectWM(int fd, FFDisplayServerResult* result)
{
    struct ucred ucred;
    socklen_t len = sizeof(struct ucred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1)
        return;

    FFstrbuf procPath;
    ffStrbufInit(&procPath);
    ffStrbufAppendF(&procPath, "/proc/%d/cmdline", ucred.pid); //We check the cmdline for the process name, because it is not trimmed.
    ffReadFileBuffer(procPath.chars, &result->wmProcessName);
    ffStrbufSubstrBeforeFirstC(&result->wmProcessName, '\0'); //Trim the arguments
    ffStrbufSubstrAfterLastC(&result->wmProcessName, '/'); //Trim the path
    ffStrbufDestroy(&procPath);
}
#else
static void waylandDetectWM(int fd, FFDisplayServerResult* result)
{
    FF_UNUSED(fd, result);
}
#endif

static void stubListener(void* data, ...)
{
    (void) data;
}

static void waylandOutputModeListener(void* data, FF_MAYBE_UNUSED struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refreshRate)
{
    if(!(flags & WL_OUTPUT_MODE_CURRENT))
        return;

    WaylandDisplay* display = data;
    display->width = width;
    display->height = height;
    display->refreshRate = refreshRate;
}

#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
static void waylandOutputScaleListener(void* data, FF_MAYBE_UNUSED struct wl_output* output, int32_t scale)
{
    WaylandDisplay* display = data;
    display->scale = scale;
}
#endif

static void waylandOutputGeometryListener(void *data,
    FF_MAYBE_UNUSED struct wl_output *output,
    FF_MAYBE_UNUSED int32_t x,
    FF_MAYBE_UNUSED int32_t y,
    FF_MAYBE_UNUSED int32_t physical_width,
    FF_MAYBE_UNUSED int32_t physical_height,
    FF_MAYBE_UNUSED int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
    FF_UNUSED(output);

    WaylandDisplay* display = data;
    display->transform = transform;
    if(display->detectName)
    {
        if(make && strcmp(make, "unknown") != 0)
            ffStrbufAppendS(&display->name, make);
        if(model && strcmp(model, "unknown") != 0)
        {
            if(display->name.length > 0)
                ffStrbufAppendC(&display->name, '-');
            ffStrbufAppendS(&display->name, model);
        }
    }
}

#ifdef WL_OUTPUT_NAME_SINCE_VERSION
static void waylandOutputNameListener(void *data, FF_MAYBE_UNUSED struct wl_output *output, const char *name)
{
    WaylandDisplay* display = data;
    if(strncmp(name, "eDP-", strlen("eDP-")) == 0)
        display->type = FF_DISPLAY_TYPE_BUILTIN;
    else if(strncmp(name, "HDMI-", strlen("HDMI-")) == 0 || strncmp(name, "DP-", strlen("DP-")) == 0)
        display->type = FF_DISPLAY_TYPE_EXTERNAL;
}
#endif

static void waylandOutputHandler(WaylandData* wldata, struct wl_registry* registry, uint32_t name, uint32_t version)
{
    struct wl_proxy* output = wldata->ffwl_proxy_marshal_constructor_versioned(
        (struct wl_proxy*) registry,
        WL_REGISTRY_BIND,
        wldata->ffwl_output_interface,
        version,
        name,
        wldata->ffwl_output_interface->name,
        version,
        NULL);
    if(output == NULL)
        return;

    WaylandDisplay display = {
        .detectName = wldata->detectName,
        .width = 0,
        .height = 0,
        .refreshRate = 0,
        .scale = 1,
        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
        .type = FF_DISPLAY_TYPE_UNKNOWN,
    };
    ffStrbufInit(&display.name);

    struct wl_output_listener outputListener = {
        .mode = waylandOutputModeListener,
        .geometry = waylandOutputGeometryListener,

        #ifdef WL_OUTPUT_DONE_SINCE_VERSION
            .done = (void*) stubListener,
        #endif

        #ifdef WL_OUTPUT_SCALE_SINCE_VERSION
            .scale = waylandOutputScaleListener,
        #endif

        #ifdef WL_OUTPUT_NAME_SINCE_VERSION
            .name = waylandOutputNameListener,
        #endif

        #ifdef WL_OUTPUT_DESCRIPTION_SINCE_VERSION
            .description = (void*) stubListener,
        #endif
    };

    wldata->ffwl_proxy_add_listener(output, (void(**)(void)) &outputListener, &display);
    wldata->ffwl_display_roundtrip(wldata->display);
    wldata->ffwl_proxy_destroy(output);

    if(display.width <= 0 || display.height <= 0)
        return;

    static FFThreadMutex mutex = FF_THREAD_MUTEX_INITIALIZER;
    ffThreadMutexLock(&mutex);

    switch(display.transform)
    {
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_270:
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        case WL_OUTPUT_TRANSFORM_FLIPPED_270: {
            int32_t temp = display.width;
            display.width = display.height;
            display.height = temp;
            break;
        }
        default:
            break;
    }

    ffdsAppendDisplay(wldata->result,
        (uint32_t) display.width,
        (uint32_t) display.height,
        display.refreshRate / 1000.0,
        (uint32_t) (display.width / display.scale),
        (uint32_t) (display.height / display.scale),
        &display.name,
        display.type
    );

    ffThreadMutexUnlock(&mutex);
}

static void waylandFractionalScalePreferredScale(void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1, uint32_t scale)
{
    printf("%f\n", scale / 120.);
}

static void waylandFractionalScaleManagerHandler(WaylandData* wldata, struct wl_registry* registry, uint32_t name, uint32_t version)
{
    struct wl_proxy* fsManager = wldata->ffwl_proxy_marshal_constructor_versioned(
        (struct wl_proxy*) registry,
        WL_REGISTRY_BIND,
        &wp_fractional_scale_manager_v1_interface,
        version,
        name,
        wp_fractional_scale_manager_v1_interface.name,
        wp_fractional_scale_manager_v1_interface.version,
        NULL);

    if(fsManager == NULL)
        return;

    struct wl_proxy* fractionalScale = wldata->ffwl_proxy_marshal_flags(
        fsManager,
        WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE,
        &wp_fractional_scale_v1_interface,
        version,
        0,
        NULL,
        NULL); // TODO: get an surface

    struct wp_fractional_scale_v1_listener fractionalScaleListener = {
        .preferred_scale = waylandFractionalScalePreferredScale,
    };

    wldata->ffwl_proxy_add_listener(fractionalScale, (void(**)(void)) &fractionalScaleListener, wldata);
    wldata->ffwl_display_roundtrip(wldata->display);
    wldata->ffwl_proxy_destroy(fsManager);
}

static void waylandGlobalAddListener(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    WaylandData* wldata = data;

    if(strcmp(interface, wldata->ffwl_output_interface->name) == 0)
        waylandOutputHandler(wldata, registry, name, version);
    else if(strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
        waylandFractionalScaleManagerHandler(wldata, registry, name, version);
}

bool detectWayland(const FFinstance* instance, FFDisplayServerResult* result)
{
    FF_LIBRARY_LOAD(wayland, &instance->config.libWayland, false, "libwayland-client" FF_LIBRARY_EXTENSION, 1)

    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_display_connect, false)
    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_display_get_fd, false)
    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_display_dispatch, false)
    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_proxy_marshal_constructor, false)
    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_display_disconnect, false)
    FF_LIBRARY_LOAD_SYMBOL(wayland, wl_registry_interface, false)

    WaylandData data;

    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_proxy_marshal_constructor_versioned, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_proxy_marshal_flags, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_proxy_add_listener, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_proxy_destroy, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_display_roundtrip, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_output_interface, false)
    FF_LIBRARY_LOAD_SYMBOL_VAR(wayland, data, wl_surface_interface, false)

    fractional_scale_v1_types[2] = data.ffwl_surface_interface;

    data.display = ffwl_display_connect(NULL);
    if(data.display == NULL)
        return false;

    data.detectName = instance->config.displayDetectName;

    waylandDetectWM(ffwl_display_get_fd(data.display), result);

    struct wl_proxy* registry = ffwl_proxy_marshal_constructor((struct wl_proxy*) data.display, WL_DISPLAY_GET_REGISTRY, ffwl_registry_interface, NULL);
    if(registry == NULL)
    {
        ffwl_display_disconnect(data.display);
        return false;
    }

    data.result = result;

    struct wl_registry_listener registry_listener = {
        .global = waylandGlobalAddListener,
        .global_remove = (void*) stubListener
    };

    data.ffwl_proxy_add_listener(registry, (void(**)(void)) &registry_listener, &data);
    ffwl_display_dispatch(data.display);
    data.ffwl_display_roundtrip(data.display);

    data.ffwl_proxy_destroy(registry);
    ffwl_display_disconnect(data.display);

    //We successfully connected to wayland and detected the display.
    //So we can set set the session type to wayland.
    //This is used as an indicator that we are running wayland by the x11 backends.
    ffStrbufSetS(&result->wmProtocolName, FF_WM_PROTOCOL_WAYLAND);
    return true;
}
#endif

void ffdsConnectWayland(const FFinstance* instance, FFDisplayServerResult* result)
{
    //Wayland requires this to be set
    if(getenv("XDG_RUNTIME_DIR") == NULL)
        return;

    #ifdef FF_HAVE_WAYLAND
        if(detectWayland(instance, result))
            return;
    #else
        FF_UNUSED(instance);
    #endif

    const char* xdgSessionType = getenv("XDG_SESSION_TYPE");

    //If XDG_SESSION_TYPE is set, and doesn't contain "wayland", we are probably not running in a wayland session.
    if(xdgSessionType != NULL && strcasecmp(xdgSessionType, "wayland") != 0)
        return;

    //If XDG_SESSION_TYPE is not set, check if WAYLAND_DISPLAY or WAYLAND_SOCKET is set.
    //If not, there is no indicator for a wayland session
    if(xdgSessionType == NULL && getenv("WAYLAND_DISPLAY") == NULL && getenv("WAYLAND_SOCKET") == NULL)
        return;

    //We are probably running a wayland compositor at this point,
    //but fastfetch was compiled without the required library, or loading the library failed.
    ffStrbufSetS(&result->wmProtocolName, FF_WM_PROTOCOL_WAYLAND);
}
