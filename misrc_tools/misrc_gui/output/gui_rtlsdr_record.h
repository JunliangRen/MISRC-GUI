/* RTL-SDR I/Q to real-RF recording. The native A/B writer is not used here. */
#ifndef GUI_RTLSDR_RECORD_H
#define GUI_RTLSDR_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_app gui_app_t;

bool gui_rtlsdr_record_requested(const gui_app_t *app);
bool gui_rtlsdr_record_validate(gui_app_t *app, char *error, size_t error_size);
/* UI-thread setup, with admission closed until counters/log/session are ready. */
bool gui_rtlsdr_record_start(gui_app_t *app, const char *path, char *error, size_t error_size);
void gui_rtlsdr_record_begin(gui_app_t *app);
/* Closes admission synchronously; safe even while a USB push is in flight. */
void gui_rtlsdr_record_request_stop(gui_app_t *app);
/* Finalization thread: drain/join/close and return the actual output count. */
uint64_t gui_rtlsdr_record_finish(gui_app_t *app);
bool gui_rtlsdr_record_has_write_error(void);

/* Called on the USB thread, before display conversion or A/B swapping. */
void gui_rtlsdr_record_push(gui_app_t *app, const uint8_t *iq, size_t bytes);
void gui_rtlsdr_record_capture_error(gui_app_t *app, const char *reason);

#endif
