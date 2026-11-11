/* A tiny virtual keyboard over /dev/uinput.
 *
 * This is the fast path: the device is created once at startup, so a paste is
 * only a handful of write(2)s. No daemon, no process spawn, no portal.
 */
#define _GNU_SOURCE
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    return (int)write(fd, &ev, sizeof ev);
}

int uinput_keyboard_open(const char *name) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "cuimoji: /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, KEY_V) < 0) {
        fprintf(stderr, "cuimoji: uinput key setup: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1209;
    setup.id.product = 0x0001;
    snprintf(setup.name, sizeof setup.name, "%s", name ? name : "cuimoji virtual keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "cuimoji: uinput device create: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void uinput_paste(int fd, bool shift) {
    if (fd < 0) return;

    emit(fd, EV_KEY, KEY_LEFTCTRL, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    if (shift) {
        emit(fd, EV_KEY, KEY_LEFTSHIFT, 1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
    }
    usleep(5000);
    emit(fd, EV_KEY, KEY_V, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(3000);
    emit(fd, EV_KEY, KEY_V, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(3000);
    if (shift) {
        emit(fd, EV_KEY, KEY_LEFTSHIFT, 0);
        emit(fd, EV_SYN, SYN_REPORT, 0);
    }
    emit(fd, EV_KEY, KEY_LEFTCTRL, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

void uinput_keyboard_close(int fd) {
    if (fd < 0) return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
