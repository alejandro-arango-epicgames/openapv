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

/* Selective tile decoding, checked against whole-picture decoding.
 *
 * usage: oapv_test_decode_tiles <a.apv> <b.apv> <c.apv> <d.apv>
 *
 *   a, b, c : 4:2:2 10-bit bitstreams, batched together. They should differ in
 *             resolution, tile grid and quantization matrix, and one should
 *             carry the tile sizes in its frame header, so that a batch that
 *             took any of those from the wrong frame decodes wrongly.
 *   d       : a bitstream whose bit depth differs from the others, to check a
 *             request whose frames disagree about sample layout is rejected.
 *
 * Tiles are picked from several frames and decoded into per-tile buffers
 * three ways: one oapvd_decode_tiles() call per frame, one
 * oapvd_decode_tiles_multi() call for all of them, and the same batch in
 * reverse order. Every tile must match the same region of an
 * oapvd_decode_frame() decode of its frame, sample for sample.
 *
 * Failing requests are checked to leave every destination untouched, and the
 * decoder to decode correctly after them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oapv.h"

#define MAX_FRMS       8
#define TILES_PER_FRM  6
#define SENTINEL       0xA5

static int g_fails = 0;

static void check(int cond, const char *what)
{
    printf("%s - %s\n", cond ? "ok  " : "FAIL", what);
    if(!cond) {
        g_fails++;
    }
}

/* one frame PBU of a bitstream file, and what oapvd_info_frame() says of it */
typedef struct {
    unsigned char  *pbu;
    int             pbu_size;
    oapv_frm_info_t fi;
} frame_t;

static unsigned int rd32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static unsigned char *load(const char *path, long *size)
{
    FILE          *fp = fopen(path, "rb");
    unsigned char *buf = NULL;

    if(fp == NULL) {
        return NULL;
    }
    if(fseek(fp, 0, SEEK_END) == 0 && (*size = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0) {
        buf = (unsigned char *)malloc((size_t)*size);
        if(buf != NULL && fread(buf, 1, (size_t)*size, fp) != (size_t)*size) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(fp);
    return buf;
}

/* the frame PBUs of every access unit in a raw APV file, in order: each access
   unit is its size, the 'aPv1' signature, then PBUs each prefixed by theirs */
static int find_frames(unsigned char *buf, long size, frame_t *frms, int max_frms)
{
    long pos = 0;
    int  n = 0;

    while(pos + 8 <= size && n < max_frms) {
        long au_end = pos + 4 + (long)rd32(buf + pos);
        if(au_end > size || rd32(buf + pos + 4) != 0x61507631) {
            break;
        }
        pos += 8;
        while(pos + 4 <= au_end && n < max_frms) {
            long pbu_size = (long)rd32(buf + pos);
            if(pbu_size <= 0 || pos + 4 + pbu_size > au_end) {
                return -1;
            }
            // a PBU that is not a frame (metadata, filler) is not reported as one
            if(OAPV_SUCCEEDED(oapvd_info_frame(buf + pos + 4, (int)pbu_size, &frms[n].fi))) {
                frms[n].pbu = buf + pos + 4;
                frms[n].pbu_size = (int)pbu_size;
                n++;
            }
            pos += 4 + pbu_size;
        }
        pos = au_end;
    }
    return n;
}

/* the width and height shifts of plane 'p', and the number of planes */
static int plane_layout(int cs, int p, int *sft_w, int *sft_h)
{
    int f = OAPV_CS_GET_FORMAT(cs);

    *sft_w = (p > 0 && (f == OAPV_CF_YCBCR420 || f == OAPV_CF_YCBCR422)) ? 1 : 0;
    *sft_h = (p > 0 && f == OAPV_CF_YCBCR420) ? 1 : 0;
    return (f == OAPV_CF_YCBCR400) ? 1 : (f == OAPV_CF_YCBCR4444) ? 4 : 3;
}

/* A whole-picture buffer. The decoder writes whole macroblocks, so storage
   covers the macroblock-aligned picture while w/h report the signalled size,
   which is what oapvd_decode_frame() checks the buffer against. */
static oapv_imgb_t *picture_create(const oapv_frm_info_t *fi)
{
    oapv_imgb_t *imgb = (oapv_imgb_t *)calloc(1, sizeof(oapv_imgb_t));
    int          aw = (fi->w + OAPV_MB_W - 1) / OAPV_MB_W * OAPV_MB_W;
    int          ah = (fi->h + OAPV_MB_H - 1) / OAPV_MB_H * OAPV_MB_H;
    int          bd = OAPV_CS_GET_BYTE_DEPTH(fi->cs);
    int          sft_w, sft_h;

    if(imgb == NULL) {
        return NULL;
    }
    imgb->cs = fi->cs;
    imgb->np = plane_layout(fi->cs, 0, &sft_w, &sft_h);
    for(int p = 0; p < imgb->np; p++) {
        plane_layout(fi->cs, p, &sft_w, &sft_h);
        imgb->w[p] = fi->w >> sft_w;
        imgb->h[p] = fi->h >> sft_h;
        imgb->aw[p] = aw >> sft_w;
        imgb->ah[p] = ah >> sft_h;
        imgb->s[p] = imgb->aw[p] * bd;
        imgb->e[p] = imgb->ah[p];
        imgb->bsize[p] = imgb->s[p] * imgb->ah[p];
        imgb->a[p] = calloc(1, (size_t)imgb->bsize[p]);
        if(imgb->a[p] == NULL) {
            return NULL;
        }
    }
    return imgb;
}

static void picture_delete(oapv_imgb_t *imgb)
{
    if(imgb != NULL) {
        for(int p = 0; p < imgb->np; p++) {
            free(imgb->a[p]);
        }
        free(imgb);
    }
}

/* where tile 'idx' of a frame sits in the picture, in luma samples, clipped to
   the macroblock-aligned picture as the decoder clips it */
static void tile_rect(const oapv_frm_info_t *fi, int idx, int *x, int *y, int *w, int *h)
{
    int tw = fi->tile_width_in_mbs * OAPV_MB_W;
    int th = fi->tile_height_in_mbs * OAPV_MB_H;
    int aw = (fi->w + OAPV_MB_W - 1) / OAPV_MB_W * OAPV_MB_W;
    int ah = (fi->h + OAPV_MB_H - 1) / OAPV_MB_H * OAPV_MB_H;

    *x = (idx % fi->tile_cols) * tw;
    *y = (idx / fi->tile_cols) * th;
    *w = (*x + tw > aw) ? aw - *x : tw;
    *h = (*y + th > ah) ? ah - *y : th;
}

/* the bytes one tile needs with every plane packed at its own width */
static size_t tile_bytes(const oapv_frm_info_t *fi, int idx)
{
    int    x, y, w, h, sft_w, sft_h;
    int    bd = OAPV_CS_GET_BYTE_DEPTH(fi->cs);
    int    np = plane_layout(fi->cs, 0, &sft_w, &sft_h);
    size_t total = 0;

    tile_rect(fi, idx, &x, &y, &w, &h);
    for(int p = 0; p < np; p++) {
        plane_layout(fi->cs, p, &sft_w, &sft_h);
        total += (size_t)(w >> sft_w) * bd * (h >> sft_h);
    }
    return total;
}

/* A selection spread over the frame, in ascending index, always including the
   last tile: at the bottom-right corner it is the one most likely to be
   clipped by the picture edge. */
static int pick_tiles(const oapv_frm_info_t *fi, int *idx)
{
    int n = 0;
    int step = fi->num_tiles / TILES_PER_FRM;

    if(step < 1) {
        step = 1;
    }
    for(int i = step / 2; i < fi->num_tiles && n < TILES_PER_FRM - 1; i += step) {
        idx[n++] = i;
    }
    if(n == 0 || idx[n - 1] != fi->num_tiles - 1) {
        idx[n++] = fi->num_tiles - 1;
    }
    return n;
}

/* the destinations of one run over the batch: every selected tile of every
   frame, back to back in one buffer, so comparing two runs is one memcmp and a
   write outside a tile's own destination cannot hide in allocator slack */
typedef struct {
    unsigned char   *buf;
    size_t           size;
    oapv_tile_req_t  reqs[MAX_FRMS][TILES_PER_FRM];
} dest_t;

static int dest_init(dest_t *d, const frame_t **frms, int num_frms, int sel[][TILES_PER_FRM], const int *num_sel)
{
    size_t off = 0;

    memset(d, 0, sizeof(dest_t));
    for(int f = 0; f < num_frms; f++) {
        for(int t = 0; t < num_sel[f]; t++) {
            d->size += tile_bytes(&frms[f]->fi, sel[f][t]);
        }
    }
    d->buf = (unsigned char *)malloc(d->size);
    if(d->buf == NULL) {
        return -1;
    }

    for(int f = 0; f < num_frms; f++) {
        const oapv_frm_info_t *fi = &frms[f]->fi;
        int                    bd = OAPV_CS_GET_BYTE_DEPTH(fi->cs);

        for(int t = 0; t < num_sel[f]; t++) {
            oapv_tile_req_t *req = &d->reqs[f][t];
            int              x, y, w, h, sft_w, sft_h;
            int              np = plane_layout(fi->cs, 0, &sft_w, &sft_h);

            tile_rect(fi, sel[f][t], &x, &y, &w, &h);
            req->idx = sel[f][t];
            req->imgb.cs = fi->cs;
            for(int p = 0; p < np; p++) {
                plane_layout(fi->cs, p, &sft_w, &sft_h);
                req->imgb.a[p] = d->buf + off;
                req->imgb.s[p] = (w >> sft_w) * bd;
                req->imgb.bsize[p] = req->imgb.s[p] * (h >> sft_h);
                off += (size_t)req->imgb.bsize[p];
            }
        }
    }
    return 0;
}

static int dest_untouched(const dest_t *d)
{
    for(size_t i = 0; i < d->size; i++) {
        if(d->buf[i] != SENTINEL) {
            return 0;
        }
    }
    return 1;
}

/* every selected tile against the same region of its frame decoded whole */
static int matches_pictures(const dest_t *d, const frame_t **frms, oapv_imgb_t **pics, int num_frms, int sel[][TILES_PER_FRM], const int *num_sel)
{
    for(int f = 0; f < num_frms; f++) {
        const oapv_frm_info_t *fi = &frms[f]->fi;
        int                    bd = OAPV_CS_GET_BYTE_DEPTH(fi->cs);

        for(int t = 0; t < num_sel[f]; t++) {
            const oapv_tile_req_t *req = &d->reqs[f][t];
            int                    x, y, w, h, sft_w, sft_h;
            int                    np = plane_layout(fi->cs, 0, &sft_w, &sft_h);

            tile_rect(fi, sel[f][t], &x, &y, &w, &h);
            for(int p = 0; p < np; p++) {
                plane_layout(fi->cs, p, &sft_w, &sft_h);
                for(int r = 0; r < (h >> sft_h); r++) {
                    const unsigned char *a = (const unsigned char *)req->imgb.a[p] + (size_t)r * req->imgb.s[p];
                    const unsigned char *b = (const unsigned char *)pics[f]->a[p] + (size_t)((y >> sft_h) + r) * pics[f]->s[p] + (size_t)(x >> sft_w) * bd;
                    if(memcmp(a, b, (size_t)(w >> sft_w) * bd) != 0) {
                        printf("       frame %d tile %d plane %d row %d differs\n", f, sel[f][t], p, r);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static void set_bitb(oapv_bitb_t *bitb, const frame_t *frm, int ssize)
{
    memset(bitb, 0, sizeof(oapv_bitb_t));
    bitb->addr = frm->pbu;
    bitb->ssize = ssize;
    bitb->bsize = frm->pbu_size;
}

static void set_frame_reqs(oapv_frame_req_t *reqs, oapv_bitb_t *bitbs, dest_t *d, const int *num_sel, int num_frms, int reverse)
{
    for(int i = 0; i < num_frms; i++) {
        int f = reverse ? num_frms - 1 - i : i;
        reqs[i].bitb = &bitbs[f];
        reqs[i].num_tiles = num_sel[f];
        reqs[i].tile_reqs = d->reqs[f];
    }
}

int main(int argc, char **argv)
{
    static frame_t    all[4][MAX_FRMS];
    unsigned char    *files[4] = { NULL };
    int               num_all[4];
    const frame_t    *frms[MAX_FRMS];
    oapv_imgb_t      *pics[MAX_FRMS] = { NULL };
    oapv_bitb_t       bitbs[MAX_FRMS];
    oapv_frame_req_t  frame_reqs[MAX_FRMS];
    static int        sel[MAX_FRMS][TILES_PER_FRM];
    int               num_sel[MAX_FRMS];
    static dest_t     per_frm, batch, rbatch, odd_dest;
    int               num_frms = 0;
    int               ret;
    char              what[128];

    if(argc != 5) {
        fprintf(stderr, "usage: %s <a.apv> <b.apv> <c.apv> <other-bit-depth.apv>\n", argv[0]);
        return 2;
    }
    for(int i = 0; i < 4; i++) {
        long size = 0;
        files[i] = load(argv[i + 1], &size);
        num_all[i] = (files[i] != NULL) ? find_frames(files[i], size, all[i], MAX_FRMS) : -1;
        if(num_all[i] < 1) {
            fprintf(stderr, "cannot read the frames of %s\n", argv[i + 1]);
            return 2;
        }
    }

    // the batch, alternating between the files so that neighbouring frames of
    // the request differ: the first two frames of a, one each of b and c
    frms[num_frms++] = &all[0][0];
    frms[num_frms++] = &all[1][0];
    frms[num_frms++] = &all[2][0];
    if(num_all[0] > 1) {
        frms[num_frms++] = &all[0][1];
    }

    for(int f = 0; f < num_frms; f++) {
        num_sel[f] = pick_tiles(&frms[f]->fi, sel[f]);
        set_bitb(&bitbs[f], frms[f], frms[f]->pbu_size);
    }
    if(dest_init(&per_frm, frms, num_frms, sel, num_sel) || dest_init(&batch, frms, num_frms, sel, num_sel) ||
       dest_init(&rbatch, frms, num_frms, sel, num_sel)) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    // the same checks on several threads and on one: a single-threaded decoder
    // takes every tile itself, so only several threads share the work list
    for(int pass = 0; pass < 2; pass++) {
        int           threads = pass ? 1 : 4;
        oapvd_cdesc_t cdesc;
        oapvd_t       did;

        memset(&cdesc, 0, sizeof(cdesc));
        cdesc.threads = threads;
        did = oapvd_create(&cdesc, &ret);
        if(did == NULL) {
            fprintf(stderr, "cannot create decoder (%d)\n", ret);
            return 2;
        }
        printf("# %d thread(s), %d frames, ", threads, num_frms);
        for(int f = 0; f < num_frms; f++) {
            printf("%dx%d:%d ", frms[f]->fi.w, frms[f]->fi.h, num_sel[f]);
        }
        printf("tiles\n");

        // the reference, decoded whole; its buffers are kept for both passes
        if(pass == 0) {
            int ok = 1;
            for(int f = 0; f < num_frms && ok; f++) {
                oapvd_stat_t stat;
                pics[f] = picture_create(&frms[f]->fi);
                ok = pics[f] != NULL && OAPV_SUCCEEDED(oapvd_decode_frame(did, &bitbs[f], pics[f], &stat));
            }
            check(ok, "whole-picture reference decode");
            if(!ok) {
                return 1;
            }
        }

        // one call per frame
        memset(per_frm.buf, SENTINEL, per_frm.size);
        ret = OAPV_OK;
        for(int f = 0; f < num_frms && OAPV_SUCCEEDED(ret); f++) {
            ret = oapvd_decode_tiles(did, &bitbs[f], num_sel[f], per_frm.reqs[f]);
        }
        check(OAPV_SUCCEEDED(ret), "oapvd_decode_tiles() per frame");
        check(matches_pictures(&per_frm, frms, pics, num_frms, sel, num_sel), "per-frame tiles match the whole-picture decode");

        // one call for every frame; the decoder has held one frame until now,
        // so this also grows what it keeps per frame
        memset(batch.buf, SENTINEL, batch.size);
        set_frame_reqs(frame_reqs, bitbs, &batch, num_sel, num_frms, 0);
        ret = oapvd_decode_tiles_multi(did, num_frms, frame_reqs);
        check(OAPV_SUCCEEDED(ret), "oapvd_decode_tiles_multi() over all frames");
        check(matches_pictures(&batch, frms, pics, num_frms, sel, num_sel), "batched tiles match the whole-picture decode");
        check(memcmp(batch.buf, per_frm.buf, batch.size) == 0, "batched and per-frame destinations are identical");

        // the frames in the opposite order
        memset(rbatch.buf, SENTINEL, rbatch.size);
        set_frame_reqs(frame_reqs, bitbs, &rbatch, num_sel, num_frms, 1);
        ret = oapvd_decode_tiles_multi(did, num_frms, frame_reqs);
        check(OAPV_SUCCEEDED(ret) && memcmp(rbatch.buf, batch.buf, batch.size) == 0, "frame order does not change the output");

        // failing requests must leave every destination untouched, including
        // those of the frames ahead of the one that fails
        memset(batch.buf, SENTINEL, batch.size);
        set_frame_reqs(frame_reqs, bitbs, &batch, num_sel, num_frms, 0);

        ret = oapvd_decode_tiles_multi(did, 0, frame_reqs);
        check(ret == OAPV_ERR_INVALID_ARGUMENT, "no frames is rejected");
        ret = oapvd_decode_tiles_multi(did, num_frms, NULL);
        check(ret == OAPV_ERR_INVALID_ARGUMENT, "no requests is rejected");

        if(num_sel[1] > 1) {
            int keep = batch.reqs[1][1].idx;
            batch.reqs[1][1].idx = batch.reqs[1][0].idx;
            ret = oapvd_decode_tiles_multi(did, num_frms, frame_reqs);
            batch.reqs[1][1].idx = keep;
            check(ret == OAPV_ERR_INVALID_ARGUMENT && dest_untouched(&batch), "a repeated tile in a later frame is rejected before any tile is written");
        }

        {
            // a frame cut short inside its tile data
            oapv_bitb_t cut = bitbs[num_frms - 1];
            frame_reqs[num_frms - 1].bitb = &cut;
            cut.ssize = frms[num_frms - 1]->pbu_size / 2;
            ret = oapvd_decode_tiles_multi(did, num_frms, frame_reqs);
            frame_reqs[num_frms - 1].bitb = &bitbs[num_frms - 1];
            check(ret == OAPV_ERR_MALFORMED_BITSTREAM && dest_untouched(&batch), "a truncated last frame is rejected before any tile is written");
        }

        {
            // a frame of a different bit depth appended to the batch
            const frame_t  *odd = &all[3][0];
            oapv_bitb_t     odd_bitb;
            const frame_t  *odd_frms[1] = { odd };
            int             odd_sel[1][TILES_PER_FRM];
            int             odd_num_sel[1];

            odd_num_sel[0] = pick_tiles(&odd->fi, odd_sel[0]);
            set_bitb(&odd_bitb, odd, odd->pbu_size);
            if(pass == 0 && dest_init(&odd_dest, odd_frms, 1, odd_sel, odd_num_sel)) {
                fprintf(stderr, "out of memory\n");
                return 2;
            }
            frame_reqs[num_frms].bitb = &odd_bitb;
            frame_reqs[num_frms].num_tiles = odd_num_sel[0];
            frame_reqs[num_frms].tile_reqs = odd_dest.reqs[0];
            memset(odd_dest.buf, SENTINEL, odd_dest.size);

            snprintf(what, sizeof(what), "a %d-bit frame in a %d-bit batch is rejected before any tile is written",
                     odd->fi.bit_depth, frms[0]->fi.bit_depth);
            ret = oapvd_decode_tiles_multi(did, num_frms + 1, frame_reqs);
            check(odd->fi.bit_depth != frms[0]->fi.bit_depth && ret == OAPV_ERR_INVALID_ARGUMENT &&
                  dest_untouched(&batch) && dest_untouched(&odd_dest), what);

            // on its own it is a valid request
            ret = oapvd_decode_tiles_multi(did, 1, &frame_reqs[num_frms]);
            check(OAPV_SUCCEEDED(ret) && !dest_untouched(&odd_dest), "that frame decodes in a request of its own");
        }

        // and the decoder is still right after all of that
        memset(batch.buf, SENTINEL, batch.size);
        set_frame_reqs(frame_reqs, bitbs, &batch, num_sel, num_frms, 0);
        ret = oapvd_decode_tiles_multi(did, num_frms, frame_reqs);
        check(OAPV_SUCCEEDED(ret) && matches_pictures(&batch, frms, pics, num_frms, sel, num_sel), "the batch decodes correctly after the failed requests");

        oapvd_delete(did);
    }

    for(int f = 0; f < num_frms; f++) {
        picture_delete(pics[f]);
    }
    free(per_frm.buf);
    free(batch.buf);
    free(rbatch.buf);
    free(odd_dest.buf);
    for(int i = 0; i < 4; i++) {
        free(files[i]);
    }

    printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
