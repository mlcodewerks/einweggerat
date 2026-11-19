#ifndef MUDLORD_LIBRETRO_DSP_COMMON_H
#define MUDLORD_LIBRETRO_DSP_COMMON_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define DSP_HAVE_SSE2 1
#else
#define DSP_HAVE_SSE2 0
#endif

#if defined(_M_ARM64)
#include <arm64_neon.h>
#define DSP_HAVE_NEON 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define DSP_HAVE_NEON 1
#else
#define DSP_HAVE_NEON 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct dsp_i16_buffer
{
    float *samples;
    size_t capacity;
};

static inline float dsp_clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int dsp_clampi(int x, int lo, int hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float dsp_db_to_gain(float db)
{
    return powf(10.0f, db * 0.05f);
}

static inline unsigned dsp_next_pow2(unsigned v)
{
    unsigned p = 1u;
    while (p < v && p < (1u << 31))
        p <<= 1;
    return p;
}

static inline float dsp_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static inline int dsp_i16_buffer_reserve(struct dsp_i16_buffer *buffer,
    size_t samples)
{
    float *next;
    size_t capacity;

    if (samples <= buffer->capacity)
        return 1;
    if (samples > SIZE_MAX / sizeof(float))
        return 0;

    capacity = buffer->capacity ? buffer->capacity : 1024u;
    while (capacity < samples)
    {
        if (capacity > SIZE_MAX / 2u)
        {
            capacity = samples;
            break;
        }
        capacity *= 2u;
    }

    next = (float *)realloc(buffer->samples, capacity * sizeof(float));
    if (!next)
        return 0;
    buffer->samples = next;
    buffer->capacity = capacity;
    return 1;
}

static inline void dsp_i16_buffer_free(struct dsp_i16_buffer *buffer)
{
    free(buffer->samples);
    buffer->samples = NULL;
    buffer->capacity = 0u;
}

static inline void dsp_i16_to_float(float *dst, const int16_t *src,
    size_t count)
{
    size_t i = 0u;
    const float scale = 1.0f / 32768.0f;

#if DSP_HAVE_SSE2
    {
        const __m128 scale4 = _mm_set1_ps(scale);
        const __m128i zero = _mm_setzero_si128();
        for (; i + 8u <= count; i += 8u)
        {
            const __m128i s16 = _mm_loadu_si128((const __m128i *)(src + i));
            const __m128i sign = _mm_cmpgt_epi16(zero, s16);
            const __m128i lo32 = _mm_unpacklo_epi16(s16, sign);
            const __m128i hi32 = _mm_unpackhi_epi16(s16, sign);
            _mm_storeu_ps(dst + i,
                _mm_mul_ps(_mm_cvtepi32_ps(lo32), scale4));
            _mm_storeu_ps(dst + i + 4u,
                _mm_mul_ps(_mm_cvtepi32_ps(hi32), scale4));
        }
    }
#elif DSP_HAVE_NEON
    for (; i + 8u <= count; i += 8u)
    {
        const int16x8_t s16 = vld1q_s16(src + i);
        const int32x4_t lo32 = vmovl_s16(vget_low_s16(s16));
        const int32x4_t hi32 = vmovl_s16(vget_high_s16(s16));
        vst1q_f32(dst + i,
            vmulq_n_f32(vcvtq_f32_s32(lo32), scale));
        vst1q_f32(dst + i + 4u,
            vmulq_n_f32(vcvtq_f32_s32(hi32), scale));
    }
#endif

    for (; i < count; ++i)
        dst[i] = (float)src[i] * scale;
}

static inline void dsp_float_to_i16(int16_t *dst, const float *src,
    size_t count)
{
    size_t i = 0u;
    const float hi_scalar = 32767.0f / 32768.0f;

#if DSP_HAVE_SSE2
    {
        const __m128 zero = _mm_setzero_ps();
        const __m128 lo = _mm_set1_ps(-1.0f);
        const __m128 hi = _mm_set1_ps(hi_scalar);
        const __m128 scale = _mm_set1_ps(32768.0f);
        const __m128 half = _mm_set1_ps(0.5f);
        const __m128 neg_half = _mm_set1_ps(-0.5f);

        for (; i + 8u <= count; i += 8u)
        {
            __m128 a = _mm_loadu_ps(src + i);
            __m128 b = _mm_loadu_ps(src + i + 4u);
            __m128 ma;
            __m128 mb;
            __m128 oa;
            __m128 ob;
            __m128i ia;
            __m128i ib;

            a = _mm_max_ps(lo, _mm_min_ps(hi, a));
            b = _mm_max_ps(lo, _mm_min_ps(hi, b));
            a = _mm_mul_ps(a, scale);
            b = _mm_mul_ps(b, scale);
            ma = _mm_cmplt_ps(a, zero);
            mb = _mm_cmplt_ps(b, zero);
            oa = _mm_or_ps(_mm_and_ps(ma, neg_half), _mm_andnot_ps(ma, half));
            ob = _mm_or_ps(_mm_and_ps(mb, neg_half), _mm_andnot_ps(mb, half));
            ia = _mm_cvttps_epi32(_mm_add_ps(a, oa));
            ib = _mm_cvttps_epi32(_mm_add_ps(b, ob));
            _mm_storeu_si128((__m128i *)(dst + i), _mm_packs_epi32(ia, ib));
        }
    }
#elif DSP_HAVE_NEON
    {
        const float32x4_t zero = vdupq_n_f32(0.0f);
        const float32x4_t lo = vdupq_n_f32(-1.0f);
        const float32x4_t hi = vdupq_n_f32(hi_scalar);
        const float32x4_t half = vdupq_n_f32(0.5f);
        const float32x4_t neg_half = vdupq_n_f32(-0.5f);

        for (; i + 8u <= count; i += 8u)
        {
            float32x4_t a = vld1q_f32(src + i);
            float32x4_t b = vld1q_f32(src + i + 4u);
            uint32x4_t ma;
            uint32x4_t mb;
            int32x4_t ia;
            int32x4_t ib;

            a = vmaxq_f32(lo, vminq_f32(hi, a));
            b = vmaxq_f32(lo, vminq_f32(hi, b));
            a = vmulq_n_f32(a, 32768.0f);
            b = vmulq_n_f32(b, 32768.0f);
            ma = vcltq_f32(a, zero);
            mb = vcltq_f32(b, zero);
            a = vaddq_f32(a, vbslq_f32(ma, neg_half, half));
            b = vaddq_f32(b, vbslq_f32(mb, neg_half, half));
            ia = vcvtq_s32_f32(a);
            ib = vcvtq_s32_f32(b);
            vst1q_s16(dst + i,
                vcombine_s16(vqmovn_s32(ia), vqmovn_s32(ib)));
        }
    }
#endif

    for (; i < count; ++i)
    {
        float v = dsp_clampf(src[i], -1.0f, hi_scalar) * 32768.0f;
        int q = v >= 0.0f ? (int)floorf(v + 0.5f) : (int)ceilf(v - 0.5f);
        if (q < -32768)
            q = -32768;
        else if (q > 32767)
            q = 32767;
        dst[i] = (int16_t)q;
    }
}

static inline void dsp_mul_copy(float *dst, const float *src,
    const float *mul, size_t count)
{
    size_t i = 0u;
#if DSP_HAVE_SSE2
    for (; i + 4u <= count; i += 4u)
        _mm_storeu_ps(dst + i,
            _mm_mul_ps(_mm_loadu_ps(src + i), _mm_loadu_ps(mul + i)));
#elif DSP_HAVE_NEON
    for (; i + 4u <= count; i += 4u)
        vst1q_f32(dst + i,
            vmulq_f32(vld1q_f32(src + i), vld1q_f32(mul + i)));
#endif
    for (; i < count; ++i)
        dst[i] = src[i] * mul[i];
}

static inline void dsp_mul_add(float *dst, const float *src,
    const float *mul, size_t count)
{
    size_t i = 0u;
#if DSP_HAVE_SSE2
    for (; i + 4u <= count; i += 4u)
    {
        const __m128 d = _mm_loadu_ps(dst + i);
        const __m128 s = _mm_loadu_ps(src + i);
        const __m128 m = _mm_loadu_ps(mul + i);
        _mm_storeu_ps(dst + i, _mm_add_ps(d, _mm_mul_ps(s, m)));
    }
#elif DSP_HAVE_NEON
    for (; i + 4u <= count; i += 4u)
    {
        const float32x4_t d = vld1q_f32(dst + i);
        const float32x4_t s = vld1q_f32(src + i);
        const float32x4_t m = vld1q_f32(mul + i);
        vst1q_f32(dst + i, vmlaq_f32(d, s, m));
    }
#endif
    for (; i < count; ++i)
        dst[i] += src[i] * mul[i];
}

static inline void dsp_square_add(float *dst, const float *src, size_t count)
{
    size_t i = 0u;
#if DSP_HAVE_SSE2
    for (; i + 4u <= count; i += 4u)
    {
        const __m128 d = _mm_loadu_ps(dst + i);
        const __m128 s = _mm_loadu_ps(src + i);
        _mm_storeu_ps(dst + i, _mm_add_ps(d, _mm_mul_ps(s, s)));
    }
#elif DSP_HAVE_NEON
    for (; i + 4u <= count; i += 4u)
    {
        const float32x4_t d = vld1q_f32(dst + i);
        const float32x4_t s = vld1q_f32(src + i);
        vst1q_f32(dst + i, vmlaq_f32(d, s, s));
    }
#endif
    for (; i < count; ++i)
        dst[i] += src[i] * src[i];
}

static inline void dsp_scale_array(float *data, size_t count, float scale)
{
    size_t i = 0u;
#if DSP_HAVE_SSE2
    {
        const __m128 s = _mm_set1_ps(scale);
        for (; i + 4u <= count; i += 4u)
            _mm_storeu_ps(data + i, _mm_mul_ps(_mm_loadu_ps(data + i), s));
    }
#elif DSP_HAVE_NEON
    for (; i + 4u <= count; i += 4u)
        vst1q_f32(data + i, vmulq_n_f32(vld1q_f32(data + i), scale));
#endif
    for (; i < count; ++i)
        data[i] *= scale;
}

static inline void dsp_ring_mul_copy(float *dst, const float *ring,
    unsigned ring_size, unsigned ring_mask, uint64_t start,
    const float *mul, size_t count)
{
    const unsigned p = (unsigned)start & ring_mask;
    size_t first = (size_t)ring_size - (size_t)p;
    if (first > count)
        first = count;
    dsp_mul_copy(dst, ring + p, mul, first);
    if (first < count)
        dsp_mul_copy(dst + first, ring, mul + first, count - first);
}

static inline void dsp_ring_mul_add(float *ring, unsigned ring_size,
    unsigned ring_mask, uint64_t start, const float *src,
    const float *mul, size_t count)
{
    const unsigned p = (unsigned)start & ring_mask;
    size_t first = (size_t)ring_size - (size_t)p;
    if (first > count)
        first = count;
    dsp_mul_add(ring + p, src, mul, first);
    if (first < count)
        dsp_mul_add(ring, src + first, mul + first, count - first);
}

static inline void dsp_ring_square_add(float *ring, unsigned ring_size,
    unsigned ring_mask, uint64_t start, const float *src, size_t count)
{
    const unsigned p = (unsigned)start & ring_mask;
    size_t first = (size_t)ring_size - (size_t)p;
    if (first > count)
        first = count;
    dsp_square_add(ring + p, src, first);
    if (first < count)
        dsp_square_add(ring, src + first, count - first);
}

#endif
