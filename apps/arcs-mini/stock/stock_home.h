#ifndef ARCS_MINI_STOCK_HOME_H
#define ARCS_MINI_STOCK_HOME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct home_nav_scr_data;
void stock_home_init(struct home_nav_scr_data *home);
void stock_home_deinit(struct home_nav_scr_data *home);
/* Nonblocking, no LVGL calls. True means the idle stock page consumed the gesture. */
bool stock_home_handle_button(uint8_t event);
void stock_home_status(char *text, size_t size);

#endif
