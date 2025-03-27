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

int oapve_param_default(oapve_param_t *param)
{
    oapv_mset(param, 0, sizeof(oapve_param_t));
    param->preset = OAPV_PRESET_DEFAULT;

    param->qp = 10; // default
    param->qp_offset_c1 = 0;
    param->qp_offset_c2 = 0;
    param->qp_offset_c3 = 0;

    param->tile_w = 16 * OAPV_MB_W; // default: 256
    param->tile_h = 16 * OAPV_MB_H; // default: 256

    param->profile_idc = OAPV_PROFILE_422_10;
    param->level_idc = OAPV_LEVEL_TO_LEVEL_IDC(4.1);
    param->band_idc = 2;

    param->use_q_matrix = 0;

    for(int c = 0; c < OAPV_MAX_CC; c++) {
        for(int i = 0; i < OAPV_BLK_D; i++) {
            param->q_matrix[c][i] = 16;
        }
    }

    param->color_description_present_flag = 0;
    param->color_primaries = 2; // unspecified color primaries
    param->transfer_characteristics = 2; // unspecified transfer characteristics
    param->matrix_coefficients = 2; // unspecified matrix coefficients
    param->full_range_flag = 0; // limited range

    return OAPV_OK;
}

///////////////////////////////////////////////////////////////////////////////
// parameter parsing helper function for encoder
static int is_digit(const char* str)
{
    while(*str) {
        if(*str < '0' || *str > '9')
            return 0;
        ++str;
    }
    return 1;
}

static int get_ival_from_skey(const oapv_dict_str_int_t * dict, const char * skey, int * ival)
{
    while(strlen(dict->key) > 0) {
        if(strcmp(dict->key, skey) == 0){
            *ival = dict->val;
            return 0;
        }
        dict++;
    }
    return -1;
}

static int kbps_str_to_int(const char *str)
{
    int kbps;
    char *s = (char *)str;
    if(strchr(s, 'K') || strchr(s, 'k')) {
        char *tmp = strtok(s, "Kk ");
        kbps = (int)(atof(tmp));
    }
    else if(strchr(s, 'M') || strchr(s, 'm')) {
        char *tmp = strtok(s, "Mm ");
        kbps = (int)(atof(tmp) * 1000);
    }
    else if(strchr(s, 'G') || strchr(s, 'g')) {
        char *tmp = strtok(s, "Gg ");
        kbps = (int)(atof(tmp) * 1000000);
    }
    else {
        kbps = atoi(s);
    }
    return kbps;
}

static int get_q_matrix(const char *str, u8 q_matrix[OAPV_BLK_D])
{
    int   t0, qcnt = 0;
    char *left;
    char *qstr = (char *)str;

    while(strlen(qstr) > 0 && qcnt < OAPV_BLK_D) {
        t0 = strtol(qstr, &left, 10);
        oapv_assert_rv(t0 >= 1 && t0 <= 255, -1);

        q_matrix[qcnt] = (u8)t0;
        qstr = left;
        qcnt++;
    }
    oapv_assert_rv(qcnt == OAPV_BLK_D, -1);
    return 0;
}

#define NAME_CMP(VAL)      else if(strcmp(name, VAL)== 0)
#define GET_INTEGER_OR_ERR(STR, F) { \
    char * left; (F) = strtol(STR, &left, 10); \
    if(strlen(left)>0) return OAPV_ERR_INVALID_ARGUMENT; \
}
#define GET_INTEGER_MIN_OR_ERR(STR, F, MIN) { \
        GET_INTEGER_OR_ERR(STR, F); \
        if((F) < (MIN)) return OAPV_ERR_INVALID_ARGUMENT; \
}
#define GET_INTEGER_MIN_MAX_OR_ERR(STR, F, MIN, MAX) { \
    GET_INTEGER_OR_ERR(STR, F); \
    if((F) < (MIN) || (F) > (MAX)) return OAPV_ERR_INVALID_ARGUMENT; \
}
#define GET_FLOAT_OR_ERR(STR, F) { \
    char * left; (F) = strtof(STR, &left); \
    if(strlen(left)>0) return OAPV_ERR_INVALID_ARGUMENT; \
}

int oapve_param_parse(oapve_param_t *param, const char *name,  const char *value)
{
    u8    q_matrix[OAPV_BLK_D];
    char  str_buf[64];
    int   ti0;
    float tf0;

    /* normalization of name and value ***************************************/
    // pass '-- prefix' if exists
    if(name[0] == '-' && name[1] == '-') { name += 2; }

    // replace '_' with '-'
    if(strlen(name) + 1 < sizeof(str_buf) && strchr(name, '_')) {
        char *c;
        strcpy(str_buf, name);
        while((c = strchr(str_buf, '_')) != 0) { *c = '-'; } // replace
        name = str_buf; // change address
    }

    /* parsing ***************************************************************/
    if(0){;}
    NAME_CMP("profile") {
        if(get_ival_from_skey(oapv_param_opts_profile, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->profile_idc = ti0;
    }
    NAME_CMP("level") {
        GET_FLOAT_OR_ERR(value, tf0);
        // validation check
        // level == [1, 1.1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 6, 6.1, 7, 7.1]
        if(tf0 == 1.0f || tf0 == 1.1f || tf0 == 2.0f || tf0 == 2.1f || \
            tf0 == 3.0f || tf0 == 3.1f || tf0 == 4.0f || tf0 == 4.1f ||\
            tf0 == 5.0f || tf0 == 5.1f || tf0 == 6.0f || tf0 == 6.1f ||\
            tf0 == 7.0f || tf0 == 7.1f) {
            param->level_idc = OAPV_LEVEL_TO_LEVEL_IDC(tf0);
        }
        else {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
    }
    NAME_CMP("band") {
        GET_INTEGER_MIN_MAX_OR_ERR(value, ti0, 0, 3);
        param->band_idc = ti0;
    }
    NAME_CMP("preset") {
        if(get_ival_from_skey(oapv_param_opts_preset, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->preset = ti0;
    }
    NAME_CMP("width") {
        GET_INTEGER_OR_ERR(value, ti0);
        oapv_assert_rv(ti0 > 0, OAPV_ERR_INVALID_ARGUMENT);
        param->w = ti0;
    }
    NAME_CMP("height") {
        GET_INTEGER_OR_ERR(value, ti0);
        oapv_assert_rv(ti0 > 0, OAPV_ERR_INVALID_ARGUMENT);
        param->h = ti0;
    }
    NAME_CMP("fps") {
        if(strpbrk(value, "/") != NULL) {
            sscanf(value, "%d/%d", &param->fps_num, &param->fps_den);
        }
        else if(strpbrk(value, ".") != NULL) {
            GET_FLOAT_OR_ERR(value, tf0);
            param->fps_num = tf0 * 10000;
            param->fps_den = 10000;
        }
        else {
            GET_INTEGER_OR_ERR(value, ti0);
            param->fps_num = ti0;
            param->fps_den = 1;
        }
    }
    NAME_CMP("qp") {
        //  QP value: 0 ~ (63 + (bitdepth - 10)*6)
        //     - 10bit input: 0 ~ 63"
        //     - 12bit input: 0 ~ 75"
        // max value cannot be decided without bitdepth value
        GET_INTEGER_MIN_MAX_OR_ERR(value, ti0, MIN_QUANT, MAX_QUANT(12));
        param->qp = ti0;
        param->rc_type = OAPV_RC_CQP;
    }
    NAME_CMP("qp_offset_c1") {
        GET_INTEGER_OR_ERR(value, ti0);
        param->qp_offset_c1 = ti0;
    }
    NAME_CMP("qp_offset_c2") {
        GET_INTEGER_OR_ERR(value, ti0);
        param->qp_offset_c2 = ti0;
    }
    NAME_CMP("qp_offset_c3") {
        GET_INTEGER_OR_ERR(value, ti0);
        param->qp_offset_c3 = ti0;
    }
    NAME_CMP("bitrate") {
        if(strlen(value) > 0) {
            strcpy(str_buf, value); // to maintain original value
            param->bitrate = kbps_str_to_int(str_buf); // unit: kbps
            if(param->bitrate <= 0) return OAPV_ERR_INVALID_ARGUMENT;
            param->rc_type = OAPV_RC_ABR;
        }
        else return OAPV_ERR_INVALID_ARGUMENT;
    }
    NAME_CMP("q-matrix-c0") {

        if(get_q_matrix(value, q_matrix)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        oapv_mcpy(param->q_matrix[Y_C], q_matrix, sizeof(u8)*OAPV_BLK_D);
        param->use_q_matrix = 1;
    }
    NAME_CMP("q-matrix-c1") {

        if(get_q_matrix(value, q_matrix)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        oapv_mcpy(param->q_matrix[U_C], q_matrix, sizeof(u8)*OAPV_BLK_D);
        param->use_q_matrix = 1;
    }
    NAME_CMP("q-matrix-c2") {

        if(get_q_matrix(value, q_matrix)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        oapv_mcpy(param->q_matrix[V_C], q_matrix, sizeof(u8)*OAPV_BLK_D);
        param->use_q_matrix = 1;
    }
    NAME_CMP("q-matrix-c3") {

        if(get_q_matrix(value, q_matrix)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        oapv_mcpy(param->q_matrix[X_C], q_matrix, sizeof(u8)*OAPV_BLK_D);
        param->use_q_matrix = 1;
    }
    NAME_CMP("tile-w") {
        GET_INTEGER_MIN_OR_ERR(value, ti0, OAPV_MIN_TILE_W);
        oapv_assert_rv((ti0 & (OAPV_MB_W - 1)) == 0, OAPV_ERR_INVALID_ARGUMENT);
        param->tile_w = ti0;
    }
    NAME_CMP("tile-h") {
        GET_INTEGER_MIN_OR_ERR(value, ti0, OAPV_MIN_TILE_H);
        oapv_assert_rv((ti0 & (OAPV_MB_W - 1)) == 0, OAPV_ERR_INVALID_ARGUMENT);
        param->tile_h = ti0;
    }
    NAME_CMP("color-primaries") {
        if(get_ival_from_skey(oapv_param_opts_color_primaries, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->color_primaries = ti0;
        param->color_description_present_flag = 1;
    }
    NAME_CMP("color-transfer") {
        if(get_ival_from_skey(oapv_param_opts_color_transfer, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->transfer_characteristics = ti0;
        param->color_description_present_flag = 1;
    }
    NAME_CMP("color-matrix") {
        if(get_ival_from_skey(oapv_param_opts_color_matrix, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->matrix_coefficients = ti0;
        param->color_description_present_flag = 1;
    }
    NAME_CMP("color-range") {
        if(get_ival_from_skey(oapv_param_opts_color_range, value, &ti0)) {
            return OAPV_ERR_INVALID_ARGUMENT;
        }
        param->full_range_flag = ti0;
        param->color_description_present_flag = 1;
    }
    return OAPV_OK;
}

