#include "g711.h"

// Canonical CCITT G.711 µ-law companding (Sun reference algorithm).
#define G711_BIAS 0x84
#define G711_CLIP 32635

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int sample = pcm;                       // widen to avoid INT16_MIN overflow
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > G711_CLIP) sample = G711_CLIP;
    sample += G711_BIAS;

    int exponent = 7;
    int mask = 0x4000;                      // scan bits 14..7
    while ((sample & mask) == 0 && exponent > 0) { mask >>= 1; exponent--; }

    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

int16_t g711_ulaw_decode(uint8_t ulaw)
{
    unsigned u = (unsigned)(~ulaw);
    int t = (int)(((u & 0x0F) << 3) + G711_BIAS);
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (G711_BIAS - t) : (t - G711_BIAS));
}

void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *ulaw, size_t n)
{
    for (size_t i = 0; i < n; i++) ulaw[i] = g711_ulaw_encode(pcm[i]);
}

void g711_ulaw_decode_buf(const uint8_t *ulaw, int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) pcm[i] = g711_ulaw_decode(ulaw[i]);
}
