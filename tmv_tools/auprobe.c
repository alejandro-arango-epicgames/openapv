/* auprobe - standalone APV access-unit structure probe.
 *
 * Walks an AU byte stream and reports, per frame PBU: profile, geometry, tile
 * grid, whether tile_size_present_in_fh_flag is set, and the tile size table.
 * Links nothing; parses the bit layout directly so it can inspect assets the
 * library rejects (e.g. profile 33 with a grid beyond 20x20).
 *
 * Also verifies the tile size table against the actual tile chain by walking
 * the 4-byte size prefixes, which is what section 3 of the handoff rests on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

/* MSB-first bit reader over a byte buffer. */
typedef struct {
    const u8 *buf;
    u64 size;     /* bytes */
    u64 bitpos;
} br_t;

static void br_init(br_t *br, const u8 *buf, u64 size)
{
    br->buf = buf;
    br->size = size;
    br->bitpos = 0;
}

static u32 br_read(br_t *br, int nbits)
{
    u32 v = 0;
    for (int i = 0; i < nbits; i++) {
        u64 byte = br->bitpos >> 3;
        int bit = 7 - (int)(br->bitpos & 7);
        u32 b = (byte < br->size) ? ((br->buf[byte] >> bit) & 1) : 0;
        v = (v << 1) | b;
        br->bitpos++;
    }
    return v;
}

static void br_align8(br_t *br)
{
    br->bitpos = (br->bitpos + 7) & ~(u64)7;
}

static u32 rd32be(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static int num_comp_of(int chroma_format_idc)
{
    switch (chroma_format_idc) {
        case 0: return 1;   /* 400 */
        case 2: return 3;   /* 422 */
        case 3: return 3;   /* 444 */
        case 4: return 4;   /* 4444 */
        default: return 0;
    }
}

static const char *pbu_type_name(int t)
{
    switch (t) {
        case 1:  return "PRIMARY_FRAME";
        case 2:  return "NON_PRIMARY_FRAME";
        case 3:  return "PREVIEW_FRAME";
        case 4:  return "DEPTH_FRAME";
        case 5:  return "ALPHA_FRAME";
        case 65: return "AU_INFO";
        case 66: return "METADATA";
        case 67: return "FILLER";
        default: return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.apv> [--dump-tile-sizes N] [--dump-chain]\n", argv[0]);
        return 1;
    }
    int dump_n = 0;
    int dump_chain = 0;      /* emit "TILE <mip> <idx> <abs_off> <size>" lines */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-tile-sizes") && i + 1 < argc) {
            dump_n = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--dump-chain")) {
            dump_chain = 1;
        }
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = (u8 *)malloc((size_t)fsz);
    if (!data) { fprintf(stderr, "oom\n"); return 1; }
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    printf("file            : %s\n", argv[1]);
    printf("file size       : %lld bytes\n", fsz);

    u32 au_size = rd32be(data);
    printf("au_size         : %u (au_size + 4 = %u, file = %lld)%s\n",
           au_size, au_size + 4, fsz,
           ((long long)au_size + 4 == fsz) ? "  [matches]" : "  [MISMATCH]");

    u32 sig = rd32be(data + 4);
    printf("signature       : 0x%08X %s\n", sig, sig == 0x61507631 ? "('aPv1') [ok]" : "[BAD]");

    /* Walk PBUs. */
    u64 pos = 8;                    /* first PBU size field */
    u64 au_end = 4 + (u64)au_size;
    int frame_ord = 0;              /* frame PBU ordinal == mip level for TMV */
    int pbu_ord = 0;

    printf("\n");
    while (pos + 8 <= (u64)fsz && pos < au_end) {
        u32 pbu_size = rd32be(data + pos);
        if (pbu_size == 0) { printf("  [pbu_size == 0 at %llu, stop]\n", pos); break; }

        const u8 *pbu = data + pos + 4;             /* PBU header starts here */
        u64 pbu_avail = (u64)fsz - (pos + 4);
        if (pbu_avail > pbu_size) pbu_avail = pbu_size;

        br_t br;
        br_init(&br, pbu, pbu_avail);

        int pbu_type = (int)br_read(&br, 8);
        int group_id = (int)br_read(&br, 16);
        (void)br_read(&br, 8);                       /* reserved_zero_8bits */

        printf("PBU[%d] @ %llu  size=%u  type=%d (%s)  group_id=%d\n",
               pbu_ord, (unsigned long long)pos, pbu_size, pbu_type, pbu_type_name(pbu_type), group_id);

        if (pbu_type == 1 || pbu_type == 2 || pbu_type == 3 ||
            pbu_type == 4 || pbu_type == 5) {
            /* ---- frame_info() ---- */
            int profile_idc = (int)br_read(&br, 8);
            int level_idc   = (int)br_read(&br, 8);
            int band_idc    = (int)br_read(&br, 3);
            u32 rsv5        = br_read(&br, 5);
            u32 frame_width  = br_read(&br, 24);
            u32 frame_height = br_read(&br, 24);
            int chroma_format_idc = (int)br_read(&br, 4);
            int bit_depth   = (int)br_read(&br, 4) + 8;
            int capture_time_distance = (int)br_read(&br, 8);
            int use_companding = (int)br_read(&br, 1);
            u32 rsv7 = br_read(&br, 7);

            /* ---- frame_header() ---- */
            u32 rsv8 = br_read(&br, 8);
            int color_desc = (int)br_read(&br, 1);
            int cp = 2, tc = 2, mc = 2, fr = 0;
            if (color_desc) {
                cp = (int)br_read(&br, 8);
                tc = (int)br_read(&br, 8);
                mc = (int)br_read(&br, 8);
                fr = (int)br_read(&br, 1);
            }
            int use_q_matrix = (int)br_read(&br, 1);
            int nc = num_comp_of(chroma_format_idc);
            if (use_q_matrix) {
                for (int c = 0; c < nc; c++)
                    for (int i = 0; i < 64; i++)
                        (void)br_read(&br, 8);
            }

            /* ---- tile_info() ---- */
            u32 tile_width_in_mbs  = br_read(&br, 20);
            u32 tile_height_in_mbs = br_read(&br, 20);

            const int MB = 16;
            u32 pic_w = ((frame_width  + MB - 1) / MB) * MB;
            u32 pic_h = ((frame_height + MB - 1) / MB) * MB;
            u32 tile_w = tile_width_in_mbs  * MB;
            u32 tile_h = tile_height_in_mbs * MB;
            u32 tile_cols = tile_w ? (pic_w + tile_w - 1) / tile_w : 0;
            u32 tile_rows = tile_h ? (pic_h + tile_h - 1) / tile_h : 0;
            u32 num_tiles = tile_cols * tile_rows;

            int tsp = (int)br_read(&br, 1);

            printf("  frame ordinal (mip) : %d\n", frame_ord);
            printf("  profile_idc         : %d%s\n", profile_idc,
                   profile_idc == 33 ? "  (422-10, RFC 9924 -- 20x20 tile limit applies)" :
                   profile_idc == 43 ? "  (422-10-UNCONST)" : "");
            printf("  level/band          : %d / %d\n", level_idc, band_idc);
            printf("  frame size          : %ux%u  (mb-aligned %ux%u)\n",
                   frame_width, frame_height, pic_w, pic_h);
            printf("  chroma/bitdepth     : %d / %d-bit   num_comp=%d\n",
                   chroma_format_idc, bit_depth, nc);
            printf("  companding          : %d   capture_time_distance=%d\n",
                   use_companding, capture_time_distance);
            printf("  color_desc_present  : %d (cp=%d tc=%d mc=%d full_range=%d)\n",
                   color_desc, cp, tc, mc, fr);
            printf("  use_q_matrix        : %d\n", use_q_matrix);
            printf("  tile size (mbs/px)  : %ux%u mbs = %ux%u px\n",
                   tile_width_in_mbs, tile_height_in_mbs, tile_w, tile_h);
            printf("  tile grid           : %ux%u = %u tiles%s\n",
                   tile_cols, tile_rows, num_tiles,
                   (tile_cols > 20 || tile_rows > 20) ? "   [EXCEEDS RFC 9924 20x20]" : "");
            printf("  tile_size_present   : %d %s\n", tsp,
                   tsp ? "[tile size table PRESENT]" : "[ABSENT -- selective decode not possible]");
            if (rsv5 || rsv7 || rsv8) {
                printf("  note: reserved bits nonzero (rsv5=%u rsv7=%u rsv8=%u)\n", rsv5, rsv7, rsv8);
            }

            if (tsp && num_tiles > 0) {
                u32 *sizes = (u32 *)malloc(sizeof(u32) * num_tiles);
                u64 sum = 0;
                int bad = 0;
                for (u32 i = 0; i < num_tiles; i++) {
                    sizes[i] = br_read(&br, 32);
                    if (sizes[i] == 0) bad++;
                    sum += sizes[i];
                }
                (void)br_read(&br, 8);      /* reserved_zero_8bits */
                br_align8(&br);
                u64 hdr_bytes = br.bitpos >> 3;         /* consumed within PBU */
                u64 tile_data_off = pos + 4 + hdr_bytes; /* absolute file offset */

                printf("  header bytes        : %llu (tile data starts at file offset %llu)\n",
                       (unsigned long long)hdr_bytes, (unsigned long long)tile_data_off);
                printf("  sum(tile_size)      : %llu   + %u prefixes*4 = %llu\n",
                       (unsigned long long)sum, num_tiles,
                       (unsigned long long)(sum + (u64)num_tiles * 4));
                u64 payload_avail = (pos + 4 + pbu_size) - tile_data_off;
                printf("  payload available   : %llu%s\n", (unsigned long long)payload_avail,
                       (sum + (u64)num_tiles * 4 == payload_avail) ? "  [EXACT MATCH]" : "  [mismatch]");
                if (bad) printf("  WARNING: %d zero-sized tiles\n", bad);

                /* Cross-check the table against the actual tile chain: walk the
                 * 4-byte prefixes the way dec_thread_tile does and compare. */
                u64 off = tile_data_off;
                u32 chain_mismatch = 0;
                for (u32 i = 0; i < num_tiles; i++) {
                    if (off + 4 > (u64)fsz) { chain_mismatch = num_tiles - i; break; }
                    u32 pref = rd32be(data + off);
                    if (pref != sizes[i]) chain_mismatch++;
                    off += 4 + sizes[i];
                }
                printf("  chain vs table      : %s (%u mismatches, chain ends at %llu, pbu ends at %llu)\n",
                       chain_mismatch ? "DIFFER" : "identical", chain_mismatch,
                       (unsigned long long)off, (unsigned long long)(pos + 4 + pbu_size));

                /* Machine-readable per-tile table, derived by walking the tile
                 * chain's own 4-byte prefixes -- the header size table is not
                 * consulted at all here, so this is independent of it.
                 * Offsets are absolute file offsets of the tile unit. */
                if (dump_chain) {
                    u64 coff = tile_data_off;
                    for (u32 i = 0; i < num_tiles; i++) {
                        if (coff + 4 > (u64)fsz) break;
                        u32 tsz = rd32be(data + coff);
                        printf("TILE %d %u %llu %u\n", frame_ord, i,
                               (unsigned long long)coff, tsz);
                        coff += 4 + tsz;
                    }
                }

                if (dump_n > 0) {
                    int n = (int)num_tiles < dump_n ? (int)num_tiles : dump_n;
                    printf("  first %d tile sizes  :", n);
                    for (int i = 0; i < n; i++) printf(" %u", sizes[i]);
                    printf("\n");
                }
                free(sizes);
            }
            printf("\n");
            frame_ord++;
        }
        else {
            printf("\n");
        }

        pos += 4 + (u64)pbu_size;
        pbu_ord++;
    }

    printf("total frame PBUs: %d\n", frame_ord);
    free(data);
    return 0;
}
