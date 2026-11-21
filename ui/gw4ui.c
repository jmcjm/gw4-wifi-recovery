/*
 * gw4ui: status screen and button menu for the Wi-Fi ADB recovery on the
 * Galaxy Watch 4. Runs instead of the Samsung recovery UI and draws directly
 * through DRM dumb buffers (fbdev as a fallback), without libdrm.
 *
 * Status text comes from /tmp/wifiadb.status (written by wifiadb.sh); the
 * signal level is read live from /proc/net/wireless.
 *
 * Buttons: KEY_POWER (upper key) confirms, KEY_APPSELECT (lower key) or a tap
 * moves the selection. Other keys, such as the wrist motion sensor reporting
 * KEY_HOMEPAGE, only wake the screen.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "font.h"

#define STATUS_FILE "/tmp/wifiadb.status"
#define MAX_INPUTS 16
#define IDLE_BLANK_MS 120000

/* ---------- display ---------- */

struct buffer {
    uint32_t handle, pitch, fb_id;
    uint64_t size;
    uint8_t *map;
};

static struct {
    int drm_fd;
    uint32_t crtc_id, conn_id;
    struct drm_mode_modeinfo mode;
    struct buffer buf[2];
    int front;

    int fb_fd; /* fbdev fallback */
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *fb_map;

    uint32_t w, h, pitch;
    uint8_t *draw; /* current back buffer */
    int bgr;       /* fbdev with blue in the low byte */
} d = {.drm_fd = -1, .fb_fd = -1};

static void logmsg(const char *fmt, ...)
{
    FILE *f = fopen("/tmp/gw4ui.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int drm_create_buffer(struct buffer *b)
{
    struct drm_mode_create_dumb c = {.width = d.w, .height = d.h, .bpp = 32};
    if (ioctl(d.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0)
        return -1;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;

    struct drm_mode_fb_cmd f = {.width = d.w, .height = d.h, .pitch = c.pitch,
                                .bpp = 32, .depth = 24, .handle = c.handle};
    if (ioctl(d.drm_fd, DRM_IOCTL_MODE_ADDFB, &f) < 0)
        return -1;
    b->fb_id = f.fb_id;

    struct drm_mode_map_dumb m = {.handle = c.handle};
    if (ioctl(d.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0)
        return -1;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d.drm_fd, m.offset);
    if (b->map == MAP_FAILED)
        return -1;
    memset(b->map, 0, b->size);
    return 0;
}

static int drm_open(void)
{
    d.drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (d.drm_fd < 0)
        return -1;
    ioctl(d.drm_fd, DRM_IOCTL_SET_MASTER, 0);

    struct drm_mode_card_res res = {0};
    if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;
    uint32_t conns[16], crtcs[16], encs[16];
    if (res.count_connectors > 16 || res.count_crtcs > 16 || res.count_encoders > 16)
        return -1;
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.count_fbs = 0;
    if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        if (c.connection != 1 || c.count_modes == 0)
            continue;
        struct drm_mode_modeinfo modes[32];
        uint32_t ce[16];
        if (c.count_modes > 32 || c.count_encoders > 16)
            continue;
        c.modes_ptr = (uintptr_t)modes;
        c.encoders_ptr = (uintptr_t)ce;
        c.count_props = 0;
        if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        d.conn_id = conns[i];
        d.mode = modes[0];

        uint32_t crtc = 0;
        if (c.encoder_id) {
            struct drm_mode_get_encoder e = {.encoder_id = c.encoder_id};
            if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0)
                crtc = e.crtc_id;
        }
        if (!crtc && c.count_encoders) {
            struct drm_mode_get_encoder e = {.encoder_id = ce[0]};
            if (ioctl(d.drm_fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0)
                for (uint32_t k = 0; k < res.count_crtcs; k++)
                    if (e.possible_crtcs & (1u << k)) {
                        crtc = crtcs[k];
                        break;
                    }
        }
        if (!crtc && res.count_crtcs)
            crtc = crtcs[0];
        d.crtc_id = crtc;
        break;
    }
    if (!d.conn_id || !d.crtc_id)
        return -1;

    d.w = d.mode.hdisplay;
    d.h = d.mode.vdisplay;
    if (drm_create_buffer(&d.buf[0]) < 0 || drm_create_buffer(&d.buf[1]) < 0)
        return -1;
    d.front = 0;
    d.pitch = d.buf[1].pitch;
    d.draw = d.buf[1].map;
    logmsg("drm: %ux%u conn %u crtc %u", d.w, d.h, d.conn_id, d.crtc_id);
    return 0;
}

static int fb_open(void)
{
    const char *paths[] = {"/dev/graphics/fb0", "/dev/fb0"};
    for (unsigned i = 0; i < 2 && d.fb_fd < 0; i++)
        d.fb_fd = open(paths[i], O_RDWR | O_CLOEXEC);
    if (d.fb_fd < 0)
        return -1;
    if (ioctl(d.fb_fd, FBIOGET_VSCREENINFO, &d.var) < 0 ||
        ioctl(d.fb_fd, FBIOGET_FSCREENINFO, &d.fix) < 0 || d.var.bits_per_pixel != 32)
        return -1;
    d.w = d.var.xres;
    d.h = d.var.yres;
    d.pitch = d.fix.line_length;
    d.fb_map = mmap(NULL, d.fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, d.fb_fd, 0);
    if (d.fb_map == MAP_FAILED)
        return -1;
    d.draw = d.fb_map;
    d.bgr = d.var.blue.offset == 0;
    ioctl(d.fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
    logmsg("fbdev: %ux%u", d.w, d.h);
    return 0;
}

static void present(void)
{
    if (d.drm_fd >= 0) {
        int back = !d.front;
        struct drm_mode_crtc c = {.crtc_id = d.crtc_id, .fb_id = d.buf[back].fb_id,
                                  .set_connectors_ptr = (uintptr_t)&d.conn_id,
                                  .count_connectors = 1, .mode = d.mode, .mode_valid = 1};
        if (ioctl(d.drm_fd, DRM_IOCTL_MODE_SETCRTC, &c) < 0)
            logmsg("setcrtc: %s", strerror(errno));
        struct drm_mode_fb_dirty_cmd dirty = {.fb_id = d.buf[back].fb_id};
        ioctl(d.drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
        d.front = back;
        d.draw = d.buf[!d.front].map;
        d.pitch = d.buf[!d.front].pitch;
    } else {
        d.var.yoffset = 0;
        ioctl(d.fb_fd, FBIOPAN_DISPLAY, &d.var);
    }
}

static void display_close(void)
{
    if (d.drm_fd >= 0) {
        ioctl(d.drm_fd, DRM_IOCTL_DROP_MASTER, 0);
        close(d.drm_fd);
        d.drm_fd = -1;
    }
    if (d.fb_fd >= 0) {
        close(d.fb_fd);
        d.fb_fd = -1;
    }
}

/* ---------- drawing ---------- */

static inline void px(int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || (uint32_t)x >= d.w || (uint32_t)y >= d.h)
        return;
    uint32_t v = rgb;
    if (d.bgr)
        v = (rgb & 0xff00) | ((rgb >> 16) & 0xff) | ((rgb & 0xff) << 16);
    *(uint32_t *)(d.draw + (size_t)y * d.pitch + (size_t)x * 4) = v;
}

static void rect(int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            px(i, j, rgb);
}

static void text_at(int x, int y, const char *s, uint32_t rgb)
{
    for (; *s; s++, x += FONT_W) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126)
            c = '?';
        const unsigned char *g = font_glyphs[c - 32];
        for (int r = 0; r < FONT_H; r++)
            for (int col = 0; col < FONT_W; col++)
                if (g[r * FONT_ROW + col / 8] & (0x80 >> (col % 8)))
                    px(x + col, y + r, rgb);
    }
}

/* Centered line, cut to the chord of the round screen at that height. */
static void text_center(int y, const char *s, uint32_t rgb)
{
    int r = (int)d.w / 2;
    int dy = y + FONT_H / 2 - (int)d.h / 2;
    int half;
    for (half = r; half > 0 && half * half + dy * dy > r * r; half--)
        ;
    int max_chars = (2 * half - 8) / FONT_W;
    char line[64];
    snprintf(line, sizeof(line), "%s", s);
    if (max_chars > 0 && (int)strlen(line) > max_chars)
        line[max_chars] = 0;
    int x = ((int)d.w - (int)strlen(line) * FONT_W) / 2;
    text_at(x, y, line, rgb);
}

/* ---------- state ---------- */

static const char *menu[] = {"Reboot system", "Samsung recovery", "Power off"};
#define MENU_N 3

static int read_signal(void)
{
    /* wifiadb.sh writes the RSSI from mf_cli; the dhd driver leaves
     * /proc/net/wireless at zero, so that file is only a fallback. */
    FILE *r = fopen("/tmp/wifiadb.rssi", "r");
    if (r) {
        int v = 0;
        if (fscanf(r, "%d", &v) == 1 && v < 0) {
            fclose(r);
            return v;
        }
        fclose(r);
    }
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
        return 0;
    char line[256];
    int level = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "wlan0:");
        if (!p)
            continue;
        int status;
        float link, lvl;
        if (sscanf(p + 6, "%x %f %f", &status, &link, &lvl) == 3)
            level = (int)lvl;
    }
    fclose(f);
    return level;
}

static void draw(int sel)
{
    rect(0, 0, d.w, d.h, 0x000000);
    int y = d.h / 2 - 190;
    text_center(y, "GW4 RECOVERY", 0xffb000);
    y += FONT_H + 6;

    FILE *f = fopen(STATUS_FILE, "r");
    int lines = 0;
    if (f) {
        char line[128];
        while (lines < 5 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            text_center(y, line, 0xffffff);
            y += FONT_H;
            lines++;
        }
        fclose(f);
    }
    if (!lines) {
        text_center(y, "Starting Wi-Fi...", 0xffffff);
        y += FONT_H;
    }
    int sig = read_signal();
    if (sig) {
        char s[32];
        snprintf(s, sizeof(s), "Signal %d dBm", sig);
        text_center(y, s, 0x80c0ff);
    }

    int my = d.h / 2 + 60;
    for (int i = 0; i < MENU_N; i++) {
        if (i == sel)
            rect(d.w / 2 - 150, my - 2, 300, FONT_H + 4, 0x1e6fd9);
        text_center(my, menu[i], i == sel ? 0xffffff : 0x4aa3ff);
        my += FONT_H + 8;
    }
    present();
}

static void set_brightness(int on)
{
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.')
            continue;
        char path[256], buf[32];
        int max = 255;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                max = atoi(buf);
            }
            close(fd);
        }
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", e->d_name);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            int len = snprintf(buf, sizeof(buf), "%d", on ? max * 2 / 3 : 0);
            write(fd, buf, len);
            close(fd);
        }
    }
    closedir(dir);
}

static void run(const char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

static void setprop(const char *k, const char *v)
{
    const char *argv[] = {"/system/bin/setprop", k, v, NULL};
    run(argv);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int main(void)
{
    /* The Samsung UI may still hold the display for a moment after ctl.stop. */
    int ok = -1;
    for (int i = 0; i < 20 && ok < 0; i++) {
        ok = drm_open();
        if (ok < 0) {
            display_close();
            ok = fb_open();
        }
        if (ok < 0) {
            display_close();
            usleep(250000);
        }
    }
    if (ok < 0) {
        logmsg("no display");
        return 1;
    }
    set_brightness(1);

    struct pollfd fds[MAX_INPUTS];
    int nfds = 0;
    DIR *dir = opendir("/dev/input");
    struct dirent *e;
    while (dir && (e = readdir(dir)) && nfds < MAX_INPUTS) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            fds[nfds].fd = fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
    }
    if (dir)
        closedir(dir);

    int sel = 0, blank = 0;
    long last_input = now_ms();
    draw(sel);

    for (;;) {
        int r = poll(fds, nfds, 1000);
        int pressed = -1;
        for (int i = 0; r > 0 && i < nfds; i++) {
            if (!(fds[i].revents & POLLIN))
                continue;
            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev))
                if (ev.type == EV_KEY && ev.value == 1) {
                    pressed = ev.code;
                    logmsg("key %d", ev.code);
                }
        }

        if (pressed >= 0) {
            last_input = now_ms();
            if (blank) {
                blank = 0;
                set_brightness(1);
                draw(sel);
                continue;
            }
            if (pressed == KEY_POWER) {
                if (sel == 0) {
                    text_center(d.h / 2 + 170, "Rebooting...", 0xffb000);
                    present();
                    sync();
                    setprop("sys.powerctl", "reboot");
                } else if (sel == 1) {
                    display_close();
                    setprop("ctl.start", "recovery");
                    return 0;
                } else {
                    sync();
                    setprop("sys.powerctl", "shutdown");
                }
            } else if (pressed == KEY_APPSELECT || pressed == KEY_BACK || pressed == BTN_TOUCH) {
                sel = (sel + 1) % MENU_N;
            }
        } else if (!blank && now_ms() - last_input > IDLE_BLANK_MS) {
            blank = 1;
            set_brightness(0);
            continue;
        }
        if (!blank)
            draw(sel);
    }
}
