#ifndef BLE_LINK_H
#define BLE_LINK_H

#include <stddef.h>

/* BLE transport: Nordic-UART-style GATT service advertising as "ClaudeBuddy".
 * Carries the same newline-delimited JSON protocol as USB serial. */

void ble_link_init(void);
bool ble_link_connected(void);

/* Send one line (newline appended, chunked to MTU). No-op when disconnected. */
void ble_link_send(const char *line);

/* Pop one received line into buf (NUL-terminated). Returns length, 0 if none. */
int ble_link_read_line(char *buf, size_t maxlen);

#endif
