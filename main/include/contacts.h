// Phonebook, persisted as one NVS blob. Small on purpose: this is a desk
// phone's speed-dial list, not an address book sync target.
#ifndef CONTACTS_H
#define CONTACTS_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTACT_NAME_MAX   24
#define CONTACT_NUMBER_MAX 20
#define CONTACTS_MAX       32

typedef struct {
    char name[CONTACT_NAME_MAX + 1];
    char number[CONTACT_NUMBER_MAX + 1];
} contact_t;

// Load the list from NVS (an absent blob is an empty list, not an error).
esp_err_t contacts_init(void);

int contacts_count(void);
const contact_t *contacts_get(int idx);           // NULL when out of range
int contacts_find_by_number(const char *number);  // -1 when unknown

// idx -1 appends. The list is kept sorted by name (case-insensitive) and
// written back to NVS on every change.
esp_err_t contacts_set(int idx, const contact_t *c);
esp_err_t contacts_delete(int idx);

// The contact's name for a number, or the number itself when unknown. The
// returned pointer is valid until the next contacts_* call.
const char *contacts_display_name(const char *number);

#ifdef __cplusplus
}
#endif

#endif // CONTACTS_H
