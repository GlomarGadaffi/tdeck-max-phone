#include "tca8418_keypad.h"
#include "board_tdeck_max.h"
#include "xl9555.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
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

// This board wires a 4-row x 10-col QWERTY matrix (see LilyGO's
// examples/keypad/keypad.ino reference for T-Deck-MAX). We only care about
// dialing/call-control here, so most letter keys map to NUL (ignored by
// callers) and only what a phone UI needs is given a real ASCII code.
#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10

static const char s_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {0,   0,   0,   0,   0,   0,   0,   0,   0,   0  },
    {0,   0,   0,   0,   0,   0,   0,   0,   0,   '\b'}, // DEL (backspace/cancel)
    {0,   0,   0,   0,   0,   0,   0,   0,   0,   '\r'}, // ENT (dial/answer)
    {0,   0,   0,   0,   0,   0,   '0', ' ', 0,   0  },
};
// Digits 1-9 and *,# aren't on this QWERTY matrix at all on real hardware
// (it's a chat-style keyboard, not a numeric keypad) -- number entry uses
// the top QWERTY row digits via a shift/ALT layer in the full driver this
// stubs toward. For the PoC's dial/answer/hangup flow this reduced map is
// sufficient; extending to full digit entry is UI-layer scope (see #9).

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] read_reg(0x%02x) skipped", reg);
    *val = 0;
    return ESP_OK;
#else
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
#endif
}

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

    // Configure Keyboard Interrupt Pin
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
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

    // Drain any stale events left in the FIFO from before reset settled.
    while (tca8418_get_key() != 0) {}

    ESP_LOGI(TAG, "TCA8418 keypad initialized successfully");
    return ESP_OK;
}

char tca8418_get_key(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    return 0; // no real FIFO to poll under QEMU
#else
    uint8_t event_count = 0;
    if (read_reg(TCA8418_REG_KEY_LCK_EC, &event_count) != ESP_OK) return 0;
    if ((event_count & TCA8418_KEY_LCK_EC_MASK) == 0) return 0;

    uint8_t raw = 0;
    if (read_reg(TCA8418_REG_KEY_EVENT_A, &raw) != ESP_OK || raw == 0) return 0;

    // Clear the key-event interrupt flag now that we've consumed an entry.
    write_reg(TCA8418_REG_INT_STAT, TCA8418_INT_STAT_K_INT);

    bool pressed = (raw & 0x80) != 0;
    int key_num = (raw & 0x7F) - 1; // datasheet: 1-based, 0x80 = press bit
    if (key_num < 0 || key_num >= KEYPAD_ROWS * KEYPAD_COLS) return 0;

    int row = key_num / KEYPAD_COLS;
    int col = (KEYPAD_COLS - 1) - (key_num % KEYPAD_COLS);

    if (!pressed) return 0; // only report presses, matching this function's existing contract
    return s_keymap[row][col];
#endif
}

void tca8418_set_backlight(bool enable)
{
    gpio_set_level(BOARD_KEYBOARD_LED, enable ? 1 : 0);
}
