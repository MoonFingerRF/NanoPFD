// ============================================================================
//  RLE332.h — per-line PackBits-style RLE for 8-bit (RGB332) instrument frames.
//
//  Our rendered frames are mostly long solid horizontal runs (sky / ground / black
//  ND background / solid UI) with short noisy stretches (text, map lines). PackBits
//  is the right fit: it encodes both runs AND literal stretches, and its worst-case
//  expansion is bounded (raw + ceil(raw/128) + 1). Plain (count,value) RLE would
//  DOUBLE noisy lines; PackBits never blows up.
//
//  Control byte C per token:
//    C & 0x80  -> RUN:     repeat the next 1 byte (C & 0x7F)+1 times   (1..128)
//    C & 0x80==0 -> LITERAL: copy the next C+1 bytes verbatim          (1..128)
//
//  Each scanline is encoded independently into a FIXED stride slot (the per-line
//  size cap) so the two render cores can encode their regions in parallel without
//  needing to know where prior lines ended. We store the actual encoded length
//  separately and only ever touch that many bytes (that's where the bandwidth win is).
// ============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

// Worst-case encoded bytes for a width-w line (all-literal: one control byte per 128).
#define RLE332_MAX_BYTES(w)  ((w) + (((w) + 127) / 128) + 1)

// Encode n bytes of src into dst. Returns the encoded length (<= RLE332_MAX_BYTES(n)).
// Min run = 3: a run of 2 costs the same as 2 literals, so encoding it as a RUN token
// would only add control-byte overhead by splitting a literal block. Keeping min-run at 3
// makes the true worst case exactly all-literal (n + ceil(n/128)) -> the bound always holds.
static inline int rle332_encode(const uint8_t *src, int n, uint8_t *dst) {
  int di = 0, i = 0;
  while (i < n) {
    int run = 1;
    while (i + run < n && src[i + run] == src[i] && run < 128) run++;
    if (run >= 3) {
      dst[di++] = (uint8_t)(0x80 | (run - 1));   // RUN token
      dst[di++] = src[i];
      i += run;
    } else {
      // LITERAL block, up to 128, ending where a run of >=3 begins.
      int start = i, lit = 0;
      while (i < n && lit < 128) {
        if (i + 2 < n && src[i + 1] == src[i] && src[i + 2] == src[i]) break;  // >=3 run ahead
        i++; lit++;
      }
      dst[di++] = (uint8_t)(lit - 1);              // LITERAL token (high bit clear)
      memcpy(dst + di, src + start, lit);
      di += lit;
    }
  }
  return di;
}

// Fused encode straight from an 8-bit INDEX line: runs are detected on the index bytes
// (cheap, and a flat line skips its whole run in a few ops) and the palette lookup pal[]
// is applied only when emitting. Output is byte-identical to decoding -> the same RGB332
// pixels. Same bound as rle332_encode. This avoids a separate full lookup pass, so a flat
// line costs ~nothing instead of n writes.
static inline int rle332_encode_indexed(const uint8_t *idx, int n, uint8_t *dst,
                                        const uint8_t *pal) {
  int di = 0, i = 0;
  while (i < n) {
    uint8_t v = idx[i];
    int run = 1, maxrun = n - i; if (maxrun > 128) maxrun = 128;
    // Run detection, 4 bytes per compare via ALIGNED 32-bit loads (the frame is mostly long
    // solid runs). Byte-step until the address is 4-aligned (the S3 traps unaligned loads),
    // then word-compare against a broadcast of v, then byte-step the tail.
    while (run < maxrun && ((uintptr_t)(idx + i + run) & 3u) && idx[i + run] == v) run++;
    if (run < maxrun && idx[i + run] == v) {
      uint32_t bc = (uint32_t)v * 0x01010101u;
      while (run + 4 <= maxrun && *(const uint32_t *)(idx + i + run) == bc) run += 4;
      while (run < maxrun && idx[i + run] == v) run++;
    }
    if (run >= 3) {
      dst[di++] = (uint8_t)(0x80 | (run - 1));
      dst[di++] = pal[v];
      i += run;
    } else {
      // LITERAL: single pass — emit while scanning, backpatch the count byte.
      int countPos = di++;
      int lit = 0;
      while (i < n && lit < 128) {
        if (i + 2 < n && idx[i + 1] == idx[i] && idx[i + 2] == idx[i]) break;  // >=3 run ahead
        dst[di++] = pal[idx[i]];
        i++; lit++;
      }
      dst[countPos] = (uint8_t)(lit - 1);
    }
  }
  return di;
}

// Decode up to n output bytes into dst from a srclen-byte encoded line.
static inline void rle332_decode(const uint8_t *src, int srclen, uint8_t *dst, int n) {
  int di = 0, i = 0;
  while (i < srclen && di < n) {
    uint8_t c = src[i++];
    if (c & 0x80) {                                // RUN
      int cnt = (c & 0x7F) + 1;
      if (cnt > n - di) cnt = n - di;
      memset(dst + di, src[i++], cnt);
      di += cnt;
    } else {                                       // LITERAL
      int cnt = c + 1;
      if (cnt > n - di) cnt = n - di;
      memcpy(dst + di, src + i, cnt);
      di += cnt; i += cnt;
    }
  }
}

#ifdef RLE332_TEST_MAIN
// Host roundtrip test: g++ -DRLE332_TEST_MAIN -x c++ RLE332.h -o /tmp/rletest && /tmp/rletest
#include <stdio.h>
#include <stdlib.h>
static int test_one(const uint8_t *line, int w, const char *name) {
  uint8_t enc[4096], dec[4096];
  int el = rle332_encode(line, w, enc);
  int bound = RLE332_MAX_BYTES(w);
  rle332_decode(enc, el, dec, w);
  int ok = (memcmp(line, dec, w) == 0) && (el <= bound);
  printf("  %-22s w=%d enc=%4d (%.1f%%) bound=%d %s\n", name, w, el, 100.0 * el / w, bound,
         ok ? "OK" : "*** FAIL ***");
  return ok;
}
int main() {
  uint8_t buf[1024];
  int w = 450, fails = 0;

  for (int i = 0; i < w; i++) buf[i] = 0x00;                 fails += !test_one(buf, w, "all-zero");
  for (int i = 0; i < w; i++) buf[i] = 0xE0;                 fails += !test_one(buf, w, "all-one-color");
  for (int i = 0; i < w; i++) buf[i] = (i & 1) ? 0xE0 : 0x03; fails += !test_one(buf, w, "alternating(worst)");
  for (int i = 0; i < w; i++) buf[i] = (i / 50) % 4 * 0x40;  fails += !test_one(buf, w, "wide-bands");
  // flat with a noisy text-like band in the middle
  for (int i = 0; i < w; i++) buf[i] = 0x00;
  for (int i = 200; i < 260; i++) buf[i] = rand() & 0xFF;    fails += !test_one(buf, w, "flat+text-band");
  // fully random (incompressible)
  for (int i = 0; i < w; i++) buf[i] = rand() & 0xFF;        fails += !test_one(buf, w, "random");
  // 1000 random fuzz lines of random widths
  int fuzzfail = 0;
  for (int t = 0; t < 1000; t++) {
    int ww = 1 + rand() % 700;
    for (int i = 0; i < ww; i++) buf[i] = (rand() % 5 == 0) ? (rand() & 0xFF) : 0x00;  // mostly flat
    uint8_t enc[1024], dec[1024];
    int el = rle332_encode(buf, ww, enc);
    rle332_decode(enc, el, dec, ww);
    if (memcmp(buf, dec, ww) != 0 || el > RLE332_MAX_BYTES(ww)) fuzzfail++;
  }
  printf("  fuzz 1000 lines: %s\n", fuzzfail ? "*** FAIL ***" : "OK");
  fails += (fuzzfail != 0);
  // indexed encoder: build a random palette, encode from index, decode, compare to pal[idx]
  uint8_t pal[256]; for (int i = 0; i < 256; i++) pal[i] = rand() & 0xFF;
  int idxfail = 0;
  for (int t = 0; t < 1000; t++) {
    int ww = 1 + rand() % 700;
    uint8_t idx[1024], expect[1024], enc[1024], dec[1024];
    for (int i = 0; i < ww; i++) idx[i] = (rand() % 5 == 0) ? (rand() & 0x0F) : 0x00;  // mostly flat
    for (int i = 0; i < ww; i++) expect[i] = pal[idx[i]];
    int el = rle332_encode_indexed(idx, ww, enc, pal);
    rle332_decode(enc, el, dec, ww);
    if (memcmp(expect, dec, ww) != 0 || el > RLE332_MAX_BYTES(ww)) idxfail++;
  }
  printf("  indexed fuzz 1000: %s\n", idxfail ? "*** FAIL ***" : "OK");
  fails += (idxfail != 0);
  printf("%s\n", fails ? "FAILED" : "ALL PASS");
  return fails ? 1 : 0;
}
#endif
