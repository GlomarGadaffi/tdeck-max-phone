#pragma once
// G.711 µ-law (PCMU, RTP payload type 0). 16-bit linear PCM <-> 8-bit µ-law.
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t g711_ulaw_encode(int16_t pcm);
int16_t g711_ulaw_decode(uint8_t ulaw);

// Buffer helpers: n samples in == n bytes out (and vice-versa).
void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *ulaw, size_t n);
void g711_ulaw_decode_buf(const uint8_t *ulaw, int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
