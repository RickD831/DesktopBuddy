#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_port_init(void);

/* Take the LVGL mutex before calling any lv_* API from outside the LVGL task.
 * timeout_ms = -1 blocks forever. */
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
