#include "libretro_dspfilter.h"
#include "dsp_common.h"

struct bitcrusher_data
{
    int bits;
    int downsample;
    float mix;
    unsigned hold_count;
    float held_l;
    float held_r;
    float quant_scale;
    struct dsp_i16_buffer i16;
};

static float bitcrusher_quantize(float x, float scale)
{
    float q;
    x = dsp_clampf(x, -1.0f, 1.0f);
    if (x >= 0.0f)
        q = floorf(x * scale + 0.5f);
    else
        q = ceilf(x * scale - 0.5f);
    return q / scale;
}

static void bitcrusher_mix_held(float *samples, unsigned frames,
    float held_l, float held_r, float mix)
{
    size_t count = (size_t)frames * 2u;
    size_t i = 0u;

#if DSP_HAVE_SSE2
    {
        const __m128 held = _mm_set_ps(held_r, held_l, held_r, held_l);
        const __m128 m = _mm_set1_ps(mix);
        for (; i + 4u <= count; i += 4u)
        {
            const __m128 dry = _mm_loadu_ps(samples + i);
            const __m128 wet = _mm_add_ps(dry,
                _mm_mul_ps(_mm_sub_ps(held, dry), m));
            _mm_storeu_ps(samples + i, wet);
        }
    }
#elif DSP_HAVE_NEON
    {
        float pair[4] = { held_l, held_r, held_l, held_r };
        const float32x4_t held = vld1q_f32(pair);
        const float32x4_t m = vdupq_n_f32(mix);
        for (; i + 4u <= count; i += 4u)
        {
            const float32x4_t dry = vld1q_f32(samples + i);
            const float32x4_t wet = vmlaq_f32(dry, vsubq_f32(held, dry), m);
            vst1q_f32(samples + i, wet);
        }
    }
#endif

    for (; i < count; i += 2u)
    {
        samples[i + 0u] = dsp_lerp(samples[i + 0u], held_l, mix);
        samples[i + 1u] = dsp_lerp(samples[i + 1u], held_r, mix);
    }
}

static void bitcrusher_process(void *data, struct dspfilter_output *output,
    const struct dspfilter_input *input)
{
    struct bitcrusher_data *bc = (struct bitcrusher_data *)data;
    float *samples = input->samples;
    unsigned frame = 0u;

    output->samples = samples;
    output->frames = input->frames;

    while (frame < input->frames)
    {
        unsigned run;
        unsigned remain;

        if (bc->hold_count == 0u)
        {
            bc->held_l = bitcrusher_quantize(samples[frame * 2u + 0u],
                bc->quant_scale);
            bc->held_r = bitcrusher_quantize(samples[frame * 2u + 1u],
                bc->quant_scale);
        }

        remain = (unsigned)bc->downsample - bc->hold_count;
        run = input->frames - frame;
        if (run > remain)
            run = remain;

        bitcrusher_mix_held(samples + frame * 2u, run,
            bc->held_l, bc->held_r, bc->mix);

        frame += run;
        bc->hold_count += run;
        if (bc->hold_count >= (unsigned)bc->downsample)
            bc->hold_count = 0u;
    }
}

static void bitcrusher_process_i16(void *data,
    struct dspfilter_output_i16 *output,
    const struct dspfilter_input_i16 *input)
{
    struct bitcrusher_data *bc = (struct bitcrusher_data *)data;
    struct dspfilter_input float_input;
    struct dspfilter_output float_output;
    const size_t count = (size_t)input->frames * 2u;

    output->samples = input->samples;
    output->frames = input->frames;
    if (!input->samples || input->frames == 0u)
        return;
    if (!dsp_i16_buffer_reserve(&bc->i16, count))
        return;

    dsp_i16_to_float(bc->i16.samples, input->samples, count);
    float_input.samples = bc->i16.samples;
    float_input.frames = input->frames;
    bitcrusher_process(data, &float_output, &float_input);
    dsp_float_to_i16(input->samples, float_output.samples,
        (size_t)float_output.frames * 2u);
    output->frames = float_output.frames;
}

static void bitcrusher_free(void *data)
{
    struct bitcrusher_data *bc = (struct bitcrusher_data *)data;
    if (!bc)
        return;
    dsp_i16_buffer_free(&bc->i16);
    free(bc);
}

static void *bitcrusher_init(const struct dspfilter_info *info,
    const struct dspfilter_config *config, void *userdata)
{
    struct bitcrusher_data *bc;
    int bits = 8;
    int downsample = 4;
    float mix = 1.0f;

    (void)info;

    bc = (struct bitcrusher_data *)calloc(1, sizeof(*bc));
    if (!bc)
        return NULL;

    config->get_int(userdata, "bits", &bits, 8);
    config->get_int(userdata, "downsample", &downsample, 4);
    config->get_float(userdata, "drywet", &mix, 1.0f);

    bc->bits = dsp_clampi(bits, 2, 24);
    bc->downsample = dsp_clampi(downsample, 1, 64);
    bc->mix = dsp_clampf(mix, 0.0f, 1.0f);
    bc->hold_count = 0u;
    bc->quant_scale = (float)((1u << (unsigned)(bc->bits - 1)) - 1u);
    if (bc->quant_scale < 1.0f)
        bc->quant_scale = 1.0f;

    if (info && info->input_rate > 1.0f)
    {
        const size_t initial = (size_t)(info->input_rate * 0.128f + 64.0f) * 2u;
        if (!dsp_i16_buffer_reserve(&bc->i16, initial))
        {
            bitcrusher_free(bc);
            return NULL;
        }
    }

    return bc;
}

static const struct dspfilter_implementation bitcrusher_plug =
{
    bitcrusher_init,
    bitcrusher_process,
    bitcrusher_free,
    DSPFILTER_API_VERSION,
    "Bitcrusher",
    "bitcrusher",
    bitcrusher_process_i16
};



#ifdef HAVE_FILTERS_BUILTIN
#define dspfilter_get_implementation bitcrusher_dspfilter_get_implementation
#endif

const struct dspfilter_implementation *
dspfilter_get_implementation(dspfilter_simd_mask_t mask)
{
    (void)mask;
    return &bitcrusher_plug;
}

#undef dspfilter_get_implementation
