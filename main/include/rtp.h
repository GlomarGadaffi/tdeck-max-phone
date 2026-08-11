#pragma once
// Minimal RTP (RFC 3550) — just enough to carry G.711 frames for the spike.
#include <stdint.h>

#define RTP_HEADER_LEN 12

#ifdef __cplusplus
extern "C" {
#endif

// Per-stream state the sender increments each frame.
typedef struct {
    uint16_t seq;
    uint32_t timestamp;   // +POC_FRAME_SAMPLES per 20 ms G.711 frame
    uint32_t ssrc;
} rtp_tx_t;

// Write a 12-byte RTP header (V=2, no padding/ext/CSRC) into `out` (>=12 bytes).
static inline void rtp_write_header(uint8_t *out, uint8_t pt, uint8_t marker,
                                    uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    out[0]  = 0x80;                                       // V=2
    out[1]  = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
    out[2]  = (uint8_t)(seq >> 8);
    out[3]  = (uint8_t)(seq & 0xFF);
    out[4]  = (uint8_t)(ts >> 24);
    out[5]  = (uint8_t)(ts >> 16);
    out[6]  = (uint8_t)(ts >> 8);
    out[7]  = (uint8_t)(ts & 0xFF);
    out[8]  = (uint8_t)(ssrc >> 24);
    out[9]  = (uint8_t)(ssrc >> 16);
    out[10] = (uint8_t)(ssrc >> 8);
    out[11] = (uint8_t)(ssrc & 0xFF);
}

#ifdef __cplusplus
}
#endif
