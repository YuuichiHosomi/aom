/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */
#include <immintrin.h>
#include <string.h>

#include "./aom_dsp_rtcd.h"
#include "aom_dsp/x86/convolve.h"

#define CONV8_ROUNDING_BITS (7)

static const uint8_t signal_pattern_0[32] = { 0, 1, 2, 3, 2, 3, 4, 5, 4, 5, 6,
                                              7, 6, 7, 8, 9, 0, 1, 2, 3, 2, 3,
                                              4, 5, 4, 5, 6, 7, 6, 7, 8, 9 };

static const uint8_t signal_pattern_1[32] = { 4, 5, 6,  7,  6,  7,  8,  9,
                                              8, 9, 10, 11, 10, 11, 12, 13,
                                              4, 5, 6,  7,  6,  7,  8,  9,
                                              8, 9, 10, 11, 10, 11, 12, 13 };

static const uint8_t signal_pattern_2[32] = { 6,  7,  8,  9,  8,  9,  10, 11,
                                              10, 11, 12, 13, 12, 13, 14, 15,
                                              6,  7,  8,  9,  8,  9,  10, 11,
                                              10, 11, 12, 13, 12, 13, 14, 15 };

static const uint32_t signal_index[8] = { 2, 3, 4, 5, 2, 3, 4, 5 };

typedef enum { PACK_8x1, PACK_8x2, PACK_16x1 } PixelPackFormat;

typedef void (*WritePixels)(const __m256i *y0, const __m256i *y1,
                            const __m256i *mask, uint16_t *dst,
                            ptrdiff_t pitch);

// Copy and average

static void convolve_copy_row(const uint16_t *src, uint16_t *dst, int width) {
  while (width >= 16) {
    const __m256i p = _mm256_loadu_si256((const __m256i *)src);
    _mm256_storeu_si256((__m256i *)dst, p);
    src += 16;
    dst += 16;
    width -= 16;
  }
  while (width >= 8) {
    const __m128i p = _mm_loadu_si128((const __m128i *)src);
    _mm_storeu_si128((__m128i *)dst, p);
    src += 8;
    dst += 8;
    width -= 8;
  }
  while (width >= 4) {
    const __m128i p = _mm_loadl_epi64((const __m128i *)src);
    _mm_storel_epi64((__m128i *)dst, p);
    src += 4;
    dst += 4;
    width -= 4;
  }
  if (width) {
    memcpy(dst, src, width * sizeof(uint16_t));
  }
}

void aom_highbd_convolve_copy_avx2(const uint8_t *src8, ptrdiff_t src_stride,
                                   uint8_t *dst8, ptrdiff_t dst_stride,
                                   const int16_t *filter_x, int filter_x_stride,
                                   const int16_t *filter_y, int filter_y_stride,
                                   int width, int h, int bd) {
  const uint16_t *src = CONVERT_TO_SHORTPTR(src8);
  uint16_t *dst = CONVERT_TO_SHORTPTR(dst8);
  (void)filter_x;
  (void)filter_y;
  (void)filter_x_stride;
  (void)filter_y_stride;
  (void)bd;

  do {
    convolve_copy_row(src, dst, width);
    h -= 1;
    src += src_stride;
    dst += dst_stride;
  } while (h > 0);
}

static void convolve_avg_row(const uint16_t *src, uint16_t *dst, int width) {
  int i = 0;
  while (width >= 16) {
    __m256i p = _mm256_loadu_si256((const __m256i *)src);
    const __m256i u = _mm256_loadu_si256((const __m256i *)dst);
    p = _mm256_avg_epu16(p, u);
    _mm256_storeu_si256((__m256i *)dst, p);
    src += 16;
    dst += 16;
    width -= 16;
  }
  while (width >= 8) {
    __m128i p = _mm_loadu_si128((const __m128i *)src);
    const __m128i u = _mm_loadu_si128((const __m128i *)dst);
    p = _mm_avg_epu16(p, u);
    _mm_storeu_si128((__m128i *)dst, p);
    src += 8;
    dst += 8;
    width -= 8;
  }
  while (width >= 4) {
    __m128i p = _mm_loadl_epi64((const __m128i *)src);
    const __m128i u = _mm_loadl_epi64((const __m128i *)dst);
    p = _mm_avg_epu16(p, u);
    _mm_storel_epi64((__m128i *)dst, p);
    src += 4;
    dst += 4;
    width -= 4;
  }
  while (width) {
    dst[i] = ROUND_POWER_OF_TWO(dst[i] + src[i], 1);
    i++;
    width--;
  }
}

void aom_highbd_convolve_avg_avx2(const uint8_t *src8, ptrdiff_t src_stride,
                                  uint8_t *dst8, ptrdiff_t dst_stride,
                                  const int16_t *filter_x, int filter_x_stride,
                                  const int16_t *filter_y, int filter_y_stride,
                                  int width, int height, int bd) {
  uint16_t *src = CONVERT_TO_SHORTPTR(src8);
  uint16_t *dst = CONVERT_TO_SHORTPTR(dst8);
  (void)filter_x;
  (void)filter_y;
  (void)filter_x_stride;
  (void)filter_y_stride;
  (void)bd;

  do {
    convolve_avg_row(src, dst, width);
    height -= 1;
    src += src_stride;
    dst += dst_stride;
  } while (height > 0);
}

// Horizontal Filtering

static INLINE void pack_pixels(const __m256i *s, __m256i *p /*p[4]*/) {
  const __m256i idx = _mm256_loadu_si256((const __m256i *)signal_index);
  const __m256i sf0 = _mm256_loadu_si256((const __m256i *)signal_pattern_0);
  const __m256i sf1 = _mm256_loadu_si256((const __m256i *)signal_pattern_1);
  const __m256i c = _mm256_permutevar8x32_epi32(*s, idx);

  p[0] = _mm256_shuffle_epi8(*s, sf0);  // x0x6
  p[1] = _mm256_shuffle_epi8(*s, sf1);  // x1x7
  p[2] = _mm256_shuffle_epi8(c, sf0);   // x2x4
  p[3] = _mm256_shuffle_epi8(c, sf1);   // x3x5
}

// Note:
//  Shared by 8x2 and 16x1 block
static INLINE void pack_16_pixels(const __m256i *s0, const __m256i *s1,
                                  __m256i *x /*x[8]*/) {
  __m256i pp[8];
  pack_pixels(s0, pp);
  pack_pixels(s1, &pp[4]);
  x[0] = _mm256_permute2x128_si256(pp[0], pp[4], 0x20);
  x[1] = _mm256_permute2x128_si256(pp[1], pp[5], 0x20);
  x[2] = _mm256_permute2x128_si256(pp[2], pp[6], 0x20);
  x[3] = _mm256_permute2x128_si256(pp[3], pp[7], 0x20);
  x[4] = x[2];
  x[5] = x[3];
  x[6] = _mm256_permute2x128_si256(pp[0], pp[4], 0x31);
  x[7] = _mm256_permute2x128_si256(pp[1], pp[5], 0x31);
}

static INLINE void pack_pixels_with_format(const uint16_t *src,
                                           PixelPackFormat fmt,
                                           ptrdiff_t stride, __m256i *x) {
  switch (fmt) {
    case PACK_8x1: {
      __m256i pp[8];
      __m256i s0;
      s0 = _mm256_loadu_si256((const __m256i *)src);
      pack_pixels(&s0, pp);
      x[0] = _mm256_permute2x128_si256(pp[0], pp[2], 0x30);
      x[1] = _mm256_permute2x128_si256(pp[1], pp[3], 0x30);
      x[2] = _mm256_permute2x128_si256(pp[2], pp[0], 0x30);
      x[3] = _mm256_permute2x128_si256(pp[3], pp[1], 0x30);
      break;
    }
    case PACK_8x2: {
      __m256i s0, s1;
      s0 = _mm256_loadu_si256((const __m256i *)src);
      s1 = _mm256_loadu_si256((const __m256i *)(src + stride));
      pack_16_pixels(&s0, &s1, x);
      break;
    }
    case PACK_16x1: {
      __m256i s0, s1;
      s0 = _mm256_loadu_si256((const __m256i *)src);
      s1 = _mm256_loadu_si256((const __m256i *)(src + 8));
      pack_16_pixels(&s0, &s1, x);
      break;
    }
    default: { assert(0); }
  }
}

static INLINE void pack_8x1_pixels(const uint16_t *src, const ptrdiff_t pitch,
                                   __m256i *x /*x[4]*/) {
  pack_pixels_with_format(src, PACK_8x1, pitch, x);
}

static INLINE void pack_8x2_pixels(const uint16_t *src, const ptrdiff_t pitch,
                                   __m256i *x /*x[8]*/) {
  pack_pixels_with_format(src, PACK_8x2, pitch, x);
}

static INLINE void pack_16x1_pixels(const uint16_t *src, const ptrdiff_t pitch,
                                    __m256i *x /*x[8]*/) {
  pack_pixels_with_format(src, PACK_16x1, pitch, x);
}

// Note:
//  Shared by horizontal and vertical filtering
static INLINE void pack_filters(const int16_t *filter, __m256i *f /*f[4]*/) {
  const __m128i h = _mm_loadu_si128((const __m128i *)filter);
  const __m256i hh = _mm256_insertf128_si256(_mm256_castsi128_si256(h), h, 1);
  const __m256i p0 = _mm256_set1_epi32(0x03020100);
  const __m256i p1 = _mm256_set1_epi32(0x07060504);
  const __m256i p2 = _mm256_set1_epi32(0x0b0a0908);
  const __m256i p3 = _mm256_set1_epi32(0x0f0e0d0c);
  f[0] = _mm256_shuffle_epi8(hh, p0);
  f[1] = _mm256_shuffle_epi8(hh, p1);
  f[2] = _mm256_shuffle_epi8(hh, p2);
  f[3] = _mm256_shuffle_epi8(hh, p3);
}

static INLINE void filter_8x1_pixels(const __m256i *sig /*sig[4]*/,
                                     const __m256i *fil /*fil[4]*/,
                                     __m256i *y) {
  __m256i a, a0, a1;

  a0 = _mm256_madd_epi16(fil[0], sig[0]);
  a1 = _mm256_madd_epi16(fil[3], sig[3]);
  a = _mm256_add_epi32(a0, a1);

  a0 = _mm256_madd_epi16(fil[1], sig[1]);
  a1 = _mm256_madd_epi16(fil[2], sig[2]);

  const __m256i min = _mm256_min_epi32(a0, a1);
  a = _mm256_add_epi32(a, min);

  const __m256i max = _mm256_max_epi32(a0, a1);
  a = _mm256_add_epi32(a, max);

  const __m256i rounding = _mm256_set1_epi32(1 << (CONV8_ROUNDING_BITS - 1));
  a = _mm256_add_epi32(a, rounding);
  *y = _mm256_srai_epi32(a, CONV8_ROUNDING_BITS);
}

static void write_8x1_pixels(const __m256i *y, const __m256i *z,
                             const __m256i *mask, uint16_t *dst,
                             ptrdiff_t pitch) {
  const __m128i a0 = _mm256_castsi256_si128(*y);
  const __m128i a1 = _mm256_extractf128_si256(*y, 1);
  __m128i res = _mm_packus_epi32(a0, a1);
  (void)z;
  (void)pitch;
  res = _mm_min_epi16(res, _mm256_castsi256_si128(*mask));
  _mm_storeu_si128((__m128i *)dst, res);
}

static void write_8x2_pixels(const __m256i *y0, const __m256i *y1,
                             const __m256i *mask, uint16_t *dst,
                             ptrdiff_t pitch) {
  __m256i a = _mm256_packus_epi32(*y0, *y1);
  a = _mm256_min_epi16(a, *mask);
  _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(a));
  _mm_storeu_si128((__m128i *)(dst + pitch), _mm256_extractf128_si256(a, 1));
}

static void write_16x1_pixels(const __m256i *y0, const __m256i *y1,
                              const __m256i *mask, uint16_t *dst,
                              ptrdiff_t dst_pitch) {
  (void)dst_pitch;
  __m256i a = _mm256_packus_epi32(*y0, *y1);
  a = _mm256_min_epi16(a, *mask);
  _mm256_storeu_si256((__m256i *)dst, a);
}

static void filter_block_width8_horiz(
    const uint16_t *src_ptr, ptrdiff_t src_pitch, const WritePixels write_8x1,
    const WritePixels write_8x2, uint16_t *dst_ptr, ptrdiff_t dst_pitch,
    uint32_t height, const int16_t *filter, int bd) {
  __m256i signal[8], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff[4];
  pack_filters(filter, ff);

  src_ptr -= 3;
  do {
    pack_8x2_pixels(src_ptr, src_pitch, signal);
    filter_8x1_pixels(signal, ff, &res0);
    filter_8x1_pixels(&signal[4], ff, &res1);
    write_8x2(&res0, &res1, &max, dst_ptr, dst_pitch);
    height -= 2;
    src_ptr += src_pitch << 1;
    dst_ptr += dst_pitch << 1;
  } while (height > 1);

  if (height > 0) {
    pack_8x1_pixels(src_ptr, src_pitch, signal);
    filter_8x1_pixels(signal, ff, &res0);
    write_8x1(&res0, &res1, &max, dst_ptr, dst_pitch);
  }
}

static void aom_highbd_filter_block1d8_h8_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_horiz(src, src_pitch, write_8x1_pixels, write_8x2_pixels,
                            dst, dst_pitch, height, filter, bd);
}

static void filter_block_width16_horiz(const uint16_t *src_ptr,
                                       ptrdiff_t src_pitch,
                                       const WritePixels write_16x1,
                                       uint16_t *dst_ptr, ptrdiff_t dst_pitch,
                                       uint32_t height, const int16_t *filter,
                                       int bd) {
  __m256i signal[8], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff[4];
  pack_filters(filter, ff);

  src_ptr -= 3;
  do {
    pack_16x1_pixels(src_ptr, src_pitch, signal);
    filter_8x1_pixels(signal, ff, &res0);
    filter_8x1_pixels(&signal[4], ff, &res1);
    write_16x1(&res0, &res1, &max, dst_ptr, dst_pitch);
    height -= 1;
    src_ptr += src_pitch;
    dst_ptr += dst_pitch;
  } while (height > 0);
}

static void aom_highbd_filter_block1d16_h8_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_horiz(src, src_pitch, write_16x1_pixels, dst, dst_pitch,
                             height, filter, bd);
}

// 2-tap horizontal filtering

static INLINE void pack_2t_filter(const int16_t *filter, __m256i *f) {
  const __m128i h = _mm_loadu_si128((const __m128i *)filter);
  const __m256i hh = _mm256_insertf128_si256(_mm256_castsi128_si256(h), h, 1);
  const __m256i p = _mm256_set1_epi32(0x09080706);
  f[0] = _mm256_shuffle_epi8(hh, p);
}

// can be used by pack_8x2_2t_pixels() and pack_16x1_2t_pixels()
// the difference is s0/s1 specifies first and second rows or,
// first 16 samples and 8-sample shifted 16 samples
static INLINE void pack_16_2t_pixels(const __m256i *s0, const __m256i *s1,
                                     __m256i *sig) {
  const __m256i idx = _mm256_loadu_si256((const __m256i *)signal_index);
  const __m256i sf2 = _mm256_loadu_si256((const __m256i *)signal_pattern_2);
  __m256i x0 = _mm256_shuffle_epi8(*s0, sf2);
  __m256i x1 = _mm256_shuffle_epi8(*s1, sf2);
  __m256i r0 = _mm256_permutevar8x32_epi32(*s0, idx);
  __m256i r1 = _mm256_permutevar8x32_epi32(*s1, idx);
  r0 = _mm256_shuffle_epi8(r0, sf2);
  r1 = _mm256_shuffle_epi8(r1, sf2);
  sig[0] = _mm256_permute2x128_si256(x0, x1, 0x20);
  sig[1] = _mm256_permute2x128_si256(r0, r1, 0x20);
}

static INLINE void pack_8x2_2t_pixels(const uint16_t *src,
                                      const ptrdiff_t pitch, __m256i *sig) {
  const __m256i r0 = _mm256_loadu_si256((const __m256i *)src);
  const __m256i r1 = _mm256_loadu_si256((const __m256i *)(src + pitch));
  pack_16_2t_pixels(&r0, &r1, sig);
}

static INLINE void pack_16x1_2t_pixels(const uint16_t *src,
                                       __m256i *sig /*sig[2]*/) {
  const __m256i r0 = _mm256_loadu_si256((const __m256i *)src);
  const __m256i r1 = _mm256_loadu_si256((const __m256i *)(src + 8));
  pack_16_2t_pixels(&r0, &r1, sig);
}

static INLINE void pack_8x1_2t_pixels(const uint16_t *src,
                                      __m256i *sig /*sig[2]*/) {
  const __m256i idx = _mm256_loadu_si256((const __m256i *)signal_index);
  const __m256i sf2 = _mm256_loadu_si256((const __m256i *)signal_pattern_2);
  __m256i r0 = _mm256_loadu_si256((const __m256i *)src);
  __m256i x0 = _mm256_shuffle_epi8(r0, sf2);
  r0 = _mm256_permutevar8x32_epi32(r0, idx);
  r0 = _mm256_shuffle_epi8(r0, sf2);
  sig[0] = _mm256_permute2x128_si256(x0, r0, 0x20);
}

// can be used by filter_8x2_2t_pixels() and filter_16x1_2t_pixels()
static INLINE void filter_16_2t_pixels(const __m256i *sig, const __m256i *f,
                                       __m256i *y0, __m256i *y1) {
  const __m256i rounding = _mm256_set1_epi32(1 << (CONV8_ROUNDING_BITS - 1));
  __m256i x0 = _mm256_madd_epi16(sig[0], *f);
  __m256i x1 = _mm256_madd_epi16(sig[1], *f);
  x0 = _mm256_add_epi32(x0, rounding);
  x1 = _mm256_add_epi32(x1, rounding);
  *y0 = _mm256_srai_epi32(x0, CONV8_ROUNDING_BITS);
  *y1 = _mm256_srai_epi32(x1, CONV8_ROUNDING_BITS);
}

static INLINE void filter_8x2_2t_pixels(const __m256i *sig, const __m256i *f,
                                        __m256i *y0, __m256i *y1) {
  filter_16_2t_pixels(sig, f, y0, y1);
}

static INLINE void filter_16x1_2t_pixels(const __m256i *sig, const __m256i *f,
                                         __m256i *y0, __m256i *y1) {
  filter_16_2t_pixels(sig, f, y0, y1);
}

static INLINE void filter_8x1_2t_pixels(const __m256i *sig, const __m256i *f,
                                        __m256i *y0) {
  const __m256i rounding = _mm256_set1_epi32(1 << (CONV8_ROUNDING_BITS - 1));
  __m256i x0 = _mm256_madd_epi16(sig[0], *f);
  x0 = _mm256_add_epi32(x0, rounding);
  *y0 = _mm256_srai_epi32(x0, CONV8_ROUNDING_BITS);
}

static void filter_block_width8_2t_horiz(
    const uint16_t *src_ptr, ptrdiff_t src_pitch, const WritePixels write_8x1,
    const WritePixels write_8x2, uint16_t *dst_ptr, ptrdiff_t dst_pitch,
    uint32_t height, const int16_t *filter, int bd) {
  __m256i signal[2], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff;
  pack_2t_filter(filter, &ff);

  src_ptr -= 3;
  do {
    pack_8x2_2t_pixels(src_ptr, src_pitch, signal);
    filter_8x2_2t_pixels(signal, &ff, &res0, &res1);
    write_8x2(&res0, &res1, &max, dst_ptr, dst_pitch);
    height -= 2;
    src_ptr += src_pitch << 1;
    dst_ptr += dst_pitch << 1;
  } while (height > 1);

  if (height > 0) {
    pack_8x1_2t_pixels(src_ptr, signal);
    filter_8x1_2t_pixels(signal, &ff, &res0);
    write_8x1(&res0, &res1, &max, dst_ptr, dst_pitch);
  }
}

static void aom_highbd_filter_block1d8_h2_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_2t_horiz(src, src_pitch, write_8x1_pixels,
                               write_8x2_pixels, dst, dst_pitch, height, filter,
                               bd);
}

static void filter_block_width16_2t_horiz(const uint16_t *src_ptr,
                                          ptrdiff_t src_pitch,
                                          const WritePixels write_16x1,
                                          uint16_t *dst_ptr,
                                          ptrdiff_t dst_pitch, uint32_t height,
                                          const int16_t *filter, int bd) {
  __m256i signal[2], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff;
  pack_2t_filter(filter, &ff);

  src_ptr -= 3;
  do {
    pack_16x1_2t_pixels(src_ptr, signal);
    filter_16x1_2t_pixels(signal, &ff, &res0, &res1);
    write_16x1(&res0, &res1, &max, dst_ptr, dst_pitch);
    height -= 1;
    src_ptr += src_pitch;
    dst_ptr += dst_pitch;
  } while (height > 0);
}

static void aom_highbd_filter_block1d16_h2_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_2t_horiz(src, src_pitch, write_16x1_pixels, dst,
                                dst_pitch, height, filter, bd);
}

// Vertical Filtering

static void pack_8x9_init(const uint16_t *src, ptrdiff_t pitch, __m256i *sig) {
  __m256i s0 = _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)src));
  __m256i s1 =
      _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)(src + pitch)));
  __m256i s2 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 2 * pitch)));
  __m256i s3 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 3 * pitch)));
  __m256i s4 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 4 * pitch)));
  __m256i s5 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 5 * pitch)));
  __m256i s6 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 6 * pitch)));

  s0 = _mm256_inserti128_si256(s0, _mm256_castsi256_si128(s1), 1);
  s1 = _mm256_inserti128_si256(s1, _mm256_castsi256_si128(s2), 1);
  s2 = _mm256_inserti128_si256(s2, _mm256_castsi256_si128(s3), 1);
  s3 = _mm256_inserti128_si256(s3, _mm256_castsi256_si128(s4), 1);
  s4 = _mm256_inserti128_si256(s4, _mm256_castsi256_si128(s5), 1);
  s5 = _mm256_inserti128_si256(s5, _mm256_castsi256_si128(s6), 1);

  sig[0] = _mm256_unpacklo_epi16(s0, s1);
  sig[4] = _mm256_unpackhi_epi16(s0, s1);
  sig[1] = _mm256_unpacklo_epi16(s2, s3);
  sig[5] = _mm256_unpackhi_epi16(s2, s3);
  sig[2] = _mm256_unpacklo_epi16(s4, s5);
  sig[6] = _mm256_unpackhi_epi16(s4, s5);
  sig[8] = s6;
}

static INLINE void pack_8x9_pixels(const uint16_t *src, ptrdiff_t pitch,
                                   __m256i *sig) {
  // base + 7th row
  __m256i s0 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 7 * pitch)));
  // base + 8th row
  __m256i s1 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(src + 8 * pitch)));
  __m256i s2 = _mm256_inserti128_si256(sig[8], _mm256_castsi256_si128(s0), 1);
  __m256i s3 = _mm256_inserti128_si256(s0, _mm256_castsi256_si128(s1), 1);
  sig[3] = _mm256_unpacklo_epi16(s2, s3);
  sig[7] = _mm256_unpackhi_epi16(s2, s3);
  sig[8] = s1;
}

static INLINE void filter_8x9_pixels(const __m256i *sig, const __m256i *f,
                                     __m256i *y0, __m256i *y1) {
  filter_8x1_pixels(sig, f, y0);
  filter_8x1_pixels(&sig[4], f, y1);
}

static INLINE void update_pixels(__m256i *sig) {
  int i;
  for (i = 0; i < 3; ++i) {
    sig[i] = sig[i + 1];
    sig[i + 4] = sig[i + 5];
  }
}

static INLINE void write_8x1_pixels_ver(const __m256i *y0, const __m256i *y1,
                                        const __m256i *mask, uint16_t *dst,
                                        ptrdiff_t pitch) {
  (void)pitch;
  const __m128i v0 = _mm256_castsi256_si128(*y0);
  const __m128i v1 = _mm256_castsi256_si128(*y1);
  __m128i p = _mm_packus_epi32(v0, v1);
  p = _mm_min_epi16(p, _mm256_castsi256_si128(*mask));
  _mm_storeu_si128((__m128i *)dst, p);
}

static void filter_block_width8_vert(const uint16_t *src_ptr,
                                     ptrdiff_t src_pitch, WritePixels write_8x1,
                                     WritePixels write_8x2, uint16_t *dst_ptr,
                                     ptrdiff_t dst_pitch, uint32_t height,
                                     const int16_t *filter, int bd) {
  __m256i signal[9], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff[4];
  pack_filters(filter, ff);

  pack_8x9_init(src_ptr, src_pitch, signal);

  do {
    pack_8x9_pixels(src_ptr, src_pitch, signal);

    filter_8x9_pixels(signal, ff, &res0, &res1);
    write_8x2(&res0, &res1, &max, dst_ptr, dst_pitch);
    update_pixels(signal);

    src_ptr += src_pitch << 1;
    dst_ptr += dst_pitch << 1;
    height -= 2;
  } while (height > 1);

  if (height > 0) {
    pack_8x9_pixels(src_ptr, src_pitch, signal);
    filter_8x9_pixels(signal, ff, &res0, &res1);
    write_8x1(&res0, &res1, &max, dst_ptr, dst_pitch);
  }
}

static void aom_highbd_filter_block1d8_v8_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_vert(src, src_pitch, write_8x1_pixels_ver,
                           write_8x2_pixels, dst, dst_pitch, height, filter,
                           bd);
}

static void pack_16x9_init(const uint16_t *src, ptrdiff_t pitch, __m256i *sig) {
  __m256i u0, u1, u2, u3;
  // load 0-6 rows
  const __m256i s0 = _mm256_loadu_si256((const __m256i *)src);
  const __m256i s1 = _mm256_loadu_si256((const __m256i *)(src + pitch));
  const __m256i s2 = _mm256_loadu_si256((const __m256i *)(src + 2 * pitch));
  const __m256i s3 = _mm256_loadu_si256((const __m256i *)(src + 3 * pitch));
  const __m256i s4 = _mm256_loadu_si256((const __m256i *)(src + 4 * pitch));
  const __m256i s5 = _mm256_loadu_si256((const __m256i *)(src + 5 * pitch));
  const __m256i s6 = _mm256_loadu_si256((const __m256i *)(src + 6 * pitch));

  u0 = _mm256_permute2x128_si256(s0, s1, 0x20);  // 0, 1 low
  u1 = _mm256_permute2x128_si256(s0, s1, 0x31);  // 0, 1 high

  u2 = _mm256_permute2x128_si256(s1, s2, 0x20);  // 1, 2 low
  u3 = _mm256_permute2x128_si256(s1, s2, 0x31);  // 1, 2 high

  sig[0] = _mm256_unpacklo_epi16(u0, u2);
  sig[4] = _mm256_unpackhi_epi16(u0, u2);

  sig[8] = _mm256_unpacklo_epi16(u1, u3);
  sig[12] = _mm256_unpackhi_epi16(u1, u3);

  u0 = _mm256_permute2x128_si256(s2, s3, 0x20);
  u1 = _mm256_permute2x128_si256(s2, s3, 0x31);

  u2 = _mm256_permute2x128_si256(s3, s4, 0x20);
  u3 = _mm256_permute2x128_si256(s3, s4, 0x31);

  sig[1] = _mm256_unpacklo_epi16(u0, u2);
  sig[5] = _mm256_unpackhi_epi16(u0, u2);

  sig[9] = _mm256_unpacklo_epi16(u1, u3);
  sig[13] = _mm256_unpackhi_epi16(u1, u3);

  u0 = _mm256_permute2x128_si256(s4, s5, 0x20);
  u1 = _mm256_permute2x128_si256(s4, s5, 0x31);

  u2 = _mm256_permute2x128_si256(s5, s6, 0x20);
  u3 = _mm256_permute2x128_si256(s5, s6, 0x31);

  sig[2] = _mm256_unpacklo_epi16(u0, u2);
  sig[6] = _mm256_unpackhi_epi16(u0, u2);

  sig[10] = _mm256_unpacklo_epi16(u1, u3);
  sig[14] = _mm256_unpackhi_epi16(u1, u3);

  sig[16] = s6;
}

static void pack_16x9_pixels(const uint16_t *src, ptrdiff_t pitch,
                             __m256i *sig) {
  // base + 7th row
  const __m256i s7 = _mm256_loadu_si256((const __m256i *)(src + 7 * pitch));
  // base + 8th row
  const __m256i s8 = _mm256_loadu_si256((const __m256i *)(src + 8 * pitch));

  __m256i u0, u1, u2, u3;
  u0 = _mm256_permute2x128_si256(sig[16], s7, 0x20);
  u1 = _mm256_permute2x128_si256(sig[16], s7, 0x31);

  u2 = _mm256_permute2x128_si256(s7, s8, 0x20);
  u3 = _mm256_permute2x128_si256(s7, s8, 0x31);

  sig[3] = _mm256_unpacklo_epi16(u0, u2);
  sig[7] = _mm256_unpackhi_epi16(u0, u2);

  sig[11] = _mm256_unpacklo_epi16(u1, u3);
  sig[15] = _mm256_unpackhi_epi16(u1, u3);

  sig[16] = s8;
}

static INLINE void filter_16x9_pixels(const __m256i *sig, const __m256i *f,
                                      __m256i *y0, __m256i *y1) {
  __m256i res[4];
  int i;
  for (i = 0; i < 4; ++i) {
    filter_8x1_pixels(&sig[i << 2], f, &res[i]);
  }

  const __m256i l0l1 = _mm256_packus_epi32(res[0], res[1]);
  const __m256i h0h1 = _mm256_packus_epi32(res[2], res[3]);
  *y0 = _mm256_permute2x128_si256(l0l1, h0h1, 0x20);
  *y1 = _mm256_permute2x128_si256(l0l1, h0h1, 0x31);
}

static INLINE void write_16x2_pixels(const __m256i *y0, const __m256i *y1,
                                     const __m256i *mask, uint16_t *dst,
                                     ptrdiff_t pitch) {
  __m256i p = _mm256_min_epi16(*y0, *mask);
  _mm256_storeu_si256((__m256i *)dst, p);
  p = _mm256_min_epi16(*y1, *mask);
  _mm256_storeu_si256((__m256i *)(dst + pitch), p);
}

static INLINE void write_16x1_pixels_ver(const __m256i *y0, const __m256i *y1,
                                         const __m256i *mask, uint16_t *dst,
                                         ptrdiff_t pitch) {
  (void)y1;
  (void)pitch;
  const __m256i p = _mm256_min_epi16(*y0, *mask);
  _mm256_storeu_si256((__m256i *)dst, p);
}

static void update_16x9_pixels(__m256i *sig) {
  update_pixels(&sig[0]);
  update_pixels(&sig[8]);
}

static void filter_block_width16_vert(const uint16_t *src_ptr,
                                      ptrdiff_t src_pitch,
                                      WritePixels write_16x1,
                                      WritePixels write_16x2, uint16_t *dst_ptr,
                                      ptrdiff_t dst_pitch, uint32_t height,
                                      const int16_t *filter, int bd) {
  __m256i signal[17], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);

  __m256i ff[4];
  pack_filters(filter, ff);

  pack_16x9_init(src_ptr, src_pitch, signal);

  do {
    pack_16x9_pixels(src_ptr, src_pitch, signal);
    filter_16x9_pixels(signal, ff, &res0, &res1);
    write_16x2(&res0, &res1, &max, dst_ptr, dst_pitch);
    update_16x9_pixels(signal);

    src_ptr += src_pitch << 1;
    dst_ptr += dst_pitch << 1;
    height -= 2;
  } while (height > 1);

  if (height > 0) {
    pack_16x9_pixels(src_ptr, src_pitch, signal);
    filter_16x9_pixels(signal, ff, &res0, &res1);
    write_16x1(&res0, &res1, &max, dst_ptr, dst_pitch);
  }
}

static void aom_highbd_filter_block1d16_v8_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_vert(src, src_pitch, write_16x1_pixels_ver,
                            write_16x2_pixels, dst, dst_pitch, height, filter,
                            bd);
}

// 2-tap vertical filtering

static void pack_16x2_init(const uint16_t *src, __m256i *sig) {
  sig[2] = _mm256_loadu_si256((const __m256i *)src);
}

static INLINE void pack_16x2_2t_pixels(const uint16_t *src, ptrdiff_t pitch,
                                       __m256i *sig) {
  // load the next row
  const __m256i u = _mm256_loadu_si256((const __m256i *)(src + pitch));
  sig[0] = _mm256_unpacklo_epi16(sig[2], u);
  sig[1] = _mm256_unpackhi_epi16(sig[2], u);
  sig[2] = u;
}

static INLINE void filter_16x2_2t_pixels(const __m256i *sig, const __m256i *f,
                                         __m256i *y0, __m256i *y1) {
  filter_16_2t_pixels(sig, f, y0, y1);
}

static void filter_block_width16_2t_vert(const uint16_t *src_ptr,
                                         ptrdiff_t src_pitch,
                                         WritePixels write_16x1,
                                         uint16_t *dst_ptr, ptrdiff_t dst_pitch,
                                         uint32_t height, const int16_t *filter,
                                         int bd) {
  __m256i signal[3], res0, res1;
  const __m256i max = _mm256_set1_epi16((1 << bd) - 1);
  __m256i ff;

  pack_2t_filter(filter, &ff);
  pack_16x2_init(src_ptr, signal);

  do {
    pack_16x2_2t_pixels(src_ptr, src_pitch, signal);
    filter_16x2_2t_pixels(signal, &ff, &res0, &res1);
    write_16x1(&res0, &res1, &max, dst_ptr, dst_pitch);

    src_ptr += src_pitch;
    dst_ptr += dst_pitch;
    height -= 1;
  } while (height > 0);
}

static void aom_highbd_filter_block1d16_v2_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_2t_vert(src, src_pitch, write_16x1_pixels, dst,
                               dst_pitch, height, filter, bd);
}

static INLINE void pack_8x1_2t_filter(const int16_t *filter, __m128i *f) {
  const __m128i h = _mm_loadu_si128((const __m128i *)filter);
  const __m128i p = _mm_set1_epi32(0x09080706);
  f[0] = _mm_shuffle_epi8(h, p);
}

static void pack_8x2_init(const uint16_t *src, __m128i *sig) {
  sig[2] = _mm_loadu_si128((const __m128i *)src);
}

static INLINE void pack_8x2_2t_pixels_ver(const uint16_t *src, ptrdiff_t pitch,
                                          __m128i *sig) {
  // load the next row
  const __m128i u = _mm_loadu_si128((const __m128i *)(src + pitch));
  sig[0] = _mm_unpacklo_epi16(sig[2], u);
  sig[1] = _mm_unpackhi_epi16(sig[2], u);
  sig[2] = u;
}

static INLINE void filter_8_2t_pixels(const __m128i *sig, const __m128i *f,
                                      __m128i *y0, __m128i *y1) {
  const __m128i rounding = _mm_set1_epi32(1 << (CONV8_ROUNDING_BITS - 1));
  __m128i x0 = _mm_madd_epi16(sig[0], *f);
  __m128i x1 = _mm_madd_epi16(sig[1], *f);
  x0 = _mm_add_epi32(x0, rounding);
  x1 = _mm_add_epi32(x1, rounding);
  *y0 = _mm_srai_epi32(x0, CONV8_ROUNDING_BITS);
  *y1 = _mm_srai_epi32(x1, CONV8_ROUNDING_BITS);
}

static void write_8x1_2t_pixels_ver(const __m128i *y0, const __m128i *y1,
                                    const __m128i *mask, uint16_t *dst) {
  __m128i res = _mm_packus_epi32(*y0, *y1);
  res = _mm_min_epi16(res, *mask);
  _mm_storeu_si128((__m128i *)dst, res);
}

typedef void (*Write8Pixels)(const __m128i *y0, const __m128i *y1,
                             const __m128i *mask, uint16_t *dst);

static void filter_block_width8_2t_vert(const uint16_t *src_ptr,
                                        ptrdiff_t src_pitch,
                                        Write8Pixels write_8x1,
                                        uint16_t *dst_ptr, ptrdiff_t dst_pitch,
                                        uint32_t height, const int16_t *filter,
                                        int bd) {
  __m128i signal[3], res0, res1;
  const __m128i max = _mm_set1_epi16((1 << bd) - 1);
  __m128i ff;

  pack_8x1_2t_filter(filter, &ff);
  pack_8x2_init(src_ptr, signal);

  do {
    pack_8x2_2t_pixels_ver(src_ptr, src_pitch, signal);
    filter_8_2t_pixels(signal, &ff, &res0, &res1);
    write_8x1(&res0, &res1, &max, dst_ptr);

    src_ptr += src_pitch;
    dst_ptr += dst_pitch;
    height -= 1;
  } while (height > 0);
}

static void aom_highbd_filter_block1d8_v2_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_2t_vert(src, src_pitch, write_8x1_2t_pixels_ver, dst,
                              dst_pitch, height, filter, bd);
}

// Calculation with averaging the input pixels

static void write_8x1_avg_pixels(const __m256i *y0, const __m256i *y1,
                                 const __m256i *mask, uint16_t *dst,
                                 ptrdiff_t pitch) {
  (void)y1;
  (void)pitch;
  const __m128i a0 = _mm256_castsi256_si128(*y0);
  const __m128i a1 = _mm256_extractf128_si256(*y0, 1);
  __m128i res = _mm_packus_epi32(a0, a1);
  const __m128i pix = _mm_loadu_si128((const __m128i *)dst);
  res = _mm_min_epi16(res, _mm256_castsi256_si128(*mask));
  res = _mm_avg_epu16(res, pix);
  _mm_storeu_si128((__m128i *)dst, res);
}

static void write_8x2_avg_pixels(const __m256i *y0, const __m256i *y1,
                                 const __m256i *mask, uint16_t *dst,
                                 ptrdiff_t pitch) {
  __m256i a = _mm256_packus_epi32(*y0, *y1);
  const __m128i pix0 = _mm_loadu_si128((const __m128i *)dst);
  const __m128i pix1 = _mm_loadu_si128((const __m128i *)(dst + pitch));
  const __m256i pix =
      _mm256_insertf128_si256(_mm256_castsi128_si256(pix0), pix1, 1);
  a = _mm256_min_epi16(a, *mask);
  a = _mm256_avg_epu16(a, pix);
  _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(a));
  _mm_storeu_si128((__m128i *)(dst + pitch), _mm256_extractf128_si256(a, 1));
}

static void write_16x1_avg_pixels(const __m256i *y0, const __m256i *y1,
                                  const __m256i *mask, uint16_t *dst,
                                  ptrdiff_t pitch) {
  (void)pitch;
  __m256i a = _mm256_packus_epi32(*y0, *y1);
  const __m256i pix = _mm256_loadu_si256((const __m256i *)dst);
  a = _mm256_min_epi16(a, *mask);
  a = _mm256_avg_epu16(a, pix);
  _mm256_storeu_si256((__m256i *)dst, a);
}

static INLINE void write_8x1_avg_pixels_ver(const __m256i *y0,
                                            const __m256i *y1,
                                            const __m256i *mask, uint16_t *dst,
                                            ptrdiff_t pitch) {
  (void)pitch;
  const __m128i v0 = _mm256_castsi256_si128(*y0);
  const __m128i v1 = _mm256_castsi256_si128(*y1);
  __m128i p = _mm_packus_epi32(v0, v1);
  const __m128i pix = _mm_loadu_si128((const __m128i *)dst);
  p = _mm_min_epi16(p, _mm256_castsi256_si128(*mask));
  p = _mm_avg_epu16(p, pix);
  _mm_storeu_si128((__m128i *)dst, p);
}

static INLINE void write_16x2_avg_pixels(const __m256i *y0, const __m256i *y1,
                                         const __m256i *mask, uint16_t *dst,
                                         ptrdiff_t pitch) {
  const __m256i pix0 = _mm256_loadu_si256((const __m256i *)dst);
  const __m256i pix1 = _mm256_loadu_si256((const __m256i *)(dst + pitch));
  __m256i p = _mm256_min_epi16(*y0, *mask);
  p = _mm256_avg_epu16(p, pix0);
  _mm256_storeu_si256((__m256i *)dst, p);

  p = _mm256_min_epi16(*y1, *mask);
  p = _mm256_avg_epu16(p, pix1);
  _mm256_storeu_si256((__m256i *)(dst + pitch), p);
}

static INLINE void write_16x1_avg_pixels_ver(const __m256i *y0,
                                             const __m256i *y1,
                                             const __m256i *mask, uint16_t *dst,
                                             ptrdiff_t pitch) {
  (void)y1;
  (void)pitch;
  __m256i p = _mm256_min_epi16(*y0, *mask);
  const __m256i pix = _mm256_loadu_si256((const __m256i *)dst);
  p = _mm256_avg_epu16(p, pix);
  _mm256_storeu_si256((__m256i *)dst, p);
}

static void write_8x1_2t_avg_pixels_ver(const __m128i *y0, const __m128i *y1,
                                        const __m128i *mask, uint16_t *dst) {
  __m128i res = _mm_packus_epi32(*y0, *y1);
  const __m128i pix = _mm_loadu_si128((const __m128i *)dst);
  res = _mm_min_epi16(res, *mask);
  res = _mm_avg_epu16(res, pix);
  _mm_storeu_si128((__m128i *)dst, res);
}

static void aom_highbd_filter_block1d8_h8_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_horiz(src, src_pitch, write_8x1_avg_pixels,
                            write_8x2_avg_pixels, dst, dst_pitch, height,
                            filter, bd);
}

static void aom_highbd_filter_block1d16_h8_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_horiz(src, src_pitch, write_16x1_avg_pixels, dst,
                             dst_pitch, height, filter, bd);
}

static void aom_highbd_filter_block1d8_v8_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_vert(src, src_pitch, write_8x1_avg_pixels_ver,
                           write_8x2_avg_pixels, dst, dst_pitch, height, filter,
                           bd);
}

static void aom_highbd_filter_block1d16_v8_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_vert(src, src_pitch, write_16x1_avg_pixels_ver,
                            write_16x2_avg_pixels, dst, dst_pitch, height,
                            filter, bd);
}

// 2-tap averaging

static void aom_highbd_filter_block1d8_h2_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_2t_horiz(src, src_pitch, write_8x1_avg_pixels,
                               write_8x2_avg_pixels, dst, dst_pitch, height,
                               filter, bd);
}

static void aom_highbd_filter_block1d16_h2_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_2t_horiz(src, src_pitch, write_16x1_avg_pixels, dst,
                                dst_pitch, height, filter, bd);
}

static void aom_highbd_filter_block1d16_v2_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width16_2t_vert(src, src_pitch, write_16x1_avg_pixels, dst,
                               dst_pitch, height, filter, bd);
}

static void aom_highbd_filter_block1d8_v2_avg_avx2(
    const uint16_t *src, ptrdiff_t src_pitch, uint16_t *dst,
    ptrdiff_t dst_pitch, uint32_t height, const int16_t *filter, int bd) {
  filter_block_width8_2t_vert(src, src_pitch, write_8x1_2t_avg_pixels_ver, dst,
                              dst_pitch, height, filter, bd);
}

typedef void HbdFilter1dFunc(const uint16_t *, ptrdiff_t, uint16_t *, ptrdiff_t,
                             uint32_t, const int16_t *, int);

#define HIGHBD_FUNC(width, dir, avg, opt) \
  aom_highbd_filter_block1d##width##_##dir##_##avg##opt

HbdFilter1dFunc HIGHBD_FUNC(4, h8, , sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, h2, , sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, v8, , sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, v2, , sse2);

#define aom_highbd_filter_block1d4_h8_avx2 HIGHBD_FUNC(4, h8, , sse2)
#define aom_highbd_filter_block1d4_h2_avx2 HIGHBD_FUNC(4, h2, , sse2)
#define aom_highbd_filter_block1d4_v8_avx2 HIGHBD_FUNC(4, v8, , sse2)
#define aom_highbd_filter_block1d4_v2_avx2 HIGHBD_FUNC(4, v2, , sse2)

HIGH_FUN_CONV_1D(horiz, x_step_q4, filter_x, h, src, , avx2);
HIGH_FUN_CONV_1D(vert, y_step_q4, filter_y, v, src - src_stride * 3, , avx2);
HIGH_FUN_CONV_2D(, avx2);

HbdFilter1dFunc HIGHBD_FUNC(4, h8, avg_, sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, h2, avg_, sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, v8, avg_, sse2);
HbdFilter1dFunc HIGHBD_FUNC(4, v2, avg_, sse2);

#define aom_highbd_filter_block1d4_h8_avg_avx2 HIGHBD_FUNC(4, h8, avg_, sse2)
#define aom_highbd_filter_block1d4_h2_avg_avx2 HIGHBD_FUNC(4, h2, avg_, sse2)
#define aom_highbd_filter_block1d4_v8_avg_avx2 HIGHBD_FUNC(4, v8, avg_, sse2)
#define aom_highbd_filter_block1d4_v2_avg_avx2 HIGHBD_FUNC(4, v2, avg_, sse2)

HIGH_FUN_CONV_1D(avg_horiz, x_step_q4, filter_x, h, src, avg_, avx2);
HIGH_FUN_CONV_1D(avg_vert, y_step_q4, filter_y, v, src - src_stride * 3, avg_,
                 avx2);
HIGH_FUN_CONV_2D(avg_, avx2);

#undef HIGHBD_FUNC
