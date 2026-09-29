#include "tdeck_kbl.h"

// Cell codes: printable ASCII is itself; everything >= 0x80 is a special.
#define X_DEL   0x81
#define X_ENT   0x82
#define X_ESC   0x83
#define X_TAB   0x84
#define X_UP    0x85
#define X_DOWN  0x86
#define X_LEFT  0x87
#define X_RIGHT 0x88
#define X_SHIFT 0x90 // modifier cells; identical in every layer
#define X_SYM   0x91
#define X_ALT   0x92
#define __      0x00 // not a key / no meaning on this layer

// Physical layout, as tca8418_decode_event() reports it (column 0 is the left
// edge, so Q is r0c0). r3c0..r3c4 are not keys.
//
//      Q  W  E  R  T  Y  U  I  O  P
//      A  S  D  F  G  H  J  K  L DEL
//     ALT Z  X  C  V  B  N  M  $ ENT
//      .  .  .  .  . SHF 0 SPC SYM SHF
static const uint8_t k_base[TDECK_KBL_ROWS][TDECK_KBL_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', X_DEL},
    {X_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', X_ENT},
    {__, __, __, __, __, X_SHIFT, '0', ' ', X_SYM, X_SHIFT},
};

static const uint8_t k_shift[TDECK_KBL_ROWS][TDECK_KBL_COLS] = {
    {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
    {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', X_DEL},
    {X_ALT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '$', X_ENT},
    {__, __, __, __, __, X_SHIFT, '0', ' ', X_SYM, X_SHIFT},
};

// The legends printed on the keycaps.
static const uint8_t k_sym[TDECK_KBL_ROWS][TDECK_KBL_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'', '"', X_DEL},
    {X_ALT, '7', '8', '9', '?', '!', ',', '.', '$', X_ENT},
    {__, __, __, __, __, X_SHIFT, '0', ' ', X_SYM, X_SHIFT},
};

// Arrow cluster on E/S/F/X (as on Meshtastic), ESC on Q, TAB on T.
static const uint8_t k_alt[TDECK_KBL_ROWS][TDECK_KBL_COLS] = {
    {X_ESC, __, X_UP, __, X_TAB, __, __, __, __, __},
    {__, X_LEFT, __, X_RIGHT, __, __, __, __, __, X_DEL},
    {X_ALT, __, X_DOWN, __, __, __, __, __, __, X_ENT},
    {__, __, __, __, __, X_SHIFT, __, ' ', X_SYM, X_SHIFT},
};

void tdeck_kbl_init(tdeck_kbl_t *k)
{
    k->mods = 0;
    k->mods_ms = 0;
}

void tdeck_kbl_clear(tdeck_kbl_t *k)
{
    k->mods = 0;
}

// Unsigned subtraction keeps this correct across a millis() wrap.
static void expire(tdeck_kbl_t *k, uint32_t now_ms)
{
    if (k->mods && (uint32_t)(now_ms - k->mods_ms) > TDECK_KBL_MOD_TIMEOUT_MS) {
        k->mods = 0;
    }
}

uint8_t tdeck_kbl_mods(tdeck_kbl_t *k, uint32_t now_ms)
{
    expire(k, now_ms);
    return k->mods;
}

static bool in_range(int row, int col)
{
    return row >= 0 && row < TDECK_KBL_ROWS && col >= 0 && col < TDECK_KBL_COLS;
}

bool tdeck_kbl_is_modifier(int row, int col)
{
    if (!in_range(row, col)) return false;
    uint8_t c = k_base[row][col];
    return c == X_SHIFT || c == X_SYM || c == X_ALT;
}

tdeck_kbl_event_t tdeck_kbl_press(tdeck_kbl_t *k, int row, int col, uint32_t now_ms)
{
    tdeck_kbl_event_t ev = {TDECK_KBL_NONE, 0};
    if (!in_range(row, col)) return ev;

    uint8_t base = k_base[row][col];
    if (base == __) return ev; // not a physical key: leave everything as it was

    expire(k, now_ms);

    if (base == X_SHIFT || base == X_SYM || base == X_ALT) {
        k->mods ^= (base == X_SHIFT) ? TDECK_KBL_SHIFT : (base == X_SYM) ? TDECK_KBL_SYM : TDECK_KBL_ALT;
        k->mods_ms = now_ms;
        return ev;
    }

    const uint8_t (*layer)[TDECK_KBL_COLS] = k_base;
    if (k->mods & TDECK_KBL_ALT) layer = k_alt;
    else if (k->mods & TDECK_KBL_SYM) layer = k_sym;
    else if (k->mods & TDECK_KBL_SHIFT) layer = k_shift;
    k->mods = 0; // one-shot: whatever this key turns out to mean, the modifiers are spent

    uint8_t c = layer[row][col];
    switch (c) {
    case __:      break;
    case X_DEL:   ev.kind = TDECK_KBL_DEL; break;
    case X_ENT:   ev.kind = TDECK_KBL_ENT; break;
    case X_ESC:   ev.kind = TDECK_KBL_ESC; break;
    case X_TAB:   ev.kind = TDECK_KBL_TAB; break;
    case X_UP:    ev.kind = TDECK_KBL_UP; break;
    case X_DOWN:  ev.kind = TDECK_KBL_DOWN; break;
    case X_LEFT:  ev.kind = TDECK_KBL_LEFT; break;
    case X_RIGHT: ev.kind = TDECK_KBL_RIGHT; break;
    default:
        ev.kind = TDECK_KBL_CHAR;
        ev.ch = (char)c;
        break;
    }
    return ev;
}
