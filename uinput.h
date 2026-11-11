/* Minimal uinput keyboard for sending the paste shortcut. */
#ifndef CUIMOJI_UINPUT_H
#define CUIMOJI_UINPUT_H

#include <stdbool.h>

int uinput_keyboard_open(const char *name);
void uinput_paste(int fd, bool shift);
void uinput_keyboard_close(int fd);

#endif
