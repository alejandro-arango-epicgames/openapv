/*
 * Copyright (c) 2022 Samsung Electronics Co., Ltd.
 * All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * - Neither the name of the copyright owner, nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "oapv_def.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <stdarg.h>
#endif

static oapv_log_callback_t current_log_callback = NULL;
static void* current_log_user_data = NULL;
static int current_log_verbosity = OAPV_LOG_DEBUG;

static oapv_cputrace_callbacks_t cputrace_callbacks = { NULL, NULL };

#define BEGIN_CPU_TRACE(name)                                     \
    if(cputrace_callbacks.begin_event) {                          \
        cputrace_callbacks.begin_event(name, __FILE__, __LINE__); \
    }

#define END_CPU_TRACE()                 \
    if(cputrace_callbacks.end_event) {  \
        cputrace_callbacks.end_event(); \
    }

/* Simple log message for trouble shooting. */
static void log_msg(int verbosity, const char *fmt, ...)
{
    if (verbosity > current_log_verbosity)
    {
        return;
    }

    char str[1024] = { '\0' };
    va_list args;
    va_start(args, fmt);
    vsprintf(str + strlen(str), fmt, args);
    va_end(args);

    if (current_log_callback != NULL) {
        current_log_callback(str, verbosity, current_log_user_data);
    }
    else {
        switch(verbosity) {
        case OAPV_LOG_ERROR:
            fprintf(stderr, "[ERROR] %s", str);
            break;
        case OAPV_LOG_WARNING:
            fprintf(stderr, "[WARNING] %s", str);
            break;
        case OAPV_LOG_INFO:
            printf("[INFO] %s", str);
            break;
        case OAPV_LOG_DEBUG:
            printf("[DEBUG] %s", str);
            break;
        default:
            printf("[UNKNOWN] %s", str);
            break;
        }
    }
}

static void imgb_pad(oapv_imgb_t *imgb, int aw, int ah, int comp_sft[N_C][2])
{
    int imgb_w = imgb->w[0];
    int imgb_h = imgb->h[0];

    if(aw == imgb_w && ah == imgb_h) { // no needs to pad
        return;
    }

    if(aw != imgb_w) {
        for(int c = 0; c < imgb->np; c++) {
            int  sw = imgb_w >> comp_sft[c][0];
            int  ew = aw >> comp_sft[c][0];
            int  th = ah >> comp_sft[c][1];
            pel *dst = (pel *)imgb->a[c];
            pel  src;

            for(int h = 0; h < th; h++) {
                src = dst[sw - 1];
                for(int w = sw; w < ew; w++) {
                    dst[w] = src;
                }
                dst += (imgb->s[c] >> 1);
            }
        }
    }

    if(ah != imgb_h) {
        for(int c = 0; c < imgb->np; c++) {
            int  sh = imgb_h >> comp_sft[c][1];
            int  eh = ah >> comp_sft[c][1];
            int  tw = aw >> comp_sft[c][0];
            pel *dst = ((pel *)imgb->a[c]) + sh * (imgb->s[c] >> 1);
            pel *src = dst - (imgb->s[c] >> 1);

            for(int h = sh; h < eh; h++) {
                oapv_mcpy(dst, src, sizeof(pel) * tw);
                dst += (imgb->s[c] >> 1);
            }
        }
    }
}

static void imgb_pad_p210(oapv_imgb_t *imgb, int aw, int ah, int comp_sft[N_C][2])
{
    int imgb_w = imgb->w[0];
    int imgb_h = imgb->h[0];

    if(aw == imgb_w && ah == imgb_h) { // no needs to pad
        return;
    }

    if(aw != imgb_w) {
        for(int c = 0; c < imgb->np; c++) {
            int  shift_w = 0;
            int  shift_h = 0;

            int  sw = imgb_w >> shift_w;
            int  ew = aw >> shift_w;
            int  th = ah >> shift_h;
            pel *dst = (pel *)imgb->a[c];
            pel  src;

            for(int h = 0; h < th; h++) {
                src = dst[sw - 1];
                for(int w = sw; w < ew; w++) {
                    dst[w] = src;
                }
                dst += (imgb->s[c] >> 1);
            }
        }
    }

    if(ah != imgb_h) {
        for(int c = 0; c < imgb->np; c++) {
            int  shift_w = 0;
            int  shift_h = 0;

            int  sh = imgb_h >> shift_h;
            int  eh = ah >> shift_h;
            int  tw = aw >> shift_w;
            pel *dst = ((pel *)imgb->a[c]) + sh * (imgb->s[c] >> 1);
            pel *src = dst - (imgb->s[c] >> 1);

            for(int h = sh; h < eh; h++) {
                oapv_mcpy(dst, src, sizeof(pel) * tw);
                dst += (imgb->s[c] >> 1);
            }
        }
    }
}

static void imgb_to_blk(oapv_imgb_t *imgb, int c, int x_l, int y_l, int w_l, int h_l, s16 *blk, int bd)
{
    u8 *src, *dst;
    int i, sft_hor, sft_ver;
    int byte_depth = (bd + 7) >> 3;

    if(c == 0) {
        sft_hor = sft_ver = 0;
    }
    else {
        u8 cfi = color_format_to_chroma_format_idc(OAPV_CS_GET_FORMAT(imgb->cs));
        sft_hor = get_chroma_sft_w(cfi);
        sft_ver = get_chroma_sft_h(cfi);
    }

    src = ((u8 *)imgb->a[c]) + ((y_l >> sft_ver) * imgb->s[c]) + ((x_l * byte_depth) >> sft_hor);
    dst = (u8 *)blk;

    for(i = 0; i < (h_l); i++) {
        oapv_mcpy(dst, src, (w_l) * byte_depth);

        src += imgb->s[c];
        dst += (w_l) * byte_depth;
    }
}

static void imgb_to_blk_16(void *src, int blk_w, int blk_h, int s_src, int offset_src, int s_dst, void *dst, int bd)
{
    const int mid_val = (1 << (bd - 1));
    s16      *s = (s16 *)src;
    s16      *d = (s16 *)dst;

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w] = s[w] - mid_val;
        }
        s = (s16 *)(((u8 *)s) + s_src);
        d = (s16 *)(((u8 *)d) + s_dst);
    }
}

static void imgb_to_blk_p21x_y(void *src, int blk_w, int blk_h, int s_src, int offset_src, int s_dst, void *dst, int bd)
{
    const int mid_val = (1 << (bd - 1));
    u16      *s = (s16 *)src;
    s16      *d = (s16 *)dst;
    int       shift_pic_bits = 16 - bd;

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w] = (s16)(s[w] >> shift_pic_bits) - mid_val;
        }
        s = (u16 *)(((u8 *)s) + s_src);
        d = (s16 *)(((u8 *)d) + s_dst);
    }
}

static void imgb_to_blk_p21x_uv(void *src, int blk_w, int blk_h, int s_src, int offset_src, int s_dst, void *dst, int bd)
{
    const int mid_val = (1 << (bd - 1));
    u16      *s = (u16 *)src + offset_src;
    s16      *d = (s16 *)dst;
    int       shift_pic_bits = 16 - bd;

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w] = (s16)(s[w * 2] >> shift_pic_bits) - mid_val;
        }
        s = (u16 *)(((u8 *)s) + s_src);
        d = (s16 *)(((u8 *)d) + s_dst);
    }
}

static void imgb_to_blk_p21x(oapv_imgb_t *imgb, int c, int x_l, int y_l, int w_l, int h_l, s16 *block, int bd)
{
    u16 *src, *dst;
    int  sft_hor, sft_ver, s_src;
    int  size_scale = 1;
    int  tc = c;
    int  shift_pic_bits = 16 - bd;

    if(c == 0) {
        sft_hor = sft_ver = 0;
    }
    else {
        u8 cfi = color_format_to_chroma_format_idc(OAPV_CS_GET_FORMAT(imgb->cs));
        sft_hor = get_chroma_sft_w(cfi);
        sft_ver = get_chroma_sft_h(cfi);
        size_scale = 2;
        tc = 1;
    }

    s_src = imgb->s[tc] >> (bd > 1 ? 1 : 0);
    src = ((u16 *)imgb->a[tc]) + ((y_l >> sft_ver) * s_src) + ((x_l * size_scale) >> sft_hor);
    dst = (u16 *)block;

    for(int i = 0; i < (h_l); i++) {
        for(int j = 0; j < (w_l); j++) {
            dst[j] = (src[j * size_scale + (c >> 1)] >> shift_pic_bits);
        }
        src += s_src;
        dst += w_l;
    }
}

static void blk_to_imgb_16(void *src, int blk_w, int blk_h, int s_src, int offset_dst, int s_dst, void *dst, int bd)
{
    const int max_val = (1 << bd) - 1;
    const int mid_val = (1 << (bd - 1));
    s16      *s = (s16 *)src;
    u16      *d = (u16 *)dst;

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w] = oapv_clip3(0, max_val, s[w] + mid_val);
        }
        s = (s16 *)(((u8 *)s) + s_src);
        d = (u16 *)(((u8 *)d) + s_dst);
    }
}

static void blk_to_imgb_p21x_y(void *src, int blk_w, int blk_h, int s_src, int offset_dst, int s_dst, void *dst, int bd)
{
    const int max_val = (1 << bd) - 1;
    const int mid_val = (1 << (bd - 1));
    s16      *s = (s16 *)src;
    u16      *d = (u16 *)dst;
    int       shift_pic_bits = 16 - bd;

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w] = oapv_clip3(0, max_val, s[w] + mid_val) << shift_pic_bits;
        }
        s = (s16 *)(((u8 *)s) + s_src);
        d = (u16 *)(((u8 *)d) + s_dst);
    }
}

static void blk_to_imgb_p21x_uv(void *src, int blk_w, int blk_h, int s_src, int x_pel, int s_dst, void *dst, int bd)
{
    const int max_val = (1 << bd) - 1;
    const int mid_val = (1 << (bd - 1));
    s16      *s = (s16 *)src;
    int       shift_pic_bits = 16 - bd;

    // x_pel is x-offset value from left boundary of picture in unit of pixel.
    // the 'dst' address has calculated by
    // dst = (s16*)((u8*)origin + y_pel*s_dst) + x_pel;
    // in case of P210 color format,
    // since 's_dst' is byte size of stride including all U and V pixel values,
    // y-offset calculation is correct.
    // however, the adding only x_pel is not enough to address the correct pixel
    // position of U or V because U & V use the same buffer plane
    // in interleaved way,
    // so, the 'dst' address should be increased by 'x_pel' to address pixel
    // position correctly.
    u16      *d = (u16 *)dst + x_pel; // p210 pixel value needs 0~65535 range

    for(int h = 0; h < blk_h; h++) {
        for(int w = 0; w < blk_w; w++) {
            d[w * 2] = ((u16)oapv_clip3(0, max_val, s[w] + mid_val)) << shift_pic_bits;
        }
        s = (s16 *)(((u8 *)s) + s_src);
        d = (u16 *)(((u8 *)d) + s_dst);
    }
}

static void fi_to_finfo(oapv_fi_t *fi, int pbu_type, int group_id, oapv_frm_info_t *finfo)
{
    finfo->w = (int)fi->frame_width; // casting to 'int' would be fine here
    finfo->h = (int)fi->frame_height; // casting to 'int' would be fine here
    finfo->cs = OAPV_CS_SET(chroma_format_idc_to_color_format(fi->chroma_format_idc), fi->bit_depth, 0);
    finfo->pbu_type = pbu_type;
    finfo->group_id = group_id;
    finfo->profile_idc = fi->profile_idc;
    finfo->level_idc = fi->level_idc;
    finfo->band_idc = fi->band_idc;
    finfo->chroma_format_idc = fi->chroma_format_idc;
    finfo->bit_depth = fi->bit_depth;
    finfo->capture_time_distance = fi->capture_time_distance;
}

static void fh_to_finfo(oapv_fh_t *fh, int pbu_type, int group_id, oapv_frm_info_t *finfo)
{
    fi_to_finfo(&fh->fi, pbu_type, group_id, finfo);
    finfo->use_q_matrix = fh->use_q_matrix;
    for(int c = 0; c < OAPV_MAX_CC; c++) {
        int mod = (1 << OAPV_LOG2_BLK) - 1;
        for(int i = 0; i < OAPV_BLK_D; i++) {
            finfo->q_matrix[c][i] = fh->q_matrix[c][i >> OAPV_LOG2_BLK][i & mod];
        }
    }
    finfo->color_description_present_flag = fh->color_description_present_flag;
    finfo->color_primaries = fh->color_primaries;
    finfo->transfer_characteristics = fh->transfer_characteristics;
    finfo->matrix_coefficients = fh->matrix_coefficients;
    finfo->full_range_flag = fh->full_range_flag;
}

///////////////////////////////////////////////////////////////////////////////
// start of encoder code
#if ENABLE_ENCODER
///////////////////////////////////////////////////////////////////////////////

static oapve_ctx_t *enc_id_to_ctx(oapve_t id)
{
    oapve_ctx_t *ctx;
    oapv_assert_rv(id, NULL);
    ctx = (oapve_ctx_t *)id;
    oapv_assert_rv((ctx)->magic == OAPVE_MAGIC_CODE, NULL);
    return ctx;
}

static oapve_ctx_t *enc_ctx_alloc(void)
{
    oapve_ctx_t *ctx;
    ctx = (oapve_ctx_t *)oapv_malloc_fast(sizeof(oapve_ctx_t));
    oapv_assert_rv(ctx, NULL);
    oapv_mset_x64a(ctx, 0, sizeof(oapve_ctx_t));
    return ctx;
}

static void enc_ctx_free(oapve_ctx_t *ctx)
{
    oapv_mfree_fast(ctx);
}

static oapve_core_t *enc_core_alloc()
{
    oapve_core_t *core;
    core = (oapve_core_t *)oapv_malloc_fast(sizeof(oapve_core_t));

    oapv_assert_rv(core, NULL);
    oapv_mset_x64a(core, 0, sizeof(oapve_core_t));

    return core;
}

static void enc_core_free(oapve_core_t *core)
{
    oapv_mfree_fast(core);
}

static int enc_core_init(oapve_core_t *core, oapve_ctx_t *ctx, int tile_idx, int thread_idx)
{
    core->tile_idx = tile_idx;
    core->ctx = ctx;
    return OAPV_OK;
}

static void enc_minus_mid_val(s16 *coef, int w_blk, int h_blk, int bit_depth)
{
    int mid_val = 1 << (bit_depth - 1);
    for(int i = 0; i < h_blk * w_blk; i++) {
        coef[i] -= mid_val;
    }
}

static int enc_set_tile_info(oapve_tile_t *ti, int w_pel, int h_pel, int tile_w,
                             int tile_h, int *num_tile_cols, int *num_tile_rows, int *num_tiles)
{
    (*num_tile_cols) = oapv_div_round_up(w_pel, tile_w);
    (*num_tile_rows) = oapv_div_round_up(h_pel, tile_h);
    (*num_tiles) = (*num_tile_cols) * (*num_tile_rows);

    for(int i = 0; i < (*num_tiles); i++) {
        int tx = (i % (*num_tile_cols)) * tile_w;
        int ty = (i / (*num_tile_cols)) * tile_h;
        ti[i].x = tx;
        ti[i].y = ty;
        ti[i].w = tx + tile_w > w_pel ? w_pel - tx : tile_w;
        ti[i].h = ty + tile_h > h_pel ? h_pel - ty : tile_h;
    }
    return OAPV_OK;
}

static double enc_block(oapve_ctx_t *ctx, oapve_core_t *core, int log2_w, int log2_h, int c)
{
    int bit_depth = ctx->bit_depth;

    oapv_trans(ctx, core->coef, log2_w, log2_h, bit_depth);
    ctx->fn_quant[0](core->coef, core->qp[c], core->q_mat_enc[c], log2_w, log2_h, bit_depth, c ? 128 : 212);

    core->dc_diff = core->coef[0] - core->prev_dc[c];
    core->prev_dc[c] = core->coef[0];

    if(ctx->imgb_r) {
        oapv_mcpy(core->coef_rec, core->coef, sizeof(s16) * OAPV_BLK_D);
        ctx->fn_dquant[0](core->coef_rec, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
        ctx->fn_itx[0](core->coef_rec, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    }

    return 0;
}

static double enc_block_rdo_medium(oapve_ctx_t *ctx, oapve_core_t *core, int log2_w, int log2_h, int c)
{
    int bit_depth = ctx->bit_depth;
    int qp = core->qp[c];
    double lambda = 0.57 * pow(2.0, (qp - 12.0) / 3.0);

    oapv_trans(ctx, core->coef, log2_w, log2_h, bit_depth);
    oapve_rdoq(core,core->coef, core->coef, log2_w, log2_h, c, bit_depth, lambda);

    core->dc_diff = core->coef[0] - core->prev_dc[c];
    core->prev_dc[c] = core->coef[0];

    if(ctx->imgb_r) {
        oapv_mcpy(core->coef_rec, core->coef, sizeof(s16) * OAPV_BLK_D);
        ctx->fn_dquant[0](core->coef_rec, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
        ctx->fn_itx[0](core->coef_rec, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    }

    return 0;
}

static double enc_block_rdo_slow(oapve_ctx_t *ctx, oapve_core_t *core, int log2_w, int log2_h, int c)
{
    ALIGNED_16(s16 org[OAPV_BLK_D]);
    ALIGNED_16(s16 recon[OAPV_BLK_D]);
    ALIGNED_16(s16 coeff[OAPV_BLK_D]);
    ALIGNED_16(s16 tmp_buf[OAPV_BLK_D]);

    ALIGNED_32(int rec_ups[OAPV_BLK_D]);
    ALIGNED_32(int rec_tmp[OAPV_BLK_D]);

    int        blk_w = 1 << log2_w;
    int        blk_h = 1 << log2_h;
    int        bit_depth = ctx->bit_depth;
    int        qp = core->qp[c];

    s16       *best_coeff = core->coef;
    s16       *best_recon = core->coef_rec;

    int        best_cost = INT_MAX;
    int        zero_dist = 0;
    const u8 *scanp = oapv_tbl_scan;
    const int  map_idx_diff[15] = { 0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7 };
    double     lambda = 0.57 * pow(2.0, (qp - 12.0) / 3.0);

    oapv_mcpy(org, core->coef, sizeof(s16) * OAPV_BLK_D);
    oapv_trans(ctx, core->coef, log2_w, log2_h, bit_depth);
    oapv_mcpy(coeff, core->coef, sizeof(s16) * OAPV_BLK_D);
    oapve_rdoq(core, coeff, coeff, log2_w, log2_h, c, bit_depth, lambda);

    {
        oapv_mcpy(recon, coeff, sizeof(s16) * OAPV_BLK_D);
        ctx->fn_dquant[0](recon, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
        ctx->fn_itx_part[0](recon, tmp_buf, ITX_SHIFT1, 1 << log2_w);
        oapv_itx_get_wo_sft(tmp_buf, recon, rec_ups, ITX_SHIFT2(bit_depth), 1 << log2_h);

        int cost = (int)ctx->fn_ssd[0](blk_w, blk_h, org, recon, blk_w, blk_w);
        oapv_mcpy(best_coeff, coeff, sizeof(s16) * OAPV_BLK_D);
        if(ctx->imgb_r) {
            oapv_mcpy(best_recon, recon, sizeof(s16) * OAPV_BLK_D);
        }
        if(cost == 0) {
            zero_dist = 1;
        }
        best_cost = cost;
    }

    for(int itr = 0; itr < (c == 0 ? 2 : 1) && !zero_dist; itr++) {
        for(int j = 0; j < OAPV_BLK_D && !zero_dist; j++) {
            int best_idx = 0;
            s16 org_coef = coeff[scanp[j]];
            int adj_rng = (c == 0 ? 13 : 5);
            if(org_coef == 0) {
                if(c == 0 && scanp[j] < 3) {
                    adj_rng = 3;
                }
                else {
                    continue;
                }
            }
            int q_step = 0;
            if(core->dq_shift[c] > 0) {
                q_step = (core->q_mat_dec[c][scanp[j]] + (1 << (core->dq_shift[c] - 1))) >> core->dq_shift[c];
            }
            else {
                q_step = (core->q_mat_dec[c][scanp[j]]) << (-core->dq_shift[c]);
            }

            for(int i = 1; i < adj_rng && !zero_dist; i++) {
                if(i > 2) {
                    if(best_idx == 0) {
                        continue;
                    }
                    else if(best_idx % 2 == 1 && i % 2 == 0) {
                        continue;
                    }
                    else if(best_idx % 2 == 0 && i % 2 == 1) {
                        continue;
                    }
                }

                s16 test_coef = org_coef + map_idx_diff[i];
                coeff[scanp[j]] = test_coef;
                int step_diff = q_step * map_idx_diff[i];
                ctx->fn_itx_adj[0](rec_ups, rec_tmp, j, step_diff, 9);
                for(int k = 0; k < 64; k++) {
                    recon[k] = (rec_tmp[k] + 512) >> 10;
                }

                int cost = (int)ctx->fn_ssd[0](blk_w, blk_h, org, recon, blk_w, blk_w);
                if(cost < best_cost) {
                    oapv_mcpy(rec_ups, rec_tmp, sizeof(int) * OAPV_BLK_D);
                    best_cost = cost;
                    best_coeff[scanp[j]] = test_coef;
                    best_idx = i;
                    if(cost == 0) {
                        zero_dist = 1;
                    }
                }
                else {
                    coeff[scanp[j]] = org_coef + map_idx_diff[best_idx];
                }
            }
        }
    }

    if(ctx->imgb_r) {
        oapv_mcpy(best_recon, best_coeff, sizeof(s16) * OAPV_BLK_D);
        ctx->fn_dquant[0](best_recon, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
        ctx->fn_itx[0](best_recon, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    }

    core->dc_diff = best_coeff[0] - core->prev_dc[c];
    core->prev_dc[c] = best_coeff[0];

    return best_cost;
}

#define OAPV_FULL_RDO_MAX_CAND 6

typedef struct oapve_coef_info oapve_coef_info_t;
struct oapve_coef_info
{
    int coef_pos;
    int coef_org;
    int coef_test;
    double cost;
};

void add_coef_list(oapve_coef_info_t* coef_list, oapve_coef_info_t coef_cur, int* list_cnt)
{
    if((*list_cnt) == OAPV_FULL_RDO_MAX_CAND && coef_cur.cost > coef_list[OAPV_FULL_RDO_MAX_CAND - 1].cost) {
        return;
    }

    int curr_pos = (*list_cnt) == OAPV_FULL_RDO_MAX_CAND ? OAPV_FULL_RDO_MAX_CAND - 1 : (*list_cnt);

    coef_list[curr_pos] = coef_cur;

    while(curr_pos > 0) {
        if(coef_list[curr_pos].cost < coef_list[curr_pos - 1].cost) {
            oapve_coef_info_t tmp = coef_list[curr_pos];
            coef_list[curr_pos] = coef_list[curr_pos - 1];
            coef_list[curr_pos - 1] = tmp;
            curr_pos--;
        }
        else {
            break;
        }
    }

    if(*list_cnt < OAPV_FULL_RDO_MAX_CAND) {
        (*list_cnt)++;
    }
}

static double enc_block_rdo_placebo(oapve_ctx_t* ctx, oapve_core_t* core, int log2_w, int log2_h, int c)
{
    ALIGNED_16(s16 org[OAPV_BLK_D]);
    ALIGNED_16(s16 recon[OAPV_BLK_D]);
    ALIGNED_16(s16 coeff[OAPV_BLK_D]);

    int        blk_w = 1 << log2_w;
    int        blk_h = 1 << log2_h;
    int        bit_depth = ctx->bit_depth;
    int        qp = core->qp[c];

    s16* best_coeff = core->coef;
    s16* best_recon = core->coef_rec;

    double     best_cost = INT_MAX;
    const u8* scanp = oapv_tbl_scan;

    oapv_mcpy(org, core->coef, sizeof(s16) * OAPV_BLK_D);
    oapv_trans(ctx, core->coef, log2_w, log2_h, bit_depth);
    ctx->fn_quant[0](core->coef, qp, core->q_mat_enc[c], log2_w, log2_h, bit_depth, c ? 128 : 128);

    oapv_mcpy(recon, core->coef, sizeof(s16) * OAPV_BLK_D);
    ctx->fn_dquant[0](recon, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
    ctx->fn_itx[0](recon, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    best_cost = (int)ctx->fn_ssd[0](blk_w, blk_h, org, recon, blk_w, blk_w);

    double lambda = (0.57 * pow(2.0, (core->qp[c] - 12) / 3.0));
    int rate_org = oapve_vlc_get_coef_rate(core, core->coef, c);
    best_cost += lambda * rate_org;

    for(int itr = 0; itr < 3; itr++) {
        int list_cnt = 0;
        oapve_coef_info_t coef_list[OAPV_FULL_RDO_MAX_CAND] = { 0 };

        for(int j = 0; j < OAPV_BLK_D; j++) {
            s16 org_coef = best_coeff[scanp[j]];
            int adj_rng = org_coef == 0 ? 3 : 2;

            oapve_coef_info_t coef_cur;
            coef_cur.cost = best_cost;
            for(int i = 1; i < adj_rng; i++) {
                s16 test_diff = org_coef == 0 ? (i == 1 ? 1 : -1) : (org_coef > 0 ? i : -i);
                s16 test_coef = org_coef + test_diff;

                oapv_mcpy(coeff, best_coeff, sizeof(s16) * OAPV_BLK_D);
                coeff[scanp[j]] = test_coef;

                int test_rate = oapve_vlc_get_coef_rate(core, coeff, c);
                ctx->fn_dquant[0](coeff, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
                ctx->fn_itx[0](coeff, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
                double cost = (int)ctx->fn_ssd[0](blk_w, blk_h, org, coeff, blk_w, blk_w);
                cost += (lambda) * (test_rate);

                if(cost < coef_cur.cost) {
                    coef_cur.cost = cost;
                    coef_cur.coef_org = org_coef;
                    coef_cur.coef_test = test_coef;
                    coef_cur.coef_pos = scanp[j];
                }
            }

            if(coef_cur.cost < best_cost) {
                add_coef_list(coef_list, coef_cur, &list_cnt);
            }
        }

        for(int j = 1; j < (1 << list_cnt) && j < (1 << OAPV_FULL_RDO_MAX_CAND); j++) {
            oapv_mcpy(coeff, best_coeff, sizeof(s16) * OAPV_BLK_D);
            for(int i = 0; i < OAPV_FULL_RDO_MAX_CAND && i < list_cnt; i++) {
                coeff[coef_list[i].coef_pos] = ((j >> i) & 1) ? coef_list[i].coef_test : coef_list[i].coef_org;
            }
            oapv_mcpy(recon, coeff, sizeof(s16) * OAPV_BLK_D);
            ctx->fn_dquant[0](recon, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
            ctx->fn_itx[0](recon, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
            double cost = (int)ctx->fn_ssd[0](blk_w, blk_h, org, recon, blk_w, blk_w);
            int test_rate = oapve_vlc_get_coef_rate(core, coeff, c);
            cost += (lambda) * (test_rate);
            if(cost < best_cost) {
                best_cost = cost;
                oapv_mcpy(best_coeff, coeff, sizeof(s16) * OAPV_BLK_D);
            }
        }
    }

    if(ctx->imgb_r) {
        oapv_mcpy(best_recon, best_coeff, sizeof(s16) * OAPV_BLK_D);
        ctx->fn_dquant[0](best_recon, core->q_mat_dec[c], log2_w, log2_h, core->dq_shift[c]);
        ctx->fn_itx[0](best_recon, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    }

    core->dc_diff = best_coeff[0] - core->prev_dc[c];
    core->prev_dc[c] = best_coeff[0];

    return best_cost;
}

static void enc_flush(oapve_ctx_t *ctx)
{
    // Release thread pool controller and created threads
    if(ctx->threads >= 1) {
        if(ctx->tpool) {
            // thread controller instance is present
            // terminate the created thread
            for(int i = 0; i < ctx->threads; i++) {
                if(ctx->thread_id[i]) {
                    // valid thread instance
                    ctx->tpool->release(&ctx->thread_id[i]);
                }
            }
            // dinitialize the tc
            oapv_tpool_deinit(ctx->tpool);
            oapv_mfree_fast(ctx->tpool);
            ctx->tpool = NULL;
        }
    }

    if(ctx->sync_obj != NULL) {
        oapv_tpool_sync_obj_delete(&ctx->sync_obj);
    }
    for(int i = 0; i < ctx->threads; i++) {
        enc_core_free(ctx->core[i]);
        ctx->core[i] = NULL;
    }

    if(ctx->tile != NULL && ctx->tile[0].bs_buf != NULL) {
        oapv_mfree_fast(ctx->tile[0].bs_buf);
    }

    // Free dynamically allocated tile array
    if(ctx->tile != NULL) {
        oapv_mfree_fast(ctx->tile);
        ctx->tile = NULL;
    }

    // Free frame header tile_size array
    if(ctx->fh.tile_size != NULL) {
        oapv_mfree_fast(ctx->fh.tile_size);
        ctx->fh.tile_size = NULL;
    }
}

static int enc_ready(oapve_ctx_t *ctx)
{
    oapve_core_t *core = NULL;
    int           ret = OAPV_OK;
    oapv_assert(ctx->core[0] == NULL);

    // Allocate tile array for maximum possible tiles
    if(ctx->tile == NULL) {
        ctx->tile = (oapve_tile_t *)oapv_malloc_fast(OAPV_MAX_TILES * sizeof(oapve_tile_t));
        oapv_assert_gv(ctx->tile != NULL, ret, OAPV_ERR_OUT_OF_MEMORY, ERR);
        oapv_mset_x64a(ctx->tile, 0, OAPV_MAX_TILES * sizeof(oapve_tile_t));
    }

    // Allocate frame header tile_size array
    if(ctx->fh.tile_size == NULL) {
        ctx->fh.tile_size = (u32 *)oapv_malloc_fast(OAPV_MAX_TILES * sizeof(u32));
        oapv_assert_gv(ctx->fh.tile_size != NULL, ret, OAPV_ERR_OUT_OF_MEMORY, ERR);
        oapv_mset_x64a(ctx->fh.tile_size, 0, OAPV_MAX_TILES * sizeof(u32));
    }

    ret = oapve_param_update(ctx);
    oapv_assert_g(ret == OAPV_OK, ERR);

    for(int i = 0; i < ctx->threads; i++) {
        core = enc_core_alloc();
        oapv_assert_gv(core != NULL, ret, OAPV_ERR_OUT_OF_MEMORY, ERR);
        ctx->core[i] = core;
    }

    // initialize the threads to NULL
    for(int i = 0; i < OAPV_MAX_THREADS; i++) {
        ctx->thread_id[i] = 0;
    }

    // get the context synchronization handle
    ctx->sync_obj = oapv_tpool_sync_obj_create();
    oapv_assert_gv(ctx->sync_obj != NULL, ret, OAPV_ERR_UNKNOWN, ERR);

    if(ctx->threads >= 1) {
        ctx->tpool = oapv_malloc(sizeof(oapv_tpool_t));
        oapv_tpool_init(ctx->tpool, ctx->threads);
        for(int i = 0; i < ctx->threads; i++) {
            ctx->thread_id[i] = ctx->tpool->create(ctx->tpool, i);
            oapv_assert_gv(ctx->thread_id[i] != NULL, ret, OAPV_ERR_UNKNOWN, ERR);
        }
    }

    // Initialize all allocated tiles
    for(int i = 0; i < OAPV_MAX_TILES; i++) {
        ctx->tile[i].stat = ENC_TILE_STAT_NOT_ENCODED;
    }
    ctx->tile[0].bs_buf = (u8 *)oapv_malloc(ctx->cdesc.max_bs_buf_size);
    oapv_assert_gv(ctx->tile[0].bs_buf, ret, OAPV_ERR_UNKNOWN, ERR);

    ctx->rc_param.alpha = OAPV_RC_ALPHA;
    ctx->rc_param.beta = OAPV_RC_BETA;
    ctx->au_bs_fmt = OAPV_CFG_VAL_AU_BS_FMT_RBAU; // default: enable raw bitstream format

    return OAPV_OK;
ERR:
    enc_flush(ctx);

    return ret;
}

static int enc_tile_comp(oapv_bs_t *bs, oapve_tile_t *tile, oapve_ctx_t *ctx, oapve_core_t *core, int c, int s_org, void *org, int s_rec, void *rec)
{
    int  mb_h, mb_w, mb_y, mb_x, blk_x, blk_y;
    s16 *o16 = NULL, *r16 = NULL;

    u8  *bs_cur = oapv_bsw_sink(bs);
    oapv_assert_rv(bsw_is_align8(bs), OAPV_ERR_MALFORMED_BITSTREAM);

    mb_w = OAPV_MB_W >> ctx->comp_sft[c][0];
    mb_h = OAPV_MB_H >> ctx->comp_sft[c][1];

    int tile_le = tile->x >> ctx->comp_sft[c][0];
    int tile_ri = (tile->w >> ctx->comp_sft[c][0]) + tile_le;
    int tile_to = tile->y >> ctx->comp_sft[c][1];
    int tile_bo = (tile->h >> ctx->comp_sft[c][1]) + tile_to;

    for(mb_y = tile_to; mb_y < tile_bo; mb_y += mb_h) {
        for(mb_x = tile_le; mb_x < tile_ri; mb_x += mb_w) {
            for(blk_y = mb_y; blk_y < (mb_y + mb_h); blk_y += OAPV_BLK_H) {
                for(blk_x = mb_x; blk_x < (mb_x + mb_w); blk_x += OAPV_BLK_W) {
                    o16 = (s16 *)((u8 *)org + blk_y * s_org) + blk_x;
                    ctx->fn_imgb_to_blk[c](o16, OAPV_BLK_W, OAPV_BLK_H, s_org, blk_x, (OAPV_BLK_W << 1), core->coef, ctx->bit_depth);

                    ctx->fn_enc_blk(ctx, core, OAPV_LOG2_BLK_W, OAPV_LOG2_BLK_H, c);
                    oapve_vlc_dc_coef(bs, core->dc_diff, &core->kparam_dc[c]);
                    oapve_vlc_ac_coef(bs, core->coef, &core->kparam_ac[c]);
                    DUMP_COEF(core->coef, OAPV_BLK_D, blk_x, blk_y, c);

                    if(rec != NULL) {
                        r16 = (s16 *)((u8 *)rec + blk_y * s_rec) + blk_x;
                        ctx->fn_blk_to_imgb[c](core->coef_rec, OAPV_BLK_W, OAPV_BLK_H, (OAPV_BLK_W << 1), blk_x, s_rec, r16, ctx->bit_depth);
                    }
                }
            }
        }
    }

    /* byte align */
    while(!bsw_is_align8(bs)) {
        oapv_bsw_write1(bs, 0);
    }

    /* de-init BSW */
    oapv_bsw_deinit(bs);

    return (int)(bs->cur - bs_cur);
}

static int enc_tile(oapve_ctx_t *ctx, oapve_core_t *core, oapve_tile_t *tile)
{
    oapv_bs_t bs;
    oapv_bsw_init(&bs, tile->bs_buf, tile->bs_buf_max, NULL);

    int qp = 0;
    if(ctx->param->rc_type != OAPV_RC_CQP) {
        oapve_rc_get_qp(ctx, tile, ctx->qp[Y_C], &qp);
    }
    else {
        qp = ctx->qp[Y_C];
    }

    tile->tile_size = 0;
    DUMP_SAVE(0);
    oapve_vlc_tile_size(&bs, tile->tile_size);
    oapve_set_tile_header(ctx, &tile->th, core->tile_idx, qp);
    oapve_vlc_tile_header(ctx, &bs, &tile->th);

    for(int c = 0; c < ctx->num_comp; c++) {
        int cnt = 0;
        core->qp[c] = tile->th.tile_qp[c];
        int qscale = oapv_quant_scale[core->qp[c] % 6];
        s32 scale_multiply_16 = (s32)(qscale << 4); // 15bit + 4bit
        for(int y = 0; y < OAPV_BLK_H; y++) {
            for(int x = 0; x < OAPV_BLK_W; x++) {
                core->q_mat_enc[c][cnt++] = scale_multiply_16 / ctx->fh.q_matrix[c][y][x];
            }
        }

        if(ctx->imgb_r || ctx->param->preset >= OAPV_PRESET_MEDIUM) {
            core->dq_shift[c] = ctx->bit_depth - 2 - (core->qp[c] / 6);

            int cnt = 0;
            u8 dq_scale = oapv_tbl_dq_scale[core->qp[c] % 6];
            for(int y = 0; y < OAPV_BLK_H; y++) {
                for(int x = 0; x < OAPV_BLK_W; x++) {
                    core->q_mat_dec[c][cnt++] = dq_scale * ctx->fh.q_matrix[c][y][x];
                }
            }
        }

        if(ctx->param->preset == OAPV_PRESET_MEDIUM || ctx->param->preset == OAPV_PRESET_SLOW) {
            oapve_init_rdoq(core, ctx->bit_depth, c);
        }
    }

    for(int c = 0; c < ctx->num_comp; c++) {
        core->kparam_dc[c] = OAPV_KPARAM_DC_MAX;
        core->kparam_ac[c] = OAPV_KPARAM_AC_MIN;
        core->prev_dc[c] = 0;

        int  tc, s_org, s_rec;
        s16 *org, *rec;

        if(OAPV_CS_GET_FORMAT(ctx->imgb_i->cs) == OAPV_CF_PLANAR2) {
            tc = c > 0 ? 1 : 0;
            org = ctx->imgb_i->a[tc];
            org += (c > 1) ? 1 : 0;
            s_org = ctx->imgb_i->s[tc];

            if(ctx->imgb_r) {
                rec = ctx->imgb_r->a[tc];
                rec += (c > 1) ? 1 : 0;
                s_rec = ctx->imgb_i->s[tc];
            }
            else {
                rec = NULL;
                s_rec = 0;
            }
        }
        else {
            org = ctx->imgb_i->a[c];
            s_org = ctx->imgb_i->s[c];
            if(ctx->imgb_r) {
                rec = ctx->imgb_r->a[c];
                s_rec = ctx->imgb_i->s[c];
            }
            else {
                rec = NULL;
                s_rec = 0;
            }
        }

        tile->th.tile_data_size[c] = enc_tile_comp(&bs, tile, ctx, core, c, s_org, org, s_rec, rec);
    }

    u32 bs_size = (int)(bs.cur - bs.beg);
    if(bs_size > tile->bs_buf_max) {
        return OAPV_ERR_OUT_OF_BS_BUF;
    }
    tile->bs_size = bs_size;

    oapv_bs_t bs_th;
    oapv_bsw_init(&bs_th, tile->bs_buf, tile->bs_size, NULL);
    tile->tile_size = bs_size - OAPV_TILE_SIZE_LEN;

    DUMP_SAVE(1);
    DUMP_LOAD(0);
    oapve_vlc_tile_size(&bs_th, tile->tile_size);
    oapve_vlc_tile_header(ctx, &bs_th, &tile->th);
    DUMP_LOAD(1);
    oapv_bsw_deinit(&bs_th);
    return OAPV_OK;
}

static int enc_thread_tile(void *arg)
{
    oapve_core_t *core = (oapve_core_t *)arg;
    oapve_ctx_t  *ctx = core->ctx;
    oapve_tile_t *tile = ctx->tile;
    int           ret = OAPV_OK, i;

    while(1) {
        // find not encoded tile
        oapv_tpool_enter_cs(ctx->sync_obj);
        for(i = 0; i < ctx->num_tiles; i++) {
            if(tile[i].stat == ENC_TILE_STAT_NOT_ENCODED) {
                tile[i].stat = ENC_TILE_STAT_ON_ENCODING;
                core->tile_idx = i;
                break;
            }
        }
        oapv_tpool_leave_cs(ctx->sync_obj);
        if(i == ctx->num_tiles) {
            break;
        }

        ret = enc_tile(ctx, core, &tile[core->tile_idx]);
        oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

        oapv_tpool_enter_cs(ctx->sync_obj);
        tile[core->tile_idx].stat = ENC_TILE_STAT_ENCODED;
        oapv_tpool_leave_cs(ctx->sync_obj);
    }
ERR:
    return ret;
}

static int enc_profile_spec[][5] = {
    // {profile-idc, cfi-min, cfi-max, bit-depth-min, bit-depth-max}
    {OAPV_PROFILE_422_10, 2, 2, 10, 10},
    {OAPV_PROFILE_422_12, 2, 2, 10, 12},
    {OAPV_PROFILE_444_10, 2, 3, 10, 10},
    {OAPV_PROFILE_444_12, 2, 3, 10, 12},
    {OAPV_PROFILE_4444_10, 2, 4, 10, 10},
    {OAPV_PROFILE_4444_12, 2, 4, 10, 12},
    {OAPV_PROFILE_400_10, 0, 0, 10, 10},
    {0, 0, 0, 0, 0} // termination
};

static int enc_check_profile(int profile_idc, int cfi, int bit_depth)
{
    int idx = 0;
    while(enc_profile_spec[idx][0] != 0) {
        if(profile_idc == enc_profile_spec[idx][0]) {
            if(cfi >= enc_profile_spec[idx][1] && cfi <= enc_profile_spec[idx][2]) { // check cfi
                if(bit_depth >= enc_profile_spec[idx][3] && bit_depth <= enc_profile_spec[idx][4]) { // check bit-depth
                    return OAPV_OK;
                }
            }
        }
        idx++;
    }
    return OAPV_ERR_INVALID_PROFILE;
}

static int enc_frm_prepare(oapve_ctx_t *ctx, oapve_param_t *param, oapv_imgb_t *imgb_i, oapv_imgb_t *imgb_r)
{
    int i, ret;

    // check basic parameters
    oapv_assert_rv(param->w == imgb_i->w[0], OAPV_ERR_INVALID_WIDTH);
    oapv_assert_rv(param->h == imgb_i->h[0], OAPV_ERR_INVALID_HEIGHT);
    oapv_assert_rv((param->qp >= MIN_QUANT && param->qp <= MAX_QUANT(10)) || param->qp == OAPVE_PARAM_QP_AUTO, OAPV_ERR_INVALID_QP);

    // check width restriction for 422
    if(OAPV_CS_GET_FORMAT(imgb_i->cs) == OAPV_CF_YCBCR422 && imgb_i->w[0] & 0x1) {
        return OAPV_ERR_INVALID_WIDTH; // odd width is spec-out in YCbCr422
    }

    // set functions related to preset
    if(param->preset == OAPV_PRESET_PLACEBO) {
        ctx->fn_enc_blk = enc_block_rdo_placebo;
    }
    else if(param->preset == OAPV_PRESET_SLOW) {
        ctx->fn_enc_blk = enc_block_rdo_slow;
    }
    else if(param->preset == OAPV_PRESET_MEDIUM) {
        ctx->fn_enc_blk = enc_block_rdo_medium;
    }
    else {
        ctx->fn_enc_blk = enc_block;
    }
    // set dimensions
    ctx->w = oapv_div_round_up(param->w, OAPV_MB_W) * OAPV_MB_W;
    ctx->h = oapv_div_round_up(param->h, OAPV_MB_H) * OAPV_MB_H;

    // set QP values
    ctx->qp_offset[Y_C] = 0;
    ctx->qp_offset[U_C] = param->qp_offset_c1;
    ctx->qp_offset[V_C] = param->qp_offset_c2;
    ctx->qp_offset[X_C] = param->qp_offset_c3;

    for(i = 0; i < N_C; i++) {
        ctx->qp[i] = oapv_clip3(MIN_QUANT, MAX_QUANT(10), param->qp + ctx->qp_offset[i]);
    }
    // color information
    ctx->cfi = color_format_to_chroma_format_idc(OAPV_CS_GET_FORMAT(imgb_i->cs));
    ctx->bit_depth = OAPV_CS_GET_BIT_DEPTH(imgb_i->cs);
    ctx->num_comp = get_num_comp(ctx->cfi);

    // check whether input frame type is suitable to profile definition
    ret = enc_check_profile(param->profile_idc, ctx->cfi, ctx->bit_depth);
    oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

    // shift parameter for each color component
    ctx->comp_sft[Y_C][0] = 0;
    ctx->comp_sft[Y_C][1] = 0;
    for(i = 1; i < ctx->num_comp; i++) {
        ctx->comp_sft[i][0] = get_chroma_sft_w(ctx->cfi);
        ctx->comp_sft[i][1] = get_chroma_sft_h(ctx->cfi);
    }

    if(OAPV_CS_GET_FORMAT(imgb_i->cs) == OAPV_CF_PLANAR2) {
        ctx->fn_imgb_to_blk_rc = imgb_to_blk_p21x;

        ctx->fn_imgb_to_blk[Y_C] = imgb_to_blk_p21x_y;
        ctx->fn_imgb_to_blk[U_C] = imgb_to_blk_p21x_uv;
        ctx->fn_imgb_to_blk[V_C] = imgb_to_blk_p21x_uv;

        ctx->fn_blk_to_imgb[Y_C] = blk_to_imgb_p21x_y;
        ctx->fn_blk_to_imgb[U_C] = blk_to_imgb_p21x_uv;
        ctx->fn_blk_to_imgb[V_C] = blk_to_imgb_p21x_uv;
        ctx->fn_imgb_pad = imgb_pad_p210;
    }
    else {
        ctx->fn_imgb_to_blk_rc = imgb_to_blk;
        for(int i = 0; i < ctx->num_comp; i++) {
            ctx->fn_imgb_to_blk[i] = imgb_to_blk_16;
            ctx->fn_blk_to_imgb[i] = blk_to_imgb_16;
        }
        ctx->fn_imgb_pad = imgb_pad;
    }
    // padding input picture, if needs
    ctx->fn_imgb_pad(imgb_i, ctx->w, ctx->h, ctx->comp_sft);

    // calculate tile info
    ret = enc_set_tile_info(ctx->tile, ctx->w, ctx->h, param->tile_w, param->tile_h, &ctx->num_tile_cols, &ctx->num_tile_rows, &ctx->num_tiles);
    oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

    // set bitstream buffer for each tile
    int buf_size = ctx->cdesc.max_bs_buf_size / ctx->num_tiles;
    ctx->tile[0].bs_buf_max = buf_size;
    for(i = 1; i < ctx->num_tiles; i++) {
        ctx->tile[i].bs_buf = ctx->tile[i - 1].bs_buf + buf_size;
        ctx->tile[i].bs_buf_max = buf_size;
    }
    // set cores
    for(i = 0; i < ctx->threads; i++) {
        ctx->core[i]->ctx = ctx;
        ctx->core[i]->thread_idx = i;
    }
    // recontruction picture
    if(imgb_r != NULL) {
        for(int c = 0; c < ctx->num_comp; c++) {
            imgb_r->w[c] = imgb_i->w[c];
            imgb_r->h[c] = imgb_i->h[c];
            imgb_r->x[c] = imgb_i->x[c];
            imgb_r->y[c] = imgb_i->y[c];
        }
        ctx->imgb_r = imgb_r;
        imgb_addref(ctx->imgb_r);
    }
    for(i = 0; i < ctx->num_tiles; i++) {
        ctx->tile[i].stat = ENC_TILE_STAT_NOT_ENCODED;
    }

    ctx->param = param;
    ctx->imgb_i = imgb_i;
    imgb_addref(ctx->imgb_i); // increase reference count of input frame
    return OAPV_OK;
}

static int enc_frm_finish(oapve_ctx_t *ctx, oapve_stat_t *stat)
{
    imgb_release(ctx->imgb_i);
    if(ctx->imgb_r) {
        imgb_release(ctx->imgb_r);
        ctx->imgb_r = NULL;
    }
    return OAPV_OK;
}

static int enc_frame(oapve_ctx_t *ctx, oapv_bs_t *bs)
{
    int        ret = OAPV_OK;

    oapv_bs_t  bs_fh;
    oapv_mcpy(&bs_fh, bs, sizeof(oapv_bs_t));

    /* write frame header */
    oapve_set_frame_header(ctx, &ctx->fh);
    oapve_vlc_frame_header(bs, ctx, &ctx->fh);

    /* de-init BSW */
    oapv_bsw_deinit(bs);

    /* rc init */
    u64 cost_sum = 0;
    if(ctx->param->rc_type != OAPV_RC_CQP) {
        oapve_rc_get_tile_cost_thread(ctx, &cost_sum);

        double bits_pic = ((double)ctx->param->bitrate * 1000) / ((double)ctx->param->fps_num / ctx->param->fps_den);
        for(int i = 0; i < ctx->num_tiles; i++) {
            ctx->tile[i].rc.target_bits_left = bits_pic * ctx->tile[i].rc.cost / cost_sum;
            ctx->tile[i].rc.target_bits = ctx->tile[i].rc.target_bits_left;
        }

        ctx->rc_param.lambda = oapve_rc_estimate_pic_lambda(ctx, cost_sum);
        if (ctx->param->qp == OAPVE_PARAM_QP_AUTO || ctx->rc_param.is_updated != 0) {
            ctx->rc_param.qp = oapve_rc_estimate_pic_qp(ctx->rc_param.lambda);
        }
        else {
            ctx->rc_param.qp = ctx->param->qp;
        }

        for(int c = 0; c < ctx->num_comp; c++) {
            ctx->qp[c] = oapv_clip3(MIN_QUANT, MAX_QUANT(10), ctx->rc_param.qp + ctx->qp_offset[c]);
        }
    }

    oapv_tpool_t *tpool = ctx->tpool;
    int           res, tidx = 0, thread_num1 = 0;
    int           parallel_task = (ctx->threads > ctx->num_tiles) ? ctx->num_tiles : ctx->threads;

    /* encode tiles ************************************/
    for(tidx = 0; tidx < (parallel_task - 1); tidx++) {
        tpool->run(ctx->thread_id[tidx], enc_thread_tile,
                   (void *)ctx->core[tidx]);
    }
    ret = enc_thread_tile((void *)ctx->core[tidx]);
    oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

    for(thread_num1 = 0; thread_num1 < parallel_task - 1; thread_num1++) {
        res = tpool->join(ctx->thread_id[thread_num1], &ret);
        oapv_assert_gv(res == TPOOL_SUCCESS, ret, OAPV_ERR_FAILED_SYSCALL, ERR);
        oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);
    }
    /****************************************************/

    for(int i = 0; i < ctx->num_tiles; i++) {
        oapv_mcpy(bs->cur, ctx->tile[i].bs_buf, ctx->tile[i].bs_size);
        bs->cur = bs->cur + ctx->tile[i].bs_size;
        ctx->fh.tile_size[i] = ctx->tile[i].bs_size - OAPV_TILE_SIZE_LEN;
    }

    /* rewrite frame header */
    if(ctx->fh.tile_size_present_in_fh_flag) {
        oapve_vlc_frame_header(&bs_fh, ctx, &ctx->fh);
        /* de-init BSW */
        oapv_bsw_sink(&bs_fh);
    }
    if(ctx->param->rc_type != 0) {
        oapve_rc_update_after_pic(ctx, cost_sum);
    }
    return ret;

ERR:
    return ret;
}

static int enc_platform_init(oapve_ctx_t *ctx)
{
    // default settings
    ctx->fn_sad = oapv_tbl_fn_sad_16b;
    ctx->fn_ssd = oapv_tbl_fn_ssd_16b;
    ctx->fn_diff = oapv_tbl_fn_diff_16b;
    ctx->fn_itx_part = oapv_tbl_fn_itx_part;
    ctx->fn_itx = oapv_tbl_fn_itx;
    ctx->fn_itx_adj = oapv_tbl_fn_itx_adj;
    ctx->fn_txb = oapv_tbl_fn_tx;
    ctx->fn_quant = oapv_tbl_fn_quant;
    ctx->fn_dquant = oapv_tbl_fn_dquant;
    ctx->fn_had8x8 = oapv_dc_removed_had8x8;
#if X86_SSE
    int check_cpu, support_sse, support_avx2;

    check_cpu = oapv_check_cpu_info_x86();
    support_sse = (check_cpu >> 0) & 1;
    support_avx2 = (check_cpu >> 2) & 1;

    if(support_avx2) {
        ctx->fn_sad = oapv_tbl_fn_sad_16b_avx;
        ctx->fn_ssd = oapv_tbl_fn_ssd_16b_avx;
        ctx->fn_diff = oapv_tbl_fn_diff_16b_avx;
        ctx->fn_itx_part = oapv_tbl_fn_itx_part_avx;
        ctx->fn_itx = oapv_tbl_fn_itx_avx;
        ctx->fn_itx_adj = oapv_tbl_fn_itx_adj_avx;
        ctx->fn_txb = oapv_tbl_fn_txb_avx;
        ctx->fn_quant = oapv_tbl_fn_quant_avx;
        ctx->fn_dquant = oapv_tbl_fn_dquant_avx;
        ctx->fn_had8x8 = oapv_dc_removed_had8x8_sse;
    }
    else if(support_sse) {
        ctx->fn_ssd = oapv_tbl_fn_ssd_16b_sse;
        ctx->fn_had8x8 = oapv_dc_removed_had8x8_sse;
    }
#elif ARM_NEON
    ctx->fn_sad = oapv_tbl_fn_sad_16b_neon;
    ctx->fn_ssd = oapv_tbl_fn_ssd_16b_neon;
    ctx->fn_diff = oapv_tbl_fn_diff_16b_neon;
    ctx->fn_itx = oapv_tbl_fn_itx_neon;
    ctx->fn_txb = oapv_tbl_fn_txb_neon;
    ctx->fn_quant = oapv_tbl_fn_quant_neon;
    ctx->fn_had8x8 = oapv_dc_removed_had8x8;
#endif
    return OAPV_OK;
}

oapve_t oapve_create(oapve_cdesc_t *cdesc, int *err)
{
    oapve_ctx_t *ctx;
    int          ret;

    DUMP_CREATE(1);
    /* memory allocation for ctx and core structure */
    ctx = (oapve_ctx_t *)enc_ctx_alloc();
    if(ctx != NULL) {
        oapv_mcpy(&ctx->cdesc, cdesc, sizeof(oapve_cdesc_t));
        ret = enc_platform_init(ctx);
        oapv_assert_g(ret == OAPV_OK, ERR);

        ret = enc_ready(ctx);
        oapv_assert_g(ret == OAPV_OK, ERR);

        /* set default value for ctx */
        ctx->magic = OAPVE_MAGIC_CODE;
        ctx->id = (oapve_t)ctx;
        if(err) {
            *err = OAPV_OK;
        }
        return (ctx->id);
    }
    else {
        ret = OAPV_ERR;
    }
ERR:
    if(ctx) {
        enc_ctx_free(ctx);
    }
    if(err) {
        *err = ret;
    }
    return NULL;
}

void oapve_delete(oapve_t eid)
{
    oapve_ctx_t *ctx;

    ctx = enc_id_to_ctx(eid);
    oapv_assert_r(ctx);

    DUMP_DELETE();
    enc_flush(ctx);
    enc_ctx_free(ctx);
}

int oapve_encode(oapve_t eid, oapv_frms_t *ifrms, oapvm_t mid, oapv_bitb_t *bitb, oapve_stat_t *stat, oapv_frms_t *rfrms)
{
    oapv_bs_t    bsw;
    oapve_ctx_t *ctx;
    oapv_frm_t  *frm;
    oapv_bs_t   *bs, bs_pbu_beg;
    int          i, ret;
    u8          *bs_pos_pbu_beg, *bs_pos_au_beg;

    ctx = enc_id_to_ctx(eid);
    oapv_assert_rv(ctx != NULL && bitb->addr && bitb->bsize > 0, OAPV_ERR_INVALID_ARGUMENT);

    bs = &bsw;

    oapv_bsw_init(bs, bitb->addr, bitb->bsize, NULL);
    oapv_mset(stat, 0, sizeof(oapve_stat_t));

    bs_pos_au_beg = oapv_bsw_sink(bs);

    if(ctx->au_bs_fmt == OAPV_CFG_VAL_AU_BS_FMT_RBAU) {
        oapv_bsw_write(bs, 0, 32); // raw bitstream byte size (skip)
    }
    oapv_bsw_write(bs, 0x61507631, 32); // signature ('aPv1')

    for(i = 0; i < ifrms->num_frms; i++) {
        // prepare for encoding a frame
        frm = &ifrms->frm[i];
        ret = enc_frm_prepare(ctx, &ctx->cdesc.param[i], frm->imgb, (rfrms != NULL) ? rfrms->frm[i].imgb : NULL);
        oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

        // write headers
        bs_pos_pbu_beg = oapv_bsw_sink(bs);            /* store pbu pos to calculate size */
        oapv_mcpy(&bs_pbu_beg, bs, sizeof(oapv_bs_t)); /* store pbu pos of ai to re-write */

        DUMP_SAVE(0);
        oapve_vlc_pbu_size(bs, 0);
        oapve_vlc_pbu_header(bs, frm->pbu_type, frm->group_id);
        // encode a frame
        ret = enc_frame(ctx, bs);
        oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

        // rewrite pbu_size
        int pbu_size = ((u8 *)oapv_bsw_sink(bs)) - bs_pos_pbu_beg - 4;
        DUMP_SAVE(1);
        DUMP_LOAD(0);
        oapve_vlc_pbu_size(&bs_pbu_beg, pbu_size);
        DUMP_LOAD(1);

        stat->frm_size[i] = pbu_size + 4 /* PUB size length*/;
        fh_to_finfo(&ctx->fh, frm->pbu_type, frm->group_id, &stat->aui.frm_info[i]);

        // add frame hash value of reconstructed frame into metadata list
        if(ctx->use_frm_hash) {
            if(frm->pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
               frm->pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME) {
                oapv_assert_rv(mid != NULL, OAPV_ERR_INVALID_ARGUMENT);
                ret = oapv_set_md5_pld(mid, frm->group_id, ctx->imgb_r);
                oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);
            }
        }

        // finishing of encoding a frame
        ret = enc_frm_finish(ctx, stat);
        oapv_assert_rv(ret == OAPV_OK, ret);
    }
    stat->aui.num_frms = ifrms->num_frms;

    // encoding metadata
    oapvm_ctx_t *md_list = mid;
    if(md_list != NULL) {
        int num_md = md_list->num;
        for(i = 0; i < num_md; i++) {
            int group_id = md_list->md_arr[i].group_id;
            bs_pos_pbu_beg = oapv_bsw_sink(bs);            /* store pbu pos to calculate size */
            oapv_mcpy(&bs_pbu_beg, bs, sizeof(oapv_bs_t)); /* store pbu pos of ai to re-write */
            DUMP_SAVE(0);

            oapve_vlc_pbu_size(bs, 0);
            oapve_vlc_pbu_header(bs, OAPV_PBU_TYPE_METADATA, group_id);
            oapve_vlc_metadata(&md_list->md_arr[i], bs);

            // rewrite pbu_size
            int pbu_size = ((u8 *)oapv_bsw_sink(bs)) - bs_pos_pbu_beg - 4;
            DUMP_SAVE(1);
            DUMP_LOAD(0);
            oapve_vlc_pbu_size(&bs_pbu_beg, pbu_size);
            DUMP_LOAD(1);
        }
    }

    if(ctx->au_bs_fmt == OAPV_CFG_VAL_AU_BS_FMT_RBAU) {
        u32 au_size = (u32)((u8 *)oapv_bsw_sink(bs) - bs_pos_au_beg) - 4;
        oapv_bsw_write_direct(bs_pos_au_beg, au_size, 32);
    }

    oapv_bsw_deinit(bs); /* de-init BSW */
    stat->write = bsw_get_write_byte(bs);

    return OAPV_OK;
}

int oapve_config(oapve_t eid, int cfg, void *buf, int *size)
{
    oapve_ctx_t *ctx;
    int          t0;

    ctx = enc_id_to_ctx(eid);
    oapv_assert_rv(ctx, OAPV_ERR_INVALID_ARGUMENT);

    switch(cfg) {
    /* set config **********************************************************/
    case OAPV_CFG_SET_QP:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        t0 = *((int *)buf);
        oapv_assert_rv(t0 >= MIN_QUANT && t0 <= MAX_QUANT(10),
                       OAPV_ERR_INVALID_ARGUMENT);
        ctx->param->qp = t0;
        break;
    case OAPV_CFG_SET_FPS_NUM:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        t0 = *((int *)buf);
        oapv_assert_rv(t0 > 0, OAPV_ERR_INVALID_ARGUMENT);
        ctx->param->fps_num = t0;
        break;
    case OAPV_CFG_SET_FPS_DEN:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        t0 = *((int *)buf);
        oapv_assert_rv(t0 > 0, OAPV_ERR_INVALID_ARGUMENT);
        ctx->param->fps_den = t0;
        break;
    case OAPV_CFG_SET_BPS:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        t0 = *((int *)buf);
        oapv_assert_rv(t0 > 0, OAPV_ERR_INVALID_ARGUMENT);
        ctx->param->bitrate = t0;
        break;
    case OAPV_CFG_SET_USE_FRM_HASH:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        ctx->use_frm_hash = (*((int *)buf)) ? 1 : 0;
        break;
    case OAPV_CFG_SET_AU_BS_FMT:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        t0 = *((int *)buf);
        oapv_assert_rv(t0 == OAPV_CFG_VAL_AU_BS_FMT_RBAU || t0 == OAPV_CFG_VAL_AU_BS_FMT_NONE, OAPV_ERR_INVALID_ARGUMENT);
        ctx->au_bs_fmt = t0;
        break;
    /* get config *******************************************************/
    case OAPV_CFG_GET_QP:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->qp;
        break;
    case OAPV_CFG_GET_WIDTH:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->w;
        break;
    case OAPV_CFG_GET_HEIGHT:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->h;
        break;
    case OAPV_CFG_GET_FPS_NUM:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->fps_num;
        break;
    case OAPV_CFG_GET_FPS_DEN:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->fps_den;
        break;
    case OAPV_CFG_GET_BPS:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->param->bitrate;
        break;
    case OAPV_CFG_GET_AU_BS_FMT:
        oapv_assert_rv(*size == sizeof(int), OAPV_ERR_INVALID_ARGUMENT);
        *((int *)buf) = ctx->au_bs_fmt;
        break;
    default:
        oapv_trace("unknown config value (%d)\n", cfg);
        oapv_assert_rv(0, OAPV_ERR_UNSUPPORTED);
    }

    return OAPV_OK;
}

///////////////////////////////////////////////////////////////////////////////
// enc of encoder code
#endif // ENABLE_ENCODER
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// start of decoder code
#if ENABLE_DECODER
///////////////////////////////////////////////////////////////////////////////
static oapvd_ctx_t *dec_id_to_ctx(oapvd_t id)
{
    oapvd_ctx_t *ctx;
    oapv_assert_rv(id, NULL);
    ctx = (oapvd_ctx_t *)id;
    oapv_assert_rv(ctx->magic == OAPVD_MAGIC_CODE, NULL);
    return ctx;
}

static oapvd_ctx_t *dec_ctx_alloc(void)
{
    oapvd_ctx_t *ctx;

    ctx = (oapvd_ctx_t *)oapv_malloc_fast(sizeof(oapvd_ctx_t));

    oapv_assert_rv(ctx != NULL, NULL);
    oapv_mset_x64a(ctx, 0, sizeof(oapvd_ctx_t));

    return ctx;
}

static void dec_ctx_free(oapvd_ctx_t *ctx)
{
    oapv_mfree_fast(ctx);
}

static oapvd_core_t *dec_core_alloc(void)
{
    oapvd_core_t *core;

    core = (oapvd_core_t *)oapv_malloc_fast(sizeof(oapvd_core_t));

    oapv_assert_rv(core, NULL);
    oapv_mset_x64a(core, 0, sizeof(oapvd_core_t));

    return core;
}

static void dec_core_free(oapvd_core_t *core)
{
    oapv_mfree_fast(core);
}

static int dec_block(oapvd_ctx_t *ctx, oapvd_core_t *core, int log2_w, int log2_h, int c)
{
    int bit_depth = ctx->bit_depth;

    // DC prediction
    core->coef[0] = core->dc_diff + core->prev_dc[c];
    core->prev_dc[c] = core->coef[0];
    
    // Inverse quantization
    ctx->fn_dquant[0](core->coef, core->q_mat[c], log2_w, log2_h, core->dq_shift[c]);
    
    // Inverse transform
    ctx->fn_itx[0](core->coef, ITX_SHIFT1, ITX_SHIFT2(bit_depth), 1 << log2_w);
    
    return OAPV_OK;
}

static int dec_set_tile_info(oapvd_tile_t* tile, int w_pel, int h_pel, int tile_w, int tile_h, int num_tile_cols, int num_tiles)
{

    for (int i = 0; i < num_tiles; i++)
    {
        int tx = (i % (num_tile_cols)) * tile_w;
        int ty = (i / (num_tile_cols)) * tile_h;
        tile[i].x = tx;
        tile[i].y = ty;
        tile[i].w = tx + tile_w > w_pel ? w_pel - tx : tile_w;
        tile[i].h = ty + tile_h > h_pel ? h_pel - ty : tile_h;
    }
    return OAPV_OK;
}

// bs is assumed to be at the memory location of the first tile. For the tile-mip selection, we are making it optional.
static int dec_frm_prepare(oapvd_ctx_t *ctx, oapv_imgb_t *imgb)
{
    ctx->imgb = imgb;
    imgb_addref(ctx->imgb); // increase reference count

    ctx->bit_depth = ctx->fh.fi.bit_depth;
    ctx->cfi = ctx->fh.fi.chroma_format_idc;
    ctx->num_comp = get_num_comp(ctx->cfi);
    ctx->comp_sft[Y_C][0] = 0;
    ctx->comp_sft[Y_C][1] = 0;

    for(int c = 1; c < ctx->num_comp; c++) {
        ctx->comp_sft[c][0] = get_chroma_sft_w(color_format_to_chroma_format_idc(OAPV_CS_GET_FORMAT(imgb->cs)));
        ctx->comp_sft[c][1] = get_chroma_sft_h(color_format_to_chroma_format_idc(OAPV_CS_GET_FORMAT(imgb->cs)));
    }

    ctx->w = oapv_align_value(ctx->fh.fi.frame_width, OAPV_MB_W);
    ctx->h = oapv_align_value(ctx->fh.fi.frame_height, OAPV_MB_H);

    if(OAPV_CS_GET_FORMAT(imgb->cs) == OAPV_CF_PLANAR2) {
        ctx->fn_block_to_imgb[Y_C] = blk_to_imgb_p21x_y;
        ctx->fn_block_to_imgb[U_C] = blk_to_imgb_p21x_uv;
        ctx->fn_block_to_imgb[V_C] = blk_to_imgb_p21x_uv;
    }
    else {
        for(int c = 0; c < ctx->num_comp; c++) {
            ctx->fn_block_to_imgb[c] = blk_to_imgb_16;
        }
    }

    int tile_w = ctx->fh.tile_width_in_mbs * OAPV_MB_W;
    int tile_h = ctx->fh.tile_height_in_mbs * OAPV_MB_H;

    ctx->num_tile_cols = (ctx->w + (tile_w - 1)) / tile_w;
    ctx->num_tile_rows = (ctx->h + (tile_h - 1)) / tile_h;
    ctx->num_tiles = ctx->num_tile_cols * ctx->num_tile_rows;

    oapv_assert_rv((ctx->num_tile_cols <= OAPV_MAX_TILE_COLS) && (ctx->num_tile_rows <= OAPV_MAX_TILE_ROWS), OAPV_ERR_MALFORMED_BITSTREAM);
    oapv_assert_rv(ctx->num_tiles <= OAPV_MAX_TILES, OAPV_ERR_MALFORMED_BITSTREAM);

    // Allocate tile array if not already allocated
    if(ctx->tile == NULL) {
        ctx->tile = (oapvd_tile_t *)oapv_malloc_fast(OAPV_MAX_TILES * sizeof(oapvd_tile_t));
        oapv_assert_rv(ctx->tile != NULL, OAPV_ERR_OUT_OF_MEMORY);
        oapv_mset_x64a(ctx->tile, 0, OAPV_MAX_TILES * sizeof(oapvd_tile_t));
    }

    dec_set_tile_info(ctx->tile, ctx->w, ctx->h, tile_w, tile_h, ctx->num_tile_cols, ctx->num_tiles);

    for(int i = 0; i < ctx->num_tiles; i++) {
        ctx->tile[i].bs_beg = NULL;
    }
    ctx->tile[0].bs_beg = oapv_bsr_sink(&ctx->bs);

    for(int i = 0; i < ctx->num_tiles; i++) {
        ctx->tile[i].stat = DEC_TILE_STAT_NOT_DECODED;
    }

    return OAPV_OK;
}

static int dec_frm_finish(oapvd_ctx_t *ctx)
{
    oapv_mset(&ctx->bs, 0, sizeof(oapv_bs_t)); // clean data
    imgb_release(ctx->imgb);                   // decrease reference cnout
    ctx->imgb = NULL;
    return OAPV_OK;
}

static int dec_tile_comp(oapvd_tile_t *tile, oapvd_ctx_t *ctx, oapvd_core_t *core, oapv_bs_t *bs, int c, int s_dst, void *dst)
{
    int  mb_h, mb_w, mb_y, mb_x, blk_y, blk_x;
    int  le, ri, to, bo;
    int  ret;
    s16 *d16;


    mb_h = OAPV_MB_H >> ctx->comp_sft[c][1];
    mb_w = OAPV_MB_W >> ctx->comp_sft[c][0];

    le = tile->x >> ctx->comp_sft[c][0];        // left position of tile
    ri = (tile->w >> ctx->comp_sft[c][0]) + le; // right pixel position of tile
    to = tile->y >> ctx->comp_sft[c][1];        // top pixel position of tile
    bo = (tile->h >> ctx->comp_sft[c][1]) + to; // bottom pixel position of tile

    for(mb_y = to; mb_y < bo; mb_y += mb_h) {
        for(mb_x = le; mb_x < ri; mb_x += mb_w) {
            for(blk_y = mb_y; blk_y < (mb_y + mb_h); blk_y += OAPV_BLK_H) {
                for(blk_x = mb_x; blk_x < (mb_x + mb_w); blk_x += OAPV_BLK_W) {
                    // clear coefficient buffers in a macroblock
                    oapv_mset_x128(core->coef, 0, sizeof(s16)*OAPV_MB_D);

                    // parse DC coefficient
                    ret = oapvd_vlc_dc_coef(bs, &core->dc_diff, &core->kparam_dc[c]);
                    if(OAPV_FAILED(ret)) {
                        return ret;
                    }

                    // parse AC coefficient
                    ret = oapvd_vlc_ac_coef(bs, core->coef, &core->kparam_ac[c]);
                    if(OAPV_FAILED(ret)) {
                        return ret;
                    }
                    DUMP_COEF(core->coef, OAPV_BLK_D, blk_x, blk_y, c);

                    // decode a block
                    ret = dec_block(ctx, core, OAPV_LOG2_BLK_W, OAPV_LOG2_BLK_H, c);
                    oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

                    // copy decoded block to image buffer
                    d16 = (s16 *)((u8 *)dst + blk_y * s_dst) + blk_x;
                    ctx->fn_block_to_imgb[c](core->coef, OAPV_BLK_W, OAPV_BLK_H, (OAPV_BLK_W << 1), blk_x, s_dst, d16, ctx->bit_depth);
                }
            }
        }
    }

    /* byte align */
    oapv_bsr_align8(bs);
    /* check actual read size of 'tile()' is equal or smaller than 'tile_data_size' in tile header */
    oapv_assert_rv(BSR_GET_READ_BYTE(bs) <= tile->th.tile_data_size[c], OAPV_ERR_MALFORMED_BITSTREAM);

    return OAPV_OK;
}

static int dec_tile(oapvd_core_t *core, oapvd_tile_t *tile)
{
    int          ret, midx, x, y, c;
    oapvd_ctx_t *ctx = core->ctx;
    oapv_bs_t    bs; // bs for 'tile()' syntax


    oapv_bsr_init(&bs, tile->bs_beg + OAPV_TILE_SIZE_LEN, tile->data_size, NULL);
    ret = oapvd_vlc_tile_header(&bs, ctx, &tile->th);
    oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

    for(c = 0; c < ctx->num_comp; c++) {
        core->qp[c] = tile->th.tile_qp[c];
        u8 dq_scale = oapv_tbl_dq_scale[core->qp[c] % 6];
        core->dq_shift[c] = ctx->bit_depth - 2 - (core->qp[c] / 6);

        core->kparam_dc[c] = OAPV_KPARAM_DC_MAX;
        core->kparam_ac[c] = OAPV_KPARAM_AC_MIN;
        core->prev_dc[c] = 0;

        midx = 0;
        for(y = 0; y < OAPV_BLK_H; y++) {
            for(x = 0; x < OAPV_BLK_W; x++) {
                core->q_mat[c][midx++] = dq_scale * ctx->fh.q_matrix[c][y][x]; // 7bit + 8bit
            }
        }
    }

    for(c = 0; c < ctx->num_comp; c++) {
        int  tc, s_dst;
        s16 *dst;
        oapv_bs_t bsc; // bs for 'tile_data()' syntax

        u8 *comp_start = BSR_GET_CUR(&bs);
        oapv_bsr_init(&bsc, comp_start, tile->th.tile_data_size[c], NULL);

        if(OAPV_CS_GET_FORMAT(ctx->imgb->cs) == OAPV_CF_PLANAR2) {
            tc = c > 0 ? 1 : 0;
            dst = ctx->imgb->a[tc];
            dst += (c > 1) ? 1 : 0;
            s_dst = ctx->imgb->s[tc];
        }
        else {
            dst = ctx->imgb->a[c];
            s_dst = ctx->imgb->s[c];
        }

        
        ret = dec_tile_comp(tile, ctx, core, &bsc, c, s_dst, dst);
        oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

        // move bs buffer to next 'tile_data()' component
        BSR_MOVE_BYTE_ALIGN(&bs, tile->th.tile_data_size[c]);
    }

    oapvd_vlc_tile_dummy_data(&bs);
    return OAPV_OK;
}

// Decode tile directly into provided views (for selective decoding)
static int dec_tile_to_views(oapvd_core_t *core, oapv_bs_t *tile_bs, 
                             oapv_imgb_t *tile_views[4], oapv_th_t *tile_header)
{
    int          ret, midx, x, y, c;
    oapvd_ctx_t *ctx = core->ctx;
    oapv_bs_t    bs;
    
    log_msg(OAPV_LOG_DEBUG, "  tile_bs size: %u bytes\n", tile_bs->size);
    log_msg(OAPV_LOG_DEBUG, "  First 8 bytes of selective tile data: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           tile_bs->beg[0], tile_bs->beg[1], tile_bs->beg[2], tile_bs->beg[3],
           tile_bs->beg[4], tile_bs->beg[5], tile_bs->beg[6], tile_bs->beg[7]);
    
    // Initialize bitstream from provided tile data
    bs = *tile_bs;
    
    // Parse tile header
    ret = oapvd_vlc_tile_header(&bs, ctx, tile_header);
    oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);
    
    // Set up quantization parameters for each component
    for(c = 0; c < ctx->num_comp; c++) {
        core->qp[c] = tile_header->tile_qp[c];
        u8 dq_scale = oapv_tbl_dq_scale[core->qp[c] % 6];
        core->dq_shift[c] = ctx->bit_depth - 2 - (core->qp[c] / 6);
        
        core->kparam_dc[c] = OAPV_KPARAM_DC_MAX;
        core->kparam_ac[c] = OAPV_KPARAM_AC_MIN;
        core->prev_dc[c] = 0;
        
        midx = 0;
        for(y = 0; y < OAPV_BLK_H; y++) {
            for(x = 0; x < OAPV_BLK_W; x++) {
                core->q_mat[c][midx++] = dq_scale * ctx->fh.q_matrix[c][y][x];
            }
        }
    }
    
    // Decode each component to its respective view
    for(c = 0; c < ctx->num_comp; c++) {
        int  tc, s_dst;
        s16 *dst;
        oapv_bs_t bsc; // bs for 'tile_data()' syntax
        
        // Skip if no view provided for this component
        if(!tile_views[c]) {
            continue;
        }
        
        oapv_bsr_init(&bsc, BSR_GET_CUR(&bs), tile_header->tile_data_size[c], NULL);
        
        // Set up destination based on color format
        if(OAPV_CS_GET_FORMAT(tile_views[c]->cs) == OAPV_CF_PLANAR2) {
            tc = c > 0 ? 1 : 0;
            dst = tile_views[c]->a[tc];
            dst += (c > 1) ? 1 : 0;
            s_dst = tile_views[c]->s[tc];
        }
        else {
            dst = tile_views[c]->a[c];
            s_dst = tile_views[c]->s[c];
        }
        
        // Create a dummy tile structure for compatibility with dec_tile_comp
        oapvd_tile_t dummy_tile;
        dummy_tile.th = *tile_header;
        dummy_tile.x = 0; // Relative to tile view
        dummy_tile.y = 0; // Relative to tile view
        dummy_tile.w = tile_views[c]->w[0];
        dummy_tile.h = tile_views[c]->h[0];
        ret = dec_tile_comp(&dummy_tile, ctx, core, &bsc, c, s_dst, dst);
        oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);
        
        // Move to next component data
        BSR_MOVE_BYTE_ALIGN(&bs, tile_header->tile_data_size[c]);
    }
    
    oapvd_vlc_tile_dummy_data(&bs);
    return OAPV_OK;
}

static int dec_thread_tile(void *arg)
{
    oapv_bs_t     bs;
    int           i, ret, run, tile_idx = 0, thread_ret = OAPV_OK;

    oapvd_core_t *core = (oapvd_core_t *)arg;
    oapvd_ctx_t  *ctx = core->ctx;
    oapvd_tile_t *tile = ctx->tile;

    while(1) {
        // find not decoded tile
        oapv_tpool_enter_cs(ctx->sync_obj);
        for(i = 0; i < ctx->num_tiles; i++) {
            if(tile[i].stat == DEC_TILE_STAT_NOT_DECODED) {
                tile[i].stat = DEC_TILE_STAT_ON_DECODING;
                tile_idx = i;
                break;
            }
        }
        oapv_tpool_leave_cs(ctx->sync_obj);
        if(i == ctx->num_tiles) {
            break;
        }

        // wait until to know bistream start position
        run = 1;
        while(run) {
            oapv_tpool_enter_cs(ctx->sync_obj);
            if(tile[tile_idx].bs_beg != NULL) {
                run = 0;
            }
            oapv_tpool_leave_cs(ctx->sync_obj);
        }
        /* read tile size */
        oapv_bsr_init(&bs, tile[tile_idx].bs_beg, OAPV_TILE_SIZE_LEN, NULL);
        ret = oapvd_vlc_tile_size(&bs, &tile[tile_idx].data_size);
        oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);
        oapv_assert_g(tile[tile_idx].bs_beg + OAPV_TILE_SIZE_LEN + (tile[tile_idx].data_size - 1) <= ctx->bs.end, ERR);

        oapv_tpool_enter_cs(ctx->sync_obj);
        if(tile_idx + 1 < ctx->num_tiles) {
            tile[tile_idx + 1].bs_beg = tile[tile_idx].bs_beg + OAPV_TILE_SIZE_LEN + tile[tile_idx].data_size;
        }
        else {
            ctx->tile_end = tile[tile_idx].bs_beg + OAPV_TILE_SIZE_LEN + tile[tile_idx].data_size;
        }
        oapv_tpool_leave_cs(ctx->sync_obj);

        ret = dec_tile(core, &tile[tile_idx]);

        oapv_tpool_enter_cs(ctx->sync_obj);
        if (OAPV_SUCCEEDED(ret)) {
            tile[tile_idx].stat = DEC_TILE_STAT_DECODED;
        }
        else {
            tile[tile_idx].stat =ret;
            thread_ret = ret;
        }
        tile[tile_idx].stat =OAPV_SUCCEEDED(ret) ? DEC_TILE_STAT_DECODED : ret;
        oapv_tpool_leave_cs(ctx->sync_obj);
    }
    return thread_ret;

ERR:
    oapv_tpool_enter_cs(ctx->sync_obj);
    tile[tile_idx].stat = DEC_TILE_STAT_SIZE_ERROR;
    if (tile_idx + 1 < ctx->num_tiles)
    {
        tile[tile_idx + 1].bs_beg = tile[tile_idx].bs_beg;
    }
    oapv_tpool_leave_cs(ctx->sync_obj);
    return OAPV_ERR_MALFORMED_BITSTREAM;
}

static void dec_flush(oapvd_ctx_t *ctx)
{
    // Free dynamically allocated tile array
    if(ctx->tile != NULL) {
        oapv_mfree_fast(ctx->tile);
        ctx->tile = NULL;
    }

    // Free tile offset cache
    if(ctx->tile_offsets_cache != NULL) {
        oapv_mfree(ctx->tile_offsets_cache);
        ctx->tile_offsets_cache = NULL;
        ctx->tile_cache_valid = 0;
        ctx->tile_cache_num_tiles = 0;
    }

    // Free frame header tile_size array
    if(ctx->fh.tile_size != NULL) {
        oapv_mfree_fast(ctx->fh.tile_size);
        ctx->fh.tile_size = NULL;
    }
    if(ctx->threads >= 2) {
        if(ctx->tpool) {
            // thread controller instance is present
            // terminate the created thread
            for(int i = 0; i < ctx->threads - 1; i++) {
                if(ctx->thread_id[i]) {
                    // valid thread instance
                    ctx->tpool->release(&ctx->thread_id[i]);
                }
            }
            // dinitialize the tpool
            oapv_tpool_deinit(ctx->tpool);
            oapv_mfree(ctx->tpool);
            ctx->tpool = NULL;
        }
    }

    oapv_tpool_sync_obj_delete(&(ctx->sync_obj));

    for(int i = 0; i < ctx->threads; i++) {
        dec_core_free(ctx->core[i]);
    }
}

static int dec_ready(oapvd_ctx_t *ctx)
{
    int i, ret = OAPV_OK;

    if (ctx->cdesc.threads == OAPV_CDESC_THREADS_AUTO) {
        int num_cores = oapv_get_num_cpu_cores();
        ctx->threads = oapv_min(OAPV_MAX_THREADS, num_cores);
    }
    else {
        ctx->threads = ctx->cdesc.threads;
    }
    oapv_assert_gv(ctx->threads > 0 && ctx->threads <= OAPV_MAX_THREADS, ret, OAPV_ERR_INVALID_ARGUMENT, ERR);

    if(ctx->core[0] == NULL) {
        // create cores
        for(i = 0; i < ctx->threads; i++) {
            ctx->core[i] = dec_core_alloc();
            oapv_assert_gv(ctx->core[i], ret, OAPV_ERR_OUT_OF_MEMORY, ERR);
            ctx->core[i]->ctx = ctx;
        }
    }

    // initialize the threads to NULL
    for(i = 0; i < ctx->threads; i++) {
        ctx->thread_id[i] = 0;
    }

    // get the context synchronization handle
    ctx->sync_obj = oapv_tpool_sync_obj_create();
    oapv_assert_gv(ctx->sync_obj != NULL, ret, OAPV_ERR_UNKNOWN, ERR);

    if(ctx->threads >= 2) {
        ctx->tpool = oapv_malloc(sizeof(oapv_tpool_t));
        oapv_tpool_init(ctx->tpool, ctx->threads - 1);
        for(i = 0; i < ctx->threads - 1; i++) {
            ctx->thread_id[i] = ctx->tpool->create(ctx->tpool, i);
            oapv_assert_gv(ctx->thread_id[i] != NULL, ret, OAPV_ERR_UNKNOWN, ERR);
        }
    }
    return OAPV_OK;

ERR:
    dec_flush(ctx);

    return ret;
}

static int dec_platform_init(oapvd_ctx_t *ctx)
{
    // default settings
    ctx->fn_itx = oapv_tbl_fn_itx;
    ctx->fn_dquant = oapv_tbl_fn_dquant;

#if X86_SSE
    int check_cpu, support_sse, support_avx2;

    check_cpu = oapv_check_cpu_info_x86();
    support_sse = (check_cpu >> 0) & 1;
    support_avx2 = (check_cpu >> 2) & 1;

    if(support_avx2) {
        ctx->fn_itx = oapv_tbl_fn_itx_avx;
        ctx->fn_dquant = oapv_tbl_fn_dquant_avx;
    }
    else if(support_sse) {
        ctx->fn_itx = oapv_tbl_fn_itx;
        ctx->fn_dquant = oapv_tbl_fn_dquant;
    }
#elif ARM_NEON
    ctx->fn_itx = oapv_tbl_fn_itx_neon;
    ctx->fn_dquant = oapv_tbl_fn_dquant;
#endif
    return OAPV_OK;
}

oapvd_t oapvd_create(oapvd_cdesc_t *cdesc, int *err)
{
    oapvd_ctx_t *ctx;
    int          ret;

    DUMP_CREATE(0);
    ctx = NULL;

    /* check if any decoder argument is correctly set */
    oapv_assert_gv((cdesc->threads > 0 && cdesc->threads <= OAPV_MAX_THREADS) || cdesc->threads == OAPV_CDESC_THREADS_AUTO , ret, OAPV_ERR_INVALID_ARGUMENT, ERR);

    /* memory allocation for ctx and core structure */
    ctx = (oapvd_ctx_t *)dec_ctx_alloc();
    oapv_assert_gv(ctx != NULL, ret, OAPV_ERR_OUT_OF_MEMORY, ERR);
    oapv_mcpy(&ctx->cdesc, cdesc, sizeof(oapvd_cdesc_t));

    /* initialize platform-specific variables */
    ret = dec_platform_init(ctx);
    oapv_assert_g(ret == OAPV_OK, ERR);

    /* ready for decoding */
    ret = dec_ready(ctx);
    oapv_assert_g(ret == OAPV_OK, ERR);

    ctx->magic = OAPVD_MAGIC_CODE;
    ctx->id = (oapvd_t)ctx;
    if(err) {
        *err = OAPV_OK;
    }
    return (ctx->id);

ERR:
    if(ctx) {
        dec_ctx_free(ctx);
    }
    if(err) {
        *err = ret;
    }
    return NULL;
}

void oapvd_delete(oapvd_t did)
{
    oapvd_ctx_t *ctx;
    ctx = dec_id_to_ctx(did);
    oapv_assert_r(ctx);

    DUMP_DELETE();
    dec_flush(ctx);
    dec_ctx_free(ctx);
}

int oapvd_decode(oapvd_t did, oapv_bitb_t *bitb, oapv_frms_t *ofrms, oapvm_t mid, oapvd_stat_t *stat)
{
    oapvd_ctx_t *ctx;
    oapv_pbuh_t  pbuh;
    int          ret = OAPV_OK;
    u32          pbu_size;
    u32          cur_read_size = 0;
    int          frame_cnt = 0;

    ctx = dec_id_to_ctx(did);
    oapv_assert_rv(ctx, OAPV_ERR_INVALID_ARGUMENT);

    // read signature ('aPv1')
    oapv_assert_rv(bitb->ssize > 4, OAPV_ERR_MALFORMED_BITSTREAM);
    u32 signature = oapv_bsr_read_direct(bitb->addr, 32);
    oapv_assert_rv(signature == 0x61507631, OAPV_ERR_MALFORMED_BITSTREAM);
    cur_read_size += 4;
    stat->read += 4;

    // decode PBUs
    do {
        oapv_bs_t   *bs;
        u32 remain = bitb->ssize - cur_read_size;
        oapv_assert_gv((remain >= 8), ret, OAPV_ERR_MALFORMED_BITSTREAM, ERR);
        oapv_bsr_init(&ctx->bs, (u8 *)bitb->addr + cur_read_size, remain, NULL);
        bs = &ctx->bs;

        ret = oapvd_vlc_pbu_size(bs, &pbu_size); // read pbu_size (4 byte)
        oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);
        remain -= 4; // size of pbu_size syntax
        oapv_assert_gv(pbu_size <= remain, ret, OAPV_ERR_MALFORMED_BITSTREAM, ERR);

        ret = oapvd_vlc_pbu_header(bs, &pbuh);
        oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

        if(pbuh.pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_PREVIEW_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_DEPTH_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_ALPHA_FRAME) {

            oapv_assert_gv(frame_cnt < OAPV_MAX_NUM_FRAMES, ret, OAPV_ERR_REACHED_MAX, ERR);

            ret = oapvd_vlc_frame_header(bs, &ctx->fh);
            oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

            ret = dec_frm_prepare(ctx, ofrms->frm[frame_cnt].imgb);
            oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

            int           res;
            oapv_tpool_t *tpool = ctx->tpool;
            int           parallel_task = 1;
            int           tidx = 0;

            parallel_task = (ctx->threads > ctx->num_tiles) ? ctx->num_tiles : ctx->threads;

            /* decode tiles ************************************/
            for(tidx = 0; tidx < (parallel_task - 1); tidx++) {
                tpool->run(ctx->thread_id[tidx], dec_thread_tile,
                           (void *)ctx->core[tidx]);
            }
            ret = dec_thread_tile((void *)ctx->core[tidx]);
            for(tidx = 0; tidx < parallel_task - 1; tidx++) {
                tpool->join(ctx->thread_id[tidx], &res);
                if(OAPV_FAILED(res)) {
                    ret = res;
                }
            }
            /****************************************************/

            /* READ FILLER HERE !!! */

            oapv_bsr_move(&ctx->bs, ctx->tile_end);
            stat->read += BSR_GET_READ_BYTE(&ctx->bs);

            fh_to_finfo(&ctx->fh, pbuh.pbu_type, pbuh.group_id, &stat->aui.frm_info[frame_cnt]);
            if(ret == OAPV_OK && ctx->use_frm_hash) {
                oapv_imgb_set_md5(ctx->imgb);
            }
            ret = dec_frm_finish(ctx); // FIX-ME
            oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

            ofrms->frm[frame_cnt].pbu_type = pbuh.pbu_type;
            ofrms->frm[frame_cnt].group_id = pbuh.group_id;
            stat->frm_size[frame_cnt] = pbu_size + 4 /* byte size of 'pbu_size' syntax */;
            frame_cnt++;
        }
        else if(pbuh.pbu_type == OAPV_PBU_TYPE_METADATA) {
            ret = oapvd_vlc_metadata(bs, pbu_size, mid, pbuh.group_id);
            oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);

            stat->read += BSR_GET_READ_BYTE(&ctx->bs);
        }
        else if(pbuh.pbu_type == OAPV_PBU_TYPE_FILLER) {
            ret = oapvd_vlc_filler(bs, (pbu_size - 4));
            oapv_assert_g(OAPV_SUCCEEDED(ret), ERR);
        }
        cur_read_size += pbu_size + 4 /* byte size of 'pbu_size' syntax */;
    } while(cur_read_size < bitb->ssize);
    stat->aui.num_frms = frame_cnt;
    oapv_assert_gv(ofrms->num_frms == frame_cnt, ret, OAPV_ERR_MALFORMED_BITSTREAM, ERR);
    return ret;

ERR:
    return ret;
}

int oapvd_config(oapvd_t did, int cfg, void *buf, int *size)
{
    oapvd_ctx_t *ctx;

    ctx = dec_id_to_ctx(did);
    oapv_assert_rv(ctx, OAPV_ERR_INVALID_ARGUMENT);

    switch(cfg) {
    /* set config ************************************************************/
    case OAPV_CFG_SET_USE_FRM_HASH:
        ctx->use_frm_hash = (*((int *)buf)) ? 1 : 0;
        break;

    default:
        oapv_assert_rv(0, OAPV_ERR_UNSUPPORTED);
    }
    return OAPV_OK;
}

int oapvd_info(void *au, int au_size, oapv_au_info_t *aui)
{
    int ret, frm_count = 0;
    u32 cur_read_size = 0;
    oapv_bs_t bs;

    DUMP_SET(0);

    // read signature ('aPv1')
    oapv_assert_rv(au_size > 4, OAPV_ERR_MALFORMED_BITSTREAM);
    u32 signature = oapv_bsr_read_direct(au, 32);
    oapv_assert_rv(signature == 0x61507631, OAPV_ERR_MALFORMED_BITSTREAM);
    cur_read_size += 4;

    // parse PBUs
    do {
        u32 pbu_size = 0;
        u32 remain = au_size - cur_read_size;
        oapv_assert_rv(remain >= 8, OAPV_ERR_MALFORMED_BITSTREAM);
        oapv_bsr_init(&bs, (u8 *)au + cur_read_size, remain, NULL);

        ret = oapvd_vlc_pbu_size(&bs, &pbu_size); // read pbu_size (4 byte)
        oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);
        remain -= 4; // size of pbu_size syntax
        oapv_assert_rv(pbu_size <= remain, OAPV_ERR_MALFORMED_BITSTREAM);

        /* pbu header */
        oapv_pbuh_t pbuh;
        ret = oapvd_vlc_pbu_header(&bs, &pbuh); // read pbu_header() (4 byte)
        oapv_assert_rv(OAPV_SUCCEEDED(ret), OAPV_ERR_MALFORMED_BITSTREAM);
        if(pbuh.pbu_type == OAPV_PBU_TYPE_AU_INFO) {
            // parse access_unit_info in PBU
            oapv_aui_t ai;

            ret = oapvd_vlc_au_info(&bs, &ai);
            oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

            aui->num_frms = ai.num_frames;
            for(int i = 0; i < ai.num_frames; i++) {
                fi_to_finfo(&ai.frame_info[i], ai.pbu_type[i], ai.group_id[i], &aui->frm_info[i]);
            }
            return OAPV_OK; // founded access_unit_info, no need to read more PBUs
        }
        if(pbuh.pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_PREVIEW_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_DEPTH_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_ALPHA_FRAME) {
            // parse frame_info in PBU
            oapv_fi_t fi;

            oapv_assert_rv(frm_count < OAPV_MAX_NUM_FRAMES, OAPV_ERR_REACHED_MAX)
            ret = oapvd_vlc_frame_info(&bs, &fi);
            oapv_assert_rv(OAPV_SUCCEEDED(ret), ret);

            fi_to_finfo(&fi, pbuh.pbu_type, pbuh.group_id, &aui->frm_info[frm_count]);
            frm_count++;
        }
        aui->num_frms = frm_count;
        cur_read_size += pbu_size + 4; /* 4byte is for pbu_size syntax itself */
    } while(cur_read_size < au_size);
    DUMP_SET(1);
    return OAPV_OK;
}

// Calculate file offsets for all tiles based on frame header
static int calculate_tile_offsets(oapv_fh_t *fh, int64_t frame_data_start, int64_t *tile_offsets)
{
    int64_t current_offset = frame_data_start;
    int num_tiles = 0;
    
    // Calculate number of tiles
    int tile_w = fh->tile_width_in_mbs * OAPV_MB_W;
    int tile_h = fh->tile_height_in_mbs * OAPV_MB_H;
    int tiles_per_row = oapv_div_round_up(fh->fi.frame_width, tile_w);
    int tiles_per_col = oapv_div_round_up(fh->fi.frame_height, tile_h);
    num_tiles = tiles_per_row * tiles_per_col;
    
    // Calculate offsets using tile sizes from frame header
    for(int i = 0; i < num_tiles; i++) {
        tile_offsets[i] = current_offset; // Point to start of tile (including 4-byte size prefix)
        current_offset += 4 + fh->tile_size[i]; // Size prefix + tile data
    }
    
    return num_tiles;
}

// Build tile offset cache for the current frame
static int oapvd_build_tile_cache(oapvd_ctx_t *ctx, int64_t frame_data_offset)
{
    // Calculate number of tiles
    int tile_w = ctx->fh.tile_width_in_mbs * OAPV_MB_W;
    int tile_h = ctx->fh.tile_height_in_mbs * OAPV_MB_H;
    int tiles_per_row = oapv_div_round_up(ctx->fh.fi.frame_width, tile_w);
    int tiles_per_col = oapv_div_round_up(ctx->fh.fi.frame_height, tile_h);
    int num_tiles = tiles_per_row * tiles_per_col;

    // Allocate cache if needed or size changed
    if(!ctx->tile_offsets_cache || ctx->tile_cache_num_tiles != num_tiles) {
        if(ctx->tile_offsets_cache) {
            oapv_mfree(ctx->tile_offsets_cache);
        }
        ctx->tile_offsets_cache = (int64_t *)oapv_malloc(num_tiles * sizeof(int64_t));
        if(!ctx->tile_offsets_cache) {
            return OAPV_ERR_OUT_OF_MEMORY;
        }
        ctx->tile_cache_num_tiles = num_tiles;
    }

    // Calculate offsets using existing function
    calculate_tile_offsets(&ctx->fh, frame_data_offset, ctx->tile_offsets_cache);

    // Mark cache as valid
    ctx->tile_cache_valid = 1;
    ctx->tile_cache_frame_offset = frame_data_offset;

    return OAPV_OK;
}

// Invalidate tile offset cache
static void oapvd_invalidate_tile_cache(oapvd_ctx_t *ctx)
{
    ctx->tile_cache_valid = 0;
}

// Get tile offset from cache (O(1) lookup)
static int64_t oapvd_get_tile_offset(oapvd_ctx_t *ctx, int tile_index)
{
    if(!ctx->tile_cache_valid || !ctx->tile_offsets_cache ||
       tile_index < 0 || tile_index >= ctx->tile_cache_num_tiles) {
        return -1; // Invalid cache or tile index
    }
    return ctx->tile_offsets_cache[tile_index];
}

// Initialize thread-local buffer manager
static int init_tile_buffer_manager(oapv_tile_buffer_mgr_t *mgr)
{
    // Initialize buffer sizes: 64K, 256K, 1M, 4M
    mgr->fast_buffer_sizes[0] = 64 * 1024;
    mgr->fast_buffer_sizes[1] = 256 * 1024;
    mgr->fast_buffer_sizes[2] = 1024 * 1024;
    mgr->fast_buffer_sizes[3] = 4 * 1024 * 1024;
    mgr->malloc_threshold = 8 * 1024 * 1024; // 8MB threshold
    
    // Allocate buffers for each thread
    for(int thread_id = 0; thread_id < OAPV_MAX_THREADS; thread_id++) {
        for(int i = 0; i < 4; i++) {
            mgr->fast_buffers[thread_id][i] = (u8 *)oapv_malloc(mgr->fast_buffer_sizes[i]);
            if(!mgr->fast_buffers[thread_id][i]) {
                return OAPV_ERR_OUT_OF_MEMORY;
            }
            mgr->buffer_in_use[thread_id][i] = 0;
        }
    }
    
    return OAPV_OK;
}

// Get buffer for compressed tile data
static u8* get_tile_buffer(oapv_tile_buffer_mgr_t *mgr, int thread_id, u32 needed_size, int *buffer_type)
{
    // Fast path: thread-local buffers
    if(thread_id >= 0 && thread_id < OAPV_MAX_THREADS) {
        for(int i = 0; i < 4; i++) {
            if(mgr->fast_buffer_sizes[i] >= needed_size) {
                *buffer_type = i;
                return mgr->fast_buffers[thread_id][i];
            }
        }
    }
    
    // Fallback: malloc for large tiles
    *buffer_type = -1; // Indicates malloc'd buffer
    return (u8 *)oapv_malloc(needed_size);
}

// Return buffer (only needed for malloc'd buffers)
static void return_tile_buffer(oapv_tile_buffer_mgr_t *mgr, u8 *buffer, int buffer_type)
{
    if(buffer_type == -1) { // malloc'd buffer
        oapv_mfree(buffer);
    }
    // Thread-local buffers don't need explicit return
}

// Create tile view pointing into user output buffer  
static int create_tile_views(oapv_imgb_t *output_buffers[4], int tile_col, int tile_row, 
                            int tile_w, int tile_h, oapv_imgb_t *views[4])
{
    for(int c = 0; c < 4; c++) {
        if(!output_buffers[c]) continue;
        
        views[c] = (oapv_imgb_t *)oapv_malloc(sizeof(oapv_imgb_t));
        if(!views[c]) return OAPV_ERR_OUT_OF_MEMORY;
        
        // Copy buffer properties  
        views[c]->cs = output_buffers[c]->cs;
        views[c]->w[0] = output_buffers[c]->w[0];
        views[c]->h[0] = output_buffers[c]->h[0]; 
        views[c]->s[0] = output_buffers[c]->s[0];
        views[c]->a[0] = output_buffers[c]->a[0];
        views[c]->refcnt = 1;
    }
    
    return OAPV_OK;
}

// Selective decode implementation
int oapvd_decode_selective(oapvd_t did, oapvd_istream_t * istream, oapv_selective_decode_t * sel_decode,
                        oapvm_t mid, oapvd_stat_t * stat)
{
    oapvd_ctx_t *ctx;
    int ret = OAPV_OK;
    
    ctx = dec_id_to_ctx(did);
    oapv_assert_rv(ctx, OAPV_ERR_INVALID_ARGUMENT);
    
    // Get target mip level from selective decode request
    int target_mip_level = sel_decode->mip_level;
    
    // Read and parse frame header to get tile information
    istream->seek(istream, 0, SEEK_SET);
    
    // Read AU size (big-endian, like in oapv_app_dec.c)
    u8 size_buf[4];
    istream->read(istream, size_buf, 4, 1);
    u32 au_size = (size_buf[0] << 24) | (size_buf[1] << 16) | (size_buf[2] << 8) | size_buf[3];
    stat->read += 4;
    
    // For selective I/O: Read entire AU first to parse headers and get tile sizes
    // Then we'll seek back and read only the target tile data
    int64_t au_start_pos = istream->tell(istream);
    
    u8  *au_buffer = (u8 *)oapv_malloc(au_size);
    if(!au_buffer) {
        return OAPV_ERR_OUT_OF_MEMORY;
    }
    istream->read(istream, au_buffer, au_size, 1);    // this is reading the whole access unit.
    
    // Parse bitstream using the buffer approach like normal decoder
    oapv_bitb_t bitb;
    bitb.addr = au_buffer;
    bitb.ssize = au_size;
    
    // Check signature from buffer
    u32 signature = oapv_bsr_read_direct(bitb.addr, 32);
    if(signature != 0x61507631) { // 'aPv1' expected by normal decoder
        log_msg(OAPV_LOG_ERROR, "Invalid signature: 0x%08X (expected aPv1)\n", signature);
        oapv_mfree(au_buffer);
        return OAPV_ERR_MALFORMED_BITSTREAM;
    }
    
    // Navigate through PBUs using cur_read_size approach like original decoder
    int current_frame = 0;
    u32 cur_read_size = 4; // Start after signature
    oapv_bs_t bs;
    
    // Loop through PBUs until we find the target mip level
    while(cur_read_size < bitb.ssize && current_frame <= target_mip_level) {
        u32 remain = bitb.ssize - cur_read_size;
        if(remain < 8) {
            log_msg(OAPV_LOG_ERROR, "Not enough data for PBU header at pos %d\n", cur_read_size);
            oapv_mfree(au_buffer);
            return OAPV_ERR_MALFORMED_BITSTREAM;
        }
        
        // Initialize bitstream reader at current position
        oapv_bsr_init(&bs, (u8*)bitb.addr + cur_read_size, remain, NULL);
        
        // Read PBU size
        u32 pbu_size;
        ret = oapvd_vlc_pbu_size(&bs, &pbu_size);
        if(OAPV_FAILED(ret)) {
            log_msg(OAPV_LOG_ERROR, "Failed to parse PBU size for frame %d: %d\n", current_frame, ret);
            oapv_mfree(au_buffer);
            return ret;
        }
        
        // Validate PBU size
        remain -= 4; // 4 bytes consumed for pbu_size
        if(pbu_size > remain) {
            log_msg(OAPV_LOG_ERROR, "PBU size %d exceeds remaining data %d at pos %d\n", pbu_size, remain, cur_read_size);
            oapv_mfree(au_buffer);
            return OAPV_ERR_MALFORMED_BITSTREAM;
        }
        
        // Read PBU header  
        oapv_pbuh_t pbuh;
        ret = oapvd_vlc_pbu_header(&bs, &pbuh);
        if(OAPV_FAILED(ret)) {
            log_msg(OAPV_LOG_ERROR, "Failed to parse PBU header for frame %d: %d\n", current_frame, ret);
            oapv_mfree(au_buffer);
            return ret;
        }
        
        // Check if this is a frame PBU
        if(pbuh.pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME) {
            
            if(current_frame == target_mip_level) {
                // Found target frame - parse its header
                ret = oapvd_vlc_frame_header(&bs, &ctx->fh);
                if(OAPV_FAILED(ret)) {
                    log_msg(OAPV_LOG_ERROR, "Failed to parse frame header for mip %d: %d\n", target_mip_level, ret);
                    oapv_mfree(au_buffer);
                    return ret;
                }
                
                // Fill in the actual frame metadata for the caller

                // Return padded dimensions for buffer allocation
                sel_decode->actual_frame_width = oapv_align_value(ctx->fh.fi.frame_width, OAPV_MB_W);
                sel_decode->actual_frame_height = oapv_align_value(ctx->fh.fi.frame_height, OAPV_MB_H);

                // Convert tile size from MBs to pixels (16 pixels per MB)
                sel_decode->actual_tile_width = ctx->fh.tile_width_in_mbs * 16;
                sel_decode->actual_tile_height = ctx->fh.tile_height_in_mbs * 16;
                sel_decode->bit_depth = ctx->fh.fi.bit_depth;
                sel_decode->chroma_format_idc = ctx->fh.fi.chroma_format_idc;

                // Check if this is a metadata-only call (no output buffers provided)
                if(sel_decode->output_buffer == NULL) {
                    oapv_mfree(au_buffer);
                    return OAPV_OK;
                }
                break; // Exit the loop - we found our target
            } else {
                current_frame++;
            }
        }
        
        // Move to next PBU using the standard approach: pbu_size + 4 bytes for pbu_size syntax
        cur_read_size += pbu_size + 4;
    }
    
    if(current_frame != target_mip_level) {
        log_msg(OAPV_LOG_ERROR, "Could not find mip level %d (only found %d frames)\n", target_mip_level, current_frame);
        oapv_mfree(au_buffer);
        return OAPV_ERR_INVALID_ARGUMENT;
    }
    
    // Initialize context using normal decoder path approach
    // Create a minimal dummy imgb for dec_frm_prepare
    oapv_imgb_t dummy_imgb;
    memset(&dummy_imgb, 0, sizeof(dummy_imgb));
    dummy_imgb.cs = OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 0); // YUV422 10-bit
    if (sel_decode->output_buffer)
    {
        dummy_imgb.cs = sel_decode->output_buffer->cs;  // use the destination cs.
    }
    dummy_imgb.refcnt = 1;

    // Initialize ctx->bs for dec_frm_prepare
    oapv_bsr_init(&ctx->bs, bs.cur, bs.end - bs.cur, NULL);
   
    ret = dec_frm_prepare(ctx, &dummy_imgb);

    if(OAPV_FAILED(ret)) {
        log_msg(OAPV_LOG_ERROR, "Failed to prepare frame context: %d\n", ret);
        oapv_mfree(au_buffer);
        return ret;
    }

    // Calculate configured tile dimensions
    int tile_w_config = ctx->fh.tile_width_in_mbs * OAPV_MB_W;
    int tile_h_config = ctx->fh.tile_height_in_mbs * OAPV_MB_H;

    // Calculate target tile coordinates
    int target_tile_col = sel_decode->tile_coords[0];
    int target_tile_row = sel_decode->tile_coords[1];

    // Calculate tile layout
    int frame_width_in_mbs = (ctx->fh.fi.frame_width + OAPV_MB_W - 1) / OAPV_MB_W;
    int tiles_per_row = (frame_width_in_mbs + ctx->fh.tile_width_in_mbs - 1) / ctx->fh.tile_width_in_mbs;
    int tile_index = target_tile_row * tiles_per_row + target_tile_col;

    // Calculate ACTUAL tile dimensions for this specific tile (handles edge cases)
    int tile_x_start = target_tile_col * tile_w_config;
    int tile_y_start = target_tile_row * tile_h_config;
    int tile_x_end = (tile_x_start + tile_w_config < ctx->fh.fi.frame_width) ?
                     tile_x_start + tile_w_config : ctx->fh.fi.frame_width;
    int tile_y_end = (tile_y_start + tile_h_config < ctx->fh.fi.frame_height) ?
                     tile_y_start + tile_h_config : ctx->fh.fi.frame_height;
    int actual_tile_w = tile_x_end - tile_x_start;
    int actual_tile_h = tile_y_end - tile_y_start;

    // Calculate frame data start position within the target mip level frame
    // After parsing frame header, bs.cur points to start of frame data (tiles)
    int64_t frame_data_offset_in_au = BSR_GET_CUR(&bs) - (u8*)bitb.addr; // Offset from AU start
    int64_t abs_frame_data_offset = au_start_pos + frame_data_offset_in_au;

    // Build tile offset cache for this frame if needed
    if(!ctx->tile_cache_valid || ctx->tile_cache_frame_offset != abs_frame_data_offset) {
        ret = oapvd_build_tile_cache(ctx, abs_frame_data_offset);
        if(OAPV_FAILED(ret)) {
            oapv_mfree(au_buffer);
            return ret;
        }
    }

    // Get target tile offset from cache
    int64_t tile_offset_abs = oapvd_get_tile_offset(ctx, tile_index);
    if(tile_offset_abs < 0) {
        log_msg(OAPV_LOG_ERROR, "Invalid tile index %d\n", tile_index);
        oapv_mfree(au_buffer);
        return OAPV_ERR_INVALID_ARGUMENT;
    }

    // Use tile size from header
    u32 tile_size = ctx->fh.tile_size[tile_index];

    // Calculate absolute file position for target tile
    // Skip the 4-byte size prefix to point to actual tile data
    int64_t tile_file_position = tile_offset_abs + 4;
    
    // Free the full AU buffer - we don't need it anymore
    oapv_mfree(au_buffer);
    au_buffer = NULL;
    
    // Seek to target tile position in file and read only that tile's data
    istream->seek(istream, tile_file_position, SEEK_SET);
    
    u8 *tile_data = (u8 *)oapv_malloc(tile_size);
    if(!tile_data) {
        return OAPV_ERR_OUT_OF_MEMORY;
    }
    
    size_t bytes_read = istream->read(istream, tile_data, 1, tile_size);
    if(bytes_read != tile_size) {
        log_msg(OAPV_LOG_ERROR, "Failed to read tile data - expected %u bytes, got %zu bytes\n", tile_size, bytes_read);
        oapv_mfree(tile_data);
        return OAPV_ERR_MALFORMED_BITSTREAM;
    }
    
    // Initialize bitstream with tile data (no size prefix since we skipped it during read)
    oapv_bs_t tile_bs;
    oapv_bsr_init(&tile_bs, tile_data, tile_size, NULL);
        
        // Use dec_tile_comp to decode each component directly into output buffers
        int thread_id = 0;
        oapvd_core_t *core = ctx->core[thread_id];
        
        // Create a proper tile structure and parse tile header
        oapvd_tile_t tile;
        memset(&tile, 0, sizeof(tile));
        
        // Set up tile geometry (required for dec_tile_comp)
        // Use relative coordinates since we pass pre-calculated destination addresses
        tile.x = 0; // relative to destination address
        tile.y = 0; // relative to destination address
        tile.w = actual_tile_w; // Use ACTUAL tile width (not configured)
        tile.h = actual_tile_h; // Use ACTUAL tile height (not configured)
        
        // Parse tile header first (required by dec_tile_comp)
        ret = oapvd_vlc_tile_header(&tile_bs, ctx, &tile.th);
        if(OAPV_FAILED(ret)) {
            log_msg(OAPV_LOG_ERROR, "Failed to parse tile header: %d\n", ret);
            oapv_mfree(tile_data);
            return ret;
        }

        // Initialize decoder context state like regular decoder
        for(int c = 0; c < ctx->num_comp; c++) {
            core->qp[c] = tile.th.tile_qp[c];
            u8 dq_scale = oapv_tbl_dq_scale[core->qp[c] % 6];
            core->dq_shift[c] = ctx->bit_depth - 2 - (core->qp[c] / 6);

            core->kparam_dc[c] = OAPV_KPARAM_DC_MAX;
            core->kparam_ac[c] = OAPV_KPARAM_AC_MIN;
            core->prev_dc[c] = 0;

            // Initialize quantization matrix (MISSING in original selective decoder!)
            int midx = 0;
            for(int y = 0; y < OAPV_BLK_H; y++) {
                for(int x = 0; x < OAPV_BLK_W; x++) {
                    core->q_mat[c][midx++] = dq_scale * ctx->fh.q_matrix[c][y][x]; // 7bit + 8bit
                }
            }
        }

        // Create component-specific bitstream reader like regular decoder
        u8 *y_start = BSR_GET_CUR(&tile_bs);

        oapv_bs_t y_bs;
        oapv_bsr_init(&y_bs, y_start, tile.th.tile_data_size[0], NULL);
        
        // Calculate destination address for Y component (account for tile position)
        int tile_x_pixels = tile_x_start; // Use calculated actual position
        int tile_y_pixels = tile_y_start; // Use calculated actual position
        int y_stride_bytes = sel_decode->output_buffer->s[0]; // stride is in bytes
        u8 *y_base_bytes = (u8*)sel_decode->output_buffer->a[0];
        u8 *y_dst_bytes = y_base_bytes + (tile_y_pixels * y_stride_bytes) + (tile_x_pixels * 2);
        u16 *y_dst = (u16*)y_dst_bytes;
        
        
        ret = dec_tile_comp(&tile, ctx, core, &y_bs, 0, 
                           sel_decode->output_buffer->s[0], 
                           y_dst);
        if(OAPV_FAILED(ret)) {
            log_msg(OAPV_LOG_ERROR, "Failed to decode Y component: %d\n", ret);
        }
        
        // Now decode U and V components
        if(OAPV_SUCCEEDED(ret)) {
            u8 *u_start = y_start + tile.th.tile_data_size[0];
            oapv_bs_t u_bs;
            oapv_bsr_init(&u_bs, u_start, tile.th.tile_data_size[1], NULL);

            // Calculate destination address for U component (4:2:2 - half width)
            int u_tile_x_pixels = tile_x_pixels / 2; // U is half width due to 4:2:2 chroma subsampling
            int u_stride_bytes = sel_decode->output_buffer->s[1]; // stride in bytes
            u8 *u_base_bytes = (u8*)sel_decode->output_buffer->a[1];
            u8 *u_dst_bytes = u_base_bytes + (tile_y_pixels * u_stride_bytes) + (u_tile_x_pixels * 2);
            u16 *u_dst = (u16*)u_dst_bytes;

            ret = dec_tile_comp(&tile, ctx, core, &u_bs, 1,
                               sel_decode->output_buffer->s[1],
                               u_dst);
            if(OAPV_FAILED(ret)) {
                log_msg(OAPV_LOG_ERROR, "Failed to decode U component: %d\n", ret);
            }
        }
        
        if(OAPV_SUCCEEDED(ret)) {
            u8 *v_start = y_start + tile.th.tile_data_size[0] + tile.th.tile_data_size[1];
            oapv_bs_t v_bs;
            oapv_bsr_init(&v_bs, v_start, tile.th.tile_data_size[2], NULL);

            // Calculate destination address for V component (4:2:2 - half width)
            int v_tile_x_pixels = tile_x_pixels / 2; // V is half width due to 4:2:2 chroma subsampling
            int v_stride_bytes = sel_decode->output_buffer->s[2]; // stride in bytes
            u8 *v_base_bytes = (u8*)sel_decode->output_buffer->a[2];
            u8 *v_dst_bytes = v_base_bytes + (tile_y_pixels * v_stride_bytes) + (v_tile_x_pixels * 2);
            u16 *v_dst = (u16*)v_dst_bytes;

            ret = dec_tile_comp(&tile, ctx, core, &v_bs, 2,
                               sel_decode->output_buffer->s[2],
                               v_dst);
            if(OAPV_FAILED(ret)) {
                log_msg(OAPV_LOG_ERROR, "Failed to decode V component: %d\n", ret);
            }
        }
    
    oapv_mfree(tile_data);
    return ret;
}

// Data structures for multi-tile decoding

/* Shared context for all tiles belonging to the same mip level.
 * This data is read-only after initialization and shared among all worker threads
 * processing tiles from this mip, reducing memory footprint and improving cache locality.
 */
typedef struct {
    int mip_level;          // Mip level identifier

    // Frame dimensions and format
    int bit_depth;
    int chroma_format_idc;
    int frame_width;        // Actual frame dimensions from mip header
    int frame_height;
    int padded_frame_width; // Padded dimensions for buffer allocation
    int padded_frame_height;

    // Quantization matrices for this mip (256 bytes - largest shared data)
    u8 q_matrix[N_C][OAPV_BLK_H][OAPV_BLK_W];

    // Output buffer for this mip
    oapv_imgb_t *output_buffer;

    // Tile configuration from mip's frame header
    int tile_width_in_mbs;
    int tile_height_in_mbs;

    // Decode context fields (to avoid shared ctx issues)
    int num_comp;               // Number of components (derived from chroma_format_idc)
    int comp_sft[N_C][2];       // Component shift values for chroma subsampling
} mip_context_t;

/* Per-tile work item. Points to shared mip_context_t to avoid duplicating
 * mip-level data across all tiles (saves ~336 bytes per tile).
 */
typedef struct {
    int tile_idx;           // Tile index in frame
    int col, row;           // Tile coordinates
    u32 size;               // Tile data size
    u64 file_offset;        // Position in file
    u8 *data;               // Pointer to tile data
    volatile int status;    // 0=NOT_DECODED, 1=ON_DECODING, 2=DECODED
    oapvd_core_t *core;     // Assigned decoder core

    // Pointer to shared mip-level context (read-only, shared among all tiles of this mip)
    const mip_context_t *mip_ctx;
} tile_work_t;

typedef struct {
    u64 start_offset;       // Start position in file
    u64 end_offset;         // End position  
    u32 total_size;         // Total bytes to read
    int first_tile_idx;     // First tile in this block
    int num_tiles;          // Number of tiles in block
    u8 *buffer;             // Allocated memory for block
} tile_read_block_t;

// Performance metrics structure
typedef struct {
    u64 io_start_ns;
    u64 io_end_ns;
    u64 decode_start_ns;
    u64 decode_end_ns;
    u32 bytes_read;
    u32 tiles_decoded;
} perf_metrics_t;

// Helper function to get current time in nanoseconds
static u64 get_time_ns() {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (u64)((counter.QuadPart * 1000000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/*****************************************************************************
 * Modular selective decode utilities - reusable for multi and multi-mip
 *****************************************************************************/

// Stream information structure
typedef struct {
    u32 au_size;
    int64_t au_start_pos;
    u32 signature;
} oapv_stream_info_t;

// Mip location information
typedef struct {
    int64_t frame_file_pos;
    u32 pbu_size;
    int64_t frame_data_offset;
    int found;
} oapv_mip_location_t;

// Stream validation and setup
static int oapvd_validate_stream(oapvd_istream_t *istream, oapv_stream_info_t *stream_info,
                                perf_metrics_t *metrics)
{
    oapv_assert_rv(istream && stream_info, OAPV_ERR_INVALID_ARGUMENT);

    // Reset stream position and read AU size
    istream->seek(istream, 0, SEEK_SET);

    u8 size_buf[4];
    istream->read(istream, size_buf, 4, 1);
    stream_info->au_size = (size_buf[0] << 24) | (size_buf[1] << 16) |
                          (size_buf[2] << 8) | size_buf[3];
    if(metrics) metrics->bytes_read += 4;

    stream_info->au_start_pos = istream->tell(istream);

    // Verify APV signature
    u8 sig_buf[4];
    istream->read(istream, sig_buf, 4, 1);
    stream_info->signature = (sig_buf[0] << 24) | (sig_buf[1] << 16) |
                            (sig_buf[2] << 8) | sig_buf[3];
    if(stream_info->signature != 0x61507631) {
        return OAPV_ERR_MALFORMED_BITSTREAM;
    }
    if(metrics) metrics->bytes_read += 4;

    return OAPV_OK;
}

// Mip level discovery - locates a specific mip frame in the AU
static int oapvd_locate_mip_frame(oapvd_istream_t *istream, oapv_stream_info_t *stream_info,
                                  int target_mip, oapv_mip_location_t *location,
                                  perf_metrics_t *metrics)
{
    oapv_assert_rv(istream && stream_info && location, OAPV_ERR_INVALID_ARGUMENT);
    oapv_assert_rv(target_mip >= 0, OAPV_ERR_INVALID_ARGUMENT);

    location->found = 0;

    if(target_mip == 0) {
        // Mip 0 optimization: always the first PBU after signature
        location->frame_file_pos = stream_info->au_start_pos + 4;

        // Read PBU size
        istream->seek(istream, location->frame_file_pos, SEEK_SET);
        u8 pbu_size_buf[4];
        istream->read(istream, pbu_size_buf, 4, 1);
        location->pbu_size = (pbu_size_buf[0] << 24) | (pbu_size_buf[1] << 16) |
                            (pbu_size_buf[2] << 8) | pbu_size_buf[3];
        if(metrics) metrics->bytes_read += 4;

        location->found = 1;
        return OAPV_OK;
    }

    // Higher mip levels: traverse PBUs sequentially
    int current_frame = 0;
    int64_t current_pos = stream_info->au_start_pos + 4; // Skip signature

    while(current_pos < stream_info->au_start_pos + stream_info->au_size &&
          current_frame <= target_mip) {

        istream->seek(istream, current_pos, SEEK_SET);

        // Read PBU size
        u8 pbu_size_buf[4];
        istream->read(istream, pbu_size_buf, 4, 1);
        u32 pbu_size = (pbu_size_buf[0] << 24) | (pbu_size_buf[1] << 16) |
                       (pbu_size_buf[2] << 8) | pbu_size_buf[3];

        if(pbu_size == 0 || pbu_size > stream_info->au_size) {
            return OAPV_ERR_MALFORMED_BITSTREAM;
        }
        if(metrics) metrics->bytes_read += 4;

        // Read and parse PBU header
        u8 pbu_header_buf[4];
        istream->read(istream, pbu_header_buf, 4, 1);
        if(metrics) metrics->bytes_read += 4;

        oapv_bs_t pbu_header_bs;
        oapv_bsr_init(&pbu_header_bs, pbu_header_buf, 4, NULL);

        oapv_pbuh_t pbuh;
        int ret = oapvd_vlc_pbu_header(&pbu_header_bs, &pbuh);
        if(OAPV_FAILED(ret)) {
            return ret;
        }

        // Check if this is a frame PBU
        if(pbuh.pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME) {

            if(current_frame == target_mip) {
                // Found target mip
                location->frame_file_pos = current_pos;
                location->pbu_size = pbu_size;
                location->found = 1;
                return OAPV_OK;
            }
            current_frame++;
        }

        // Skip to next PBU
        current_pos += 4 + pbu_size;
    }

    // Target mip not found
    location->found = 0;
    return OAPV_OK;
}

// Batch mip level discovery - locates multiple mip frames in a single traversal
static int oapvd_locate_all_mips(oapvd_istream_t *istream, oapv_stream_info_t *stream_info,
                                 const int *requested_mips, int num_mips,
                                 oapv_mip_location_t *locations,
                                 perf_metrics_t *metrics)
{
    oapv_assert_rv(istream && stream_info && requested_mips && locations, OAPV_ERR_INVALID_ARGUMENT);
    oapv_assert_rv(num_mips > 0, OAPV_ERR_INVALID_ARGUMENT);

    // Initialize all locations as not found
    for(int i = 0; i < num_mips; i++) {
        locations[i].found = 0;
    }

    // Find the highest requested mip level for early termination
    int max_mip = requested_mips[0];
    for(int i = 1; i < num_mips; i++) {
        if(requested_mips[i] > max_mip) {
            max_mip = requested_mips[i];
        }
    }

    // Create a lookup map for fast mip-to-index resolution
    // Map mip level -> index in locations array (-1 if not requested)
    int mip_to_idx[256]; // Support up to 256 mip levels
    for(int i = 0; i < 256; i++) {
        mip_to_idx[i] = -1;
    }
    for(int i = 0; i < num_mips; i++) {
        if(requested_mips[i] < 256) {
            mip_to_idx[requested_mips[i]] = i;
        }
    }

    // Single traversal through PBU stream
    int current_frame = 0;
    int64_t current_pos = stream_info->au_start_pos + 4; // Skip signature
    int found_count = 0;

    while(current_pos < stream_info->au_start_pos + stream_info->au_size &&
          current_frame <= max_mip) {

        istream->seek(istream, current_pos, SEEK_SET);

        // Read PBU size
        u8 pbu_size_buf[4];
        istream->read(istream, pbu_size_buf, 4, 1);
        u32 pbu_size = (pbu_size_buf[0] << 24) | (pbu_size_buf[1] << 16) |
                       (pbu_size_buf[2] << 8) | pbu_size_buf[3];

        if(pbu_size == 0 || pbu_size > stream_info->au_size) {
            return OAPV_ERR_MALFORMED_BITSTREAM;
        }

        if(metrics) {
            metrics->bytes_read += 4;
        }

        // Read and parse PBU header
        u8 pbu_header_buf[4];
        istream->read(istream, pbu_header_buf, 4, 1);
        
        if(metrics) {
            metrics->bytes_read += 4;
        }

        oapv_bs_t pbu_header_bs;
        oapv_bsr_init(&pbu_header_bs, pbu_header_buf, 4, NULL);

        oapv_pbuh_t pbuh;
        int ret = oapvd_vlc_pbu_header(&pbu_header_bs, &pbuh);

        if(OAPV_FAILED(ret)) {
            return ret;
        }

        // Check if this is a frame PBU
        if(pbuh.pbu_type == OAPV_PBU_TYPE_PRIMARY_FRAME ||
           pbuh.pbu_type == OAPV_PBU_TYPE_NON_PRIMARY_FRAME) {

            // Check if this mip level is requested
            if(current_frame < 256) {
                int idx = mip_to_idx[current_frame];
                if(idx >= 0) {
                    // Found a requested mip
                    locations[idx].frame_file_pos = current_pos;
                    locations[idx].pbu_size = pbu_size;
                    locations[idx].found = 1;
                    found_count++;

                    // Early termination if all mips found
                    if(found_count == num_mips) {
                        return OAPV_OK;
                    }
                }
            }
            current_frame++;
        }

        // Skip to next PBU
        current_pos += 4 + pbu_size;
    }

    // Return success even if not all mips found (caller checks found flags)
    return OAPV_OK;
}

// Frame header parsing with progressive buffer expansion
static int oapvd_parse_frame_headers(oapvd_istream_t *istream, oapv_mip_location_t *location,
                                     oapvd_ctx_t *ctx, oapv_fh_t *frame_header,
                                     perf_metrics_t *metrics)
{
    oapv_assert_rv(istream && location && ctx && frame_header, OAPV_ERR_INVALID_ARGUMENT);
    oapv_assert_rv(location->found, OAPV_ERR_INVALID_ARGUMENT);

    // I/O optimization: progressive header reading
    const u32 INITIAL_HEADER_CHUNK = 8192;  // Start with 8KB
    const u32 MAX_HEADER_CHUNK = 65536;     // Maximum 64KB
    u32 header_buffer_size = (location->pbu_size < INITIAL_HEADER_CHUNK) ?
                            location->pbu_size : INITIAL_HEADER_CHUNK;

    // Allocate initial header buffer
    u8 *frame_buffer = (u8 *)oapv_malloc(header_buffer_size);
    if(!frame_buffer) {
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    // Read PBU header and initial frame data
    istream->seek(istream, location->frame_file_pos + 4, SEEK_SET); // Skip PBU size
    u8 pbu_header_buf[4];
    istream->read(istream, pbu_header_buf, 4, 1);
    if(metrics) metrics->bytes_read += 4;

    // Copy PBU header and read initial frame data
    memcpy(frame_buffer, pbu_header_buf, 4);
    u32 remaining_to_read = header_buffer_size - 4;
    if(remaining_to_read > location->pbu_size - 4) {
        remaining_to_read = location->pbu_size - 4;
    }
    istream->read(istream, frame_buffer + 4, remaining_to_read, 1);
    if(metrics) metrics->bytes_read += remaining_to_read;

    // Progressive parsing with buffer expansion
    oapv_bs_t pbu_bs;
    oapv_pbuh_t pbuh_check;
    int parse_success = 0;

    while(!parse_success && header_buffer_size <= location->pbu_size) {
        // Initialize bitstream for VLC parsing
        oapv_bsr_init(&pbu_bs, frame_buffer, header_buffer_size, NULL);

        // Try parsing PBU header
        int ret = oapvd_vlc_pbu_header(&pbu_bs, &pbuh_check);
        if(OAPV_FAILED(ret)) {
            // Check if we need more data
            long bytes_consumed = BSR_GET_CUR(&pbu_bs) - pbu_bs.beg;
            if(bytes_consumed >= (long)(header_buffer_size * 0.9) &&
               header_buffer_size < location->pbu_size) {
                // Expand buffer
                u32 new_size = header_buffer_size * 2;
                if(new_size > location->pbu_size) new_size = location->pbu_size;
                if(new_size > MAX_HEADER_CHUNK) new_size = MAX_HEADER_CHUNK;

                u8 *new_buffer = (u8 *)oapv_realloc(frame_buffer, new_size);
                if(!new_buffer) {
                    oapv_mfree(frame_buffer);
                    return OAPV_ERR_OUT_OF_MEMORY;
                }
                frame_buffer = new_buffer;

                // Read additional data
                u32 additional_bytes = new_size - header_buffer_size;
                istream->read(istream, frame_buffer + header_buffer_size, additional_bytes, 1);
                if(metrics) metrics->bytes_read += additional_bytes;
                header_buffer_size = new_size;
                continue;
            } else {
                oapv_mfree(frame_buffer);
                return ret;
            }
        }

        // Try parsing frame header
        ret = oapvd_vlc_frame_header(&pbu_bs, frame_header);
        if(OAPV_FAILED(ret)) {
            // Check if we need more data
            long bytes_consumed = BSR_GET_CUR(&pbu_bs) - pbu_bs.beg;
            if(bytes_consumed >= (long)(header_buffer_size * 0.9) &&
               header_buffer_size < location->pbu_size) {
                // Expand buffer
                u32 new_size = header_buffer_size * 2;
                if(new_size > location->pbu_size) new_size = location->pbu_size;
                if(new_size > MAX_HEADER_CHUNK) new_size = MAX_HEADER_CHUNK;

                u8 *new_buffer = (u8 *)oapv_realloc(frame_buffer, new_size);
                if(!new_buffer) {
                    oapv_mfree(frame_buffer);
                    return OAPV_ERR_OUT_OF_MEMORY;
                }
                frame_buffer = new_buffer;

                // Read additional data
                u32 additional_bytes = new_size - header_buffer_size;
                istream->read(istream, frame_buffer + header_buffer_size, additional_bytes, 1);
                if(metrics) metrics->bytes_read += additional_bytes;
                header_buffer_size = new_size;
                continue;
            } else {
                oapv_mfree(frame_buffer);
                return ret;
            }
        }

        parse_success = 1;
    }

    if(!parse_success) {
        oapv_mfree(frame_buffer);
        return OAPV_ERR_MALFORMED_BITSTREAM;
    }

    // Calculate frame data offset for later use
    long header_consumed = BSR_GET_CUR(&pbu_bs) - pbu_bs.beg;
    location->frame_data_offset = location->frame_file_pos + 4 + header_consumed;

    oapv_mfree(frame_buffer);
    return OAPV_OK;
}

/*
 * Single-mip multi-tile selective decoder (backward compatibility wrapper).
 * Internally uses oapvd_decode_selective_multi_mips for implementation.
 */
int oapvd_decode_selective_multi(oapvd_t did, oapvd_istream_t *istream, oapv_selective_decode_t *sel_decode,
                                oapvm_t mid, oapvd_stat_t *stat)
{
    /* Validate input parameters */
    oapv_assert_rv(sel_decode, OAPV_ERR_INVALID_ARGUMENT);

    /* Create a single-entry multi-mip request wrapper */
    oapv_mip_request_t mip_request;
    memset(&mip_request, 0, sizeof(mip_request));

    /* Copy parameters from single-mip API to multi-mip structure */
    mip_request.mip_level = sel_decode->mip_level;
    mip_request.num_tiles = sel_decode->num_tiles;
    memcpy(mip_request.tile_coords, sel_decode->tile_coords, sizeof(sel_decode->tile_coords));
    mip_request.output_buffer = sel_decode->output_buffer;

    /* Create multi-mip decode structure with single mip */
    oapv_multi_mip_decode_t multi_mip_decode;
    multi_mip_decode.num_mips = 1;
    multi_mip_decode.mip_requests = &mip_request;

    /* Call the unified multi-mip implementation */
    int ret = oapvd_decode_selective_multi_mips(did, istream, &multi_mip_decode, mid, stat);

    /* Check per-mip status and propagate error if needed */
    if(OAPV_SUCCEEDED(ret) && OAPV_FAILED(mip_request.status)) {
        ret = mip_request.status;
    }

    /* Copy metadata back to caller's structure */
    sel_decode->actual_frame_width = mip_request.frame_width_mb_aligned;
    sel_decode->actual_frame_height = mip_request.frame_height_mb_aligned;
    sel_decode->actual_tile_width = mip_request.tile_width_mb_aligned;
    sel_decode->actual_tile_height = mip_request.tile_height_mb_aligned;
    sel_decode->bit_depth = mip_request.bit_depth;
    sel_decode->chroma_format_idc = mip_request.chroma_format_idc;

    return ret;
}

// Multi-mip worker structure for thread parallelism
typedef struct {
    oapvd_ctx_t *ctx;
    oapvd_core_t *core;
    tile_work_t *work_queue;
    int num_tiles;
    oapv_sync_obj_t sync_obj;
    volatile int *tiles_completed;
    perf_metrics_t *metrics;

    // Pipeline synchronization for batched I/O
    volatile int *tiles_ready;      // Count of tiles with data loaded (NOT_DECODED status)
    volatile int *io_complete;      // Flag: 1 when all I/O finished
    volatile int *next_tile_idx;    // Atomic counter for next tile to claim (eliminates O(n²) scanning)
} multi_mip_worker_t;

/*
 * Worker thread function for multi-mip tile decoding.
 * Processes tiles from a work queue, handling tiles from different mip levels
 * with thread-safe access to shared resources.
 */
static int dec_thread_tile_selective_multi_mip(void *arg)
{
    multi_mip_worker_t *worker = (multi_mip_worker_t*)arg;
    oapvd_ctx_t *ctx = worker->ctx;
    oapvd_core_t *core = worker->core;
    tile_work_t *work_queue = worker->work_queue;
    int num_tiles = worker->num_tiles;

    while(1) {
        // Atomically claim next tile index
        int tile_idx = oapv_tpool_atomic_inc(worker->sync_obj, worker->next_tile_idx) - 1;

        // Check if we've exceeded total tile count
        if(tile_idx >= num_tiles) {
            break;
        }

        tile_work_t *work = &work_queue[tile_idx];

        // Wait for tile data to be loaded (batched I/O)
        while(1) {
            oapv_tpool_enter_cs(worker->sync_obj);
            int current_status = work->status;
            oapv_tpool_leave_cs(worker->sync_obj);

            if(current_status != DEC_TILE_STAT_NOT_READY) {
                break;
            }

            // Wait for I/O batch to load this tile
            oapv_tpool_yield();
        }

        // Tile ownership is guaranteed via atomic counter.

        BEGIN_CPU_TRACE("DecodeTile");

        // Commenting out unnecessary ON_DECODING assignment.
        //
        //oapv_tpool_enter_cs(worker->sync_obj);
        //work->status = DEC_TILE_STAT_ON_DECODING;
        //oapv_tpool_leave_cs(worker->sync_obj);

        oapv_bs_t tile_bs;
        oapv_bsr_init(&tile_bs, work->data, work->size, NULL);

        oapvd_tile_t tile;
        memset(&tile, 0, sizeof(tile));

        /* Access shared mip context (read-only, safe for concurrent access) */
        const mip_context_t *mip_ctx = work->mip_ctx;

        int tile_w_config = mip_ctx->tile_width_in_mbs * OAPV_MB_W;
        int tile_h_config = mip_ctx->tile_height_in_mbs * OAPV_MB_H;

        int tile_x_start = work->col * tile_w_config;
        int tile_y_start = work->row * tile_h_config;
        int tile_x_end = (tile_x_start + tile_w_config < mip_ctx->frame_width) ?
                        tile_x_start + tile_w_config : mip_ctx->frame_width;
        int tile_y_end = (tile_y_start + tile_h_config < mip_ctx->frame_height) ?
                        tile_y_start + tile_h_config : mip_ctx->frame_height;

        tile.w = tile_x_end - tile_x_start;
        tile.h = tile_y_end - tile_y_start;

        /* Create thread-local context copy to avoid race conditions when
         * multiple threads decode tiles from different mip levels */
        oapvd_ctx_t local_ctx = *ctx;
        local_ctx.num_comp = mip_ctx->num_comp;
        memcpy(local_ctx.comp_sft, mip_ctx->comp_sft, sizeof(local_ctx.comp_sft));

        int ret = oapvd_vlc_tile_header(&tile_bs, &local_ctx, &tile.th);

        if(OAPV_FAILED(ret)) {
            work->status = DEC_TILE_STAT_ERROR;
            continue;
        }

        // Keep track of the tile header size to be able to calculate the start of each components 
        // from the start of the work buffer (see component loop below).
        size_t tile_header_size = (BSR_GET_CUR(&tile_bs))-tile_bs.beg;

        int num_comp = get_num_comp(mip_ctx->chroma_format_idc);
        oapv_assert_rv(mip_ctx->num_comp == num_comp, OAPV_ERR_INVALID_ARGUMENT);

        for(int c = 0; c < num_comp; c++) {
            core->qp[c] = tile.th.tile_qp[c];
            u8 dq_scale = oapv_tbl_dq_scale[core->qp[c] % 6];
            core->dq_shift[c] = mip_ctx->bit_depth - 2 - (core->qp[c] / 6);

            core->kparam_dc[c] = OAPV_KPARAM_DC_MAX;
            core->kparam_ac[c] = OAPV_KPARAM_AC_MIN;
            core->prev_dc[c] = 0;

            int midx = 0;
            for(int y = 0; y < OAPV_BLK_H; y++) {
                for(int x = 0; x < OAPV_BLK_W; x++) {
                    core->q_mat[c][midx++] = dq_scale * mip_ctx->q_matrix[c][y][x];
                }
            }
        }

        for(int c = 0; c < num_comp; c++) {
            if(!mip_ctx->output_buffer) continue;

            int comp_stride_bytes;
            u16 *tile_dst;

            if(mip_ctx->output_buffer->tiled_layout) {
                /* Tiled output: tile-major layout with planes interleaved within
                 * each tile. Each tile occupies `tile_size` bytes; plane c's data
                 * lives at the same intra-tile offset for every tile, and `a[c]`
                 * is pre-biased to point at that offset in tile 0. So tile (col,row)
                 * for component c starts at:
                 *     a[c] + (row * num_tile_cols + col) * tile_size
                 *
                 * With tile_for_comp.x/y reset to 0 below, dec_tile_comp's inner
                 * loop writes blocks at tile-local coords [0..tile_h_c) x
                 * [0..tile_w_c), so we just pass the per-tile row stride. */
                const int    tile_stride_c = mip_ctx->output_buffer->tile_stride[c];
                const int    ntc           = mip_ctx->output_buffer->num_tile_cols;
                const size_t tile_bytes    = (size_t)mip_ctx->output_buffer->tile_size;
                const size_t tile_offset   = ((size_t)work->row * (size_t)ntc + (size_t)work->col) * tile_bytes;
                tile_dst          = (u16*)((u8*)mip_ctx->output_buffer->a[c] + tile_offset);
                comp_stride_bytes = tile_stride_c;
            }
            else {
                comp_stride_bytes = mip_ctx->output_buffer->s[c];
                u8 *comp_base_bytes = (u8*)mip_ctx->output_buffer->a[c];

                int comp_tile_x_pixels = (c > 0 && mip_ctx->chroma_format_idc == 2) ?
                                        tile_x_start / 2 : tile_x_start;
                int comp_tile_y_pixels = tile_y_start;

                u8 *tile_dst_bytes = comp_base_bytes + (comp_tile_y_pixels * comp_stride_bytes) +
                                    (comp_tile_x_pixels * 2);
                tile_dst = (u16*)tile_dst_bytes;
            }

            oapv_bs_t comp_bs;

            // Calculate the start of the component in the work data: start_of_work_buffer + tile_header_size + prev_tile_comp_sizes
            size_t comp_data_offset = 0;
            for(int prev_c = 0; prev_c < c; prev_c++) {
                comp_data_offset += tile.th.tile_data_size[prev_c];
            }

            oapv_bsr_init(&comp_bs, work->data + tile_header_size + comp_data_offset,
                        tile.th.tile_data_size[c], NULL);

            oapvd_tile_t tile_for_comp = tile;
            tile_for_comp.x = 0;
            tile_for_comp.y = 0;

            ret = dec_tile_comp(&tile_for_comp, &local_ctx, core, &comp_bs, c, comp_stride_bytes, tile_dst);

            if(OAPV_FAILED(ret)) {
                work->status = DEC_TILE_STAT_ERROR;
                break;
            }
        }

        if(ret == OAPV_OK) {
            oapv_tpool_enter_cs(worker->sync_obj);
            work->status = DEC_TILE_STAT_DECODED;
            (*worker->tiles_completed)++;
            oapv_tpool_leave_cs(worker->sync_obj);
        }

        END_CPU_TRACE();
    }

    return OAPV_OK;
}

/*
 * Decodes selective tiles from multiple mip levels in a single operation.
 * Performs coalesced I/O for efficient random access and uses multi-threading
 * for parallel tile decoding across mip levels.
 *
 * Algorithm:
 * 1. Locate each requested mip level in the bitstream
 * 2. Parse frame headers and prepare decode context for each mip
 * 3. Build work queue of all requested tiles across all mips
 * 4. Coalesce I/O requests to minimize seeks
 * 5. Decode tiles in parallel using worker threads
 */
int oapvd_decode_selective_multi_mips(oapvd_t did, oapvd_istream_t *istream,
                                     oapv_multi_mip_decode_t *multi_mip_decode,
                                     oapvm_t mid, oapvd_stat_t *stat)
{
    oapvd_ctx_t *ctx;
    int ret = OAPV_OK;
    perf_metrics_t metrics = {0};

    ctx = dec_id_to_ctx(did);
    oapv_assert_rv(ctx, OAPV_ERR_INVALID_ARGUMENT);
    oapv_assert_rv(multi_mip_decode && multi_mip_decode->num_mips > 0, OAPV_ERR_INVALID_ARGUMENT);

    oapvd_invalidate_tile_cache(ctx);

    metrics.io_start_ns = get_time_ns();

    oapv_stream_info_t stream_info;
    ret = oapvd_validate_stream(istream, &stream_info, &metrics);
    if(OAPV_FAILED(ret)) {
        return ret;
    }

    int total_tiles = 0;
    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        total_tiles += multi_mip_decode->mip_requests[m].num_tiles;
    }

    tile_work_t *work_queue = (tile_work_t *)oapv_calloc(total_tiles, sizeof(tile_work_t));
    if(!work_queue) {
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    /* Allocate shared mip contexts (one per mip level) */
    mip_context_t *mip_contexts = (mip_context_t *)oapv_calloc(multi_mip_decode->num_mips, sizeof(mip_context_t));
    if(!mip_contexts) {
        oapv_mfree(mip_contexts);
        oapv_mfree(work_queue);
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    typedef struct {
        int num_comp;
        int comp_sft[N_C][2];
    } mip_decode_ctx_t;

    typedef struct {
        int mip_level;
        oapv_mip_request_t *mip_req;
        int64_t frame_file_offset;
        int64_t frame_data_offset;
        u32 pbu_size;
        oapv_fh_t frame_header;
        int num_tiles_in_frame;
        mip_decode_ctx_t decode_ctx;
        int found;
    } mip_info_t;

    mip_info_t *mip_infos = (mip_info_t*)oapv_calloc(multi_mip_decode->num_mips, sizeof(mip_info_t));
    if(!mip_infos) {
        oapv_mfree(mip_contexts);
        oapv_mfree(mip_contexts);
            oapv_mfree(work_queue);
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        mip_infos[m].mip_level = multi_mip_decode->mip_requests[m].mip_level;
        mip_infos[m].mip_req = &multi_mip_decode->mip_requests[m];
        mip_infos[m].found = 0;
        mip_infos[m].num_tiles_in_frame = 0;
        mip_infos[m].frame_header.tile_size = NULL;
    }

    BEGIN_CPU_TRACE("Locate Mips");

    /* Build array of requested mip levels for batch lookup */

    int *requested_mips = (int *)oapv_malloc(multi_mip_decode->num_mips * sizeof(int));

    if(!requested_mips) {
        oapv_mfree(mip_infos);
        oapv_mfree(mip_contexts);
        oapv_mfree(work_queue);
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        requested_mips[m] = mip_infos[m].mip_level;
    }

    /* Allocate locations array for batch results */
    oapv_mip_location_t *locations = (oapv_mip_location_t *)oapv_malloc(
        multi_mip_decode->num_mips * sizeof(oapv_mip_location_t));
    if(!locations) {
        oapv_mfree(requested_mips);
        oapv_mfree(mip_infos);
        oapv_mfree(mip_contexts);
        oapv_mfree(work_queue);
        return OAPV_ERR_OUT_OF_MEMORY;
    }

    /* Locate all requested mip levels in a single traversal */
    ret = oapvd_locate_all_mips(istream, &stream_info, requested_mips,
                                multi_mip_decode->num_mips, locations, &metrics);

    if(OAPV_FAILED(ret)) {
        oapv_mfree(locations);
        oapv_mfree(requested_mips);
        oapv_mfree(mip_infos);
        oapv_mfree(mip_contexts);
        oapv_mfree(work_queue);
        return ret;
    }

    /* Copy results back to mip_infos */
    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        if(locations[m].found) {
            mip_infos[m].frame_file_offset = locations[m].frame_file_pos;
            mip_infos[m].pbu_size = locations[m].pbu_size;
            mip_infos[m].found = 1;
        } else {
            mip_infos[m].found = 0;
        }
    }

    /* Clean up temporary arrays */
    oapv_mfree(locations);
    oapv_mfree(requested_mips);

    END_CPU_TRACE();

    BEGIN_CPU_TRACE("Parse headers");

    /* Parse frame headers and initialize decode context for each mip */
    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        mip_info_t *mip_info = &mip_infos[m];
        if(!mip_info->found) continue;

        oapv_mip_location_t location;
        location.frame_file_pos = mip_info->frame_file_offset;
        location.pbu_size = mip_info->pbu_size;
        location.found = 1;

        ret = oapvd_parse_frame_headers(istream, &location, ctx, &ctx->fh, &metrics);
        if(OAPV_FAILED(ret)) {
            mip_info->mip_req->status = ret;
            continue;
        }

        mip_info->frame_header = ctx->fh;
        mip_info->frame_data_offset = location.frame_data_offset;

        oapv_imgb_t dummy_imgb;
        memset(&dummy_imgb, 0, sizeof(dummy_imgb));
        dummy_imgb.cs = OAPV_CS_SET(OAPV_CF_YCBCR422, 10, 0);
        if(mip_info->mip_req->output_buffer) {
            dummy_imgb.cs = mip_info->mip_req->output_buffer->cs;
        }
        dummy_imgb.refcnt = 1;

        u8 dummy_tile_data = 0;
        oapv_bsr_init(&ctx->bs, &dummy_tile_data, 0, NULL);

        ret = dec_frm_prepare(ctx, &dummy_imgb);
        if(OAPV_FAILED(ret)) {
            mip_info->mip_req->status = ret;
            continue;
        }

        mip_info->decode_ctx.num_comp = ctx->num_comp;
        memcpy(mip_info->decode_ctx.comp_sft, ctx->comp_sft, sizeof(ctx->comp_sft));

        /* Transfer tile_size array ownership from ctx to mip_info */
        mip_info->num_tiles_in_frame = ctx->num_tiles;
        ctx->fh.tile_size = NULL;

        mip_info->mip_req->frame_width_mb_aligned = oapv_align_value(ctx->fh.fi.frame_width, OAPV_MB_W);
        mip_info->mip_req->frame_height_mb_aligned = oapv_align_value(ctx->fh.fi.frame_height, OAPV_MB_H);
        mip_info->mip_req->tile_width_mb_aligned = ctx->fh.tile_width_in_mbs * OAPV_MB_W;
        mip_info->mip_req->tile_height_mb_aligned = ctx->fh.tile_height_in_mbs * OAPV_MB_H;
        mip_info->mip_req->bit_depth = ctx->fh.fi.bit_depth;
        mip_info->mip_req->chroma_format_idc = ctx->fh.fi.chroma_format_idc;
        mip_info->mip_req->status = OAPV_OK;

    }

    END_CPU_TRACE();

    BEGIN_CPU_TRACE("Build Work Queue");

    /* Build work queue from all requested tiles across mips */
    int work_queue_idx = 0;
    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        mip_info_t *mip_info = &mip_infos[m];
        if(!mip_info->found) {
            mip_info->mip_req->status = OAPV_ERR_NOT_FOUND;
            continue;
        }

        if(mip_info->mip_req->output_buffer == NULL) {
            continue;
        }

        /* Populate shared mip context for this mip level */
        mip_context_t *mip_ctx = &mip_contexts[m];
        oapv_fh_t *fh = &mip_info->frame_header;

        mip_ctx->mip_level = mip_info->mip_level;
        mip_ctx->bit_depth = fh->fi.bit_depth;
        mip_ctx->chroma_format_idc = fh->fi.chroma_format_idc;
        mip_ctx->frame_width = fh->fi.frame_width;
        mip_ctx->frame_height = fh->fi.frame_height;
        mip_ctx->padded_frame_width = mip_info->mip_req->frame_width_mb_aligned;
        mip_ctx->padded_frame_height = mip_info->mip_req->frame_height_mb_aligned;
        mip_ctx->tile_width_in_mbs = fh->tile_width_in_mbs;
        mip_ctx->tile_height_in_mbs = fh->tile_height_in_mbs;
        mip_ctx->output_buffer = mip_info->mip_req->output_buffer;
        mip_ctx->num_comp = mip_info->decode_ctx.num_comp;
        memcpy(mip_ctx->q_matrix, fh->q_matrix, sizeof(mip_ctx->q_matrix));
        memcpy(mip_ctx->comp_sft, mip_info->decode_ctx.comp_sft, sizeof(mip_ctx->comp_sft));

        int frame_width_in_mbs = (fh->fi.frame_width + OAPV_MB_W - 1) / OAPV_MB_W;
        int frame_height_in_mbs = (fh->fi.frame_height + OAPV_MB_H - 1) / OAPV_MB_H;
        int tiles_per_row = (frame_width_in_mbs + fh->tile_width_in_mbs - 1) / fh->tile_width_in_mbs;
        int tiles_per_col = (frame_height_in_mbs + fh->tile_height_in_mbs - 1) / fh->tile_height_in_mbs;

        /* Validate all tile coordinates for this mip before creating work items */
        int has_invalid_tiles = 0;
        for(int t = 0; t < mip_info->mip_req->num_tiles; t++) {
            int col = mip_info->mip_req->tile_coords[t * 2];
            int row = mip_info->mip_req->tile_coords[t * 2 + 1];

            if(col < 0 || col >= tiles_per_row || row < 0 || row >= tiles_per_col) {
                log_msg(OAPV_LOG_ERROR, "Invalid tile coordinates (%d,%d) for mip %d (valid range: 0-%d, 0-%d)\n",
                        col, row, mip_info->mip_level, tiles_per_row - 1, tiles_per_col - 1);
                has_invalid_tiles = 1;
            }
        }

        if(has_invalid_tiles) {
            mip_info->mip_req->status = OAPV_ERR_INVALID_ARGUMENT;
            continue; /* Skip entire mip if any tile coordinate is invalid */
        }

        /* Create work items for each requested tile, pointing to shared mip context */
        for(int t = 0; t < mip_info->mip_req->num_tiles; t++) {
            int col = mip_info->mip_req->tile_coords[t * 2];
            int row = mip_info->mip_req->tile_coords[t * 2 + 1];
            int tile_idx = row * tiles_per_row + col;

            tile_work_t *work = &work_queue[work_queue_idx++];
            work->tile_idx = tile_idx;
            work->col = col;
            work->row = row;
            work->status = DEC_TILE_STAT_NOT_READY;  /* Tile data not yet loaded */
            work->mip_ctx = mip_ctx;  /* Point to shared mip context */

            if(fh->tile_size != NULL && tile_idx < OAPV_MAX_TILES) {
                work->size = fh->tile_size[tile_idx];
            } else {
                work->size = 65536;
            }

            work->file_offset = mip_info->frame_data_offset;
            for(int prev = 0; prev < tile_idx; prev++) {
                if(fh->tile_size != NULL && prev < OAPV_MAX_TILES) {
                    work->file_offset += fh->tile_size[prev];
                    work->file_offset += 4;
                }
            }
            work->file_offset += 4;

        }
    }

    END_CPU_TRACE();

    for(int m = 0; m < multi_mip_decode->num_mips; m++) {
        if(mip_infos[m].frame_header.tile_size) {
            oapv_mfree(mip_infos[m].frame_header.tile_size);
            mip_infos[m].frame_header.tile_size = NULL;
        }
    }
    oapv_mfree(mip_infos);

    /* Early return if no tiles to decode (metadata-only call) */
    if(work_queue_idx == 0) {
        oapv_mfree(work_queue);
        oapv_mfree(mip_contexts);
        return OAPV_OK;
    }

    /* Sort work queue by file offset to enable coalesced I/O */
    for(int i = 0; i < work_queue_idx - 1; i++) {
        for(int j = i + 1; j < work_queue_idx; j++) {
            if(work_queue[j].file_offset < work_queue[i].file_offset) {
                tile_work_t temp = work_queue[i];
                work_queue[i] = work_queue[j];
                work_queue[j] = temp;
            }
        }
    }

    /* Coalesce adjacent tiles into larger read blocks */
    const u64 COALESCE_THRESHOLD = 64 * 1024;
    tile_read_block_t *read_blocks = (tile_read_block_t*)oapv_calloc(work_queue_idx, sizeof(tile_read_block_t));
    int num_blocks = 0;

    if(work_queue_idx > 0) {
        read_blocks[0].start_offset = work_queue[0].file_offset;
        read_blocks[0].end_offset = work_queue[0].file_offset + work_queue[0].size;
        read_blocks[0].first_tile_idx = 0;
        read_blocks[0].num_tiles = 1;
        num_blocks = 1;

        for(int i = 1; i < work_queue_idx; i++) {
            u64 gap = work_queue[i].file_offset - read_blocks[num_blocks-1].end_offset;

            if(gap <= COALESCE_THRESHOLD) {
                read_blocks[num_blocks-1].end_offset = work_queue[i].file_offset + work_queue[i].size;
                read_blocks[num_blocks-1].num_tiles++;
            } else {
                read_blocks[num_blocks].start_offset = work_queue[i].file_offset;
                read_blocks[num_blocks].end_offset = work_queue[i].file_offset + work_queue[i].size;
                read_blocks[num_blocks].first_tile_idx = i;
                read_blocks[num_blocks].num_tiles = 1;
                num_blocks++;
            }
        }
    }

    /* Pre-allocate all read block buffers (done once for simplicity) */
    for(int b = 0; b < num_blocks; b++) {
        read_blocks[b].total_size = (u32)(read_blocks[b].end_offset - read_blocks[b].start_offset);
        read_blocks[b].buffer = (u8*)oapv_malloc(read_blocks[b].total_size);
        if(!read_blocks[b].buffer) {
            for(int j = 0; j < b; j++) {
                oapv_mfree(read_blocks[j].buffer);
            }
            oapv_mfree(read_blocks);
            oapv_mfree(mip_contexts);
            oapv_mfree(work_queue);
            return OAPV_ERR_OUT_OF_MEMORY;
        }
    }

    int num_threads = ctx->threads;
    if(num_threads <= 0) num_threads = 1;

    /* Pipeline synchronization variables */
    oapv_sync_obj_t sync_obj = oapv_tpool_sync_obj_create();
    volatile int tiles_completed = 0;
    volatile int tiles_ready = 0;      // Count of tiles ready to decode
    volatile int io_complete = 0;       // Flag: all I/O finished
    volatile int next_tile_idx = 0;     // Atomic counter for tile claiming

    /* Load first batch BEFORE starting worker threads to avoid spinning */
    BEGIN_CPU_TRACE("Batched I/O");

    int batch_size = num_threads;  // Tiles per batch
    int tiles_loaded = 0;
    int block_idx = 0;  // Track current read block

    /* Load first batch synchronously */
    int tile_idx_in_block = 0;  // Track position within current block
    while(tiles_loaded < batch_size && block_idx < num_blocks) {
        int b = block_idx;

        /* Read coalesced block if we're starting a new block */
        if(tile_idx_in_block == 0) {
            istream->seek(istream, (int64_t)read_blocks[b].start_offset, SEEK_SET);
            istream->read(istream, read_blocks[b].buffer, read_blocks[b].total_size, 1);
            metrics.bytes_read += read_blocks[b].total_size;
        }

        /* Assign data pointers for tiles in this block */
        for(int t = tile_idx_in_block; t < read_blocks[b].num_tiles; t++) {
            int tile_idx = read_blocks[b].first_tile_idx + t;
            u64 tile_offset_in_block = work_queue[tile_idx].file_offset - read_blocks[b].start_offset;
            work_queue[tile_idx].data = read_blocks[b].buffer + tile_offset_in_block;
            work_queue[tile_idx].status = DEC_TILE_STAT_NOT_DECODED;  /* Mark ready */
            tiles_loaded++;

            if(tiles_loaded >= batch_size) {
                tile_idx_in_block = t + 1;  /* Remember where we stopped */
                goto first_batch_done;  /* Exit both loops */
            }
        }

        /* Finished this block, move to next */
        block_idx++;
        tile_idx_in_block = 0;
    }

first_batch_done:
    tiles_ready = tiles_loaded;

    metrics.decode_start_ns = get_time_ns();

    /* Now start worker threads - they immediately find work ready */
    multi_mip_worker_t worker;
    worker.ctx = ctx;
    worker.core = ctx->core[num_threads - 1];  /* Main thread will use the last one */
    worker.work_queue = work_queue;
    worker.num_tiles = work_queue_idx;
    worker.sync_obj = sync_obj;
    worker.tiles_completed = &tiles_completed;
    worker.tiles_ready = &tiles_ready;
    worker.io_complete = &io_complete;
    worker.next_tile_idx = &next_tile_idx;
    worker.metrics = &metrics;

    oapv_tpool_t *tpool = ctx->tpool;
    multi_mip_worker_t **thread_workers = NULL;
    int num_worker_threads = num_threads;

    if(num_threads > 1) {
        thread_workers = (multi_mip_worker_t **)oapv_malloc(num_worker_threads * sizeof(multi_mip_worker_t *));
        if(!thread_workers) {
            oapv_tpool_sync_obj_delete(&sync_obj);
            for(int b = 0; b < num_blocks; b++) {
                oapv_mfree(read_blocks[b].buffer);
            }
            oapv_mfree(read_blocks);
            oapv_mfree(mip_contexts);
            oapv_mfree(work_queue);
            return OAPV_ERR_OUT_OF_MEMORY;
        }

        for(int t = 0; t < num_worker_threads; t++) {
            thread_workers[t] = (multi_mip_worker_t *)oapv_malloc(sizeof(multi_mip_worker_t));
            if(!thread_workers[t]) {
                for(int j = 0; j < t; j++) {
                    oapv_mfree(thread_workers[j]);
                }
                oapv_mfree(thread_workers);
                oapv_tpool_sync_obj_delete(&sync_obj);
                for(int b = 0; b < num_blocks; b++) {
                    oapv_mfree(read_blocks[b].buffer);
                }
                oapv_mfree(read_blocks);
                oapv_mfree(mip_contexts);
                oapv_mfree(work_queue);
                return OAPV_ERR_OUT_OF_MEMORY;
            }
            *thread_workers[t] = worker;
            thread_workers[t]->core = ctx->core[t];

            tpool->run(ctx->thread_id[t], dec_thread_tile_selective_multi_mip, thread_workers[t]);
        }
    }

    /* Main thread continues loading remaining batches while workers decode */
    for(int b = block_idx; b < num_blocks; b++) {
        /* Read coalesced block if not already read (first block may be partially processed) */
        int start_tile = (b == block_idx) ? tile_idx_in_block : 0;

        if(start_tile == 0) {
            istream->seek(istream, (int64_t)read_blocks[b].start_offset, SEEK_SET);
            istream->read(istream, read_blocks[b].buffer, read_blocks[b].total_size, 1);
            metrics.bytes_read += read_blocks[b].total_size;
        }

        /* Assign data pointers and mark tiles ready in batches */
        for(int t = start_tile; t < read_blocks[b].num_tiles; t++) {
            int tile_idx = read_blocks[b].first_tile_idx + t;
            u64 tile_offset_in_block = work_queue[tile_idx].file_offset - read_blocks[b].start_offset;
            work_queue[tile_idx].data = read_blocks[b].buffer + tile_offset_in_block;
            tiles_loaded++;

            /* Batch boundary: make tiles available to workers */
            if(tiles_loaded % batch_size == 0 || tile_idx == work_queue_idx - 1) {

                int batch_start = tiles_loaded - (tiles_loaded % batch_size);

                if(tiles_loaded % batch_size == 0) {
                    batch_start = tiles_loaded - batch_size;
                } else {
                    batch_start = (tiles_loaded / batch_size) * batch_size;
                }
                int batch_end = tiles_loaded;

                oapv_tpool_enter_cs(sync_obj);

                /* Tag batch as read but not decoded yet (NOT_READY -> NOT_DECODED) */

                for(int i = batch_start; i < batch_end; i++) {
                    if(work_queue[i].status == DEC_TILE_STAT_NOT_READY) {
                        work_queue[i].status = DEC_TILE_STAT_NOT_DECODED;
                    }
                }

                /* Signal workers that there are new tiles available for decoding */
                tiles_ready = batch_end;

                oapv_tpool_leave_cs(sync_obj);
            }
        }
    }

    /* Signal I/O complete */
    io_complete = 1;
    metrics.io_end_ns = get_time_ns();

    END_CPU_TRACE();

    /* Main thread helps decode after I/O completes */
    if(num_threads > 1) {
        /* Main thread participates in decoding */
        dec_thread_tile_selective_multi_mip(&worker);

        /* Wait for worker threads to complete */
        for(int t = 0; t < num_worker_threads; t++) {
            int thread_ret;
            tpool->join(ctx->thread_id[t], &thread_ret);
            oapv_mfree(thread_workers[t]);
        }
        oapv_mfree(thread_workers);
    } else {
        /* Single-threaded: main thread does all work */
        dec_thread_tile_selective_multi_mip(&worker);
    }

    oapv_tpool_sync_obj_delete(&sync_obj);
    for(int b = 0; b < num_blocks; b++) {
        oapv_mfree(read_blocks[b].buffer);
    }
    oapv_mfree(read_blocks);
    oapv_mfree(mip_contexts);
            oapv_mfree(work_queue);

    metrics.decode_end_ns = get_time_ns();
    metrics.tiles_decoded = tiles_completed;

    double io_time_ms = (metrics.io_end_ns - metrics.io_start_ns) / 1000000.0;
    double decode_time_ms = (metrics.decode_end_ns - metrics.decode_start_ns) / 1000000.0;
    double total_time_ms = (metrics.decode_end_ns - metrics.io_start_ns) / 1000000.0;

    log_msg(OAPV_LOG_INFO, "\nMulti-Mip Performance:\n");
    log_msg(OAPV_LOG_INFO, "  Mips decoded: %d\n", multi_mip_decode->num_mips);
    log_msg(OAPV_LOG_INFO, "  Total tiles: %d\n", work_queue_idx);
    log_msg(OAPV_LOG_INFO, "  I/O time: %.2f ms\n", io_time_ms);
    log_msg(OAPV_LOG_INFO, "  Decode time: %.2f ms\n", decode_time_ms);
    log_msg(OAPV_LOG_INFO, "  Total time: %.2f ms\n", total_time_ms);
    log_msg(OAPV_LOG_INFO, "  Bytes read: %u\n", metrics.bytes_read);
    log_msg(OAPV_LOG_INFO, "  Throughput: %.2f tiles/sec\n", work_queue_idx * 1000.0 / total_time_ms);

    if(stat) {
        stat->read = metrics.bytes_read;
    }

    return ret;
}

#endif // ENABLE_DECODER
///////////////////////////////////////////////////////////////////////////////

const char *oapv_version(unsigned int *ver_num)
{
    static char oapv_version_string[16];
    snprintf(oapv_version_string, sizeof(oapv_version_string), "%d.%d.%d.%d",
        OAPV_VER_APISET, OAPV_VER_MAJOR, OAPV_VER_MINOR, OAPV_VER_PATCH);

    if(ver_num != NULL)
        *ver_num = OAPV_VER_NUM;

    return (char*)oapv_version_string;
}

void oapv_set_logging_callback(oapv_log_callback_t callback, void* user_data)
{
    current_log_callback = callback;
    current_log_user_data = user_data;
}

void oapv_set_logging_verbosity(int verbosity)
{
    current_log_verbosity = verbosity;
}

int oapv_set_cputrace_callbacks(const oapv_cputrace_callbacks_t *callbacks)
{
    // Make sure all the callbacks are properly set.
    if(callbacks->begin_event && callbacks->end_event) {
        cputrace_callbacks = *callbacks;
        return OAPV_OK;
    }
    return OAPV_ERR_INVALID_ARGUMENT;
}