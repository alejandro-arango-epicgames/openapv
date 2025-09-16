#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../inc/oapv.h"

typedef unsigned char u8;
typedef unsigned short u16;

void delete_frame_buffer(oapv_imgb_t *imgb)
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

// Function to create frame buffers for full frame (3840x2160)
oapv_imgb_t *create_full_frame_buffer(int width, int height, int num_components, int bit_depth)
{
    oapv_imgb_t *imgb = malloc(sizeof(oapv_imgb_t));
    if (!imgb) return NULL;
    
    memset(imgb, 0, sizeof(oapv_imgb_t));
    
    imgb->np = num_components;

    for(int c = 0; c < num_components; ++c) {

        // Set dimensions based on component (Y=full, U/V=half width for 4:2:2)
        if(c == 0) { // Y component
            imgb->w[c] = width;
            imgb->h[c] = height;
            imgb->s[c] = width * 2; // stride in bytes (16-bit pixels)
        }
        else { // U/V components (4:2:2)
            imgb->w[c] = width / 2;
            imgb->h[c] = height;
            imgb->s[c] = (width / 2) * 2; // stride in bytes (16-bit pixels)
        }

        int buffer_size = imgb->w[c] * imgb->h[c] * 2; // 2 bytes per pixel for 10-bit
        imgb->a[c] = calloc(buffer_size, 1);
        if(!imgb->a[c]) {
            delete_frame_buffer(imgb);
            return NULL;
        }

        printf("Created frame buffer: component=%d, %dx%d, stride=%d, buffer_size=%d bytes\n",
               c, imgb->w[c], imgb->h[c], imgb->s[c], buffer_size);

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
    
    // Write Y4M header for full frame, 10-bit 4:2:2
    fprintf(fp, "YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C422p10\n", width, height);
    
    // Write single frame header
    fprintf(fp, "FRAME\n");
    
    // Get the raw data pointers
    u16* y_data = (u16*)frame_buffer->a[0];
    u16* u_data = (u16*)frame_buffer->a[1];
    u16* v_data = (u16*)frame_buffer->a[2];
    
    // Y4M expects little-endian 16-bit samples for 10-bit
    // Write Y plane
    for (int i = 0; i < width * height; i++) {
        u16 val = y_data[i];
        fwrite(&val, 2, 1, fp);
    }
    
    // Write U plane (half width for 4:2:2)
    for (int i = 0; i < (width/2) * height; i++) {
        u16 val = u_data[i];
        fwrite(&val, 2, 1, fp);
    }
    
    // Write V plane (half width for 4:2:2)
    for (int i = 0; i < (width/2) * height; i++) {
        u16 val = v_data[i];
        fwrite(&val, 2, 1, fp);
    }
    
    fclose(fp);
    printf("Written Y4M frame to: %s\n", filename);
}

// Write raw YUV file for Python conversion
void write_frame_raw(const char* filename, oapv_imgb_t* frame_buffer) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        printf("ERROR: Cannot create output file %s\n", filename);
        return;
    }
    
    int width = frame_buffer->w[0];
    int height = frame_buffer->h[0];
    
    // Write header with dimensions and format info
    int header[5] = {width, height, 10, 422, 1}; // width, height, bit_depth, chroma_format, version
    fwrite(header, sizeof(int), 5, fp);
    
    // Get the raw data pointers
    u16* y_data = (u16*)frame_buffer->a[0];
    u16* u_data = (u16*)frame_buffer->a[1];
    u16* v_data = (u16*)frame_buffer->a[2];
    
    // Write Y plane
    fwrite(y_data, 2, width * height, fp);
    
    // Write U plane (half width for 4:2:2)
    fwrite(u_data, 2, (width/2) * height, fp);
    
    // Write V plane (half width for 4:2:2)
    fwrite(v_data, 2, (width/2) * height, fp);
    
    fclose(fp);
    printf("Written raw frame to: %s\n", filename);
}

long file_bitreader_tell(oapvd_bitr_t *bitr)
{
    FILE *fp = (FILE *)bitr->data;

    return ftell(fp);
}

int file_bitreader_seek(oapvd_bitr_t *bitr, long offset, int origin)
{
    FILE *fp = (FILE *)bitr->data;

    return fseek(fp, offset, origin);
}

size_t file_bitreader_read(oapvd_bitr_t *bitr, void *buffer, size_t size, size_t count)
{
    FILE *fp = (FILE *)bitr->data;

    return fread(buffer, size, count, fp);
}

void file_bitreader_init(oapvd_bitr_t *bitr, FILE *fp)
{
    bitr->data = fp;
    bitr->tell = file_bitreader_tell;
    bitr->seek = file_bitreader_seek;
    bitr->read = file_bitreader_read;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <apv_file> <tile_x> <tile_y> [mip_level]\n", argv[0]);
        printf("Example: %s test/media/koala_tiled/koala_tiled_0000.apv1 0 0 0\n", argv[0]);
        printf("         %s test/media/koala_tiled/koala_tiled_0000.apv1 0 1 1\n", argv[0]);
        printf("\nThis will decode the specified tile into its correct position in a full frame buffer\n");
        printf("mip_level: 0 = primary frame (3840x2160), 1+ = secondary frames (smaller resolutions)\n");
        return -1;
    }
    
    const char* filename = argv[1];
    int tile_x = atoi(argv[2]);
    int tile_y = atoi(argv[3]);
    int mip_level = (argc >= 5) ? atoi(argv[4]) : 1; // Default to mip level 1 for backward compatibility
    
    // Frame dimensions will be determined from mip level metadata
    // Initial values for display only - actual values come from decoder
    int display_frame_width = 3840;
    int display_frame_height = 2160;
    int display_tile_width = 256;
    int display_tile_height = 256;
    
    printf("Visual Full Frame Test: Decoding tile [%d,%d] into full frame buffer\n", tile_x, tile_y);
    printf("Initial dimensions: %dx%d, Tile size: %dx%d (will be updated from mip level metadata)\n", 
           display_frame_width, display_frame_height, display_tile_width, display_tile_height);
    
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        printf("ERROR: Cannot open input file: %s\n", filename);
        return -1;
    }
    
    // Create decoder
    oapvd_cdesc_t cdesc = {0};
    cdesc.threads = 1;
    int err;
    oapvd_t decoder_id = oapvd_create(&cdesc, &err);
    if (decoder_id == NULL) {
        printf("ERROR: Failed to create decoder (error code: %d)\n", err);
        fclose(fp);
        return -1;
    }
    
    // Buffers will be created after getting actual frame dimensions from decoder
    oapv_imgb_t *frame_buffer = NULL;
    
    // Prepare selective decode structure
    oapv_selective_decode_t sel_decode = {0};
    sel_decode.num_tiles = 1;
    sel_decode.tile_coords[0] = tile_x;
    sel_decode.tile_coords[1] = tile_y;
    sel_decode.mip_level = mip_level;
    
    oapvd_bitr_t bitr;
    file_bitreader_init(&bitr, fp);

    // First, call decoder with NULL buffers to get frame metadata
    printf("Getting frame metadata for mip level %d...\n", sel_decode.mip_level);
    oapvd_stat_t stat_metadata = {0};
    int ret_metadata = oapvd_decode_selective(decoder_id, &bitr, &sel_decode, 0, &stat_metadata);
    
    if (OAPV_FAILED(ret_metadata)) {
        printf("ERROR: Failed to get frame metadata (return code: %d)\n", ret_metadata);
        oapvd_delete(decoder_id);
        fclose(fp);
        return -1;
    }
    
    // Now we have the actual frame dimensions - create buffers accordingly
    printf("Creating buffers based on actual mip level dimensions: %dx%d\n", 
           sel_decode.actual_frame_width, sel_decode.actual_frame_height);
           
    frame_buffer = create_full_frame_buffer(sel_decode.actual_frame_width, sel_decode.actual_frame_height, 3, sel_decode.bit_depth);
    
    if (!frame_buffer) {
        printf("ERROR: Failed to allocate frame buffers\n");
        oapvd_delete(decoder_id);
        fclose(fp);
        return -1;
    }
    
    // Set the buffers for actual decoding
    sel_decode.output_buffer = frame_buffer;
    
    // Calculate expected tile position in frame using actual tile size
    int tile_x_pos = tile_x * sel_decode.actual_tile_width;
    int tile_y_pos = tile_y * sel_decode.actual_tile_height;
    printf("Tile should be decoded at position (%d, %d) in the %dx%d frame\n", 
           tile_x_pos, tile_y_pos, sel_decode.actual_frame_width, sel_decode.actual_frame_height);
    
    // Decode the tile into the full frame buffer
    printf("Calling oapvd_decode_selective for tile [%d,%d]...\n", tile_x, tile_y);
    oapvd_stat_t stat = {0};
    int ret = oapvd_decode_selective(decoder_id, fp, &sel_decode, 0, &stat);
    
    if (ret == OAPV_OK) {
        printf("SUCCESS: Tile decoded successfully\n");
        
        // Analyze the frame to see where data was written
        u16* y_data = (u16*)frame_buffer->a[0];
        u16* u_data = (u16*)frame_buffer->a[1];
        u16* v_data = (u16*)frame_buffer->a[2];
        
        // Check full frame statistics
        int y_nonzero_total = 0, u_nonzero_total = 0, v_nonzero_total = 0;
        int y_min = 65535, y_max = 0;
        
        for(int i = 0; i < sel_decode.actual_frame_width * sel_decode.actual_frame_height; i++) {
            if(y_data[i] != 0) {
                y_nonzero_total++;
                if(y_data[i] < y_min) y_min = y_data[i];
                if(y_data[i] > y_max) y_max = y_data[i];
            }
        }
        
        for(int i = 0; i < (sel_decode.actual_frame_width/2) * sel_decode.actual_frame_height; i++) {
            if(u_data[i] != 0) u_nonzero_total++;
            if(v_data[i] != 0) v_nonzero_total++;
        }
        
        printf("\nFull frame statistics:\n");
        printf("  Y: %d/%d non-zero pixels (%.2f%%) range: %d-%d\n", 
               y_nonzero_total, sel_decode.actual_frame_width*sel_decode.actual_frame_height, 
               100.0*y_nonzero_total/(sel_decode.actual_frame_width*sel_decode.actual_frame_height), y_min, y_max);
        printf("  U: %d/%d non-zero pixels (%.2f%%)\n", 
               u_nonzero_total, (sel_decode.actual_frame_width/2)*sel_decode.actual_frame_height,
               100.0*u_nonzero_total/((sel_decode.actual_frame_width/2)*sel_decode.actual_frame_height));
        printf("  V: %d/%d non-zero pixels (%.2f%%)\n", 
               v_nonzero_total, (sel_decode.actual_frame_width/2)*sel_decode.actual_frame_height,
               100.0*v_nonzero_total/((sel_decode.actual_frame_width/2)*sel_decode.actual_frame_height));
        
        // Check the specific tile area
        int tile_y_nonzero = 0;
        for(int y = tile_y_pos; y < tile_y_pos + sel_decode.actual_tile_height && y < sel_decode.actual_frame_height; y++) {
            for(int x = tile_x_pos; x < tile_x_pos + sel_decode.actual_tile_width && x < sel_decode.actual_frame_width; x++) {
                if(y_data[y * sel_decode.actual_frame_width + x] != 0) {
                    tile_y_nonzero++;
                }
            }
        }
        printf("\nTile area [%d,%d] statistics:\n", tile_x, tile_y);
        printf("  Expected position: (%d,%d) to (%d,%d)\n", 
               tile_x_pos, tile_y_pos, tile_x_pos+sel_decode.actual_tile_width-1, tile_y_pos+sel_decode.actual_tile_height-1);
        printf("  Y pixels in tile area: %d/%d non-zero\n", tile_y_nonzero, sel_decode.actual_tile_width*sel_decode.actual_tile_height);
        
        // Create output filenames
        char y4m_filename[256];
        char raw_filename[256];
        snprintf(y4m_filename, sizeof(y4m_filename), "output/full_frame_mip%d_tile_%d_%d.y4m", mip_level, tile_x, tile_y);
        snprintf(raw_filename, sizeof(raw_filename), "output/full_frame_mip%d_tile_%d_%d.raw", mip_level, tile_x, tile_y);
        
        // Write output files
        write_frame_y4m(y4m_filename, frame_buffer);
        write_frame_raw(raw_filename, frame_buffer);
        
        printf("\nFull frame with tile [%d,%d] saved successfully!\n", tile_x, tile_y);
        printf("View with: ffplay %s\n", y4m_filename);
        printf("Convert to PNG: python test/src/convert_tile_to_png.py %s\n", raw_filename);
        
    } else {
        printf("ERROR: Selective decode failed (return code: %d)\n", ret);
    }
    
    // Cleanup
    delete_frame_buffer(frame_buffer);
    oapvd_delete(decoder_id);
    fclose(fp);
    
    return ret == OAPV_OK ? 0 : -1;
}