#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_FPS 60
#define DEFAULT_PRESET "ultrafast"
#define SELECT_TOLERANCE 2

struct app_paths {
    char state_dir[PATH_MAX];
    char pidfile[PATH_MAX];
    char indicator_pidfile[PATH_MAX];
    char outfilefile[PATH_MAX];
    char logfile[PATH_MAX];
};

struct geometry {
    int x;
    int y;
    int w;
    int h;
};

struct start_options {
    int fps;
    const char *display;
    const char *output;
    const char *geometry_text;
};

static unsigned long alloc_indicator_color(Display *dpy, int screen);
static int launch_stop_command(void);
static void make_window_clickthrough(Display *dpy, Window root, Window win, unsigned width, unsigned height);
static Window create_overlay_window(Display *dpy, Window root,
                                    int x, int y, unsigned width, unsigned height,
                                    unsigned long pixel, const char *name);

static void usage(FILE *out)
{
    fprintf(out,
        "quickrec - quick screen region recorder\n\n"
        "Usage:\n"
        "  quickrec                 Toggle recording\n"
        "  quickrec start [options] Start a new region recording\n"
        "  quickrec stop            Stop the current recording\n"
        "  quickrec toggle [opts]   Stop if running, otherwise start\n"
        "  quickrec status          Show recorder status\n"
        "  quickrec help            Show this help\n\n"
        "Start options:\n"
        "  -f, --fps N             Capture framerate (default: %d)\n"
        "  -g, --geometry SPEC     Use WxH+X+Y instead of interactive selection\n"
        "  -o, --output PATH       Output file (.mp4)\n"
        "\n"
        "Environment:\n"
        "  DISPLAY                 X11 display to capture (required)\n"
        "  QUICKREC_FPS            Default FPS override\n"
        "  QUICKREC_OUTPUT_DIR     Default output directory override\n",
        DEFAULT_FPS);
}

static int path_join(char *dst, size_t dstsz, const char *a, const char *b)
{
    int n;

    if (!a || !*a || !b || !*b) {
        errno = EINVAL;
        return -1;
    }

    if (a[strlen(a) - 1] == '/')
        n = snprintf(dst, dstsz, "%s%s", a, b);
    else
        n = snprintf(dst, dstsz, "%s/%s", a, b);

    if (n < 0 || (size_t)n >= dstsz) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;

    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }

    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;

    return 0;
}

static int ensure_parent_dir(const char *path)
{
    char tmp[PATH_MAX];
    char *slash;

    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    slash = strrchr(tmp, '/');
    if (!slash)
        return 0;
    if (slash == tmp)
        return 0;

    *slash = '\0';
    return mkdir_p(tmp);
}

static int write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    if (fputs(text, fp) == EOF || fputc('\n', fp) == EOF) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0)
        return -1;
    return 0;
}

static int read_line_file(const char *path, char *buf, size_t bufsz)
{
    FILE *fp = fopen(path, "r");
    size_t len;

    if (!fp)
        return -1;
    if (!fgets(buf, (int)bufsz, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return 0;
}

static int read_pid_file(const char *path, pid_t *pid)
{
    char buf[64];
    char *end;
    long value;

    if (read_line_file(path, buf, sizeof(buf)) < 0)
        return -1;

    errno = 0;
    value = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0' || value <= 0) {
        errno = EINVAL;
        return -1;
    }

    *pid = (pid_t)value;
    return 0;
}

static int process_alive(pid_t pid)
{
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

static int process_cmdline_contains(pid_t pid, const char *needle)
{
    char path[64];
    char buf[4096];
    ssize_t n;
    int fd;

    if (!needle || !*needle)
        return 0;

    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;

    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }
    buf[n] = '\0';

    return strstr(buf, needle) != NULL;
}

static int command_exists(const char *name)
{
    char *path, *copy, *saveptr, *dir;
    char candidate[PATH_MAX];

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;

    path = getenv("PATH");
    if (!path || !*path)
        return 0;

    copy = strdup(path);
    if (!copy)
        return 0;

    for (dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
        if (*dir == '\0')
            dir = ".";
        if (path_join(candidate, sizeof(candidate), dir, name) == 0 && access(candidate, X_OK) == 0) {
            free(copy);
            return 1;
        }
    }

    free(copy);
    return 0;
}

static int init_paths(struct app_paths *paths)
{
    const char *home = getenv("HOME");
    const char *xdg_state = getenv("XDG_STATE_HOME");
    char state_root[PATH_MAX];

    if (!home || !*home) {
        fprintf(stderr, "quickrec: HOME is not set\n");
        return -1;
    }

    if (xdg_state && *xdg_state) {
        if (snprintf(state_root, sizeof(state_root), "%s", xdg_state) >= (int)sizeof(state_root)) {
            fprintf(stderr, "quickrec: XDG_STATE_HOME path is too long\n");
            return -1;
        }
    } else {
        if (snprintf(state_root, sizeof(state_root), "%s/.local/state", home) >= (int)sizeof(state_root)) {
            fprintf(stderr, "quickrec: state path is too long\n");
            return -1;
        }
    }

    if (mkdir_p(state_root) < 0) {
        fprintf(stderr, "quickrec: cannot create %s: %s\n", state_root, strerror(errno));
        return -1;
    }

    if (path_join(paths->state_dir, sizeof(paths->state_dir), state_root, "quickrec") < 0 ||
        path_join(paths->pidfile, sizeof(paths->pidfile), paths->state_dir, "ffmpeg.pid") < 0 ||
        path_join(paths->indicator_pidfile, sizeof(paths->indicator_pidfile), paths->state_dir, "indicator.pid") < 0 ||
        path_join(paths->outfilefile, sizeof(paths->outfilefile), paths->state_dir, "current_output") < 0 ||
        path_join(paths->logfile, sizeof(paths->logfile), paths->state_dir, "ffmpeg.log") < 0) {
        fprintf(stderr, "quickrec: state path is too long\n");
        return -1;
    }

    if (mkdir_p(paths->state_dir) < 0) {
        fprintf(stderr, "quickrec: cannot create %s: %s\n", paths->state_dir, strerror(errno));
        return -1;
    }

    return 0;
}

static int default_output_path(char *dst, size_t dstsz)
{
    const char *home = getenv("HOME");
    const char *output_dir = getenv("QUICKREC_OUTPUT_DIR");
    char dir[PATH_MAX];
    char stamp[64];
    time_t now = time(NULL);
    struct tm tm;

    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }

    if (output_dir && *output_dir) {
        if (snprintf(dir, sizeof(dir), "%s", output_dir) >= (int)sizeof(dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (snprintf(dir, sizeof(dir), "%s/Workspace/quickrec/recordings", home) >= (int)sizeof(dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (mkdir_p(dir) < 0)
        return -1;

    if (!localtime_r(&now, &tm))
        return -1;
    if (strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm) == 0) {
        errno = EINVAL;
        return -1;
    }

    if (snprintf(dst, dstsz, "%s/quickrec-%s.mp4", dir, stamp) >= (int)dstsz) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int resolve_output_path(const char *input, char *dst, size_t dstsz)
{
    char cwd[PATH_MAX];

    if (!input || !*input) {
        errno = EINVAL;
        return -1;
    }

    if (input[0] == '/') {
        if (snprintf(dst, dstsz, "%s", input) >= (int)dstsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if (!getcwd(cwd, sizeof(cwd)))
        return -1;
    if (snprintf(dst, dstsz, "%s/%s", cwd, input) >= (int)dstsz) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int parse_geometry_spec(const char *text, struct geometry *geo)
{
    long w, h, x, y;
    int consumed;
    char *end;
    const char *rest;

    if (!text || !*text) {
        fprintf(stderr, "quickrec: empty geometry spec\n");
        return -1;
    }

    if (sscanf(text, "%ldx%ld%n", &w, &h, &consumed) != 2) {
        fprintf(stderr, "quickrec: invalid geometry '%s' (expected WxH+X+Y)\n", text);
        return -1;
    }

    rest = text + consumed;
    errno = 0;
    x = strtol(rest, &end, 10);
    if (errno != 0 || end == rest) {
        fprintf(stderr, "quickrec: invalid geometry '%s' (expected WxH+X+Y)\n", text);
        return -1;
    }

    rest = end;
    errno = 0;
    y = strtol(rest, &end, 10);
    if (errno != 0 || end == rest || *end != '\0') {
        fprintf(stderr, "quickrec: invalid geometry '%s' (expected WxH+X+Y)\n", text);
        return -1;
    }

    if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX ||
        x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) {
        fprintf(stderr, "quickrec: geometry out of range: %s\n", text);
        return -1;
    }

    geo->w = (int)w;
    geo->h = (int)h;
    geo->x = (int)x;
    geo->y = (int)y;
    return 0;
}

static int normalize_geometry(struct geometry *geo)
{
    int adjusted = 0;

    if (geo->w <= 0 || geo->h <= 0) {
        fprintf(stderr, "quickrec: invalid region %dx%d\n", geo->w, geo->h);
        return -1;
    }

    if ((geo->w & 1) != 0) {
        if (geo->w <= 1) {
            fprintf(stderr, "quickrec: selected region width must be at least 2 pixels for H.264\n");
            return -1;
        }
        geo->w -= 1;
        adjusted = 1;
    }

    if ((geo->h & 1) != 0) {
        if (geo->h <= 1) {
            fprintf(stderr, "quickrec: selected region height must be at least 2 pixels for H.264\n");
            return -1;
        }
        geo->h -= 1;
        adjusted = 1;
    }

    return adjusted;
}

static int recorder_running(const struct app_paths *paths, pid_t *pid_out)
{
    pid_t pid;

    if (read_pid_file(paths->pidfile, &pid) < 0)
        return 0;

    if (!process_alive(pid) || !process_cmdline_contains(pid, "ffmpeg")) {
        unlink(paths->pidfile);
        return 0;
    }

    if (pid_out)
        *pid_out = pid;
    return 1;
}

static void hide_border_windows(Display *dpy, Window border[4])
{
    for (int i = 0; i < 4; ++i) {
        if (border[i])
            XUnmapWindow(dpy, border[i]);
    }
}

static void position_border_windows(Display *dpy, Window border[4], int screen_w, int screen_h,
                                    const struct geometry *geo)
{
    int left_x = geo->x > 0 ? geo->x - 1 : geo->x;
    int right_x = geo->x + geo->w < screen_w ? geo->x + geo->w : geo->x + geo->w - 1;
    int top_y = geo->y > 0 ? geo->y - 1 : geo->y;
    int bottom_y = geo->y + geo->h < screen_h ? geo->y + geo->h : geo->y + geo->h - 1;
    int horiz_w = geo->w + (geo->x > 0 ? 1 : 0) + (geo->x + geo->w < screen_w ? 1 : 0);

    if (horiz_w < 1)
        horiz_w = 1;

    XMoveResizeWindow(dpy, border[0], left_x, top_y, (unsigned)horiz_w, 1);
    XMoveResizeWindow(dpy, border[1], left_x, bottom_y, (unsigned)horiz_w, 1);
    XMoveResizeWindow(dpy, border[2], left_x, geo->y, 1, (unsigned)(geo->h > 0 ? geo->h : 1));
    XMoveResizeWindow(dpy, border[3], right_x, geo->y, 1, (unsigned)(geo->h > 0 ? geo->h : 1));

    for (int i = 0; i < 4; ++i)
        XMapRaised(dpy, border[i]);
}

static void geometry_from_points(int x1, int y1, int x2, int y2, struct geometry *geo)
{
    geo->x = x1 < x2 ? x1 : x2;
    geo->y = y1 < y2 ? y1 : y2;
    geo->w = x1 < x2 ? (x2 - x1) : (x1 - x2);
    geo->h = y1 < y2 ? (y2 - y1) : (y1 - y2);
}

static int get_window_geometry(Display *dpy, Window root, Window win, struct geometry *geo)
{
    XWindowAttributes attr;
    Window child;
    int x;
    int y;

    if (!win || win == root)
        return -1;
    if (!XGetWindowAttributes(dpy, win, &attr))
        return -1;
    if (attr.map_state != IsViewable || attr.width <= 0 || attr.height <= 0)
        return -1;
    if (!XTranslateCoordinates(dpy, win, root, 0, 0, &x, &y, &child))
        return -1;

    geo->x = x;
    geo->y = y;
    geo->w = attr.width;
    geo->h = attr.height;
    return 0;
}

static int query_pointer_window_geometry(Display *dpy, Window root, int *root_x, int *root_y,
                                         struct geometry *geo)
{
    Window root_return;
    Window child_return;
    int win_x;
    int win_y;
    unsigned int mask;

    if (!XQueryPointer(dpy, root, &root_return, &child_return, root_x, root_y,
                       &win_x, &win_y, &mask))
        return -1;

    if (get_window_geometry(dpy, root, child_return, geo) < 0)
        return -1;

    return 0;
}

static int drag_cursor_shape(int start_x, int start_y, int pointer_x, int pointer_y)
{
    char a = start_y > pointer_y;
    char b = start_x > pointer_x;
    char c = (a << 1) | b;

    switch (c) {
    case 0: return XC_lr_angle;
    case 1: return XC_ll_angle;
    case 2: return XC_ur_angle;
    case 3: return XC_ul_angle;
    default: return XC_cross;
    }
}

static Cursor cursor_for_shape(int shape,
                               Cursor cursor_cross,
                               Cursor cursor_lr,
                               Cursor cursor_ll,
                               Cursor cursor_ur,
                               Cursor cursor_ul)
{
    switch (shape) {
    case XC_lr_angle: return cursor_lr;
    case XC_ll_angle: return cursor_ll;
    case XC_ur_angle: return cursor_ur;
    case XC_ul_angle: return cursor_ul;
    case XC_cross:
    default:
        return cursor_cross;
    }
}

static int select_region(struct geometry *geo)
{
    Display *dpy;
    int screen;
    int screen_w;
    int screen_h;
    Window root;
    Window border[4] = {0, 0, 0, 0};
    Cursor cursor_cross;
    Cursor cursor_lr;
    Cursor cursor_ll;
    Cursor cursor_ur;
    Cursor cursor_ul;
    int current_cursor_shape;
    unsigned long pixel;
    int pointer_x = 0;
    int pointer_y = 0;
    int start_x = 0;
    int start_y = 0;
    int pressed = 0;
    int dragging = 0;
    int cancelled = 0;
    int done = 0;
    struct geometry hover_geo;
    struct geometry drag_geo;
    int have_hover = 0;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "quickrec: cannot open display\n");
        return -1;
    }

    screen = DefaultScreen(dpy);
    screen_w = DisplayWidth(dpy, screen);
    screen_h = DisplayHeight(dpy, screen);
    root = RootWindow(dpy, screen);
    cursor_cross = XCreateFontCursor(dpy, XC_cross);
    cursor_lr = XCreateFontCursor(dpy, XC_lr_angle);
    cursor_ll = XCreateFontCursor(dpy, XC_ll_angle);
    cursor_ur = XCreateFontCursor(dpy, XC_ur_angle);
    cursor_ul = XCreateFontCursor(dpy, XC_ul_angle);
    current_cursor_shape = XC_cross;
    pixel = alloc_indicator_color(dpy, screen);

    for (int i = 0; i < 4; ++i) {
        border[i] = create_overlay_window(dpy, root, 0, 0, 1, 1, pixel, NULL);
        if (!border[i]) {
            fprintf(stderr, "quickrec: failed to create selection overlay\n");
            for (int j = 0; j < i; ++j)
                XDestroyWindow(dpy, border[j]);
            if (cursor_cross != None)
                XFreeCursor(dpy, cursor_cross);
            if (cursor_lr != None)
                XFreeCursor(dpy, cursor_lr);
            if (cursor_ll != None)
                XFreeCursor(dpy, cursor_ll);
            if (cursor_ur != None)
                XFreeCursor(dpy, cursor_ur);
            if (cursor_ul != None)
                XFreeCursor(dpy, cursor_ul);
            XCloseDisplay(dpy);
            return -1;
        }
        XUnmapWindow(dpy, border[i]);
    }

    if (XGrabPointer(dpy, root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cursor_cross, CurrentTime) != GrabSuccess) {
        fprintf(stderr, "quickrec: failed to grab pointer for selection\n");
        for (int i = 0; i < 4; ++i)
            XDestroyWindow(dpy, border[i]);
        if (cursor_cross != None)
            XFreeCursor(dpy, cursor_cross);
        if (cursor_lr != None)
            XFreeCursor(dpy, cursor_lr);
        if (cursor_ll != None)
            XFreeCursor(dpy, cursor_ll);
        if (cursor_ur != None)
            XFreeCursor(dpy, cursor_ur);
        if (cursor_ul != None)
            XFreeCursor(dpy, cursor_ul);
        XCloseDisplay(dpy);
        return -1;
    }

    if (query_pointer_window_geometry(dpy, root, &pointer_x, &pointer_y, &hover_geo) == 0) {
        have_hover = 1;
        position_border_windows(dpy, border, screen_w, screen_h, &hover_geo);
    } else {
        hide_border_windows(dpy, border);
    }
    XFlush(dpy);

    while (!done && !cancelled) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case MotionNotify:
            pointer_x = ev.xmotion.x_root;
            pointer_y = ev.xmotion.y_root;
            if (pressed) {
                if (!dragging &&
                    ((pointer_x > start_x ? pointer_x - start_x : start_x - pointer_x) > SELECT_TOLERANCE ||
                     (pointer_y > start_y ? pointer_y - start_y : start_y - pointer_y) > SELECT_TOLERANCE)) {
                    dragging = 1;
                }
                if (dragging) {
                    int wanted_shape = drag_cursor_shape(start_x, start_y, pointer_x, pointer_y);
                    if (wanted_shape != current_cursor_shape) {
                        XChangeActivePointerGrab(dpy,
                                                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                                 cursor_for_shape(wanted_shape,
                                                                  cursor_cross,
                                                                  cursor_lr,
                                                                  cursor_ll,
                                                                  cursor_ur,
                                                                  cursor_ul),
                                                 CurrentTime);
                        current_cursor_shape = wanted_shape;
                    }
                    geometry_from_points(start_x, start_y, pointer_x, pointer_y, &drag_geo);
                    if (drag_geo.w > 0 && drag_geo.h > 0)
                        position_border_windows(dpy, border, screen_w, screen_h, &drag_geo);
                    else
                        hide_border_windows(dpy, border);
                }
            } else {
                if (current_cursor_shape != XC_cross) {
                    XChangeActivePointerGrab(dpy,
                                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                             cursor_cross,
                                             CurrentTime);
                    current_cursor_shape = XC_cross;
                }
                if (query_pointer_window_geometry(dpy, root, &pointer_x, &pointer_y, &hover_geo) == 0) {
                    have_hover = 1;
                    position_border_windows(dpy, border, screen_w, screen_h, &hover_geo);
                } else {
                    have_hover = 0;
                    hide_border_windows(dpy, border);
                }
            }
            XFlush(dpy);
            break;

        case ButtonPress:
            if (ev.xbutton.button == Button3) {
                cancelled = 1;
                break;
            }
            if (ev.xbutton.button == Button1) {
                pressed = 1;
                dragging = 0;
                start_x = ev.xbutton.x_root;
                start_y = ev.xbutton.y_root;
            }
            break;

        case ButtonRelease:
            if (ev.xbutton.button == Button3) {
                cancelled = 1;
                break;
            }
            if (ev.xbutton.button != Button1 || !pressed)
                break;

            pressed = 0;
            pointer_x = ev.xbutton.x_root;
            pointer_y = ev.xbutton.y_root;

            if (dragging) {
                geometry_from_points(start_x, start_y, pointer_x, pointer_y, geo);
                if (geo->w <= 0 || geo->h <= 0)
                    cancelled = 1;
                else
                    done = 1;
            } else if (query_pointer_window_geometry(dpy, root, &pointer_x, &pointer_y, geo) == 0) {
                done = 1;
            } else if (have_hover) {
                *geo = hover_geo;
                done = 1;
            } else {
                cancelled = 1;
            }
            break;
        }
    }

    XUngrabPointer(dpy, CurrentTime);
    for (int i = 0; i < 4; ++i)
        XDestroyWindow(dpy, border[i]);
    if (cursor_cross != None)
        XFreeCursor(dpy, cursor_cross);
    if (cursor_lr != None)
        XFreeCursor(dpy, cursor_lr);
    if (cursor_ll != None)
        XFreeCursor(dpy, cursor_ll);
    if (cursor_ur != None)
        XFreeCursor(dpy, cursor_ur);
    if (cursor_ul != None)
        XFreeCursor(dpy, cursor_ul);
    XCloseDisplay(dpy);

    if (cancelled || geo->w <= 0 || geo->h <= 0) {
        fprintf(stderr, "quickrec: selection cancelled\n");
        return -1;
    }

    return 0;
}

static int spawn_ffmpeg(const struct geometry *geo, const struct start_options *opt,
                        const struct app_paths *paths, pid_t *pid_out)
{
    int errpipe[2];
    pid_t pid;

    if (pipe(errpipe) < 0)
        return -1;

    if (fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return -1;
    }

    if (pid == 0) {
        int logfd;
        int devnull;
        int child_errno;
        char fpsbuf[16];
        char sizebuf[64];
        char inputbuf[128];

        close(errpipe[0]);

        if (setsid() < 0) {
            child_errno = errno;
            (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
            _exit(1);
        }

        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            if (dup2(devnull, STDIN_FILENO) < 0) {
                child_errno = errno;
                (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
                _exit(1);
            }
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        logfd = open(paths->logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd < 0) {
            child_errno = errno;
            (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
            _exit(1);
        }

        if (dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0) {
            child_errno = errno;
            (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
            _exit(1);
        }
        if (logfd > STDERR_FILENO)
            close(logfd);

        snprintf(fpsbuf, sizeof(fpsbuf), "%d", opt->fps);
        snprintf(sizebuf, sizeof(sizebuf), "%dx%d", geo->w, geo->h);
        snprintf(inputbuf, sizeof(inputbuf), "%s+%d,%d", opt->display, geo->x, geo->y);

        execlp("ffmpeg",
               "ffmpeg",
               "-y",
               "-hide_banner",
               "-loglevel", "warning",
               "-f", "x11grab",
               "-framerate", fpsbuf,
               "-video_size", sizebuf,
               "-i", inputbuf,
               "-c:v", "libx264",
               "-preset", DEFAULT_PRESET,
               "-pix_fmt", "yuv420p",
               opt->output,
               (char *)NULL);

        child_errno = errno;
        (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
        _exit(127);
    }

    close(errpipe[1]);

    {
        int child_errno = 0;
        ssize_t n = read(errpipe[0], &child_errno, sizeof(child_errno));
        close(errpipe[0]);
        if (n > 0) {
            waitpid(pid, NULL, 0);
            errno = child_errno;
            return -1;
        }
    }

    *pid_out = pid;
    return 0;
}

static int wait_for_exit(pid_t pid, int timeout_ms)
{
    const int interval_us = 100000;
    int waited_ms = 0;

    while (waited_ms < timeout_ms) {
        if (!process_alive(pid))
            return 0;
        usleep(interval_us);
        waited_ms += interval_us / 1000;
    }

    return process_alive(pid) ? -1 : 0;
}

static volatile sig_atomic_t indicator_running = 1;

static void on_indicator_signal(int sig)
{
    (void)sig;
    indicator_running = 0;
}

static unsigned long alloc_indicator_color(Display *dpy, int screen)
{
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor color;
    XColor exact;

    if (XAllocNamedColor(dpy, cmap, "#fe6b6b", &color, &exact))
        return color.pixel;
    return WhitePixel(dpy, screen);
}

static void make_window_clickthrough(Display *dpy, Window root, Window win, unsigned width, unsigned height)
{
    Pixmap input_mask;
    GC gc;
    int shape_event_base;
    int shape_error_base;

    if (!XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base))
        return;

#ifdef ShapeInput
    input_mask = XCreatePixmap(dpy, root, width, height, 1);
    gc = XCreateGC(dpy, input_mask, 0, NULL);
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, input_mask, gc, 0, 0, width, height);
    XFreeGC(dpy, gc);
    XShapeCombineMask(dpy, win, ShapeInput, 0, 0, input_mask, ShapeSet);
    XFreePixmap(dpy, input_mask);
#else
    (void)root;
    (void)width;
    (void)height;
#endif
}

static Window create_overlay_window(Display *dpy, Window root,
                                    int x, int y, unsigned width, unsigned height,
                                    unsigned long pixel, const char *name)
{
    XSetWindowAttributes attrs;
    unsigned long mask;
    Window win;
    XClassHint class_hint;

    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.border_pixel = 0;
    attrs.background_pixel = pixel;
    attrs.event_mask = ExposureMask;
    mask = CWOverrideRedirect | CWBorderPixel | CWBackPixel | CWEventMask;

    win = XCreateWindow(dpy, root,
                        x, y, width, height,
                        0, CopyFromParent, InputOutput, CopyFromParent,
                        mask, &attrs);
    if (!win)
        return 0;

    if (name) {
        XStoreName(dpy, win, name);
        class_hint.res_name = (char *)name;
        class_hint.res_class = "Quickrec";
        XSetClassHint(dpy, win, &class_hint);
    }

    make_window_clickthrough(dpy, root, win, width, height);
    XMapRaised(dpy, win);
    return win;
}

static int run_indicator_loop(int notify_fd, const struct geometry *geo)
{
    Display *dpy;
    int screen;
    int screen_w;
    int screen_h;
    Window root;
    Window border[4] = {0, 0, 0, 0};
    KeyCode stop_code;
    unsigned int stop_mod = ControlMask | ShiftMask;
    unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    int stop_requested = 0;
    unsigned long pixel;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        errno = EIO;
        return -1;
    }

    screen = DefaultScreen(dpy);
    screen_w = DisplayWidth(dpy, screen);
    screen_h = DisplayHeight(dpy, screen);
    root = RootWindow(dpy, screen);
    stop_code = XKeysymToKeycode(dpy, XK_Escape);
    pixel = alloc_indicator_color(dpy, screen);

    for (int i = 0; i < 4; ++i) {
        border[i] = create_overlay_window(dpy, root, 0, 0, 1, 1, pixel, NULL);
        if (!border[i]) {
            while (i-- > 0)
                XDestroyWindow(dpy, border[i]);
            XCloseDisplay(dpy);
            errno = EIO;
            return -1;
        }
    }

    position_border_windows(dpy, border, screen_w, screen_h, geo);
    if (stop_code != 0) {
        for (int i = 0; i < 4; ++i)
            XGrabKey(dpy, stop_code, stop_mod | locks[i], root, True, GrabModeAsync, GrabModeAsync);
    }
    XFlush(dpy);

    if (notify_fd >= 0) {
        close(notify_fd);
        notify_fd = -1;
    }

    indicator_running = 1;
    signal(SIGTERM, on_indicator_signal);
    signal(SIGINT, on_indicator_signal);

    while (indicator_running) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                XClearWindow(dpy, ev.xexpose.window);
            } else if (ev.type == KeyPress && ev.xkey.keycode == stop_code && !stop_requested) {
                pid_t child = fork();
                if (child == 0) {
                    if (launch_stop_command() < 0)
                        _exit(127);
                }
                stop_requested = 1;
                if (stop_code != 0) {
                    for (int i = 0; i < 4; ++i)
                        XUngrabKey(dpy, stop_code, stop_mod | locks[i], root);
                }
            }
        }
        usleep(50000);
    }

    if (stop_code != 0) {
        for (int i = 0; i < 4; ++i)
            XUngrabKey(dpy, stop_code, stop_mod | locks[i], root);
    }
    for (int i = 0; i < 4; ++i)
        XDestroyWindow(dpy, border[i]);
    XCloseDisplay(dpy);
    return 0;
}

static int spawn_indicator(const struct geometry *geo, pid_t *pid_out)
{
    int errpipe[2];
    pid_t pid;

    if (pipe(errpipe) < 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return -1;
    }

    if (pid == 0) {
        int child_errno;
        int devnull;

        close(errpipe[0]);

        if (setsid() < 0) {
            child_errno = errno;
            (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
            _exit(1);
        }

        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            if (dup2(devnull, STDIN_FILENO) < 0 ||
                dup2(devnull, STDOUT_FILENO) < 0 ||
                dup2(devnull, STDERR_FILENO) < 0) {
                child_errno = errno;
                (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
                _exit(1);
            }
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        if (run_indicator_loop(errpipe[1], geo) < 0) {
            child_errno = errno ? errno : EIO;
            (void)!write(errpipe[1], &child_errno, sizeof(child_errno));
            close(errpipe[1]);
            _exit(1);
        }

        _exit(0);
    }

    close(errpipe[1]);

    {
        int child_errno = 0;
        ssize_t n = read(errpipe[0], &child_errno, sizeof(child_errno));
        close(errpipe[0]);
        if (n > 0) {
            waitpid(pid, NULL, 0);
            errno = child_errno;
            return -1;
        }
    }

    *pid_out = pid;
    return 0;
}

static void cleanup_indicator_pidfile(const struct app_paths *paths)
{
    pid_t pid;

    if (read_pid_file(paths->indicator_pidfile, &pid) < 0)
        return;
    if (!process_alive(pid))
        unlink(paths->indicator_pidfile);
}

static int stop_indicator(const struct app_paths *paths)
{
    pid_t pid;

    if (read_pid_file(paths->indicator_pidfile, &pid) < 0)
        return 0;

    if (!process_alive(pid)) {
        unlink(paths->indicator_pidfile);
        return 0;
    }

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        return -1;
    if (wait_for_exit(pid, 1000) < 0) {
        if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
            return -1;
        (void)wait_for_exit(pid, 1000);
    }

    unlink(paths->indicator_pidfile);
    return 0;
}

static int get_self_exe_path(char *buf, size_t bufsz)
{
    ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
    if (n < 0)
        return -1;
    if ((size_t)n >= bufsz - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

static int launch_stop_command(void)
{
    char self[PATH_MAX];

    if (get_self_exe_path(self, sizeof(self)) < 0)
        return -1;

    execl(self, "quickrec", "stop", (char *)NULL);
    return -1;
}

static int parse_positive_int(const char *text, const char *label, int *value_out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 1000) {
        fprintf(stderr, "quickrec: invalid %s: %s\n", label, text);
        return -1;
    }

    *value_out = (int)value;
    return 0;
}

static int prompt_recording_name(char *buf, size_t bufsz)
{
    FILE *fp;
    int status;

    if (!command_exists("dmenu"))
        return 0;

    fp = popen("printf '' | dmenu -c -p 'Recording name:' -S '.mp4'", "r");
    if (!fp)
        return -1;

    if (!fgets(buf, (int)bufsz, fp))
        buf[0] = '\0';

    status = pclose(fp);
    if (status == -1)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 0;

    {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
    }

    return buf[0] != '\0';
}

static void maybe_rename_recording_with_dmenu(const struct app_paths *paths, char *outfile, size_t outsz)
{
    char name[PATH_MAX];
    char basename[PATH_MAX];
    char dir[PATH_MAX];
    char newpath[PATH_MAX];
    char *slash;
    int rc;

    if (!outfile || !*outfile)
        return;
    if (access(outfile, F_OK) != 0)
        return;

    rc = prompt_recording_name(name, sizeof(name));
    if (rc < 0) {
        fprintf(stderr, "quickrec: warning: failed to prompt for recording name: %s\n", strerror(errno));
        return;
    }
    if (rc == 0)
        return;

    if (snprintf(basename, sizeof(basename), "%s.mp4", name) >= (int)sizeof(basename)) {
        fprintf(stderr, "quickrec: warning: chosen recording name is too long\n");
        return;
    }

    if (snprintf(dir, sizeof(dir), "%s", outfile) >= (int)sizeof(dir)) {
        fprintf(stderr, "quickrec: warning: current output path is too long\n");
        return;
    }

    slash = strrchr(dir, '/');
    if (!slash) {
        if (snprintf(dir, sizeof(dir), ".") >= (int)sizeof(dir))
            return;
    } else if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    if (path_join(newpath, sizeof(newpath), dir, basename) < 0) {
        fprintf(stderr, "quickrec: warning: new output path is too long\n");
        return;
    }

    if (rename(outfile, newpath) < 0) {
        fprintf(stderr, "quickrec: warning: failed to rename output: %s\n", strerror(errno));
        return;
    }

    if (snprintf(outfile, outsz, "%s", newpath) >= (int)outsz) {
        fprintf(stderr, "quickrec: warning: renamed output path is too long\n");
        return;
    }

    if (write_text_file(paths->outfilefile, outfile) < 0)
        fprintf(stderr, "quickrec: warning: failed to update last output path: %s\n", strerror(errno));
}

static int start_recording(int argc, char **argv, const struct app_paths *paths)
{
    struct geometry geo;
    struct start_options opt;
    char default_output[PATH_MAX];
    char resolved_output[PATH_MAX];
    char pidbuf[64];
    pid_t pid;
    pid_t indicator_pid;
    int have_indicator = 0;
    int adjusted;
    int status;

    opt.fps = DEFAULT_FPS;
    opt.display = getenv("DISPLAY");
    opt.output = NULL;
    opt.geometry_text = NULL;

    if (!opt.display || !*opt.display) {
        fprintf(stderr, "quickrec: DISPLAY is not set\n");
        return 1;
    }

    {
        const char *fps_env = getenv("QUICKREC_FPS");
        if (fps_env && *fps_env && parse_positive_int(fps_env, "QUICKREC_FPS", &opt.fps) < 0)
            return 1;
    }

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fps")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "quickrec: missing value for %s\n", argv[i]);
                return 1;
            }
            if (parse_positive_int(argv[++i], "fps", &opt.fps) < 0)
                return 1;
        } else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--geometry")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "quickrec: missing value for %s\n", argv[i]);
                return 1;
            }
            opt.geometry_text = argv[++i];
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "quickrec: missing value for %s\n", argv[i]);
                return 1;
            }
            opt.output = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "quickrec: unknown option: %s\n", argv[i]);
            return 1;
        } else if (!opt.output) {
            opt.output = argv[i];
        } else {
            fprintf(stderr, "quickrec: unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    cleanup_indicator_pidfile(paths);

    if (recorder_running(paths, &pid)) {
        fprintf(stderr, "quickrec: already recording (pid %ld)\n", (long)pid);
        return 1;
    }

    (void)stop_indicator(paths);

    if (!command_exists("ffmpeg")) {
        fprintf(stderr, "quickrec: ffmpeg is not installed\n");
        return 1;
    }

    if (!opt.output) {
        if (default_output_path(default_output, sizeof(default_output)) < 0) {
            fprintf(stderr, "quickrec: cannot build output path: %s\n", strerror(errno));
            return 1;
        }
        opt.output = default_output;
    }

    if (resolve_output_path(opt.output, resolved_output, sizeof(resolved_output)) < 0) {
        fprintf(stderr, "quickrec: invalid output path: %s\n", strerror(errno));
        return 1;
    }
    opt.output = resolved_output;

    if (ensure_parent_dir(opt.output) < 0) {
        fprintf(stderr, "quickrec: cannot create output directory: %s\n", strerror(errno));
        return 1;
    }

    if (opt.geometry_text) {
        if (parse_geometry_spec(opt.geometry_text, &geo) < 0)
            return 1;
    } else {
        if (select_region(&geo) < 0)
            return 1;
    }

    adjusted = normalize_geometry(&geo);
    if (adjusted < 0)
        return 1;
    if (adjusted > 0)
        printf("quickrec: adjusted region to even size for H.264 -> %dx%d+%d+%d\n",
               geo.w, geo.h, geo.x, geo.y);

    if (spawn_indicator(&geo, &indicator_pid) == 0) {
        have_indicator = 1;
        snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)indicator_pid);
        if (write_text_file(paths->indicator_pidfile, pidbuf) < 0) {
            fprintf(stderr, "quickrec: warning: failed to write indicator state: %s\n", strerror(errno));
            have_indicator = 0;
            kill(indicator_pid, SIGTERM);
        }
    } else {
        fprintf(stderr, "quickrec: warning: failed to start recording overlay: %s\n", strerror(errno));
    }

    if (spawn_ffmpeg(&geo, &opt, paths, &pid) < 0) {
        fprintf(stderr, "quickrec: failed to start ffmpeg: %s\n", strerror(errno));
        if (have_indicator)
            (void)stop_indicator(paths);
        return 1;
    }

    usleep(250000);
    status = waitpid(pid, NULL, WNOHANG);
    if (status == pid) {
        fprintf(stderr, "quickrec: ffmpeg exited immediately; see %s\n", paths->logfile);
        if (have_indicator)
            (void)stop_indicator(paths);
        return 1;
    }

    snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)pid);
    if (write_text_file(paths->pidfile, pidbuf) < 0 || write_text_file(paths->outfilefile, opt.output) < 0) {
        fprintf(stderr, "quickrec: failed to write state: %s\n", strerror(errno));
        kill(pid, SIGINT);
        if (have_indicator)
            (void)stop_indicator(paths);
        return 1;
    }

    printf("quickrec: recording %dx%d+%d+%d -> %s (pid %ld)\n",
           geo.w, geo.h, geo.x, geo.y, opt.output, (long)pid);
    printf("quickrec: stop hotkey -> Ctrl+Shift+Esc\n");
    return 0;
}

static int stop_recording(const struct app_paths *paths)
{
    char outfile[PATH_MAX] = "";
    pid_t pid;

    cleanup_indicator_pidfile(paths);

    if (read_line_file(paths->outfilefile, outfile, sizeof(outfile)) < 0)
        outfile[0] = '\0';

    if (!recorder_running(paths, &pid)) {
        if (stop_indicator(paths) < 0)
            fprintf(stderr, "quickrec: warning: failed to stop indicator: %s\n", strerror(errno));
        printf("quickrec: idle\n");
        return 0;
    }

    if (kill(pid, SIGINT) < 0 && errno != ESRCH) {
        fprintf(stderr, "quickrec: failed to stop recorder: %s\n", strerror(errno));
        return 1;
    }

    if (wait_for_exit(pid, 5000) < 0) {
        if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
            fprintf(stderr, "quickrec: failed to terminate recorder: %s\n", strerror(errno));
            return 1;
        }
    }

    if (wait_for_exit(pid, 2000) < 0) {
        if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
            fprintf(stderr, "quickrec: failed to kill recorder: %s\n", strerror(errno));
            return 1;
        }
        (void)wait_for_exit(pid, 1000);
    }

    unlink(paths->pidfile);

    if (stop_indicator(paths) < 0)
        fprintf(stderr, "quickrec: warning: failed to stop indicator: %s\n", strerror(errno));

    maybe_rename_recording_with_dmenu(paths, outfile, sizeof(outfile));

    if (outfile[0] != '\0')
        printf("quickrec: stopped -> %s\n", outfile);
    else
        printf("quickrec: stopped\n");

    return 0;
}

static int status_recording(const struct app_paths *paths)
{
    char outfile[PATH_MAX] = "";
    pid_t pid;
    int have_outfile = read_line_file(paths->outfilefile, outfile, sizeof(outfile)) == 0;

    cleanup_indicator_pidfile(paths);

    if (recorder_running(paths, &pid)) {
        if (have_outfile)
            printf("quickrec: recording -> %s (pid %ld)\n", outfile, (long)pid);
        else
            printf("quickrec: recording (pid %ld)\n", (long)pid);
    } else {
        printf("quickrec: idle\n");
        if (have_outfile)
            printf("quickrec: last file -> %s\n", outfile);
    }

    return 0;
}

static int toggle_recording(int argc, char **argv, const struct app_paths *paths)
{
    pid_t pid;

    if (recorder_running(paths, &pid))
        return stop_recording(paths);
    return start_recording(argc, argv, paths);
}

int main(int argc, char **argv)
{
    struct app_paths paths;
    const char *cmd;

    if (init_paths(&paths) < 0)
        return 1;

    cmd = argc > 1 ? argv[1] : "toggle";

    if (!strcmp(cmd, "start"))
        return start_recording(argc - 2, argv + 2, &paths);
    if (!strcmp(cmd, "stop"))
        return stop_recording(&paths);
    if (!strcmp(cmd, "toggle"))
        return toggle_recording(argc - 2, argv + 2, &paths);
    if (!strcmp(cmd, "status"))
        return status_recording(&paths);
    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage(stdout);
        return 0;
    }

    fprintf(stderr, "quickrec: unknown command: %s\n\n", cmd);
    usage(stderr);
    return 1;
}
