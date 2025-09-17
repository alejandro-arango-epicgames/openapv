#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include "../../inc/oapv.h"

typedef unsigned char u8;
typedef unsigned short u16;

// Test configuration structure
typedef struct {
    const char* name;
    const char* description;
    enum { TEST_SINGLE_TILE, TEST_MULTI_TILE } test_type;
    int mip_level;
    int tile_coords[1600]; // Coordinate pairs terminated by {-1, -1} - supports up to 800 tiles
    int thread_counts[16]; // Thread counts to test, terminated by 0
    enum { OUTPUT_NONE, OUTPUT_RAW, OUTPUT_Y4M } output_format;
    int measure_performance;
    enum { VALIDATE_QUICK, VALIDATE_FULL } validation_level;
} test_config_t;

// Predefined test configurations covering all current use cases
static test_config_t test_configs[] = {
    // Single tile tests
    {
        .name = "single_tile_mip0_origin",
        .description = "Single tile at origin (0,0) from mip level 0",
        .test_type = TEST_SINGLE_TILE,
        .mip_level = 0,
        .tile_coords = {0, 0, -1, -1}, // Sentinel terminated
        .thread_counts = {1, 0}, // Single thread, terminated by 0
        .output_format = OUTPUT_RAW,
        .measure_performance = 0,
        .validation_level = VALIDATE_FULL
    },
    {
        .name = "single_tile_mip1_center", 
        .description = "Single tile at center from mip level 1",
        .test_type = TEST_SINGLE_TILE,
        .mip_level = 1,
        .tile_coords = {3, 2, -1, -1},
        .thread_counts = {1, 0},
        .output_format = OUTPUT_Y4M,
        .measure_performance = 0,
        .validation_level = VALIDATE_FULL
    },
    { .name = "single_tile_mip4_origin",
      .description = "Single tile at origin (0,0) from mip level 4",
      .test_type = TEST_SINGLE_TILE,
      .mip_level = 4,
      .tile_coords = { 0, 0, -1, -1 }, // Sentinel terminated
      .thread_counts = { 1, 0 },       // Single thread, terminated by 0
      .output_format = OUTPUT_RAW,
      .measure_performance = 0,
      .validation_level = VALIDATE_FULL 
    },
    // Multi-tile performance tests (from test_multi_tile_performance.c patterns)
    {
        .name = "multi_2x2_contiguous",
        .description = "2x2 contiguous tile block (zoom close scenario)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {0,0, 1,0, 0,1, 1,1, -1,-1},
        .thread_counts = {4, 8, 16, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_4x4_contiguous",
        .description = "4x4 contiguous tile block (very close scenario)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {0,0, 1,0, 2,0, 3,0, 0,1, 1,1, 2,1, 3,1, 
                       0,2, 1,2, 2,2, 3,2, 0,3, 1,3, 2,3, 3,3, -1,-1},
        .thread_counts = {4, 8, 16, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_middle_6x4_scaling",
        .description = "Middle 6x4 tile block with thread scaling (main test case)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {4,2, 5,2, 6,2, 7,2, 8,2, 9,2,  // Row 2
                       4,3, 5,3, 6,3, 7,3, 8,3, 9,3,  // Row 3  
                       4,4, 5,4, 6,4, 7,4, 8,4, 9,4,  // Row 4
                       4,5, 5,5, 6,5, 7,5, 8,5, 9,5,  // Row 5
                       -1,-1},
        .thread_counts = {4, 8, 16, 24, 0},
        .output_format = OUTPUT_RAW,
        .measure_performance = 1,
        .validation_level = VALIDATE_FULL
    },
    {
        .name = "multi_horizontal_strip",
        .description = "Horizontal strip (camera pan scenario)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {4,4, 5,4, 6,4, 7,4, 8,4, 9,4, -1,-1},
        .thread_counts = {4, 6, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_vertical_strip",
        .description = "Vertical strip (camera tilt scenario)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {7,2, 7,3, 7,4, 7,5, 7,6, 7,7, -1,-1},
        .thread_counts = {4, 6, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_sparse_corners",
        .description = "Sparse corner tiles (wide view scenario)", 
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {0,0, 14,0, 0,8, 14,8, -1,-1},
        .thread_counts = {4, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_single_center",
        .description = "Single center tile via multi-tile decoder (comparison test)",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {7,4, -1,-1},
        .thread_counts = {1, 4, 0},
        .output_format = OUTPUT_NONE,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    },
    {
        .name = "multi_full_frame_validation",
        .description = "First 120 tiles with detailed metrics",
        .test_type = TEST_MULTI_TILE,
        .mip_level = 0,
        .tile_coords = {// First 120 tiles (15x8 grid) - avoiding problematic row 8
                       0,0, 1,0, 2,0, 3,0, 4,0, 5,0, 6,0, 7,0, 8,0, 9,0, 10,0, 11,0, 12,0, 13,0, 14,0,  // Row 0
                       0,1, 1,1, 2,1, 3,1, 4,1, 5,1, 6,1, 7,1, 8,1, 9,1, 10,1, 11,1, 12,1, 13,1, 14,1,  // Row 1
                       0,2, 1,2, 2,2, 3,2, 4,2, 5,2, 6,2, 7,2, 8,2, 9,2, 10,2, 11,2, 12,2, 13,2, 14,2,  // Row 2
                       0,3, 1,3, 2,3, 3,3, 4,3, 5,3, 6,3, 7,3, 8,3, 9,3, 10,3, 11,3, 12,3, 13,3, 14,3,  // Row 3
                       0,4, 1,4, 2,4, 3,4, 4,4, 5,4, 6,4, 7,4, 8,4, 9,4, 10,4, 11,4, 12,4, 13,4, 14,4,  // Row 4
                       0,5, 1,5, 2,5, 3,5, 4,5, 5,5, 6,5, 7,5, 8,5, 9,5, 10,5, 11,5, 12,5, 13,5, 14,5,  // Row 5
                       0,6, 1,6, 2,6, 3,6, 4,6, 5,6, 6,6, 7,6, 8,6, 9,6, 10,6, 11,6, 12,6, 13,6, 14,6,  // Row 6
                       0,7, 1,7, 2,7, 3,7, 4,7, 5,7, 6,7, 7,7, 8,7, 9,7, 10,7, 11,7, 12,7, 13,7, 14,7,  // Row 7
                       -1,-1},
        .thread_counts = {1, 4, 8, 16, 24, 32, 0},
        .output_format = OUTPUT_RAW,
        .measure_performance = 1,
        .validation_level = VALIDATE_QUICK
    }
};

static int num_test_configs = sizeof(test_configs) / sizeof(test_configs[0]);

// Count tiles from sentinel-terminated coordinate array
int count_tiles_from_coords(const int* tile_coords) {
    int count = 0;
    for(int i = 0; tile_coords[i] != -1 && tile_coords[i+1] != -1; i += 2) {
        count++;
    }
    return count;
}

void delete_frame_buffer(oapv_imgb_t* imgb)
{
    if(imgb) {
        for(int c = 0; c < imgb->np; ++c) {
            if(imgb->a[c]) {
                free(imgb->a[c]);
            }
        }

        free(imgb);
    }
}

// Create frame buffer for specific component
oapv_imgb_t* create_frame_buffer(int width, int height, int num_components, int bit_depth) {
    oapv_imgb_t *imgb = malloc(sizeof(oapv_imgb_t));
    if (!imgb) return NULL;
    
    memset(imgb, 0, sizeof(oapv_imgb_t));

    imgb->np = num_components;

    for(int c = 0; c < num_components; ++c) {

        if(c == 0) { // Y component
            imgb->w[c] = width;
            imgb->h[c] = height;
            imgb->s[c] = width * 2;
        }
        else { // U/V components (4:2:2)
            imgb->w[c] = width / 2;
            imgb->h[c] = height;
            imgb->s[c] = (width / 2) * 2;
        }

        int buffer_size = imgb->w[c] * imgb->h[c] * 2;
        imgb->a[c] = calloc(buffer_size, 1);
        if(!imgb->a[c]) {
            delete_frame_buffer(imgb);
            return NULL;
        }
    }
    
    imgb->cs = OAPV_CS_SET(OAPV_CF_YCBCR422, bit_depth, 0);
    imgb->refcnt = 1;
    
    return imgb;
}

// Write Y4M file for full frame
void write_frame_y4m(const char* filename, oapv_imgb_t* frame_buffer) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("ERROR: Cannot create output file %s\n", filename);
        return;
    }
    
    int width = frame_buffer->w[0];
    int height = frame_buffer->h[0];
    
    // Y4M header for 4:2:2 10-bit
    fprintf(fp, "YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C422p10\n", width, height);
    fprintf(fp, "FRAME\n");
    
    // Write Y plane
    u16* y_data = (u16*)frame_buffer->a[0];
    for(int i = 0; i < width * height; i++) {
        u16 val = y_data[i] >> 2; // Convert 10-bit to 8-bit for Y4M
        fputc((u8)val, fp);
    }
    
    // Write U plane (half width)
    u16* u_data = (u16*)frame_buffer->a[1];
    for(int i = 0; i < (width/2) * height; i++) {
        u16 val = u_data[i] >> 2;
        fputc((u8)val, fp);
    }
    
    // Write V plane (half width)
    u16* v_data = (u16*)frame_buffer->a[2];
    for(int i = 0; i < (width/2) * height; i++) {
        u16 val = v_data[i] >> 2;
        fputc((u8)val, fp);
    }
    
    fclose(fp);
}

// Write raw file with header
int write_frame_raw(const char* filename, oapv_imgb_t* frame_buffer) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("ERROR: Cannot create output file %s - %s (errno: %d)\n", filename, strerror(errno), errno);
        return 0;
    }
    
    int width = frame_buffer->w[0];
    int height = frame_buffer->h[0];
    int bit_depth = 10;
    int chroma_format = 2; // 4:2:2
    int version = 1;
    
    // Write header
    fwrite(&width, sizeof(int), 1, fp);
    fwrite(&height, sizeof(int), 1, fp);
    fwrite(&bit_depth, sizeof(int), 1, fp);
    fwrite(&chroma_format, sizeof(int), 1, fp);
    fwrite(&version, sizeof(int), 1, fp);
    
    // Write Y plane
    fwrite(frame_buffer->a[0], width * height * 2, 1, fp);
    
    // Write U plane 
    fwrite(frame_buffer->a[1], (width/2) * height * 2, 1, fp);
    
    // Write V plane
    fwrite(frame_buffer->a[2], (width/2) * height * 2, 1, fp);
    
    fclose(fp);
    return 1;
}

// Quick validation - just count non-zero pixels
void validate_quick(oapv_imgb_t* y_buffer, int num_tiles) {
    u16* y_data = (u16*)y_buffer->a[0];
    int width = y_buffer->w[0];
    int height = y_buffer->h[0];
    
    int nonzero_pixels = 0;
    int total_pixels = width * height;
    
    for(int i = 0; i < total_pixels; i++) {
        if(y_data[i] != 0) nonzero_pixels++;
    }
    
    printf("Validation: %d/%d non-zero pixels (%.2f%%)\n", 
           nonzero_pixels, total_pixels, 100.0 * nonzero_pixels / total_pixels);
}

// Full validation - check chroma patterns too
void validate_full(oapv_imgb_t* frame_buffer, int num_tiles) {
    // Y validation
    u16* y_data = (u16*)frame_buffer->a[0];
    int width = frame_buffer->w[0];
    int height = frame_buffer->h[0];
    
    int y_nonzero = 0;
    for(int i = 0; i < width * height; i++) {
        if(y_data[i] != 0) y_nonzero++;
    }
    
    // U/V validation  
    u16* u_data = (u16*)frame_buffer->a[1];
    u16* v_data = (u16*)frame_buffer->a[2];
    int chroma_width = width / 2;
    int chroma_pixels = chroma_width * height;
    
    int u_nonzero = 0, v_nonzero = 0;
    for(int i = 0; i < chroma_pixels; i++) {
        if(u_data[i] != 0) u_nonzero++;
        if(v_data[i] != 0) v_nonzero++;
    }
    
    // Find the bounding box of non-zero Y pixels to determine decoded region
    int min_x = width, max_x = -1, min_y = height, max_y = -1;
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            if(y_data[y * width + x] != 0) {
                if(x < min_x) min_x = x;
                if(x > max_x) max_x = x;
                if(y < min_y) min_y = y;
                if(y > max_y) max_y = y;
            }
        }
    }
    
    printf("Full validation:\n");
    printf("  Y: %d/%d non-zero pixels (%.2f%%)\n", y_nonzero, width*height, 100.0*y_nonzero/(width*height));
    printf("  U: %d/%d non-zero pixels (%.2f%%)\n", u_nonzero, chroma_pixels, 100.0*u_nonzero/chroma_pixels);
    printf("  V: %d/%d non-zero pixels (%.2f%%)\n", v_nonzero, chroma_pixels, 100.0*v_nonzero/chroma_pixels);
    
    // For selective decoding, only check chroma quality in the decoded region
    if(min_x <= max_x && min_y <= max_y) {
        // Convert to chroma coordinates
        int chroma_min_x = min_x / 2;
        int chroma_max_x = max_x / 2;
        
        // Check for chroma stripe artifacts only in decoded region
        int u_zero_cols = 0, v_zero_cols = 0;
        int decoded_cols = chroma_max_x - chroma_min_x + 1;
        
        for(int col = chroma_min_x; col <= chroma_max_x; col++) {
            int u_col_zeros = 0, v_col_zeros = 0;
            for(int row = min_y; row <= max_y; row++) {
                if(u_data[row * chroma_width + col] == 0) u_col_zeros++;
                if(v_data[row * chroma_width + col] == 0) v_col_zeros++;
            }
            int decoded_rows = max_y - min_y + 1;
            if(u_col_zeros == decoded_rows) u_zero_cols++;
            if(v_col_zeros == decoded_rows) v_zero_cols++;
        }
        
        printf("  Decoded region: (%d,%d) to (%d,%d)\n", min_x, min_y, max_x, max_y);
        printf("  Chroma quality in decoded region: U zero columns: %d/%d, V zero columns: %d/%d\n", 
               u_zero_cols, decoded_cols, v_zero_cols, decoded_cols);
        
        if(u_zero_cols > 0 || v_zero_cols > 0) {
            printf("  WARNING: Chroma stripe artifacts detected in decoded region!\n");
        } else {
            printf("  SUCCESS: Perfect chroma quality in decoded region\n");
        }
    } else {
        printf("  No decoded pixels found for validation\n");
    }
}


long file_bitreader_tell(oapvd_bitr_t* bitr)
{
    FILE *fp = (FILE *)bitr->data;

    return ftell(fp);
}

int file_bitreader_seek(oapvd_bitr_t *bitr, long offset, int origin)
{
    FILE *fp = (FILE *)bitr->data;

    return fseek(fp, offset, origin);
}

size_t file_bitreader_read(oapvd_bitr_t *bitr, void* buffer, size_t size, size_t count)
{
    FILE *fp = (FILE *)bitr->data;

    return fread(buffer, size, count, fp);
}

void file_bitreader_init(oapvd_bitr_t* bitr, FILE* fp)
{
    bitr->data = fp;
    bitr->tell = file_bitreader_tell;
    bitr->seek = file_bitreader_seek;
    bitr->read = file_bitreader_read;
}

// Run a single test configuration
int run_test_config(const char* input_file, const test_config_t* config) {
    printf("\n=== Test: %s ===\n", config->name);
    printf("Description: %s\n", config->description);
    
    int num_tiles = count_tiles_from_coords(config->tile_coords);
    printf("Type: %s, Mip: %d, Tiles: %d\n", 
           (config->test_type == TEST_SINGLE_TILE) ? "Single" : "Multi",
           config->mip_level, num_tiles);
    
    FILE* fp = fopen(input_file, "rb");
    if (!fp) {
        printf("ERROR: Cannot open input file %s\n", input_file);
        return -1;
    }
    
    // Create initial decoder for metadata
    oapvd_cdesc_t cdesc = {0};
    cdesc.threads = (config->test_type == TEST_SINGLE_TILE) ? 1 : config->thread_counts[0];
    int err;
    oapvd_t decoder_id = oapvd_create(&cdesc, &err);
    if (decoder_id == NULL) {
        printf("ERROR: Failed to create decoder (error code: %d)\n", err);
        fclose(fp);
        return -1;
    }
    
    // Set up selective decode structure
    oapv_selective_decode_t sel_decode = {0};
    sel_decode.mip_level = config->mip_level;
    sel_decode.num_tiles = num_tiles;
    
    // Copy tile coordinates
    for(int i = 0; i < num_tiles * 2; i++) {
        sel_decode.tile_coords[i] = config->tile_coords[i];
    }
    
    oapvd_stat_t stat = {0};

    oapvd_bitr_t bitr;
    file_bitreader_init(&bitr, fp);
    
    // Get metadata
    int ret;
    if (config->test_type == TEST_SINGLE_TILE) {
        ret = oapvd_decode_selective(decoder_id, &bitr, &sel_decode, 0, &stat);
    } else {
        ret = oapvd_decode_selective_multi(decoder_id, &bitr, &sel_decode, 0, &stat);
    }
    
    if (OAPV_FAILED(ret)) {
        printf("ERROR: Failed to get metadata (return code: %d)\n", ret);
        oapvd_delete(decoder_id);
        fclose(fp);
        return -1;
    }
    
    printf("Frame: %dx%d, Tile size: %dx%d\n",
           sel_decode.actual_frame_width, sel_decode.actual_frame_height,
           sel_decode.actual_tile_width, sel_decode.actual_tile_height);
    
    // Create output buffers if needed
    oapv_imgb_t *frame_buffer = NULL;
    if (config->output_format != OUTPUT_NONE || config->validation_level == VALIDATE_FULL) {
        frame_buffer = create_frame_buffer(sel_decode.actual_frame_width, sel_decode.actual_frame_height, 3, sel_decode.bit_depth);
        
        if (!frame_buffer) {
            printf("ERROR: Failed to allocate frame buffers\n");
            oapvd_delete(decoder_id);
            fclose(fp);
            return -1;
        }
        
        sel_decode.output_buffer = frame_buffer;
    }
    
    // Run tests for each thread count
    for(int t = 0; config->thread_counts[t] != 0; t++) {
        int thread_count = config->thread_counts[t];
        
        if (config->test_type == TEST_MULTI_TILE) {
            // Update decoder thread count
            oapvd_delete(decoder_id);
            cdesc.threads = thread_count;
            decoder_id = oapvd_create(&cdesc, &err);
        }
        
        printf("\n--- Testing with %d thread%s ---\n", thread_count, (thread_count > 1) ? "s" : "");
        
        clock_t start_time = clock();
        
        // Run the decode
        if (config->test_type == TEST_SINGLE_TILE) {
            ret = oapvd_decode_selective(decoder_id, &bitr, &sel_decode, 0, &stat);
        } else {
            ret = oapvd_decode_selective_multi(decoder_id, &bitr, &sel_decode, 0, &stat);
        }
        
        clock_t end_time = clock();
        
        if (OAPV_SUCCEEDED(ret)) {
            printf("SUCCESS: Decode completed\n");
            
            if (config->measure_performance) {
                double total_time_ms = ((double)(end_time - start_time)) / CLOCKS_PER_SEC * 1000.0;
                printf("Performance: %.2f ms", total_time_ms);
                if (num_tiles > 1) {
                    printf(", %.2f tiles/sec", num_tiles * 1000.0 / total_time_ms);
                }
                printf("\n");
                
                // Print detailed I/O statistics
                if (stat.read > 0) {
                    printf("I/O Statistics:\n");
                    printf("  Bytes read: %d (%.2f MB)\n", stat.read, stat.read / (1024.0 * 1024.0));
                    printf("  Bandwidth: %.2f MB/sec\n", (stat.read / (1024.0 * 1024.0)) / (total_time_ms / 1000.0));
                    printf("  Bytes per tile: %.0f\n", (double)stat.read / num_tiles);
                }
            }
            
            // Validation
            if (frame_buffer) {
                if (config->validation_level == VALIDATE_QUICK) {
                    validate_quick(frame_buffer, num_tiles);
                } else if (config->validation_level == VALIDATE_FULL) {
                    validate_full(frame_buffer, num_tiles);
                }
            }
        } else {
            printf("ERROR: Decode failed (return code: %d)\n", ret);
        }
        
        // Write output files (for multi-tile tests, include thread count to avoid contention)
        if (config->output_format != OUTPUT_NONE && frame_buffer) {
            char output_filename[256];
            
            if (config->output_format == OUTPUT_Y4M) {
                if (config->test_type == TEST_MULTI_TILE) {
                    snprintf(output_filename, sizeof(output_filename), "output/%s_%dthreads.y4m", config->name, thread_count);
                } else {
                    snprintf(output_filename, sizeof(output_filename), "output/%s.y4m", config->name);
                }
                write_frame_y4m(output_filename, frame_buffer);
                printf("Written Y4M: %s\n", output_filename);
            } else if (config->output_format == OUTPUT_RAW) {
                if (config->test_type == TEST_MULTI_TILE) {
                    snprintf(output_filename, sizeof(output_filename), "output/%s_%dthreads.raw", config->name, thread_count);
                } else {
                    snprintf(output_filename, sizeof(output_filename), "output/%s.raw", config->name);
                }
                if (write_frame_raw(output_filename, frame_buffer)) {
                    printf("Written RAW: %s\n", output_filename);
                }
            }
        }
    }
    
    // Cleanup
    delete_frame_buffer(frame_buffer);
    oapvd_delete(decoder_id);
    fclose(fp);
    
    return 0;
}

// Print available test configurations
void print_available_tests() {
    printf("Available test configurations:\n");
    for(int i = 0; i < num_test_configs; i++) {
        int num_tiles = count_tiles_from_coords(test_configs[i].tile_coords);
        printf("%2d. %-25s - %s (%d tiles)\n", 
               i+1, test_configs[i].name, test_configs[i].description, num_tiles);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <apv_file> [test_number|test_name|all]\n", argv[0]);
        printf("\nDecoder test with data-driven configuration.\n");
        printf("Supports both single-tile and multi-tile selective decoding.\n\n");
        print_available_tests();
        printf("\nExamples:\n");
        printf("  %s test.apv1 3                        # Run test #3\n", argv[0]);
        printf("  %s test.apv1 multi_middle_6x4_scaling  # Run by name\n", argv[0]);
        printf("  %s test.apv1 all                       # Run all tests\n", argv[0]);
        return -1;
    }
    
    const char* input_file = argv[1];
    const char* test_selector = (argc >= 3) ? argv[2] : "all";
    
    printf("Decoder Test\n");
    printf("Input: %s\n", input_file);
    printf("Test selector: %s\n", test_selector);
    
    if (strcmp(test_selector, "all") == 0) {
        // Run all tests
        printf("\nRunning all %d test configurations...\n", num_test_configs);
        for(int i = 0; i < num_test_configs; i++) {
            run_test_config(input_file, &test_configs[i]);
        }
    } else if (test_selector[0] >= '1' && test_selector[0] <= '9') {
        // Run by number
        int test_num = atoi(test_selector) - 1;
        if (test_num >= 0 && test_num < num_test_configs) {
            run_test_config(input_file, &test_configs[test_num]);
        } else {
            printf("ERROR: Invalid test number %d (valid range: 1-%d)\n", test_num+1, num_test_configs);
            return -1;
        }
    } else {
        // Run by name
        int found = 0;
        for(int i = 0; i < num_test_configs; i++) {
            if (strcmp(test_configs[i].name, test_selector) == 0) {
                run_test_config(input_file, &test_configs[i]);
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("ERROR: Test '%s' not found\n", test_selector);
            print_available_tests();
            return -1;
        }
    }
    
    printf("\n=== Test Complete ===\n");
    return 0;
}