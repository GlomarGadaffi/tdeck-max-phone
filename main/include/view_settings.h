// Settings: read-only vitals (WiFi/heap/PSRAM/build) plus inline-editable
// fields for WiFi, password, IP (DHCP/static), timezone and front-light,
// and a confirmed Power off. Everything edits on the one screen, so every
// keystroke is a PARTIAL refresh. Persisted in NVS and re-applied at boot by
// settings_apply_at_boot(). Snapshot of tdeck-glopanel's view_settings
// (2026-09-09) moved onto the scr_mgr lifecycle.
#ifndef VIEW_SETTINGS_H
#define VIEW_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

// Register SCR_SETTINGS with scr_mgr.
void view_settings_register(void);

// Apply persisted settings (static IP config, TZ, front-light) at boot.
// Call after epaper_display_init() and BEFORE the first WiFi connect, so the
// static-IP config is in place when the STA netif is created.
void settings_apply_at_boot(void);

#ifdef __cplusplus
}
#endif

#endif // VIEW_SETTINGS_H
