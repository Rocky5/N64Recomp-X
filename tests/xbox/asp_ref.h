/* asp_ref.h - straightforward reference arithmetic for test_asp_hle.c (see asp_ref.c). */
#ifndef ASP_REF_H
#define ASP_REF_H

#include <stdint.h>

/* VADPCM stream decode: `hist` = the last 16 samples before the stream (only
 * [14], [15] are used), updated to the stream's last 16. book = 8 predictors x
 * 2 rows x 8 (s16, order 2), as A_LOADADPCM loads it. */
void ref_adpcm(const int16_t book[128], const uint8_t* frames, int nframes, int16_t hist[16], int16_t* out);

/* Resampler over a whole stream: z[0] is tap 0 of output 0, output j reads
 * z[idx..idx+3] with idx = (frac0 + j * step) >> 16, step = pitch << 1. */
void ref_resample(const int16_t* z, int nout, uint32_t pitch, uint32_t frac0, const int16_t lut[256], int per_tap,
                  int16_t* out);

/* Envelope: per channel value/target/rate/seq (Q16.16), as A_SETVOL sets them. */
typedef struct {
    int32_t value[2], target[2], rate[2], seq[2];
} RefEnv;
void ref_env_init(RefEnv* e, int16_t vl, int16_t vr, int16_t tl, int16_t tr, int32_t rl, int32_t rr);
/* Volumes (value >> 16) for the next n samples (n a multiple of 8): vol[c][i]. */
void ref_env_volumes(RefEnv* e, int n, int16_t* vol_l, int16_t* vol_r);

int16_t ref_gain(int16_t vol, int16_t amount);                 /* vol x dry|wet, Q15, rounded, saturated */
void ref_mix(int16_t* dst, const int16_t* src, int n, int16_t gain);   /* in order, element by element */
void ref_mix_var(int16_t* dst, const int16_t* src, int n, const int16_t* gains);
void ref_interleave(int16_t* out, const int16_t* l, const int16_t* r, int n);

/* Order-2 lowpass (A_POLEF): table = h1[8], h2[8]; y[0], y[1] = y[-2], y[-1], updated. */
void ref_polef(const int16_t table[16], int16_t gain, const int16_t* x, int n, int16_t y[2], int16_t* out);

/* nead: 2-bit VADPCM (5-byte frames, scale capped at 14), otherwise as ref_adpcm. */
void ref_adpcm2(const int16_t book[128], const uint8_t* frames, int nframes, int16_t hist[16], int16_t* out);

/* nead ENVMIXER over n samples (a multiple of 8): levels env[3] (dry left,
 * dry right, wet; unsigned Q16) step by step[3] after every 8 samples; neg[4]
 * inverts dry left, dry right, wet left, wet right; out[4] = the four sends
 * (dry L, dry R, wet L, wet R), accumulated. */
void ref_envmix_nead(const int16_t* in, int n, uint16_t env[3], const uint16_t step[3], const int neg[4],
                     int16_t* const out[4]);

/* n_aspMain's linear ENVMIXER over n samples: value/step/target per channel (Q16.16). */
typedef struct {
    int64_t value[2], step[2], target[2];
    int16_t dry, wet;
} RefLin;
void ref_envmix_lin(const int16_t* in, int n, RefLin* e, int16_t* const out[4]);

#endif
