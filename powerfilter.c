/*
 * powerfilter - software debounce for a flaky power button (rooted Android)
 * ==========================================================================
 *
 * THE PROBLEM
 *   A worn/corroded switch can "bounce": for a few milliseconds the contact
 *   opens and closes by itself. The kernel then reports something like
 *
 *       POWER DOWN ... POWER UP (3 ms later) ... POWER DOWN ... POWER UP
 *
 *   even though you pressed the button once. Android sees two presses, so
 *   the screen may turn off and straight back on, a double-press gesture may
 *   fire, and key chords (Power + Volume Down) get cancelled.
 *
 * THE FIX (what this program does)
 *   It sits between the kernel's input device and Android:
 *
 *       real device (/dev/input/event0)  -->  powerfilter  -->  virtual device
 *                                                                   |
 *                                                                Android
 *
 *   1. It creates a virtual input device with uinput that looks exactly like
 *      the real one (same name, same keys).
 *   2. It "grabs" the real device (EVIOCGRAB). While grabbed, ONLY this
 *      program receives the real events; Android no longer sees them.
 *   3. It forwards every event to the virtual device unchanged, EXCEPT the
 *      power key release, which is delayed by <debounce_ms>:
 *        - If the key is pressed again inside that window, it was a bounce:
 *          the release and the new press are both thrown away, so Android
 *          just sees one long, clean press.
 *        - If nothing happens in the window, the release is forwarded late
 *          (by <debounce_ms>), which is the price for filtering.
 *
 * USAGE
 *   powerfilter [/dev/input/eventN | -n devname] [debounce_ms] [-v]
 *   -n devname  find the device by its name (e.g. -n qpnp_pon) instead of by
 *               path. Event numbers can change between boots; names don't.
 *               The first match is used: the real device (created at boot)
 *               always has a lower number than our own virtual copy.
 *   defaults: /dev/input/event0   70
 *   -v  print a timestamped log line for every power event and for every
 *       other key, including how long after the release each bounce came.
 *       Useful for picking the right debounce_ms.
 *
 * STOPPING / REVERTING
 *   Nothing is persistent. Ctrl+C, kill, kill -9, a crash or a reboot all
 *   end with the kernel releasing the grab and deleting the virtual device,
 *   which puts the phone back to normal. On a normal stop (Ctrl+C/kill) the
 *   program also releases the power key if it was logically held, so the key
 *   can never end up "stuck down".
 *
 * NOTE: while this program runs, the real device can't be read by anyone
 * else (that is what a grab means). To see the real raw events with getevent,
 * stop this program first. To see the filtered events, read the virtual
 * device instead (it appears as a second device with the same name).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Set by the signal handler; the main loop checks it and exits cleanly. */
static volatile sig_atomic_t stop_flag;
static void on_sig(int s) { (void)s; stop_flag = 1; }

static int verbose;

/* Milliseconds from a clock that never jumps backwards (not wall-clock). */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* printf-style logger, only active with -v. Prefixes "[seconds.millis]". */
static void vlog(const char *fmt, ...) {
    if (!verbose) return;
    long long t = now_ms();
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%lld.%03lld] ", t / 1000, t % 1000);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Write one raw input event to the virtual device. The kernel fills in the
 * timestamp for us when the time field is zero. */
static void emit(int ufd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(ufd, &ev, sizeof ev) < 0) { /* nothing sensible to do */ }
}

/* Send a key event followed by SYN_REPORT. Input events are delivered in
 * "packets"; a SYN_REPORT marks the end of a packet, and Android only acts
 * on the keys once it sees it. */
static void emit_key(int ufd, int code, int value) {
    emit(ufd, EV_KEY, code, value);
    emit(ufd, EV_SYN, SYN_REPORT, 0);
}

/* The uinput device node has a different path depending on the ROM. */
static int open_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    return fd;
}

/* Name of the device to look up when -n is given (NULL = use the path). */
static const char *devname = NULL;

/* Look through /dev/input/event0..63 for a device whose name equals `name`
 * and copy its path into `out`. Returns 0 on success, -1 if not found.
 * Needed because the event number of the power key can change between boots
 * (and between ROMs), while its name ("qpnp_pon") stays the same. */
static int find_device_by_name(const char *name, char *out, size_t outsz) {
    for (int i = 0; i < 64; i++) {
        char p[64], n[80] = "";
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDONLY);
        if (f < 0) continue;
        ioctl(f, EVIOCGNAME(sizeof n - 1), n);
        close(f);
        if (!strcmp(n, name)) { snprintf(out, outsz, "%s", p); return 0; }
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *path = "/dev/input/event0";
    int delay = 70;                       /* debounce window in ms */

    /* Arguments: a path starts with '/', "-v" turns on logging, any other
     * argument is treated as the debounce time in ms. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) devname = argv[++i];
        else if (argv[i][0] == '/') path = argv[i];
        else delay = atoi(argv[i]);
    }

    /* If -n was given, resolve the name to the current /dev/input/eventN. */
    static char found[64];
    if (devname) {
        if (find_device_by_name(devname, found, sizeof found) < 0) {
            fprintf(stderr, "powerfilter: no input device named \"%s\"\n", devname);
            return 1;
        }
        path = found;
    }

    /* ---- Step 1: open the real device and learn what it looks like ---- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open input device"); return 1; }

    /* Name and bus/vendor/product IDs. We copy them to the virtual device.
     * Android picks the key layout file (.kl) by device name, so using the
     * same name means the same keys map to the same functions. */
    char name[80] = "powerfilter";
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    struct input_id id;
    memset(&id, 0, sizeof id);
    ioctl(fd, EVIOCGID, &id);

    /* Bitmap of every key this device can produce (here: power and
     * volume down). The virtual device must advertise the same keys. */
    unsigned char keybits[(KEY_MAX + 8) / 8];
    memset(keybits, 0, sizeof keybits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);

    /* ---- Step 2: create the virtual device ---- */
    int ufd = open_uinput();
    if (ufd < 0) { perror("open uinput"); return 1; }

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);     /* it will emit key events...      */
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);     /* ...and sync markers             */
    for (int k = 0; k <= KEY_MAX; k++)    /* ...for exactly the same keys    */
        if (keybits[k / 8] & (1 << (k % 8)))
            ioctl(ufd, UI_SET_KEYBIT, k);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    strncpy(ud.name, name, UINPUT_MAX_NAME_SIZE - 1);
    ud.id = id;
    if (write(ufd, &ud, sizeof ud) < 0) { perror("write uinput_user_dev"); return 1; }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

    /* ---- Step 3: grab the real device ----
     * Done AFTER the virtual device exists, so there is no moment where
     * Android has neither device and loses a button press. */
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        ioctl(ufd, UI_DEV_DESTROY);
        return 1;
    }

    /* Install signal handlers WITHOUT SA_RESTART so a signal interrupts
     * poll() immediately instead of waiting for its timeout. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    fprintf(stderr, "powerfilter: %s (\"%s\"), debounce %d ms. Ctrl+C or kill to stop.\n",
            path, name, delay);

    /* ---- Step 4: the filter loop ----
     * State machine for the power key:
     *   down      : we have told Android "power is pressed" and not yet
     *               told it "power is released".
     *   pending   : the real key was released, but we are still holding that
     *               release back, waiting to see if it was just a bounce.
     *   deadline  : when the held-back release becomes real (ms).
     *   released_at : when the real release arrived, to measure bounce gaps.
     */
    int down = 0, pending = 0;
    long long deadline = 0, released_at = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (!stop_flag) {
        /* Sleep until either an event arrives or the pending release is due.
         * With nothing pending we still wake every 500 ms to check stop_flag. */
        int timeout = 500;
        if (pending) {
            long long left = deadline - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        int r = poll(&pfd, 1, timeout);
        if (r < 0 && errno != EINTR) break;

        /* The window expired with no new press: it was a real release, so
         * forward it now. This runs BEFORE reading new events so a press
         * that arrives after the window is never mistaken for a bounce. */
        if (pending && now_ms() >= deadline) {
            vlog("POWER UP    -> no bounce within %d ms, release forwarded", delay);
            emit_key(ufd, KEY_POWER, 0);
            down = 0;
            pending = 0;
        }

        if (r > 0 && (pfd.revents & POLLIN)) {
            struct input_event evs[32];
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;                    /* device vanished: just exit */
            }

            for (size_t i = 0; i < (size_t)n / sizeof evs[0]; i++) {
                struct input_event *e = &evs[i];

                if (e->type == EV_KEY && e->code == KEY_POWER) {
                    /* ---- power key: the only event we treat specially ---- */
                    if (e->value == 1) {                     /* press */
                        if (pending) {
                            /* A press right after a release = bounce. Drop
                             * both: Android never saw the release, so it
                             * still thinks the key is held down. */
                            pending = 0;
                            vlog("POWER DOWN  -> %lld ms after release: BOUNCE, ignored",
                                 now_ms() - released_at);
                        } else if (!down) {
                            vlog("POWER DOWN  -> forwarded");
                            emit_key(ufd, KEY_POWER, 1);
                            down = 1;
                        }
                    } else if (e->value == 0) {              /* release */
                        if (down && !pending) {
                            /* Don't forward yet; wait to see if it bounces. */
                            pending = 1;
                            released_at = now_ms();
                            deadline = released_at + delay;
                            vlog("POWER UP    -> held back for up to %d ms", delay);
                        }
                    } else if (down && !pending) {
                        /* value 2 = auto-repeat while held; pass it on */
                        emit_key(ufd, KEY_POWER, 2);
                    }
                } else if (e->type == EV_KEY || e->type == EV_SYN) {
                    /* Everything else (e.g. KEY_VOLUMEDOWN on this same
                     * device) goes straight through, untouched. */
                    if (e->type == EV_KEY)
                        vlog("key %d value %d -> passed through", e->code, e->value);
                    if (write(ufd, e, sizeof *e) < 0) { /* ignore */ }
                }
            }
        }
    }

    /* ---- Cleanup: put everything back ---- */
    if (down) emit_key(ufd, KEY_POWER, 0);   /* never leave the key stuck down */
    ioctl(fd, EVIOCGRAB, 0);                 /* give the real device back      */
    ioctl(ufd, UI_DEV_DESTROY);              /* remove the virtual device      */
    close(ufd);
    close(fd);
    fprintf(stderr, "powerfilter: stopped, everything restored.\n");
    return 0;
}
