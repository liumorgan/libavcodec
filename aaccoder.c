/*
 * AAC coefficients encoder
 * Copyright (C) 2008-2009 Konstantin Shishkov
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file libavcodec/aaccoder.c
 * AAC coefficients encoder
 */

/***********************************
 *              TODOs:
 * speedup quantizer selection
 * add sane pulse detection
 ***********************************/

#include "avcodec.h"
#include "put_bits.h"
#include "aac.h"
#include "aacenc.h"
#include "aactab.h"

/** bits needed to code codebook run value for long windows */
static const uint8_t run_value_bits_long[64] = {
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 15
};

/** bits needed to code codebook run value for short windows */
static const uint8_t run_value_bits_short[16] = {
    3, 3, 3, 3, 3, 3, 3, 6, 6, 6, 6, 6, 6, 6, 6, 9
};

static const uint8_t *run_value_bits[2] = {
    run_value_bits_long, run_value_bits_short
};


/**
 * Quantize one coefficient.
 * @return absolute value of the quantized coefficient
 * @see 3GPP TS26.403 5.6.2 "Scalefactor determination"
 */
static av_always_inline int quant(float coef, const float Q)
{
    float a = coef * Q;
    return sqrtf(a * sqrtf(a)) + 0.4054;
}

static void quantize_bands(int (*out)[2], const float *in, const float *scaled,
                           int size, float Q34, int is_signed, int maxval)
{
    int i;
    double qc;
    for (i = 0; i < size; i++) {
        qc = scaled[i] * Q34;
        out[i][0] = (int)FFMIN(qc,          (double)maxval);
        out[i][1] = (int)FFMIN(qc + 0.4054, (double)maxval);
        if (is_signed && in[i] < 0.0f) {
            out[i][0] = -out[i][0];
            out[i][1] = -out[i][1];
        }
    }
}

static void abs_pow34_v(float *out, const float *in, const int size)
{
#ifndef USE_REALLY_FULL_SEARCH
    int i;
    for (i = 0; i < size; i++) {
        float a = fabsf(in[i]);
        out[i] = sqrtf(a * sqrtf(a));
    }
#endif /* USE_REALLY_FULL_SEARCH */
}

static const uint8_t aac_cb_range [12] = {0, 3, 3, 3, 3, 9, 9, 8, 8, 13, 13, 17};
static const uint8_t aac_cb_maxval[12] = {0, 1, 1, 2, 2, 4, 4, 7, 7, 12, 12, 16};

/**
 * Calculate rate distortion cost for quantizing with given codebook
 *
 * @return quantization distortion
 */
static float quantize_band_cost(struct AACEncContext *s, const float *in,
                                const float *scaled, int size, int scale_idx,
                                int cb, const float lambda, const float uplim,
                                int *bits)
{
    const float IQ = ff_aac_pow2sf_tab[200 + scale_idx - SCALE_ONE_POS + SCALE_DIV_512];
    const float  Q = ff_aac_pow2sf_tab[200 - scale_idx + SCALE_ONE_POS - SCALE_DIV_512];
    const float CLIPPED_ESCAPE = 165140.0f*IQ;
    int i, j, k;
    float cost = 0;
    const int dim = cb < FIRST_PAIR_BT ? 4 : 2;
    int resbits = 0;
#ifndef USE_REALLY_FULL_SEARCH
    const float  Q34 = sqrtf(Q * sqrtf(Q));
    const int range  = aac_cb_range[cb];
    const int maxval = aac_cb_maxval[cb];
    int offs[4];
#endif /* USE_REALLY_FULL_SEARCH */

    if (!cb) {
        for (i = 0; i < size; i++)
            cost += in[i]*in[i];
        if (bits)
            *bits = 0;
        return cost * lambda;
    }
#ifndef USE_REALLY_FULL_SEARCH
    offs[0] = 1;
    for (i = 1; i < dim; i++)
        offs[i] = offs[i-1]*range;
    quantize_bands(s->qcoefs, in, scaled, size, Q34, !IS_CODEBOOK_UNSIGNED(cb), maxval);
#endif /* USE_REALLY_FULL_SEARCH */
    for (i = 0; i < size; i += dim) {
        float mincost;
        int minidx  = 0;
        int minbits = 0;
        const float *vec;
#ifndef USE_REALLY_FULL_SEARCH
        int (*quants)[2] = &s->qcoefs[i];
        mincost = 0.0f;
        for (j = 0; j < dim; j++)
            mincost += in[i+j]*in[i+j];
        minidx = IS_CODEBOOK_UNSIGNED(cb) ? 0 : 40;
        minbits = ff_aac_spectral_bits[cb-1][minidx];
        mincost = mincost * lambda + minbits;
        for (j = 0; j < (1<<dim); j++) {
            float rd = 0.0f;
            int curbits;
            int curidx = IS_CODEBOOK_UNSIGNED(cb) ? 0 : 40;
            int same   = 0;
            for (k = 0; k < dim; k++) {
                if ((j & (1 << k)) && quants[k][0] == quants[k][1]) {
                    same = 1;
                    break;
                }
            }
            if (same)
                continue;
            for (k = 0; k < dim; k++)
                curidx += quants[k][!!(j & (1 << k))] * offs[dim - 1 - k];
            curbits =  ff_aac_spectral_bits[cb-1][curidx];
            vec     = &ff_aac_codebook_vectors[cb-1][curidx*dim];
#else
        mincost = INFINITY;
        vec = ff_aac_codebook_vectors[cb-1];
        for (j = 0; j < ff_aac_spectral_sizes[cb-1]; j++, vec += dim) {
            float rd = 0.0f;
            int curbits = ff_aac_spectral_bits[cb-1][j];
#endif /* USE_REALLY_FULL_SEARCH */
            if (IS_CODEBOOK_UNSIGNED(cb)) {
                for (k = 0; k < dim; k++) {
                    float t = fabsf(in[i+k]);
                    float di;
                    if (vec[k] == 64.0f) { //FIXME: slow
                        //do not code with escape sequence small values
                        if (t < 39.0f*IQ) {
                            rd = INFINITY;
                            break;
                        }
                        if (t >= CLIPPED_ESCAPE) {
                            di = t - CLIPPED_ESCAPE;
                            curbits += 21;
                        } else {
                            int c = av_clip(quant(t, Q), 0, 8191);
                            di = t - c*cbrtf(c)*IQ;
                            curbits += av_log2(c)*2 - 4 + 1;
                        }
                    } else {
                        di = t - vec[k]*IQ;
                    }
                    if (vec[k] != 0.0f)
                        curbits++;
                    rd += di*di;
                }
            } else {
                for (k = 0; k < dim; k++) {
                    float di = in[i+k] - vec[k]*IQ;
                    rd += di*di;
                }
            }
            rd = rd * lambda + curbits;
            if (rd < mincost) {
                mincost = rd;
                minidx  = j;
                minbits = curbits;
            }
        }
        cost    += mincost;
        resbits += minbits;
        if (cost >= uplim)
            return uplim;
    }

    if (bits)
        *bits = resbits;
    return cost;
}

static void quantize_and_encode_band(struct AACEncContext *s, PutBitContext *pb,
                                     const float *in, int size, int scale_idx,
                                     int cb, const float lambda)
{
    const float IQ = ff_aac_pow2sf_tab[200 + scale_idx - SCALE_ONE_POS + SCALE_DIV_512];
    const float  Q = ff_aac_pow2sf_tab[200 - scale_idx + SCALE_ONE_POS - SCALE_DIV_512];
    const float CLIPPED_ESCAPE = 165140.0f*IQ;
    const int dim = (cb < FIRST_PAIR_BT) ? 4 : 2;
    int i, j, k;
#ifndef USE_REALLY_FULL_SEARCH
    const float  Q34 = sqrtf(Q * sqrtf(Q));
    const int range  = aac_cb_range[cb];
    const int maxval = aac_cb_maxval[cb];
    int offs[4];
    float *scaled = s->scoefs;
#endif /* USE_REALLY_FULL_SEARCH */

//START_TIMER
    if (!cb)
        return;

#ifndef USE_REALLY_FULL_SEARCH
    offs[0] = 1;
    for (i = 1; i < dim; i++)
        offs[i] = offs[i-1]*range;
    abs_pow34_v(scaled, in, size);
    quantize_bands(s->qcoefs, in, scaled, size, Q34, !IS_CODEBOOK_UNSIGNED(cb), maxval);
#endif /* USE_REALLY_FULL_SEARCH */
    for (i = 0; i < size; i += dim) {
        float mincost;
        int minidx  = 0;
        int minbits = 0;
        const float *vec;
#ifndef USE_REALLY_FULL_SEARCH
        int (*quants)[2] = &s->qcoefs[i];
        mincost = 0.0f;
        for (j = 0; j < dim; j++)
            mincost += in[i+j]*in[i+j];
        minidx = IS_CODEBOOK_UNSIGNED(cb) ? 0 : 40;
        minbits = ff_aac_spectral_bits[cb-1][minidx];
        mincost = mincost * lambda + minbits;
        for (j = 0; j < (1<<dim); j++) {
            float rd = 0.0f;
            int curbits;
            int curidx = IS_CODEBOOK_UNSIGNED(cb) ? 0 : 40;
            int same   = 0;
            for (k = 0; k < dim; k++) {
                if ((j & (1 << k)) && quants[k][0] == quants[k][1]) {
                    same = 1;
                    break;
                }
            }
            if (same)
                continue;
            for (k = 0; k < dim; k++)
                curidx += quants[k][!!(j & (1 << k))] * offs[dim - 1 - k];
            curbits =  ff_aac_spectral_bits[cb-1][curidx];
            vec     = &ff_aac_codebook_vectors[cb-1][curidx*dim];
#else
        vec = ff_aac_codebook_vectors[cb-1];
        mincost = INFINITY;
        for (j = 0; j < ff_aac_spectral_sizes[cb-1]; j++, vec += dim) {
            float rd = 0.0f;
            int curbits = ff_aac_spectral_bits[cb-1][j];
            int curidx  = j;
#endif /* USE_REALLY_FULL_SEARCH */
            if (IS_CODEBOOK_UNSIGNED(cb)) {
                for (k = 0; k < dim; k++) {
                    float t = fabsf(in[i+k]);
                    float di;
                    if (vec[k] == 64.0f) { //FIXME: slow
                        //do not code with escape sequence small values
                        if (t < 39.0f*IQ) {
                            rd = INFINITY;
                            break;
                        }
                        if (t >= CLIPPED_ESCAPE) {
                            di = t - CLIPPED_ESCAPE;
                            curbits += 21;
                        } else {
                            int c = av_clip(quant(t, Q), 0, 8191);
                            di = t - c*cbrtf(c)*IQ;
                            curbits += av_log2(c)*2 - 4 + 1;
                        }
                    } else {
                        di = t - vec[k]*IQ;
                    }
                    if (vec[k] != 0.0f)
                        curbits++;
                    rd += di*di;
                }
            } else {
                for (k = 0; k < dim; k++) {
                    float di = in[i+k] - vec[k]*IQ;
                    rd += di*di;
                }
            }
            rd = rd * lambda + curbits;
            if (rd < mincost) {
                mincost = rd;
                minidx  = curidx;
                minbits = curbits;
            }
        }
        put_bits(pb, ff_aac_spectral_bits[cb-1][minidx], ff_aac_spectral_codes[cb-1][minidx]);
        if (IS_CODEBOOK_UNSIGNED(cb))
            for (j = 0; j < dim; j++)
                if (ff_aac_codebook_vectors[cb-1][minidx*dim+j] != 0.0f)
                    put_bits(pb, 1, in[i+j] < 0.0f);
        if (cb == ESC_BT) {
            for (j = 0; j < 2; j++) {
                if (ff_aac_codebook_vectors[cb-1][minidx*2+j] == 64.0f) {
                    int coef = av_clip(quant(fabsf(in[i+j]), Q), 0, 8191);
                    int len = av_log2(coef);

                    put_bits(pb, len - 4 + 1, (1 << (len - 4 + 1)) - 2);
                    put_bits(pb, len, coef & ((1 << len) - 1));
                }
            }
        }
    }
//STOP_TIMER("quantize_and_encode")
}

/**
 * structure used in optimal codebook search
 */
typedef struct BandCodingPath {
    int prev_idx; ///< pointer to the previous path point
    float cost;   ///< path cost
    int run;
} BandCodingPath;

/**
 * Encode band info for single window group bands.
 */
static void encode_window_bands_info(AACEncContext *s, SingleChannelElement *sce,
                                     int win, int group_len, const float lambda)
{
    BandCodingPath path[120][12];
    int w, swb, cb, start, start2, size;
    int i, j;
    const int max_sfb  = sce->ics.max_sfb;
    const int run_bits = sce->ics.num_windows == 1 ? 5 : 3;
    const int run_esc  = (1 << run_bits) - 1;
    int idx, ppos, count;
    int stackrun[120], stackcb[120], stack_len;
    float next_minrd = INFINITY;
    int next_mincb = 0;

    abs_pow34_v(s->scoefs, sce->coeffs, 1024);
    start = win*128;
    for (cb = 0; cb < 12; cb++) {
        path[0][cb].cost     = 0.0f;
        path[0][cb].prev_idx = -1;
        path[0][cb].run      = 0;
    }
    for (swb = 0; swb < max_sfb; swb++) {
        start2 = start;
        size = sce->ics.swb_sizes[swb];
        if (sce->zeroes[win*16 + swb]) {
            for (cb = 0; cb < 12; cb++) {
                path[swb+1][cb].prev_idx = cb;
                path[swb+1][cb].cost     = path[swb][cb].cost;
                path[swb+1][cb].run      = path[swb][cb].run + 1;
            }
        } else {
            float minrd = next_minrd;
            int mincb = next_mincb;
            next_minrd = INFINITY;
            next_mincb = 0;
            for (cb = 0; cb < 12; cb++) {
                float cost_stay_here, cost_get_here;
                float rd = 0.0f;
                for (w = 0; w < group_len; w++) {
                    FFPsyBand *band = &s->psy.psy_bands[s->cur_channel*PSY_MAX_BANDS+(win+w)*16+swb];
                    rd += quantize_band_cost(s, sce->coeffs + start + w*128,
                                             s->scoefs + start + w*128, size,
                                             sce->sf_idx[(win+w)*16+swb], cb,
                                             lambda / band->threshold, INFINITY, NULL);
                }
                cost_stay_here = path[swb][cb].cost + rd;
                cost_get_here  = minrd              + rd + run_bits + 4;
                if (   run_value_bits[sce->ics.num_windows == 8][path[swb][cb].run]
                    != run_value_bits[sce->ics.num_windows == 8][path[swb][cb].run+1])
                    cost_stay_here += run_bits;
                if (cost_get_here < cost_stay_here) {
                    path[swb+1][cb].prev_idx = mincb;
                    path[swb+1][cb].cost     = cost_get_here;
                    path[swb+1][cb].run      = 1;
                } else {
                    path[swb+1][cb].prev_idx = cb;
                    path[swb+1][cb].cost     = cost_stay_here;
                    path[swb+1][cb].run      = path[swb][cb].run + 1;
                }
                if (path[swb+1][cb].cost < next_minrd) {
                    next_minrd = path[swb+1][cb].cost;
                    next_mincb = cb;
                }
            }
        }
        start += sce->ics.swb_sizes[swb];
    }

    //convert resulting path from backward-linked list
    stack_len = 0;
    idx       = 0;
    for (cb = 1; cb < 12; cb++)
        if (path[max_sfb][cb].cost < path[max_sfb][idx].cost)
            idx = cb;
    ppos = max_sfb;
    while (ppos > 0) {
        cb = idx;
        stackrun[stack_len] = path[ppos][cb].run;
        stackcb [stack_len] = cb;
        idx = path[ppos-path[ppos][cb].run+1][cb].prev_idx;
        ppos -= path[ppos][cb].run;
        stack_len++;
    }
    //perform actual band info encoding
    start = 0;
    for (i = stack_len - 1; i >= 0; i--) {
        put_bits(&s->pb, 4, stackcb[i]);
        count = stackrun[i];
        memset(sce->zeroes + win*16 + start, !stackcb[i], count);
        //XXX: memset when band_type is also uint8_t
        for (j = 0; j < count; j++) {
            sce->band_type[win*16 + start] =  stackcb[i];
            start++;
        }
        while (count >= run_esc) {
            put_bits(&s->pb, run_bits, run_esc);
            count -= run_esc;
        }
        put_bits(&s->pb, run_bits, count);
    }
}

typedef struct TrellisPath {
    float cost;
    int prev;
    int min_val;
    int max_val;
} TrellisPath;

static void search_for_quantizers_anmr(AVCodecContext *avctx, AACEncContext *s,
                                       SingleChannelElement *sce,
                                       const float lambda)
{
    int q, w, w2, g, start = 0;
    int i, j;
    int idx;
    TrellisPath paths[121][256];
    int bandaddr[121];
    int minq;
    float mincost;

    for (i = 0; i < 256; i++) {
        paths[0][i].cost    = 0.0f;
        paths[0][i].prev    = -1;
        paths[0][i].min_val = i;
        paths[0][i].max_val = i;
    }
    for (j = 1; j < 121; j++) {
        for (i = 0; i < 256; i++) {
            paths[j][i].cost    = INFINITY;
            paths[j][i].prev    = -2;
            paths[j][i].min_val = INT_MAX;
            paths[j][i].max_val = 0;
        }
    }
    idx = 1;
    abs_pow34_v(s->scoefs, sce->coeffs, 1024);
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        start = w*128;
        for (g = 0; g < sce->ics.num_swb; g++) {
            const float *coefs = sce->coeffs + start;
            float qmin, qmax;
            int nz = 0;

            bandaddr[idx] = w * 16 + g;
            qmin = INT_MAX;
            qmax = 0.0f;
            for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                FFPsyBand *band = &s->psy.psy_bands[s->cur_channel*PSY_MAX_BANDS+(w+w2)*16+g];
                if (band->energy <= band->threshold || band->threshold == 0.0f) {
                    sce->zeroes[(w+w2)*16+g] = 1;
                    continue;
                }
                sce->zeroes[(w+w2)*16+g] = 0;
                nz = 1;
                for (i = 0; i < sce->ics.swb_sizes[g]; i++) {
                    float t = fabsf(coefs[w2*128+i]);
                    if (t > 0.0f)
                        qmin = FFMIN(qmin, t);
                    qmax = FFMAX(qmax, t);
                }
            }
            if (nz) {
                int minscale, maxscale;
                float minrd = INFINITY;
                //minimum scalefactor index is when minimum nonzero coefficient after quantizing is not clipped
                minscale = av_clip_uint8(log2(qmin)*4 - 69 + SCALE_ONE_POS - SCALE_DIV_512);
                //maximum scalefactor index is when maximum coefficient after quantizing is still not zero
                maxscale = av_clip_uint8(log2(qmax)*4 +  6 + SCALE_ONE_POS - SCALE_DIV_512);
                for (q = minscale; q < maxscale; q++) {
                    float dists[12], dist;
                    memset(dists, 0, sizeof(dists));
                    for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                        FFPsyBand *band = &s->psy.psy_bands[s->cur_channel*PSY_MAX_BANDS+(w+w2)*16+g];
                        int cb;
                        for (cb = 0; cb <= ESC_BT; cb++)
                            dists[cb] += quantize_band_cost(s, coefs + w2*128, s->scoefs + start + w2*128, sce->ics.swb_sizes[g],
                                                            q, cb, lambda / band->threshold, INFINITY, NULL);
                    }
                    dist = dists[0];
                    for (i = 1; i <= ESC_BT; i++)
                        dist = FFMIN(dist, dists[i]);
                    minrd = FFMIN(minrd, dist);

                    for (i = FFMAX(q - SCALE_MAX_DIFF, 0); i < FFMIN(q + SCALE_MAX_DIFF, 256); i++) {
                        float cost;
                        int minv, maxv;
                        if (isinf(paths[idx - 1][i].cost))
                            continue;
                        cost = paths[idx - 1][i].cost + dist
                               + ff_aac_scalefactor_bits[q - i + SCALE_DIFF_ZERO];
                        minv = FFMIN(paths[idx - 1][i].min_val, q);
                        maxv = FFMAX(paths[idx - 1][i].max_val, q);
                        if (cost < paths[idx][q].cost && maxv-minv < SCALE_MAX_DIFF) {
                            paths[idx][q].cost    = cost;
                            paths[idx][q].prev    = i;
                            paths[idx][q].min_val = minv;
                            paths[idx][q].max_val = maxv;
                        }
                    }
                }
            } else {
                for (q = 0; q < 256; q++) {
                    if (!isinf(paths[idx - 1][q].cost)) {
                        paths[idx][q].cost = paths[idx - 1][q].cost + 1;
                        paths[idx][q].prev = q;
                        paths[idx][q].min_val = FFMIN(paths[idx - 1][q].min_val, q);
                        paths[idx][q].max_val = FFMAX(paths[idx - 1][q].max_val, q);
                        continue;
                    }
                    for (i = FFMAX(q - SCALE_MAX_DIFF, 0); i < FFMIN(q + SCALE_MAX_DIFF, 256); i++) {
                        float cost;
                        int minv, maxv;
                        if (isinf(paths[idx - 1][i].cost))
                            continue;
                        cost = paths[idx - 1][i].cost + ff_aac_scalefactor_bits[q - i + SCALE_DIFF_ZERO];
                        minv = FFMIN(paths[idx - 1][i].min_val, q);
                        maxv = FFMAX(paths[idx - 1][i].max_val, q);
                        if (cost < paths[idx][q].cost && maxv-minv < SCALE_MAX_DIFF) {
                            paths[idx][q].cost    = cost;
                            paths[idx][q].prev    = i;
                            paths[idx][q].min_val = minv;
                            paths[idx][q].max_val = maxv;
                        }
                    }
                }
            }
            sce->zeroes[w*16+g] = !nz;
            start += sce->ics.swb_sizes[g];
            idx++;
        }
    }
    idx--;
    mincost = paths[idx][0].cost;
    minq    = 0;
    for (i = 1; i < 256; i++) {
        if (paths[idx][i].cost < mincost) {
            mincost = paths[idx][i].cost;
            minq = i;
        }
    }
    while (idx) {
        sce->sf_idx[bandaddr[idx]] = minq;
        minq = paths[idx][minq].prev;
        idx--;
    }
    //set the same quantizers inside window groups
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w])
        for (g = 0;  g < sce->ics.num_swb; g++)
            for (w2 = 1; w2 < sce->ics.group_len[w]; w2++)
                sce->sf_idx[(w+w2)*16+g] = sce->sf_idx[w*16+g];
}

/**
 * two-loop quantizers search taken from ISO 13818-7 Appendix C
 */
static void search_for_quantizers_twoloop(AVCodecContext *avctx,
                                          AACEncContext *s,
                                          SingleChannelElement *sce,
                                          const float lambda)
{
    int start = 0, i, w, w2, g;
    int destbits = avctx->bit_rate * 1024.0 / avctx->sample_rate / avctx->channels;
    float dists[128], uplims[128];
    int fflag, minscaler;
    int its  = 0;
    int allz = 0;
    float minthr = INFINITY;

    //XXX: some heuristic to determine initial quantizers will reduce search time
    memset(dists, 0, sizeof(dists));
    //determine zero bands and upper limits
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        for (g = 0;  g < sce->ics.num_swb; g++) {
            int nz = 0;
            float uplim = 0.0f;
            for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                FFPsyBand *band = &s->psy.psy_bands[s->cur_channel*PSY_MAX_BANDS+(w+w2)*16+g];
                uplim += band->threshold;
                if (band->energy <= band->threshold || band->threshold == 0.0f) {
                    sce->zeroes[(w+w2)*16+g] = 1;
                    continue;
                }
                nz = 1;
            }
            uplims[w*16+g] = uplim *512;
            sce->zeroes[w*16+g] = !nz;
            if (nz)
                minthr = FFMIN(minthr, uplim);
            allz = FFMAX(allz, nz);
        }
    }
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        for (g = 0;  g < sce->ics.num_swb; g++) {
            if (sce->zeroes[w*16+g]) {
                sce->sf_idx[w*16+g] = SCALE_ONE_POS;
                continue;
            }
            sce->sf_idx[w*16+g] = SCALE_ONE_POS + FFMIN(log2(uplims[w*16+g]/minthr)*4,59);
        }
    }

    if (!allz)
        return;
    abs_pow34_v(s->scoefs, sce->coeffs, 1024);
    //perform two-loop search
    //outer loop - improve quality
    do {
        int tbits, qstep;
        minscaler = sce->sf_idx[0];
        //inner loop - quantize spectrum to fit into given number of bits
        qstep = its ? 1 : 32;
        do {
            int prev = -1;
            tbits = 0;
            fflag = 0;
            for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
                start = w*128;
                for (g = 0;  g < sce->ics.num_swb; g++) {
                    const float *coefs = sce->coeffs + start;
                    const float *scaled = s->scoefs + start;
                    int bits = 0;
                    int cb;
                    float mindist = INFINITY;
                    int minbits = 0;

                    if (sce->zeroes[w*16+g] || sce->sf_idx[w*16+g] >= 218) {
                        start += sce->ics.swb_sizes[g];
                        continue;
                    }
                    minscaler = FFMIN(minscaler, sce->sf_idx[w*16+g]);
                    for (cb = 0; cb <= ESC_BT; cb++) {
                        float dist = 0.0f;
                        int bb = 0;
                        for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                            int b;
                            dist += quantize_band_cost(s, coefs + w2*128,
                                                       scaled + w2*128,
                                                       sce->ics.swb_sizes[g],
                                                       sce->sf_idx[w*16+g],
                                                       cb,
                                                       lambda,
                                                       INFINITY,
                                                       &b);
                            bb += b;
                        }
                        if (dist < mindist) {
                            mindist = dist;
                            minbits = bb;
                        }
                    }
                    dists[w*16+g] = (mindist - minbits) / lambda;
                    bits = minbits;
                    if (prev != -1) {
                        bits += ff_aac_scalefactor_bits[sce->sf_idx[w*16+g] - prev + SCALE_DIFF_ZERO];
                    }
                    tbits += bits;
                    start += sce->ics.swb_sizes[g];
                    prev = sce->sf_idx[w*16+g];
                }
            }
            if (tbits > destbits) {
                for (i = 0; i < 128; i++)
                    if (sce->sf_idx[i] < 218 - qstep)
                        sce->sf_idx[i] += qstep;
            } else {
                for (i = 0; i < 128; i++)
                    if (sce->sf_idx[i] > 60 - qstep)
                        sce->sf_idx[i] -= qstep;
            }
            qstep >>= 1;
            if (!qstep && tbits > destbits*1.02)
                qstep = 1;
            if (sce->sf_idx[0] >= 217)
                break;
        } while (qstep);

        fflag = 0;
        minscaler = av_clip(minscaler, 60, 255 - SCALE_MAX_DIFF);
        for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
            start = w*128;
            for (g = 0; g < sce->ics.num_swb; g++) {
                int prevsc = sce->sf_idx[w*16+g];
                if (dists[w*16+g] > uplims[w*16+g] && sce->sf_idx[w*16+g] > 60)
                    sce->sf_idx[w*16+g]--;
                sce->sf_idx[w*16+g] = av_clip(sce->sf_idx[w*16+g], minscaler, minscaler + SCALE_MAX_DIFF);
                sce->sf_idx[w*16+g] = FFMIN(sce->sf_idx[w*16+g], 219);
                if (sce->sf_idx[w*16+g] != prevsc)
                    fflag = 1;
            }
        }
        its++;
    } while (fflag && its < 10);
}

static void search_for_quantizers_faac(AVCodecContext *avctx, AACEncContext *s,
                                       SingleChannelElement *sce,
                                       const float lambda)
{
    int start = 0, i, w, w2, g;
    float uplim[128], maxq[128];
    int minq, maxsf;
    float distfact = ((sce->ics.num_windows > 1) ? 85.80 : 147.84) / lambda;
    int last = 0, lastband = 0, curband = 0;
    float avg_energy = 0.0;
    if (sce->ics.num_windows == 1) {
        start = 0;
        for (i = 0; i < 1024; i++) {
            if (i - start >= sce->ics.swb_sizes[curband]) {
                start += sce->ics.swb_sizes[curband];
                curband++;
            }
            if (sce->coeffs[i]) {
                avg_energy += sce->coeffs[i] * sce->coeffs[i];
                last = i;
                lastband = curband;
            }
        }
    } else {
        for (w = 0; w < 8; w++) {
            const float *coeffs = sce->coeffs + w*128;
            start = 0;
            for (i = 0; i < 128; i++) {
                if (i - start >= sce->ics.swb_sizes[curband]) {
                    start += sce->ics.swb_sizes[curband];
                    curband++;
                }
                if (coeffs[i]) {
                    avg_energy += coeffs[i] * coeffs[i];
                    last = FFMAX(last, i);
                    lastband = FFMAX(lastband, curband);
                }
            }
        }
    }
    last++;
    avg_energy /= last;
    if (avg_energy == 0.0f) {
        for (i = 0; i < FF_ARRAY_ELEMS(sce->sf_idx); i++)
            sce->sf_idx[i] = SCALE_ONE_POS;
        return;
    }
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        start = w*128;
        for (g = 0; g < sce->ics.num_swb; g++) {
            float *coefs   = sce->coeffs + start;
            const int size = sce->ics.swb_sizes[g];
            int start2 = start, end2 = start + size, peakpos = start;
            float maxval = -1, thr = 0.0f, t;
            maxq[w*16+g] = 0.0f;
            if (g > lastband) {
                maxq[w*16+g] = 0.0f;
                start += size;
                for (w2 = 0; w2 < sce->ics.group_len[w]; w2++)
                    memset(coefs + w2*128, 0, sizeof(coefs[0])*size);
                continue;
            }
            for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                for (i = 0; i < size; i++) {
                    float t = coefs[w2*128+i]*coefs[w2*128+i];
                    maxq[w*16+g] = FFMAX(maxq[w*16+g], fabsf(coefs[w2*128 + i]));
                    thr += t;
                    if (sce->ics.num_windows == 1 && maxval < t) {
                        maxval  = t;
                        peakpos = start+i;
                    }
                }
            }
            if (sce->ics.num_windows == 1) {
                start2 = FFMAX(peakpos - 2, start2);
                end2   = FFMIN(peakpos + 3, end2);
            } else {
                start2 -= start;
                end2   -= start;
            }
            start += size;
            thr = pow(thr / (avg_energy * (end2 - start2)), 0.3 + 0.1*(lastband - g) / lastband);
            t   = 1.0 - (1.0 * start2 / last);
            uplim[w*16+g] = distfact / (1.4 * thr + t*t*t + 0.075);
        }
    }
    memset(sce->sf_idx, 0, sizeof(sce->sf_idx));
    abs_pow34_v(s->scoefs, sce->coeffs, 1024);
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        start = w*128;
        for (g = 0;  g < sce->ics.num_swb; g++) {
            const float *coefs  = sce->coeffs + start;
            const float *scaled = s->scoefs   + start;
            const int size      = sce->ics.swb_sizes[g];
            int scf, prev_scf, step;
            int min_scf = 0, max_scf = 255;
            float curdiff;
            if (maxq[w*16+g] < 21.544) {
                sce->zeroes[w*16+g] = 1;
                start += size;
                continue;
            }
            sce->zeroes[w*16+g] = 0;
            scf  = prev_scf = av_clip(SCALE_ONE_POS - SCALE_DIV_512 - log2(1/maxq[w*16+g])*16/3, 60, 218);
            step = 16;
            for (;;) {
                float dist = 0.0f;
                int quant_max;

                for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                    int b;
                    dist += quantize_band_cost(s, coefs + w2*128,
                                               scaled + w2*128,
                                               sce->ics.swb_sizes[g],
                                               scf,
                                               ESC_BT,
                                               lambda,
                                               INFINITY,
                                               &b);
                    dist -= b;
                }
                dist *= 1.0f / 512.0f / lambda;
                quant_max = quant(maxq[w*16+g], ff_aac_pow2sf_tab[200 - scf + SCALE_ONE_POS - SCALE_DIV_512]);
                if (quant_max >= 8191) { // too much, return to the previous quantizer
                    sce->sf_idx[w*16+g] = prev_scf;
                    break;
                }
                prev_scf = scf;
                curdiff = fabsf(dist - uplim[w*16+g]);
                if (curdiff == 0.0f)
                    step = 0;
                else
                    step = fabsf(log2(curdiff));
                if (dist > uplim[w*16+g])
                    step = -step;
                if (FFABS(step) <= 1 || (step > 0 && scf >= max_scf) || (step < 0 && scf <= min_scf)) {
                    sce->sf_idx[w*16+g] = scf;
                    break;
                }
                scf += step;
                if (step > 0)
                    min_scf = scf;
                else
                    max_scf = scf;
            }
            start += size;
        }
    }
    minq = sce->sf_idx[0] ? sce->sf_idx[0] : INT_MAX;
    for (i = 1; i < 128; i++) {
        if (!sce->sf_idx[i])
            sce->sf_idx[i] = sce->sf_idx[i-1];
        else
            minq = FFMIN(minq, sce->sf_idx[i]);
    }
    if (minq == INT_MAX)
        minq = 0;
    minq = FFMIN(minq, SCALE_MAX_POS);
    maxsf = FFMIN(minq + SCALE_MAX_DIFF, SCALE_MAX_POS);
    for (i = 126; i >= 0; i--) {
        if (!sce->sf_idx[i])
            sce->sf_idx[i] = sce->sf_idx[i+1];
        sce->sf_idx[i] = av_clip(sce->sf_idx[i], minq, maxsf);
    }
}

static void search_for_quantizers_fast(AVCodecContext *avctx, AACEncContext *s,
                                       SingleChannelElement *sce,
                                       const float lambda)
{
    int start = 0, i, w, w2, g;
    int minq = 255;

    memset(sce->sf_idx, 0, sizeof(sce->sf_idx));
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w]) {
        start = w*128;
        for (g = 0; g < sce->ics.num_swb; g++) {
            for (w2 = 0; w2 < sce->ics.group_len[w]; w2++) {
                FFPsyBand *band = &s->psy.psy_bands[s->cur_channel*PSY_MAX_BANDS+(w+w2)*16+g];
                if (band->energy <= band->threshold) {
                    sce->sf_idx[(w+w2)*16+g] = 218;
                    sce->zeroes[(w+w2)*16+g] = 1;
                } else {
                    sce->sf_idx[(w+w2)*16+g] = av_clip(SCALE_ONE_POS - SCALE_DIV_512 + log2(band->threshold), 80, 218);
                    sce->zeroes[(w+w2)*16+g] = 0;
                }
                minq = FFMIN(minq, sce->sf_idx[(w+w2)*16+g]);
            }
        }
    }
    for (i = 0; i < 128; i++) {
        sce->sf_idx[i] = 140;
        //av_clip(sce->sf_idx[i], minq, minq + SCALE_MAX_DIFF - 1);
    }
    //set the same quantizers inside window groups
    for (w = 0; w < sce->ics.num_windows; w += sce->ics.group_len[w])
        for (g = 0;  g < sce->ics.num_swb; g++)
            for (w2 = 1; w2 < sce->ics.group_len[w]; w2++)
                sce->sf_idx[(w+w2)*16+g] = sce->sf_idx[w*16+g];
}

static void search_for_ms(AACEncContext *s, ChannelElement *cpe,
                          const float lambda)
{
    int start = 0, i, w, w2, g;
    float M[128], S[128];
    float *L34 = s->scoefs, *R34 = s->scoefs + 128, *M34 = s->scoefs + 128*2, *S34 = s->scoefs + 128*3;
    SingleChannelElement *sce0 = &cpe->ch[0];
    SingleChannelElement *sce1 = &cpe->ch[1];
    if (!cpe->common_window)
        return;
    for (w = 0; w < sce0->ics.num_windows; w += sce0->ics.group_len[w]) {
        for (g = 0;  g < sce0->ics.num_swb; g++) {
            if (!cpe->ch[0].zeroes[w*16+g] && !cpe->ch[1].zeroes[w*16+g]) {
                float dist1 = 0.0f, dist2 = 0.0f;
                for (w2 = 0; w2 < sce0->ics.group_len[w]; w2++) {
                    FFPsyBand *band0 = &s->psy.psy_bands[(s->cur_channel+0)*PSY_MAX_BANDS+(w+w2)*16+g];
                    FFPsyBand *band1 = &s->psy.psy_bands[(s->cur_channel+1)*PSY_MAX_BANDS+(w+w2)*16+g];
                    float minthr = FFMIN(band0->threshold, band1->threshold);
                    float maxthr = FFMAX(band0->threshold, band1->threshold);
                    for (i = 0; i < sce0->ics.swb_sizes[g]; i++) {
                        M[i] = (sce0->coeffs[start+w2*128+i]
                              + sce1->coeffs[start+w2*128+i]) * 0.5;
                        S[i] =  sce0->coeffs[start+w2*128+i]
                              - sce1->coeffs[start+w2*128+i];
                    }
                    abs_pow34_v(L34, sce0->coeffs+start+w2*128, sce0->ics.swb_sizes[g]);
                    abs_pow34_v(R34, sce1->coeffs+start+w2*128, sce0->ics.swb_sizes[g]);
                    abs_pow34_v(M34, M,                         sce0->ics.swb_sizes[g]);
                    abs_pow34_v(S34, S,                         sce0->ics.swb_sizes[g]);
                    dist1 += quantize_band_cost(s, sce0->coeffs + start + w2*128,
                                                L34,
                                                sce0->ics.swb_sizes[g],
                                                sce0->sf_idx[(w+w2)*16+g],
                                                sce0->band_type[(w+w2)*16+g],
                                                lambda / band0->threshold, INFINITY, NULL);
                    dist1 += quantize_band_cost(s, sce1->coeffs + start + w2*128,
                                                R34,
                                                sce1->ics.swb_sizes[g],
                                                sce1->sf_idx[(w+w2)*16+g],
                                                sce1->band_type[(w+w2)*16+g],
                                                lambda / band1->threshold, INFINITY, NULL);
                    dist2 += quantize_band_cost(s, M,
                                                M34,
                                                sce0->ics.swb_sizes[g],
                                                sce0->sf_idx[(w+w2)*16+g],
                                                sce0->band_type[(w+w2)*16+g],
                                                lambda / maxthr, INFINITY, NULL);
                    dist2 += quantize_band_cost(s, S,
                                                S34,
                                                sce1->ics.swb_sizes[g],
                                                sce1->sf_idx[(w+w2)*16+g],
                                                sce1->band_type[(w+w2)*16+g],
                                                lambda / minthr, INFINITY, NULL);
                }
                cpe->ms_mask[w*16+g] = dist2 < dist1;
            }
            start += sce0->ics.swb_sizes[g];
        }
    }
}

AACCoefficientsEncoder ff_aac_coders[] = {
    {
        search_for_quantizers_faac,
        encode_window_bands_info,
        quantize_and_encode_band,
        search_for_ms,
    },
    {
        search_for_quantizers_anmr,
        encode_window_bands_info,
        quantize_and_encode_band,
        search_for_ms,
    },
    {
        search_for_quantizers_twoloop,
        encode_window_bands_info,
        quantize_and_encode_band,
        search_for_ms,
    },
    {
        search_for_quantizers_fast,
        encode_window_bands_info,
        quantize_and_encode_band,
        search_for_ms,
    },
};
