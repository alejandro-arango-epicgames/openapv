/* apvreprofile - batch-promote published APV assets to the UNCONST profiles.
 *
 * Published 12K/16K TMV assets were encoded before the UNCONST profile extensions
 * existed, so they carry a constrained profile (422-10 = 33, etc.) together with a
 * tile grid beyond the RFC 9924 20x20 limit. A decoder that honours the limit -
 * which upstream OpenAPV and anything built from it does - rejects those frames
 * outright, so the coarse mips decode and the fine ones come back black.
 *
 * profile_idc is the first byte of frame_info(), byte-aligned at pbu_start + 8
 * (4-byte pbu_size, then the 4-byte PBU header), so promoting it is a one-byte
 * patch per frame header. Nothing else in the bitstream depends on the value, and
 * the assets already satisfy the UNCONST profiles' chroma and bit-depth
 * constraints - the extension only removes the tile-grid limit.
 *
 * Safety, because this edits production media in place:
 *   - dry run unless --apply is given;
 *   - a file is fully parsed and validated before any byte of it is written;
 *   - only files that actually need it are touched, and already-UNCONST files are
 *     skipped, so the tool is idempotent;
 *   - all frame PBUs in a file are promoted together, so an access unit never
 *     ends up with mixed profiles;
 *   - --revert applies the inverse mapping, so a run is undoable.
 *
 * Build: gcc -O2 -o apvreprofile.exe apvreprofile.c
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The only platform-specific calls this tool makes are a case-insensitive string
   compare and 64-bit file seeks. */
#if defined(_WIN32)
    #define tool_stricmp  _stricmp
    #define tool_fseek64  _fseeki64
    #define tool_ftell64  _ftelli64
#else
    #include <strings.h>
    #define tool_stricmp  strcasecmp
    #define tool_fseek64  fseeko
    #define tool_ftell64  ftello
#endif

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define MAX_FRAMES   64      /* frame PBUs per access unit (mip pyramid depth) */
#define HDR_SCAN     1024    /* bytes of each PBU parsed; covers a full q matrix */
#define RFC_MAX_COLS 20
#define RFC_MAX_ROWS 20
#define MB           16

/* Constrained profile -> UNCONST counterpart. The UNCONST extension differs only
 * in dropping the tile-grid limit, so the chroma/bit-depth pairing is preserved. */
static const struct { int constrained, unconst; } PROFILE_MAP[] = {
    { 33, 43 },  /* 422-10  */
    { 44, 54 },  /* 422-12  */
    { 55, 65 },  /* 444-10  */
    { 66, 76 },  /* 444-12  */
    { 77, 87 },  /* 4444-10 */
    { 88, 98 },  /* 4444-12 */
    { 99, 109 }, /* 400-10  */
};
#define NPROFILES ((int)(sizeof(PROFILE_MAP) / sizeof(PROFILE_MAP[0])))

static int map_profile(int p, int revert)
{
    for(int i = 0; i < NPROFILES; i++) {
        if(revert) {
            if(p == PROFILE_MAP[i].unconst) return PROFILE_MAP[i].constrained;
        }
        else {
            if(p == PROFILE_MAP[i].constrained) return PROFILE_MAP[i].unconst;
        }
    }
    return -1; /* nothing to do for this profile */
}

/* --------------------------------------------------------------- bit reader */

typedef struct { const u8 *buf; u64 size, bitpos; } br_t;

static u32 br_read(br_t *br, int nbits)
{
    u32 v = 0;
    for(int i = 0; i < nbits; i++) {
        u64 byte = br->bitpos >> 3;
        int bit = 7 - (int)(br->bitpos & 7);
        u32 b = (byte < br->size) ? ((br->buf[byte] >> bit) & 1) : 0;
        v = (v << 1) | b;
        br->bitpos++;
    }
    return v;
}

static u32 rd32be(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static int num_comp_of(int chroma_format_idc)
{
    switch(chroma_format_idc) {
        case 0: return 1; case 2: return 3; case 3: return 3; case 4: return 4;
        default: return 0;
    }
}

/* --------------------------------------------------------------- file scan */

typedef struct {
    u64 profile_off;  /* file offset of the profile_idc byte */
    int profile;
    int tile_cols, tile_rows;
    int oversized;
} frame_t;

typedef struct {
    frame_t frames[MAX_FRAMES];
    int     nframes;
    int     n_oversized;
    int     n_mappable;      /* frames whose profile has an UNCONST counterpart */
    int     n_already;       /* frames already on the target profile */
} scan_t;

/* Returns 0 on success, non-zero with *err set on a malformed file. */
static int scan_file(const char *path, int revert, scan_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    *err = NULL;

    FILE *f = fopen(path, "rb");
    if(!f) { *err = "cannot open"; return 1; }

    u8 head[8];
    if(fread(head, 1, 8, f) != 8) { *err = "too small"; fclose(f); return 1; }

    if(tool_fseek64(f, 0, SEEK_END) != 0) { *err = "seek failed"; fclose(f); return 1; }
    long long fsz = tool_ftell64(f);

    u32 au_size = rd32be(head);
    if(rd32be(head + 4) != 0x61507631) { *err = "bad signature"; fclose(f); return 1; }
    if((long long)au_size + 4 != fsz) { *err = "au_size != file size"; fclose(f); return 1; }

    u64 pos = 8;                     /* first pbu_size field */
    u64 au_end = 4 + (u64)au_size;

    while(pos + 4 <= (u64)fsz && pos < au_end) {
        u8 szbuf[4];
        if(tool_fseek64(f, (long long)pos, SEEK_SET) != 0) { *err = "seek failed"; fclose(f); return 1; }
        if(fread(szbuf, 1, 4, f) != 4) { *err = "truncated pbu size"; fclose(f); return 1; }

        u32 pbu_size = rd32be(szbuf);
        if(pbu_size < 4 || pos + 4 + (u64)pbu_size > (u64)fsz) { *err = "bad pbu size"; fclose(f); return 1; }

        u8 pbu[HDR_SCAN];
        size_t want = pbu_size < HDR_SCAN ? pbu_size : HDR_SCAN;
        if(fread(pbu, 1, want, f) != want) { *err = "truncated pbu"; fclose(f); return 1; }

        int pbu_type = pbu[0];
        if(pbu_type >= 1 && pbu_type <= 5) {          /* any frame PBU */
            if(out->nframes >= MAX_FRAMES) { *err = "too many frame PBUs"; fclose(f); return 1; }

            br_t br = { pbu, want, 0 };
            br_read(&br, 8);  /* pbu_type */
            br_read(&br, 16); /* group_id */
            br_read(&br, 8);  /* reserved */

            /* frame_info() */
            int profile = (int)br_read(&br, 8);
            br_read(&br, 8);                       /* level_idc */
            br_read(&br, 3);                       /* band_idc  */
            br_read(&br, 5);                       /* reserved  */
            u32 fw = br_read(&br, 24);
            u32 fh = br_read(&br, 24);
            int chroma = (int)br_read(&br, 4);
            br_read(&br, 4);                       /* bit_depth */
            br_read(&br, 8);                       /* capture_time_distance */
            br_read(&br, 1);                       /* use_companding */
            br_read(&br, 7);                       /* reserved */

            /* frame_header() up to tile_info() */
            br_read(&br, 8);                       /* reserved */
            if(br_read(&br, 1)) {                  /* color_description_present */
                br_read(&br, 8); br_read(&br, 8); br_read(&br, 8); br_read(&br, 1);
            }
            int nc = num_comp_of(chroma);
            if(nc == 0) { *err = "bad chroma_format_idc"; fclose(f); return 1; }
            if(br_read(&br, 1)) {                  /* use_q_matrix */
                for(int c = 0; c < nc; c++)
                    for(int i = 0; i < 64; i++) br_read(&br, 8);
            }
            u32 tw_mbs = br_read(&br, 20);
            u32 th_mbs = br_read(&br, 20);

            if((br.bitpos >> 3) > want) { *err = "frame header beyond scan window"; fclose(f); return 1; }
            if(tw_mbs == 0 || th_mbs == 0) { *err = "zero tile size"; fclose(f); return 1; }

            u32 pic_w = ((fw + MB - 1) / MB) * MB;
            u32 pic_h = ((fh + MB - 1) / MB) * MB;
            u32 tile_w = tw_mbs * MB, tile_h = th_mbs * MB;

            frame_t *fr = &out->frames[out->nframes++];
            fr->profile_off = pos + 4 + 4;         /* pbu_start + 8 */
            fr->profile = profile;
            fr->tile_cols = (int)((pic_w + tile_w - 1) / tile_w);
            fr->tile_rows = (int)((pic_h + tile_h - 1) / tile_h);
            fr->oversized = (fr->tile_cols > RFC_MAX_COLS || fr->tile_rows > RFC_MAX_ROWS);

            if(fr->oversized) out->n_oversized++;
            if(map_profile(profile, revert) >= 0) out->n_mappable++;
            else if(map_profile(profile, !revert) >= 0) out->n_already++;
        }

        pos += 4 + (u64)pbu_size;
    }

    fclose(f);
    if(out->nframes == 0) { *err = "no frame PBUs"; return 1; }
    return 0;
}

/* Applies the mapped profile to every frame PBU. Returns bytes written, -1 on error. */
static int apply_file(const char *path, const scan_t *sc, int revert)
{
    FILE *f = fopen(path, "r+b");
    if(!f) return -1;

    int written = 0;
    for(int i = 0; i < sc->nframes; i++) {
        int to = map_profile(sc->frames[i].profile, revert);
        if(to < 0) continue;                        /* nothing to do for this frame */
        if(tool_fseek64(f, (long long)sc->frames[i].profile_off, SEEK_SET) != 0) { fclose(f); return -1; }
        u8 b = (u8)to;
        if(fwrite(&b, 1, 1, f) != 1) { fclose(f); return -1; }
        written++;
    }
    if(fflush(f) != 0) { fclose(f); return -1; }
    fclose(f);
    return written;
}

/* --------------------------------------------------------------------- run */

typedef struct {
    int apply, revert, verbose, verify;
    const char *pattern;
    /* tallies */
    int scanned, needed, patched, skipped_ok, skipped_already, errors, verify_fail;
} ctx_t;

static int has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && tool_stricmp(name + n - s, suffix) == 0;
}

static void process_file(const char *path, ctx_t *cx)
{
    scan_t sc;
    const char *err = NULL;

    cx->scanned++;
    if(scan_file(path, cx->revert, &sc, &err) != 0) {
        printf("  ERROR  %s: %s\n", path, err);
        cx->errors++;
        return;
    }

    /* Promote only assets that actually need it. A file whose every level fits the
     * RFC limit decodes fine as-is, and rewriting it would be churn. On --revert the
     * test is inverted: bring back anything currently on an UNCONST profile. */
    int needs = cx->revert ? (sc.n_mappable > 0) : (sc.n_oversized > 0 && sc.n_mappable > 0);

    if(!needs) {
        if(sc.n_already > 0 && !cx->revert) {
            cx->skipped_already++;
            if(cx->verbose) printf("  skip   %s (already UNCONST)\n", path);
        }
        else {
            cx->skipped_ok++;
            if(cx->verbose) printf("  skip   %s (grid within %dx%d)\n", path, RFC_MAX_COLS, RFC_MAX_ROWS);
        }
        return;
    }

    cx->needed++;

    if(cx->verbose || !cx->apply) {
        printf("  %s %s\n", cx->apply ? "patch " : "would ", path);
        for(int i = 0; i < sc.nframes; i++) {
            int to = map_profile(sc.frames[i].profile, cx->revert);
            printf("      mip %-2d %3dx%-3d grid%s  profile %d -> %d\n",
                   i, sc.frames[i].tile_cols, sc.frames[i].tile_rows,
                   sc.frames[i].oversized ? " [oversized]" : "            ",
                   sc.frames[i].profile, to < 0 ? sc.frames[i].profile : to);
        }
    }

    if(!cx->apply) return;

    int n = apply_file(path, &sc, cx->revert);
    if(n < 0) {
        printf("  ERROR  %s: write failed\n", path);
        cx->errors++;
        return;
    }
    cx->patched++;

    if(cx->verify) {
        scan_t re;
        const char *rerr = NULL;
        int bad = 0;
        if(scan_file(path, cx->revert, &re, &rerr) != 0) {
            printf("  VERIFY %s: re-scan failed (%s)\n", path, rerr);
            bad = 1;
        }
        else if(re.nframes != sc.nframes) {
            printf("  VERIFY %s: frame count changed\n", path);
            bad = 1;
        }
        else {
            for(int i = 0; i < re.nframes; i++) {
                int expect = map_profile(sc.frames[i].profile, cx->revert);
                if(expect < 0) expect = sc.frames[i].profile;
                if(re.frames[i].profile != expect) {
                    printf("  VERIFY %s: mip %d is %d, expected %d\n",
                           path, i, re.frames[i].profile, expect);
                    bad = 1;
                }
            }
        }
        if(bad) cx->verify_fail++;
    }
}

static void process_dir(const char *dir, ctx_t *cx)
{
    DIR *d = opendir(dir);
    if(!d) { printf("  ERROR  %s: cannot open directory\n", dir); cx->errors++; return; }

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

        struct stat st;
        if(stat(path, &st) != 0) continue;

        if(st.st_mode & S_IFDIR) {
            process_dir(path, cx);
        }
        else if(has_suffix(e->d_name, cx->pattern)) {
            process_file(path, cx);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    ctx_t cx;
    memset(&cx, 0, sizeof cx);
    cx.pattern = ".apv1";
    cx.verify = 1;

    const char *target = NULL;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--apply")) cx.apply = 1;
        else if(!strcmp(argv[i], "--revert")) cx.revert = 1;
        else if(!strcmp(argv[i], "--verbose")) cx.verbose = 1;
        else if(!strcmp(argv[i], "--no-verify")) cx.verify = 0;
        else if(!strcmp(argv[i], "--ext") && i + 1 < argc) cx.pattern = argv[++i];
        else if(argv[i][0] != '-' && !target) target = argv[i];
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }

    if(!target) {
        fprintf(stderr,
            "usage: %s <file-or-directory> [--apply] [--revert] [--ext .apv1]\n"
            "          [--verbose] [--no-verify]\n"
            "\n"
            "Promotes constrained APV profiles (33/44/55/...) to their UNCONST\n"
            "counterparts (43/54/65/...) in every frame PBU, so decoders that enforce\n"
            "the RFC 9924 20x20 tile-grid limit accept published 12K/16K assets.\n"
            "\n"
            "Dry run by default; pass --apply to write. Only files with at least one\n"
            "oversized level are touched, and the edit is one byte per frame header.\n"
            "--revert undoes a run.\n", argv[0]);
        return 2;
    }

    printf("mode           : %s%s\n",
           cx.apply ? "APPLY (writing in place)" : "dry run (no writes)",
           cx.revert ? ", REVERT (UNCONST -> constrained)" : "");
    printf("target         : %s\n", target);
    printf("extension      : %s\n\n", cx.pattern);

    struct stat st;
    if(stat(target, &st) != 0) { fprintf(stderr, "cannot stat %s\n", target); return 1; }

    if(st.st_mode & S_IFDIR) process_dir(target, &cx);
    else                     process_file(target, &cx);

    printf("\n");
    printf("scanned        : %d\n", cx.scanned);
    printf("needing change : %d\n", cx.needed);
    if(cx.apply) printf("patched        : %d\n", cx.patched);
    printf("skipped (ok)   : %d\n", cx.skipped_ok);
    if(!cx.revert) printf("skipped (done) : %d\n", cx.skipped_already);
    printf("errors         : %d\n", cx.errors);
    if(cx.apply && cx.verify) printf("verify failures: %d\n", cx.verify_fail);

    if(!cx.apply && cx.needed > 0) {
        printf("\nDry run only. Re-run with --apply to write these changes.\n");
    }
    return (cx.errors || cx.verify_fail) ? 1 : 0;
}
