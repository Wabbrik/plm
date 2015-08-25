#define NAME "gypsy"  

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>

#include "util.h"
#include "mixer.h"
#include "coder.h"
#include "predictor.h"
#include "model.h"

/******************************************************************************
 * GLOBAL STATE 
 ******************************************************************************/

/* Compression level (0-9) */
int level = DEFAULT_OPTION;  

/* COMPRESS OR DECOMPRESS (defined in util.h) */
int MODE;

/******************************************************************************
 * FUNCTIONS 
 ******************************************************************************/

/**
 * compress()
 * ``````````
 * Compress a file.
 *
 * @filename: Path of source file
 * @filesize: Size of source file
 * @enc     : Encoder object
 * Return   : Nothing 
 */
void compress(FILE *dst, FILE *src, long src_size, struct ac_t *ac) 
{
        int i;
        int j;
        int p    = 2048; /* Prob is (0-4096), so start this at 1/2 = 2048 */
        int byte = 0;
        int bit;
        uint32_t code;
        uint32_t word = 0;
        uint8_t  part = 0;

        /* For each byte in the file */
        for (i=0; i<src_size; i++) {

                /* Read a byte from the source stream */
                byte = getc(src);

                /* For each bit in the byte */
                for (j=7; j>=0; j--) {

                        bit = (byte >> j) & 1;

                        if (j == 0) {
                                /* 
                                 * Add the full byte (sans trailing '1')
                                 * to the word history 
                                 */
                                word = (word << 8) + byte;
                                /* 
                                 * Use a trailing '1' bit here to ensure
                                 * that consecutive zeroes are hashed to
                                 * something different as this '1' gets
                                 * left-shifted.
                                 */
                                part = 1;
                        } else {
                                /* 
                                 * Add the bit to the partially-read
                                 * byte.
                                 */
                                part = (part << 1) | bit;
                        }

                        /* Pass the actual bit and prediction to the encoder */
                        ac_encode_bit(ac, p, bit);

                        while ((code = ac_encode_flush(ac))!=UINT32_MAX) {
                                /* Write any ready bytes to the archive */
                                putc(code, dst);
                        }

                        /* Predict the next bit using the model */
                        p = MODEL(word, part, bit, (j == 0));

                        /* Smooth the prediction with SSE */
                        p = SMOOTH(p, word, part, bit);
                }
        }

        ac_encode_finish(ac, dst);
}


/**
 * decompress()
 * ````````````
 * Decompress a file.
 * 
 * @path : Intended path of target (decompressed) file. 
 * @size : Intended size of target (decomrpessed) file (recorded in header)
 * @enc  : Encoder object
 * Return: Nothing
 */
void decompress(FILE *dst, FILE *src, long dst_size, struct ac_t *ac) 
{
        int i;
        int j;
        int b;
        int p = 2048;
        int bit;
        int byte;
        int next_byte;
        uint32_t word;
        uint8_t part;

        /* For each byte of the eventual (decompressed) target */
        for (i=0; i<dst_size; i++) {

                byte = 0;

                /* For each bit of the byte */
                for (j=0; j<8; j++) {

                        /* Decode a bit */
                        bit = ac_decode_bit(ac, p);

                        /* Build the byte by adding this bit */
                        byte = (byte << 1) | bit;

                        if (j == 7) {
                                /* 
                                 * Add the full byte (sans trailing '1')
                                 * to the word history 
                                 */
                                word = (word << 8) + byte;
                                /* 
                                 * Use a trailing '1' bit here to ensure
                                 * that consecutive zeroes are hashed to
                                 * something different as this '1' gets
                                 * left-shifted.
                                 */
                                part = 1;
                        } else {
                                /* 
                                 * Add the bit to the partially-read
                                 * byte.
                                 */
                                part = (part << 1) | bit;
                        }

                        for (;;) {
                                /* Get the next byte from the source */
                                next_byte = getc(src);

                                /* Try to add it to the encoder */
                                if (!ac_decode_try_add_byte(ac, next_byte)) {
                                        /* Push it back if unsuccessful */
                                        ungetc(next_byte, src);    
                                        break;
                                }
                        }

                        /* Predict the next bit using the model */
                        p = MODEL(word, part, bit, (j == 7));

                        /* Smooth the prediction with SSE */
                        p = SMOOTH(p, word, part, bit);
                }

                /* Write the decoded byte to the destination. */
                putc(byte, dst); 
        }
}

/******************************************************************************
 * MAIN 
 ******************************************************************************/

int main(int argc, char** argv) 
{
        level = DEFAULT_OPTION;

        struct ac_t ac;

        char header[255];     /* Will hold the header line */
        char compressor[255];

        char *source_name;
        char *source_path;
        FILE *source_file = 0;  // compressed file
        long  source_size;

        char  target_name[4096];
        char *target_path;
        FILE *target_file = 0;
        long  target_size;
        long  start;

        if (argc < 2) {
                printf("MEM_MAX:%d\n\n", MEM_MAX);
                printf("Usage: "NAME" [-d] <filename>\n");
                exit(1);
        }

        if (!strcmp(argv[1], "-d")) {

                if (argc <= 3) {
                        printf("USAGE:"NAME" -d <input> <output>\n");
                        exit(1);
                }

                MODE = DECOMPRESS;

                source_path = argv[2];
                target_path = argv[3];

        } else {
                MODE = COMPRESS;

                source_path = argv[1];
        }

        ilog_init(); // TODO: fix this shit!
        stretch_init(); // TODO: fix this shit!
        log_init();

        if (MODE == COMPRESS) {

                sprintf(target_name, "%s.zpaq", basename(strdup(source_path)));

                target_file = fopen(target_name, "wb+");
                source_file = fopen(source_path, "rb");

                if (!target_file) { 
                        printf("Error creating %s\n", target_name);
                        exit(1);
                }
                if (!source_file) { 
                        printf("Error opening %s\n", source_path);
                        exit(1);
                }

                source_size = file_length(source_file);

                /*
                 * Header format:
                 *      program_name:level:source_size
                 */
                fprintf(target_file, "%s:%d:%ld\r\n\x1A", 
                        NAME,
                        level, 
                        source_size
                );

                printf("Creating archive %s...\n", target_name);
        }

        if (MODE == DECOMPRESS) {

                source_file = fopen(source_path, "rb+");
                source_size = file_length(source_file);

                if (!source_file) { 
                        printf("Error opening %s\n", source_path);
                        exit(1);
                }

                /* 
                 * Read header and get options
                 */
                fscanf(source_file, "%255[^:]:%d:%ld\r\n\x1A", 
                        &compressor, 
                        &level, 
                        &target_size
                );

                printf("compressor:%s\nlevel:%d\ntarget-size:%ld\n", compressor, level, target_size);

                if (strcmp(compressor, NAME) != 0) {
                        printf("%s: not a "NAME" file\n", source_path);
                        exit(1);
                }
                
                if (level<0 || level>9) { 
                        level = DEFAULT_OPTION;
                }

                printf("Inflating %s at level %d\n", 
                        source_path, 
                        level
                );
        }

        if (MODE == COMPRESS) {
        
                ac_init(&ac, target_file);

                if (!(source_file = fopen(source_path, "rb"))) {
                        perror(source_path);
                        exit(1);
                }

                start = ftell(target_file);

                compress(target_file, source_file, source_size, &ac);

                printf("%ld -> %ld\n", source_size, ftell(target_file)-start);

        } else {
                ac_init(&ac, source_file);

                /*
                 * If an output file already exists, then we will 
                 * compare the result of this decompression with 
                 * its contents.
                 */
                if ((target_file = fopen(target_path, "rb"))) {
                        printf("File exists.\n");
                        fclose(target_file);
                        return 1;
                }

                /* Create a new file */
                if (!(target_file = fopen(target_path, "wb"))) {
                        printf("Could not open file.\n");
                        exit(1);
                }

                decompress(target_file, source_file, target_size, &ac);

                printf("%ld -> %ld\n", source_size, ftell(target_file));
        }

        log_close();

        return 0;
}

