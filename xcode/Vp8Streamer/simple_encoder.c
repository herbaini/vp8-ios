/*
 Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 
 Use of this source code is governed by a BSD-style license
 that can be found in the LICENSE file in the root of the source
 tree. An additional intellectual property rights grant can be found
 in the file PATENTS.  All contributing project authors may
 be found in the AUTHORS file in the root of the source tree.
 */


/*
 This is an example of a simple encoder loop. It takes an input file in
 YV12 format, passes it through the encoder, and writes the compressed
 frames to disk in IVF format. Other decoder examples build upon this
 one.
 
 The details of the IVF format have been elided from this example for
 simplicity of presentation, as IVF files will not generally be used by
 your application. In general, an IVF file consists of a file header,
 followed by a variable number of frames. Each frame consists of a frame
 header followed by a variable length payload. The length of the payload
 is specified in the first four bytes of the frame header. The payload is
 the raw compressed data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx_encoder.h"
#include "vp8cx.h"
#define vpx_interface (vpx_codec_vp8_cx())
#define fourcc    0x30385056

#define IVF_FILE_HDR_SZ  (32)
#define IVF_FRAME_HDR_SZ (12)

static void mem_put_le16(char *mem, unsigned int val) {
    mem[0] = val;
    mem[1] = val>>8;
}

static void mem_put_le32(char *mem, unsigned int val) {
    mem[0] = val;
    mem[1] = val>>8;
    mem[2] = val>>16;
    mem[3] = val>>24;
}

static void die(const char *fmt, ...) {
    va_list ap;
    
    va_start(ap, fmt);
    vprintf(fmt, ap);
    if(fmt[strlen(fmt)-1] != '\n')
        printf("\n");
    exit(EXIT_FAILURE);
}

static void die_codec(vpx_codec_ctx_t *ctx, const char *s) {                  //
    const char *detail = vpx_codec_error_detail(ctx);                         //
    //
    printf("%s: %s\n", s, vpx_codec_error(ctx));                              //
    if(detail)                                                                //
        printf("    %s\n",detail);                                            //
    exit(EXIT_FAILURE);                                                       //
}                                                                             //

static int read_frame(FILE *f, vpx_image_t *img) {
    
    size_t nbytes, to_read;
    int    res = 1;
    
    to_read = (img->w * img->h * 3) / 2;
    nbytes = fread(img->planes[0], 1, to_read, f);
    img->fmt = VPX_IMG_FMT_YV12;
    
    if(nbytes != to_read) {
        res = 0;
        if(nbytes > 0)
            printf("Warning: Read partial frame. Check your width & height!\n");
    }
    
    return res;
}

static void write_ivf_file_header(FILE *outfile,
                                  const vpx_codec_enc_cfg_t *cfg,
                                  int frame_cnt) {
    char header[32];
    
    if(cfg->g_pass != VPX_RC_ONE_PASS && cfg->g_pass != VPX_RC_LAST_PASS)
        return;
    header[0] = 'D';
    header[1] = 'K';
    header[2] = 'I';
    header[3] = 'F';
    mem_put_le16(header+4,  0);                   /* version */
    mem_put_le16(header+6,  32);                  /* headersize */
    mem_put_le32(header+8,  fourcc);              /* headersize */
    mem_put_le16(header+12, cfg->g_w);            /* width */
    mem_put_le16(header+14, cfg->g_h);            /* height */
    mem_put_le32(header+16, cfg->g_timebase.den); /* rate */
    mem_put_le32(header+20, cfg->g_timebase.num); /* scale */
    mem_put_le32(header+24, frame_cnt);           /* length */
    mem_put_le32(header+28, 0);                   /* unused */
    
    if(fwrite(header, 1, 32, outfile));
}


static void write_ivf_frame_header(const vpx_codec_cx_pkt_t *pkt, char* header)
{
    //char             header[12];
    vpx_codec_pts_t  pts;
    
    if(pkt->kind != VPX_CODEC_CX_FRAME_PKT)
        return;
    
    pts = pkt->data.frame.pts;
    mem_put_le32(header, pkt->data.frame.sz);
    mem_put_le32(header+4, pts&0xFFFFFFFF);
    mem_put_le32(header+8, pts >> 32);
    
    //if(fwrite(header, 1, 12, outfile));
}


static vpx_codec_ctx_t      codec;
static vpx_codec_enc_cfg_t  cfg;
static int                  frame_cnt = 0;
static vpx_codec_err_t      res;
static int                  frame_avail;
static int                  got_data;
static int                  flags = 0;

int setup_encoder(long width, long height)
{
    
    // Create image using dimensions
    if(width < 16 || width%2 || height <16 || height%2)
        die("Invalid resolution: %ldx%ld", width, height);
    
    printf("Using %s\n",vpx_codec_iface_name(vpx_interface));
    
    /* Populate encoder configuration */                                      //
    res = vpx_codec_enc_config_default(vpx_interface, &cfg, 0);               //
    if(res) {                                                                 //
        printf("Failed to get config: %s\n", vpx_codec_err_to_string(res));   //
        return EXIT_FAILURE;                                                  //
    }                                                                         //
    
    /* Update the default configuration with our settings */                  //
    cfg.rc_target_bitrate = width * height * cfg.rc_target_bitrate            //
    / cfg.g_w / cfg.g_h;                              //
    cfg.g_w = width;                                                          //
    cfg.g_h = height;                                                         //
    
    /* Initialize codec */                                                //
    if(vpx_codec_enc_init(&codec, vpx_interface, &cfg, 0))                    //
        die_codec(&codec, "Failed to initialize encoder");                //
    
    frame_avail = 1;
    got_data = 0;
    
    return EXIT_SUCCESS;
}
 
const vpx_codec_cx_pkt_t * encode_frame(vpx_image_t *raw)
{
    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    
    frame_avail = 1;

    if(vpx_codec_encode(&codec, frame_avail? raw : NULL, frame_cnt, 1, flags, VPX_DL_REALTIME))
        die_codec(&codec, "Failed to encode frame");
    got_data = 0;

    // Sometimes there might be more than one packet, so if you get errors this is why
    pkt = vpx_codec_get_cx_data(&codec, &iter);
        
    got_data = 1;
    switch(pkt->kind) {
        case VPX_CODEC_CX_FRAME_PKT:                                
            
            // Write header and frame to file
            //write_ivf_frame_header(outfile, pkt);                   
            //fwrite(pkt->data.frame.buf, 1, pkt->data.frame.sz, outfile); 
            
            break;
            
        default:
            printf("WARNING - Got a different kind of packet, don't know how to handle");
            break;
    }
    
    printf(pkt->kind == VPX_CODEC_CX_FRAME_PKT
           && (pkt->data.frame.flags & VPX_FRAME_IS_KEY)? "K":".");
    fflush(stdout);

    frame_cnt++;

    return pkt;
}

int finalise_encoder()
{
    
    printf("Processed %d frames.\n",frame_cnt-1);
    if(vpx_codec_destroy(&codec))                                             //
        die_codec(&codec, "Failed to destroy codec");                         //
    
    return EXIT_SUCCESS;
}