/*
 * SSE2/SSSE3/AVX2-optimized routines to support checksumming of bytes.
 *
 * Copyright (C) 1996 Andrew Tridgell
 * Copyright (C) 1996 Paul Mackerras
 * Copyright (C) 2004-2020 Wayne Davison
 * Copyright (C) 2020 Jorrit Jongma
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */
/*
 * Optimization target for get_checksum1() was the Intel Atom D2700, the
 * slowest CPU in the test set and the most likely to be CPU limited during
 * transfers. The combination of intrinsics was chosen specifically for the
 * most gain on that CPU, other combinations were occasionally slightly
 * faster on the others.
 *
 * While on more modern CPUs transfers are less likely to be CPU limited
 * (at least by this specific function), lower CPU usage is always better.
 * Improvements may still be seen when matching chunks from NVMe storage
 * even on newer CPUs.
 *
 * Benchmarks (in MB/s)            C    SSE2   SSSE3    AVX2
 * - Intel Atom D2700            550     750    1000     N/A
 * - Intel i7-7700hq            1850    2550    4050    6200
 * - AMD ThreadRipper 2950x     2900    5600    8950    8100
 *
 * Curiously the AMD is slower with AVX2 than SSSE3, while the Intel is
 * significantly faster. AVX2 is kept because it's more likely to relieve
 * the bottleneck on the slower CPU.
 *
 * This optimization for get_checksum1() is intentionally limited to x86-64
 * as no 32-bit CPU was available for testing. As 32-bit CPUs only have half
 * the available xmm registers, this optimized version may not be faster than
 * the pure C version anyway. Note that all x86-64 CPUs support at least SSE2.
 *
 * This file is compiled using GCC 4.8+'s C++ front end to allow the use of
 * the target attribute, selecting the fastest code path based on runtime
 * detection of CPU capabilities.
 */

#ifdef __x86_64__
#ifdef __cplusplus

#include "rsync.h"

#ifdef HAVE_SIMD

#include <immintrin.h>

/* Compatibility functions to let our SSSE3 algorithm run on SSE2 */

__attribute__ ((target("sse2"))) static inline __m128i sse_interleave_odd_epi16(__m128i a, __m128i b) {
    return _mm_packs_epi32(
        _mm_srai_epi32(a, 16),
        _mm_srai_epi32(b, 16)
    );
}

__attribute__ ((target("sse2"))) static inline __m128i sse_interleave_even_epi16(__m128i a, __m128i b) {
    return sse_interleave_odd_epi16(
        _mm_slli_si128(a, 2),
        _mm_slli_si128(b, 2)
    );
}

__attribute__ ((target("sse2"))) static inline __m128i sse_mulu_odd_epi8(__m128i a, __m128i b) {
    return _mm_mullo_epi16(
        _mm_srli_epi16(a, 8),
        _mm_srai_epi16(b, 8)
    );
}

__attribute__ ((target("sse2"))) static inline __m128i sse_mulu_even_epi8(__m128i a, __m128i b) {
    return _mm_mullo_epi16(
        _mm_and_si128(a, _mm_set1_epi16(0xFF)),
        _mm_srai_epi16(_mm_slli_si128(b, 1), 8)
    );
}

__attribute__ ((target("sse2"))) static inline __m128i sse_hadds_epi16(__m128i a, __m128i b) {
    return _mm_adds_epi16(
        sse_interleave_even_epi16(a, b),
        sse_interleave_odd_epi16(a, b)
    );
}

__attribute__ ((target("ssse3"))) static inline __m128i sse_hadds_epi16(__m128i a, __m128i b) {
    return _mm_hadds_epi16(a, b);
}

__attribute__ ((target("sse2"))) static inline __m128i sse_maddubs_epi16(__m128i a, __m128i b) {
    return _mm_adds_epi16(
        sse_mulu_even_epi8(a, b),
        sse_mulu_odd_epi8(a, b)
    );
}

__attribute__ ((target("ssse3"))) static inline __m128i sse_maddubs_epi16(__m128i a, __m128i b) {
    return _mm_maddubs_epi16(a, b);
}

__attribute__ ((target("default"))) static inline __m128i sse_interleave_odd_epi16(__m128i a, __m128i b) { }
__attribute__ ((target("default"))) static inline __m128i sse_interleave_even_epi16(__m128i a, __m128i b) { }
__attribute__ ((target("default"))) static inline __m128i sse_mulu_odd_epi8(__m128i a, __m128i b) { }
__attribute__ ((target("default"))) static inline __m128i sse_mulu_even_epi8(__m128i a, __m128i b) { }
__attribute__ ((target("default"))) static inline __m128i sse_hadds_epi16(__m128i a, __m128i b) { }
__attribute__ ((target("default"))) static inline __m128i sse_maddubs_epi16(__m128i a, __m128i b) { }

/*
  Original loop per 4 bytes:
    s2 += 4*(s1 + buf[i]) + 3*buf[i+1] + 2*buf[i+2] + buf[i+3] + 10*CHAR_OFFSET;
    s1 += buf[i] + buf[i+1] + buf[i+2] + buf[i+3] + 4*CHAR_OFFSET;

  SSE2/SSSE3 loop per 32 bytes:
    int16 t1[8];
    int16 t2[8];
    for (int j = 0; j < 8; j++) {
      t1[j] = buf[j*4 + i] + buf[j*4 + i+1] + buf[j*4 + i+2] + buf[j*4 + i+3];
      t2[j] = 4*buf[j*4 + i] + 3*buf[j*4 + i+1] + 2*buf[j*4 + i+2] + buf[j*4 + i+3];
    }
    s2 += 32*s1 + (uint32)(
              28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6] +
              t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
          ) + 528*CHAR_OFFSET;
    s1 += (uint32)(t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]) +
          32*CHAR_OFFSET;
 */
/*
  Both sse2 and ssse3 targets must be specified here or we lose (a lot) of
  performance, possibly due to not unrolling+inlining the called targeted
  functions.
 */
__attribute__ ((target("sse2", "ssse3"))) static int32 get_checksum1_sse2_32(schar* buf, int32 len, int32 i, uint32* ps1, uint32* ps2) {
    if (len > 32) {
        int aligned = ((uintptr_t)buf & 15) == 0;

        uint32 x[4] = {0};
        x[0] = *ps1;
        __m128i ss1 = _mm_loadu_si128((__m128i_u*)x);
        x[0] = *ps2;
        __m128i ss2 = _mm_loadu_si128((__m128i_u*)x);

        const int16 mul_t1_buf[8] = {28, 24, 20, 16, 12, 8, 4, 0};
        __m128i mul_t1 = _mm_loadu_si128((__m128i_u*)mul_t1_buf);

        for (; i < (len-32); i+=32) {
            // Load ... 2*[int8*16]
            // SSSE3 has _mm_lqqdu_si128, but this requires another
            // target function for each SSE2 and SSSE3 loads. For reasons
            // unknown (to me) we lose about 10% performance on some CPUs if
            // we do that right here. We just use _mm_loadu_si128 as for all
            // but a handful of specific old CPUs they are synonymous, and
            // take the 1-5% hit on those specific CPUs where it isn't.
            __m128i in8_1, in8_2;
            if (!aligned) {
                in8_1 = _mm_loadu_si128((__m128i_u*)&buf[i]);
                in8_2 = _mm_loadu_si128((__m128i_u*)&buf[i + 16]);
            } else {
                in8_1 = _mm_load_si128((__m128i_u*)&buf[i]);
                in8_2 = _mm_load_si128((__m128i_u*)&buf[i + 16]);
            }

            // (1*buf[i] + 1*buf[i+1]), (1*buf[i+2], 1*buf[i+3]), ... 2*[int16*8]
            // Fastest, even though multiply by 1
            __m128i mul_one = _mm_set1_epi8(1);
            __m128i add16_1 = sse_maddubs_epi16(mul_one, in8_1);
            __m128i add16_2 = sse_maddubs_epi16(mul_one, in8_2);

            // (4*buf[i] + 3*buf[i+1]), (2*buf[i+2], buf[i+3]), ... 2*[int16*8]
            __m128i mul_const = _mm_set1_epi32(4 + (3 << 8) + (2 << 16) + (1 << 24));
            __m128i mul_add16_1 = sse_maddubs_epi16(mul_const, in8_1);
            __m128i mul_add16_2 = sse_maddubs_epi16(mul_const, in8_2);

            // s2 += 32*s1
            ss2 = _mm_add_epi32(ss2, _mm_slli_epi32(ss1, 5));

            // [sum(t1[0]..t1[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
            // Shifting left, then shifting right again and shuffling (rather than just
            // shifting right as with mul32 below) to cheaply end up with the correct sign
            // extension as we go from int16 to int32.
            __m128i sum_add32 = _mm_add_epi16(add16_1, add16_2);
            sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 2));
            sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 4));
            sum_add32 = _mm_add_epi16(sum_add32, _mm_slli_si128(sum_add32, 8));
            sum_add32 = _mm_srai_epi32(sum_add32, 16);
            sum_add32 = _mm_shuffle_epi32(sum_add32, 3);

            // [sum(t2[0]..t2[7]), X, X, X] [int32*4]; faster than multiple _mm_hadds_epi16
            __m128i sum_mul_add32 = _mm_add_epi16(mul_add16_1, mul_add16_2);
            sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 2));
            sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 4));
            sum_mul_add32 = _mm_add_epi16(sum_mul_add32, _mm_slli_si128(sum_mul_add32, 8));
            sum_mul_add32 = _mm_srai_epi32(sum_mul_add32, 16);
            sum_mul_add32 = _mm_shuffle_epi32(sum_mul_add32, 3);

            // s1 += t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7]
            ss1 = _mm_add_epi32(ss1, sum_add32);

            // s2 += t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7]
            ss2 = _mm_add_epi32(ss2, sum_mul_add32);

            // [t1[0] + t1[1], t1[2] + t1[3] ...] [int16*8]
            // We could've combined this with generating sum_add32 above and
            // save an instruction but benchmarking shows that as being slower
            __m128i add16 = sse_hadds_epi16(add16_1, add16_2);

            // [t1[0], t1[1], ...] -> [t1[0]*28 + t1[1]*24, ...] [int32*4]
            __m128i mul32 = _mm_madd_epi16(add16, mul_t1);

            // [sum(mul32), X, X, X] [int32*4]; faster than multiple _mm_hadd_epi32
            mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 4));
            mul32 = _mm_add_epi32(mul32, _mm_srli_si128(mul32, 8));

            // s2 += 28*t1[0] + 24*t1[1] + 20*t1[2] + 16*t1[3] + 12*t1[4] + 8*t1[5] + 4*t1[6]
            ss2 = _mm_add_epi32(ss2, mul32);

#if CHAR_OFFSET != 0
            // s1 += 32*CHAR_OFFSET
            __m128i char_offset_multiplier = _mm_set1_epi32(32 * CHAR_OFFSET);
            ss1 = _mm_add_epi32(ss1, char_offset_multiplier);

            // s2 += 528*CHAR_OFFSET
            char_offset_multiplier = _mm_set1_epi32(528 * CHAR_OFFSET);
            ss2 = _mm_add_epi32(ss2, char_offset_multiplier);
#endif
        }

        _mm_store_si128((__m128i_u*)x, ss1);
        *ps1 = x[0];
        _mm_store_si128((__m128i_u*)x, ss2);
        *ps2 = x[0];
    }
    return i;
}

/*
  AVX2 loop per 64 bytes:
    int16 t1[16];
    int16 t2[16];
    for (int j = 0; j < 16; j++) {
      t1[j] = buf[j*4 + i] + buf[j*4 + i+1] + buf[j*4 + i+2] + buf[j*4 + i+3];
      t2[j] = 4*buf[j*4 + i] + 3*buf[j*4 + i+1] + 2*buf[j*4 + i+2] + buf[j*4 + i+3];
    }
    s2 += 64*s1 + (uint32)(
              60*t1[0] + 56*t1[1] + 52*t1[2] + 48*t1[3] + 44*t1[4] + 40*t1[5] + 36*t1[6] + 32*t1[7] + 28*t1[8] + 24*t1[9] + 20*t1[10] + 16*t1[11] + 12*t1[12] + 8*t1[13] + 4*t1[14] +
              t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7] + t2[8] + t2[9] + t2[10] + t2[11] + t2[12] + t2[13] + t2[14] + t2[15]
          ) + 2080*CHAR_OFFSET;
    s1 += (uint32)(t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7] + t1[8] + t1[9] + t1[10] + t1[11] + t1[12] + t1[13] + t1[14] + t1[15]) +
          64*CHAR_OFFSET;
 */
__attribute__ ((target("avx2"))) static int32 get_checksum1_avx2_64(schar* buf, int32 len, int32 i, uint32* ps1, uint32* ps2) {
    if (len > 64) {
        // Instructions reshuffled compared to SSE2 for slightly better performance
        int aligned = ((uintptr_t)buf & 31) == 0;

        uint32 x[8] = {0};
        x[0] = *ps1;
        __m256i ss1 = _mm256_lddqu_si256((__m256i_u*)x);
        x[0] = *ps2;
        __m256i ss2 = _mm256_lddqu_si256((__m256i_u*)x);

        // The order gets shuffled compared to SSE2
        const int16 mul_t1_buf[16] = {60, 56, 52, 48, 28, 24, 20, 16, 44, 40, 36, 32, 12, 8, 4, 0};
        __m256i mul_t1 = _mm256_lddqu_si256((__m256i_u*)mul_t1_buf);

        for (; i < (len-64); i+=64) {
            // Load ... 2*[int8*32]
            __m256i in8_1, in8_2;
            if (!aligned) {
                in8_1 = _mm256_lddqu_si256((__m256i_u*)&buf[i]);
                in8_2 = _mm256_lddqu_si256((__m256i_u*)&buf[i + 32]);
            } else {
                in8_1 = _mm256_load_si256((__m256i_u*)&buf[i]);
                in8_2 = _mm256_load_si256((__m256i_u*)&buf[i + 32]);
            }

            // Prefetch for next loops. This has no observable effect on the
            // tested AMD but makes as much as 20% difference on the Intel.
            // Curiously that same Intel sees no benefit from this with SSE2
            // or SSSE3.
            _mm_prefetch(&buf[i + 64], _MM_HINT_T0);
            _mm_prefetch(&buf[i + 96], _MM_HINT_T0);
            _mm_prefetch(&buf[i + 128], _MM_HINT_T0);
            _mm_prefetch(&buf[i + 160], _MM_HINT_T0);

            // (1*buf[i] + 1*buf[i+1]), (1*buf[i+2], 1*buf[i+3]), ... 2*[int16*16]
            // Fastest, even though multiply by 1
            __m256i mul_one = _mm256_set1_epi8(1);
            __m256i add16_1 = _mm256_maddubs_epi16(mul_one, in8_1);
            __m256i add16_2 = _mm256_maddubs_epi16(mul_one, in8_2);

            // (4*buf[i] + 3*buf[i+1]), (2*buf[i+2], buf[i+3]), ... 2*[int16*16]
            __m256i mul_const = _mm256_set1_epi32(4 + (3 << 8) + (2 << 16) + (1 << 24));
            __m256i mul_add16_1 = _mm256_maddubs_epi16(mul_const, in8_1);
            __m256i mul_add16_2 = _mm256_maddubs_epi16(mul_const, in8_2);

            // s2 += 64*s1
            ss2 = _mm256_add_epi32(ss2, _mm256_slli_epi32(ss1, 6));

            // [t1[0] + t1[1], t1[2] + t1[3] ...] [int16*16]
            __m256i add16 = _mm256_hadds_epi16(add16_1, add16_2);

            // [t1[0], t1[1], ...] -> [t1[0]*60 + t1[1]*56, ...] [int32*8]
            __m256i mul32 = _mm256_madd_epi16(add16, mul_t1);

            // [sum(t1[0]..t1[15]), X, X, X, X, X, X, X] [int32*8]
            __m256i sum_add32 = _mm256_add_epi16(add16_1, add16_2);
            sum_add32 = _mm256_add_epi16(sum_add32, _mm256_permute4x64_epi64(sum_add32, 2 + (3 << 2) + (0 << 4) + (1 << 6)));
            sum_add32 = _mm256_add_epi16(sum_add32, _mm256_slli_si256(sum_add32, 2));
            sum_add32 = _mm256_add_epi16(sum_add32, _mm256_slli_si256(sum_add32, 4));
            sum_add32 = _mm256_add_epi16(sum_add32, _mm256_slli_si256(sum_add32, 8));
            sum_add32 = _mm256_srai_epi32(sum_add32, 16);
            sum_add32 = _mm256_shuffle_epi32(sum_add32, 3);

            // s1 += t1[0] + t1[1] + t1[2] + t1[3] + t1[4] + t1[5] + t1[6] + t1[7] + t1[8] + t1[9] + t1[10] + t1[11] + t1[12] + t1[13] + t1[14] + t1[15]
            ss1 = _mm256_add_epi32(ss1, sum_add32);

            // [sum(t2[0]..t2[15]), X, X, X, X, X, X, X] [int32*8]
            __m256i sum_mul_add32 = _mm256_add_epi16(mul_add16_1, mul_add16_2);
            sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_permute4x64_epi64(sum_mul_add32, 2 + (3 << 2) + (0 << 4) + (1 << 6)));
            sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_slli_si256(sum_mul_add32, 2));
            sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_slli_si256(sum_mul_add32, 4));
            sum_mul_add32 = _mm256_add_epi16(sum_mul_add32, _mm256_slli_si256(sum_mul_add32, 8));
            sum_mul_add32 = _mm256_srai_epi32(sum_mul_add32, 16);
            sum_mul_add32 = _mm256_shuffle_epi32(sum_mul_add32, 3);

            // s2 += t2[0] + t2[1] + t2[2] + t2[3] + t2[4] + t2[5] + t2[6] + t2[7] + t2[8] + t2[9] + t2[10] + t2[11] + t2[12] + t2[13] + t2[14] + t2[15]
            ss2 = _mm256_add_epi32(ss2, sum_mul_add32);

            // [sum(mul32), X, X, X, X, X, X, X] [int32*8]
            mul32 = _mm256_add_epi32(mul32, _mm256_permute2x128_si256(mul32, mul32, 1));
            mul32 = _mm256_add_epi32(mul32, _mm256_srli_si256(mul32, 4));
            mul32 = _mm256_add_epi32(mul32, _mm256_srli_si256(mul32, 8));

            // s2 += 60*t1[0] + 56*t1[1] + 52*t1[2] + 48*t1[3] + 44*t1[4] + 40*t1[5] + 36*t1[6] + 32*t1[7] + 28*t1[8] + 24*t1[9] + 20*t1[10] + 16*t1[11] + 12*t1[12] + 8*t1[13] + 4*t1[14]
            ss2 = _mm256_add_epi32(ss2, mul32);

#if CHAR_OFFSET != 0
            // s1 += 64*CHAR_OFFSET
            __m256i char_offset_multiplier = _mm256_set1_epi32(64 * CHAR_OFFSET);
            ss1 = _mm256_add_epi32(ss1, char_offset_multiplier);

            // s2 += 2080*CHAR_OFFSET
            char_offset_multiplier = _mm256_set1_epi32(2080 * CHAR_OFFSET);
            ss2 = _mm256_add_epi32(ss2, char_offset_multiplier);
#endif
        }

        _mm256_store_si256((__m256i_u*)x, ss1);
        *ps1 = x[0];
        _mm256_store_si256((__m256i_u*)x, ss2);
        *ps2 = x[0];
    }
    return i;
}

__attribute__ ((target("default"))) static int32 get_checksum1_avx2_64(schar* buf, int32 len, int32 i, uint32* ps1, uint32* ps2) {
    return i;
}

__attribute__ ((target("default"))) static int32 get_checksum1_sse2_32(schar* buf, int32 len, int32 i, uint32* ps1, uint32* ps2) {
    return i;
}

static inline int32 get_checksum1_default_1(schar* buf, int32 len, int32 i, uint32* ps1, uint32* ps2) {
	uint32 s1 = *ps1;
	uint32 s2 = *ps2;
	for (; i < (len-4); i+=4) {
		s2 += 4*(s1 + buf[i]) + 3*buf[i+1] + 2*buf[i+2] + buf[i+3] + 10*CHAR_OFFSET;
		s1 += (buf[i+0] + buf[i+1] + buf[i+2] + buf[i+3] + 4*CHAR_OFFSET);
	}
	for (; i < len; i++) {
		s1 += (buf[i]+CHAR_OFFSET); s2 += s1;
	}
	*ps1 = s1;
	*ps2 = s2;
    return i;
}

extern "C" {

uint32 get_checksum1(char *buf1, int32 len) {
    int32 i = 0;
    uint32 s1 = 0;
    uint32 s2 = 0;

    // multiples of 64 bytes using AVX2 (if available)
    i = get_checksum1_avx2_64((schar*)buf1, len, i, &s1, &s2);

    // multiples of 32 bytes using SSE2/SSSE3 (if available)
    i = get_checksum1_sse2_32((schar*)buf1, len, i, &s1, &s2);

    // whatever is left
    i = get_checksum1_default_1((schar*)buf1, len, i, &s1, &s2);

    return (s1 & 0xffff) + (s2 << 16);
}

}
#endif /* HAVE_SIMD */
#endif /* __cplusplus */
#endif /* __x86_64__ */
