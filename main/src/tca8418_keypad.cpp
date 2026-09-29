// Driver core ported verbatim from tdeck-max-phone (main/src/tca8418_keypad.cpp):
// register access, init sequence, the FIFO drain, and -- most importantly --
// two hardware-measured decode facts that must never be "corrected" back to
// vendor docs (see tca8418_get_key() below). Only the keymap and the
// press-event dispatch in tca8418_get_key() are new for this project.
#include "tca8418_keypad.h"
#include "tdeck_kbl.h"
#include "board_tdeck_max.h"
#include "xl9555.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TCA8418_KEYPAD";

// TCA8418 register map (TI/NXP datasheet).
#define TCA8418_REG_CFG           0x01
#define TCA8418_REG_INT_STAT      0x02
#define TCA8418_REG_KEY_LCK_EC    0x03
#define TCA8418_REG_KEY_EVENT_A   0x04
#define TCA8418_REG_KP_GPIO1      0x1D
#define TCA8418_REG_KP_GPIO2      0x1E
#define TCA8418_REG_KP_GPIO3      0x1F

#define TCA8418_CFG_AI            (1 << 7) // auto-increment on multi-byte reads
#define TCA8418_CFG_KE_IEN        (1 << 0) // key-event interrupt enable
#define TCA8418_INT_STAT_K_INT    (1 << 0) // key-event interrupt flag (write 1 to clear)
#define TCA8418_KEY_LCK_EC_MASK   0x0F     // event-count nibble in KEY_LCK_EC

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10

// This board wires a 4-row x 10-col QWERTY matrix (per tdeck-max-phone's
// hardware bring-up, itself sourced from LilyGO's examples/keypad/keypad.ino
// and examples/factory/ui_deckpro.cpp:1415-1420, and confirmed on real
// hardware 2026-08-13: column decode correct, r3c6 really is '0').
//
// Physical layout (both layers share the same physical position):
//
//      Q  W  E  R  T  Y  U  I  O  P         #  1  2  3  (  )  _  -  +  @
//      A  S  D  F  G  H  J  K  L DEL   -->  *  4  5  6  /  :  ;  '  " DEL
//     ALT Z  X  C  V  B  N  M  $ ENT       ALT 7  8  9  ?  !  ,  .  -  ENT
//      .  .  .  .  . UP  0 SPC SYM UP       .  .  .  .  . UP  0 SPC SYM UP
//
// tdeck-max-phone (a phone, digits-first) binds the RIGHT-hand table as its
// default layer and leaves ALT/SYM inert. This project is text-entry-first
// (WiFi SSID/password, and general list navigation), so it adopts the
// modifier scheme LilyGO's own factory firmware uses for its WiFi password
// screen (examples/factory/ui_deckpro.cpp, wifi_password_process_event):
//
//   ALT tap   -> toggles abc/ABC (latching case toggle; letters only)
//   UP hold   -> digit/symbol layer (right-hand table) while held, either
//                UP key; momentary, released with the key
//   UP tap    -> list navigation (TCA8418_KEY_NAV_A/B), emitted on RELEASE
//                and only if no other key was pressed during the hold
//   SYM       -> ESC/back (TCA8418_KEY_ESC). The factory firmware uses it
//                for show/hide password; this project needs a back key more.
//
// Two keys are therefore tracked across both edges (the two UP keys) and
// ALT acts on its press edge only; every other key keeps the original
// press-only contract. A latching CASE toggle is fine (it's visible in the
// wizard's hint and in what you type); the factory-firmware bug this repo's
// notes warn about was the SYMBOL layer latching, which stays momentary.
//
// r3c0..r3c4 are NOT physical keys (confirmed from vendor source,
// tdeck-max-phone docs U5) and stay NUL in both layers.
// Base layer emits LOWERCASE. The keycaps are printed uppercase (as on any
// keyboard), but WiFi passwords and SSIDs are overwhelmingly lowercase-heavy
// and the first bring-up build, which emitted 'Q','W',... literally, could
// not join a network at all. (The two bottom-row "UP" keys ARE Shift keys --
// Meshtastic's driver treats them so -- but this legacy scheme spends them on
// the symbol layer and list navigation, so uppercase here is only reachable
// through the ALT case toggle. Text-entry mode, tdeck_kbl.h, uses them as Shift.)
static const char s_keymap_base[KEYPAD_ROWS][KEYPAD_COLS] = {
    //   c0    c1   c2   c3   c4   c5              c6   c7        c8              c9
    {   'q',  'w', 'e', 'r', 't', 'y',            'u', 'i',      'o',            'p'   }, // r0
    {   'a',  's', 'd', 'f', 'g', 'h',            'j', 'k',      'l',            '\b'  }, // r1: ...DEL
    {    0,   'z', 'x', 'c', 'v', 'b',            'n', 'm',      '$',            '\r'  }, // r2: ALT...ENT
    {    0,    0,   0,   0,   0, TCA8418_KEY_NAV_A,'0', ' ', TCA8418_KEY_ESC, TCA8418_KEY_NAV_B }, // r3  gitleaks:allow (keymap, not a key)
};

// Digit/symbol layer, verbatim from the factory firmware's
// wifi_password_chat_map (ui_deckpro.cpp) -- the legends printed on the keys.
static const char s_keymap_sym[KEYPAD_ROWS][KEYPAD_COLS] = {
    //   c0    c1   c2   c3   c4   c5              c6   c7        c8              c9
    {   '#',  '1', '2', '3', '(', ')',            '_', '-',      '+',            '@'   }, // r0
    {   '*',  '4', '5', '6', '/', ':',            ';', '\'',     '"',            '\b'  }, // r1
    {    0,   '7', '8', '9', '?', '!',            ',', '.',       0,             '\r'  }, // r2
    {    0,    0,   0,   0,   0, TCA8418_KEY_NAV_A,'0', ' ', TCA8418_KEY_ESC, TCA8418_KEY_NAV_B }, // r3  gitleaks:allow (keymap, not a key)
};

// Modifier bookkeeping (see the layering comment above).
#define ALT_ROW 2
#define ALT_COL 0
#define UP_ROW  3
#define UP_COL_A 5
#define UP_COL_B 9
static bool s_caps = false;
static tca8418_layout_t s_layout = TCA8418_LAYOUT_QWERTY;
static bool s_up_held[2] = {false, false}; // [0]=r3c5 (NAV_A), [1]=r3c9 (NAV_B)
static bool s_up_used = false;              // another key was pressed during an UP hold
static bool s_text_entry = false;           // tca8418_set_text_entry()
static tdeck_kbl_t s_kbl;                   // one-shot modifier state for text-entry mode

#if !CONFIG_TDECK_MAX_SIM_MODE
static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_KEYBOARD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // repeated start
    i2c_master_write_byte(cmd, (BOARD_KEYBOARD_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}
#endif // !CONFIG_TDECK_MAX_SIM_MODE

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] write_reg(0x%02x, 0x%02x) skipped", reg, val);
    return ESP_OK;
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_KEYBOARD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

// Consume exactly one FIFO entry and return its raw event byte (0 if the
// FIFO is empty or I2C failed).
static uint8_t read_raw_event(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    return 0;
#else
    uint8_t event_count = 0;
    esp_err_t r = read_reg(TCA8418_REG_KEY_LCK_EC, &event_count);
    if (r != ESP_OK) return 0;
    if ((event_count & TCA8418_KEY_LCK_EC_MASK) == 0) return 0;

    uint8_t raw = 0;
    if (read_reg(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK) return 0;

    // Clear the key-event interrupt flag now that we've consumed an entry;
    // this is also what releases the INT pin once the FIFO drains.
    write_reg(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);
    return raw;
#endif
}

esp_err_t tca8418_init(void)
{
    ESP_LOGI(TAG, "Initializing TCA8418 keypad controller...");

    // Reset TCA8418 via XL9555
    xl9555_reset_keyboard();
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configure Keyboard Backlight LED Pin
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_KEYBOARD_LED);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(BOARD_KEYBOARD_LED, 1); // Enable backlight by default

    // Configure Keyboard Interrupt Pin. Despite the name, this project never
    // installs a GPIO ISR for it -- tca8418_key_pending() below just polls
    // the level directly (INT is active-low and stays asserted while events
    // are queued, so one GPIO read stands in for an I2C status check).
    // GPIO_INTR_DISABLE reflects that actual usage; edge-triggering an
    // interrupt vector that nothing ever services was live in the ported
    // source too, but was misleading about how this pin is really consumed.
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_KEYBOARD_INT);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Enable rows 0-3 (KP_GPIO1 bits 0-3) and columns 0-9 (KP_GPIO2 bits
    // 0-7, KP_GPIO3 bits 0-1) as keypad matrix pins, per TCA8418 datasheet
    // and the vendor reference's Adafruit_TCA8418::matrix(4, 10).
    esp_err_t ret = write_reg(TCA8418_REG_KP_GPIO1, (1 << KEYPAD_ROWS) - 1);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to configure keypad rows"); return ret; }
    ret = write_reg(TCA8418_REG_KP_GPIO2, 0xFF);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to configure keypad cols (0-7)"); return ret; }
    ret = write_reg(TCA8418_REG_KP_GPIO3, (1 << (KEYPAD_COLS - 8)) - 1);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to configure keypad cols (8-9)"); return ret; }

    // Enable key-event interrupt + auto-increment for FIFO reads.
    ret = write_reg(TCA8418_REG_CFG, TCA8418_CFG_AI | TCA8418_CFG_KE_IEN);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to configure CFG register"); return ret; }

    // Drain stale events left in the FIFO from before reset settled. Uses
    // the RAW reader, not tca8418_get_key(): most keys map to NUL, and a
    // drain loop keyed on get_key()'s return value stops at the first such
    // key, leaving the FIFO partly full.
    int drained = 0;
    while (read_raw_event() != 0 && ++drained < 32) {}
    if (drained) ESP_LOGI(TAG, "drained %d stale key event(s)", drained);

    s_caps = false;
    s_up_held[0] = s_up_held[1] = false;
    s_up_used = false;
    tdeck_kbl_init(&s_kbl);
    ESP_LOGI(TAG, "TCA8418 keypad initialized successfully");
    return ESP_OK;
}

bool tca8418_key_pending(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    return false;
#else
    // INT is active-low and stays asserted while events are queued, so this
    // is a single GPIO read standing in for 2-3 I2C transactions.
    return gpio_get_level(BOARD_KEYBOARD_INT) == 0;
#endif
}

char tca8418_get_key(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    return 0; // no real FIFO to poll under QEMU
#else
    uint8_t raw = read_raw_event();
    if (raw == 0) return 0;

    // bit 7 SET = PRESS. MEASURED on real hardware 2026-08-13 by
    // tdeck-max-phone -- do NOT "fix" this to match the Adafruit driver's
    // comment, which claims the opposite. Held one key for 4.58 s with
    // CONFIG_TDECK_MAX_KEYPAD_DEBUG=y:
    //
    //   key[1] t=29144ms raw=0x8a bit7=1   <- press  (first event of the hold)
    //   key[2] t=33728ms raw=0x0a bit7=0   <- release, 4584 ms later
    //
    // Two vendor sources say otherwise and both are wrong for this board;
    // tracked and closed as invalid in tdeck-max-phone#35.
    bool pressed = (raw & 0x80) != 0;

    int key_num = (raw & 0x7F) - 1; // 1-based on the wire
    if (key_num < 0 || key_num >= KEYPAD_ROWS * KEYPAD_COLS) return 0;

    // Column reversal also MEASURED in the same session: the controller
    // numbers this matrix right-to-left, so Q is key_num 9 and P is key_num 0.
    int row = key_num / KEYPAD_COLS;
    int col = (KEYPAD_COLS - 1) - (key_num % KEYPAD_COLS);

#if CONFIG_TDECK_MAX_KEYPAD_DEBUG
    static uint32_t s_evt_seq = 0;
    ESP_LOGI(TAG,
             "key[%3lu] t=%7lums raw=0x%02x bit7=%d key_num=%2d -> r%dc%d  [%s]",
             (unsigned long)++s_evt_seq,
             (unsigned long)(esp_timer_get_time() / 1000),
             raw, (raw & 0x80) ? 1 : 0, key_num, row, col,
             pressed ? "PRESS" : "RELEASE");
#endif

    // Text-entry mode (QWERTY layout only): one-shot Shift/Sym/Alt, press edges
    // only (tdeck_kbl.h).
    if (s_text_entry && s_layout == TCA8418_LAYOUT_QWERTY) {
        if (!pressed) return 0;
        const tdeck_kbl_event_t ev = tdeck_kbl_press(&s_kbl, row, col, (uint32_t)(esp_timer_get_time() / 1000));
        switch (ev.kind) {
        case TDECK_KBL_CHAR:  return ev.ch;
        case TDECK_KBL_DEL:   return '\b';
        case TDECK_KBL_ENT:   return '\r';
        case TDECK_KBL_ESC:   return TCA8418_KEY_ESC;
        case TDECK_KBL_UP:    return TCA8418_KEY_NAV_A;
        case TDECK_KBL_DOWN:  return TCA8418_KEY_NAV_B;
        case TDECK_KBL_LEFT:  return TCA8418_KEY_LEFT;
        case TDECK_KBL_RIGHT: return TCA8418_KEY_RIGHT;
        default:              return 0; // a modifier was toggled, or the key means nothing here
        }
    }

    // UP keys: tracked across both edges. Held -> symbol layer for other
    // keys; tapped alone -> navigation, emitted on release.
    if (row == UP_ROW && (col == UP_COL_A || col == UP_COL_B)) {
        int idx = (col == UP_COL_A) ? 0 : 1;
        if (pressed) {
            s_up_held[idx] = true;
            return 0;
        }
        bool was_held = s_up_held[idx];
        s_up_held[idx] = false;
        bool tap = was_held && !s_up_used;
        if (!s_up_held[0] && !s_up_held[1]) s_up_used = false;
        return tap ? (idx == 0 ? TCA8418_KEY_NAV_A : TCA8418_KEY_NAV_B) : 0;
    }

    // ALT: case toggle on the press edge only; never produces a character.
    if (row == ALT_ROW && col == ALT_COL) {
        if (pressed) s_caps = !s_caps;
        return 0;
    }

    if (!pressed) return 0; // every other key stays press-only, matching the original contract

    if (s_layout == TCA8418_LAYOUT_DIALPAD) {
        // Digits-first: the printed alt legends ARE the layer, no modifier
        // needed (holding UP is harmless). Letters and space are swallowed
        // so a stray key can never reach a dial buffer.
        char d = s_keymap_sym[row][col];
        bool dial = (d >= '0' && d <= '9') || d == '*' || d == '#' || d == '+';
        bool ctrl = d == '\b' || d == '\r' || d == TCA8418_KEY_ESC;
        return (dial || ctrl) ? d : 0;
    }

    if (s_up_held[0] || s_up_held[1]) {
        s_up_used = true;
        return s_keymap_sym[row][col];
    }
    char c = s_keymap_base[row][col];
    if (s_caps && c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
    return c;
#endif
}

void tca8418_set_layout(tca8418_layout_t layout)
{
    s_layout = layout;
}

tca8418_layout_t tca8418_get_layout(void)
{
    return s_layout;
}

void tca8418_set_text_entry(bool on)
{
    if (on == s_text_entry) return;
    s_text_entry = on;
    // Start the other mode from a clean slate: a modifier armed in one must not leak into
    // the other, and UP edges seen in text mode were never tracked by the legacy path.
    tdeck_kbl_init(&s_kbl);
    s_up_held[0] = s_up_held[1] = false;
    s_up_used = false;
}

bool tca8418_text_entry(void)
{
    return s_text_entry;
}

bool tca8418_caps_enabled(void)
{
    if (s_text_entry && s_layout == TCA8418_LAYOUT_QWERTY) {
        return (tdeck_kbl_mods(&s_kbl, (uint32_t)(esp_timer_get_time() / 1000)) & TDECK_KBL_SHIFT) != 0;
    }
    return s_caps;
}

void tca8418_set_backlight(bool enable)
{
    gpio_set_level(BOARD_KEYBOARD_LED, enable ? 1 : 0);
}
