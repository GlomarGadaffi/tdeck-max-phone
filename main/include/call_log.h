// Recent calls, newest first, persisted as one NVS blob. Replaces the old
// single "last dialled" key: redial-from-idle is the newest connected
// outbound entry, and the dialer shows the last few calls as rows.
#ifndef CALL_LOG_H
#define CALL_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CALL_LOG_MAX        8
#define CALL_LOG_NUMBER_MAX 20

typedef enum { CALL_DIR_OUT = 0, CALL_DIR_IN = 1 } call_dir_t;

typedef struct {
    char     number[CALL_LOG_NUMBER_MAX + 1];
    uint8_t  dir;         // call_dir_t
    uint8_t  answered;    // connected (media flowed) vs missed/failed
    uint16_t duration_s;
} call_log_entry_t;

esp_err_t call_log_init(void);
int call_log_count(void);
const call_log_entry_t *call_log_get(int idx);   // 0 = newest; NULL out of range
void call_log_add(const char *number, call_dir_t dir, bool answered, uint16_t duration_s);

// Newest connected outbound number, or "" if none. Valid until the next
// call_log_add().
const char *call_log_last_dialled(void);

#ifdef __cplusplus
}
#endif

#endif // CALL_LOG_H
