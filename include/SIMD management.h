/************************************************************
 * File: SIMD management.h              Created: 2025/09/18 *
 *                                Last modified: 2026/08/16 *
 *                                                          *
 * Desc:                                                    *
 *                                                          *
 * MIT license             Copyright (c) David William Bull *
 ************************************************************/
#pragma once

#include <immintrin.h>
#include "typedefs.h"

///== Fused multiply-add, single-rounded where the ISA carries it
namespace simd {
   inline fl32x4 __vectorcall fmadd_ps(fl32x4 a, fl32x4 b, fl32x4 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm_fmadd_ps(a, b, c);
#else
      return _mm_add_ps(_mm_mul_ps(a, b), c);
#endif
   }

   inline fl32x4 __vectorcall fmsub_ps(fl32x4 a, fl32x4 b, fl32x4 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm_fmsub_ps(a, b, c);
#else
      return _mm_sub_ps(_mm_mul_ps(a, b), c);
#endif
   }

   inline fl32x4 __vectorcall fnmadd_ps(fl32x4 a, fl32x4 b, fl32x4 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm_fnmadd_ps(a, b, c);
#else
      return _mm_sub_ps(c, _mm_mul_ps(a, b));
#endif
   }

#if defined(__AVX__)
   inline fl32x8 __vectorcall fmadd_ps(fl32x8 a, fl32x8 b, fl32x8 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm256_fmadd_ps(a, b, c);
#else
      return _mm256_add_ps(_mm256_mul_ps(a, b), c);
#endif
   }

   inline fl32x8 __vectorcall fmsub_ps(fl32x8 a, fl32x8 b, fl32x8 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm256_fmsub_ps(a, b, c);
#else
      return _mm256_sub_ps(_mm256_mul_ps(a, b), c);
#endif
   }

   inline fl32x8 __vectorcall fnmadd_ps(fl32x8 a, fl32x8 b, fl32x8 c) noexcept {
#if defined(__FMA__) || defined(__AVX2__)
      return _mm256_fnmadd_ps(a, b, c);
#else
      return _mm256_sub_ps(c, _mm256_mul_ps(a, b));
#endif
   }
#endif
}
///== Fused multiply-add, single-rounded where the ISA carries it

///== Intrinsics macros
#if !defined(__FMA__) && !defined(__AVX2__)

#define _mm_abs_pd(input)  _mm_and_pd(_mm_castsi128_pd(_mm_set1_epi64x(0x07FFFFFFFFFFFFFFF)), (input))

#if defined(__AVX__)
#define _mm256_abs_pd(input)  _mm256_and_pd(_mm256_castsi256_pd(_mm256_set1_epi64x(0x07FFFFFFFFFFFFFFF)), (input))
#endif

#endif
///== Intrinsics macros
