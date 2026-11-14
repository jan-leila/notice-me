// Pure-C horizontal "marquee" timing for text wider than its box (no pebble.h, no floating point; host-tested).
// A loop: hold at the start, then scroll left at NM_MARQUEE_SPEED px/s through the whole text and a
// NM_MARQUEE_GAP px gap, so a second copy drawn text_w + gap px to the right comes round to exactly where the first
// started; then hold again and repeat, for as long as the text is shown.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NM_MARQUEE_HOLD_MS 1000
#define NM_MARQUEE_SPEED 35    // px per second
#define NM_MARQUEE_GAP 24      // px between the end of the text and its next copy

// Offset in px (0 <= offset < text_w + NM_MARQUEE_GAP, how far the text is shifted left) elapsed_ms after the
// marquee (re)started. Draw the text at x - offset and again at x - offset + text_w + NM_MARQUEE_GAP.
// *wait_ms (may be NULL) = ms until the offset next changes; 0 only when the text fits (it never moves).
int nm_marquee_offset(int text_w, int box_w, uint32_t elapsed_ms, uint32_t *wait_ms);
// ms one loop takes (0 when the text fits).
uint32_t nm_marquee_cycle_ms(int text_w, int box_w);
