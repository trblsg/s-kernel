/* SPDX-License-Identifier: MIT
 * S9: bounds-checked texture footprint for Apple's private MTLTextureLayout SPI.
 * Inspired by Redfin SweetTextureLayout; arithmetic is independent of Metal so
 * it can be tested on Linux before wiring into the iOS frontend.
 */
#ifndef S9_TEXTURE_LAYOUT_H
#define S9_TEXTURE_LAYOUT_H
#include <stdint.h>
#include <stddef.h>

typedef struct s9_texture_layout {
    uint64_t row_bytes, block_rows, size, alignment;
    uint32_t block_width, block_height, bytes_per_block;
    uint64_t width, height;
} s9_texture_layout;

static inline int s9_tex_mul(uint64_t x, uint64_t y, uint64_t *out) {
    if (y && x > UINT64_MAX / y) return 0;
    *out = x * y;
    return 1;
}
static inline int s9_tex_add(uint64_t x, uint64_t y, uint64_t *out) {
    if (x > UINT64_MAX - y) return 0;
    *out = x + y;
    return 1;
}

/* Only formats whose memory footprints are understood; never guess for
 * depth/stencil, multi-plane, PVRTC or Apple-private formats. */
static inline int s9_tex_block(uint32_t fmt, uint32_t *bw, uint32_t *bh, uint32_t *b) {
    static const uint8_t astc[15][2] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{0,0},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
    };
    if ((fmt >= 186 && fmt <= 200) || (fmt >= 204 && fmt <= 218)) {
        uint32_t i = fmt >= 204 ? fmt - 204 : fmt - 186;
        if (!astc[i][0]) return 0;
        *bw = astc[i][0]; *bh = astc[i][1]; *b = 16; return 1;
    }
    switch (fmt) {
    case 170: case 172: case 180: case 181: case 182: case 183:
        *bw=4;*bh=4;*b=8;return 1;
    case 174: case 176: case 178: case 179:
        *bw=4;*bh=4;*b=16;return 1;
    default: break;
    }
    uint32_t bytes=0;
    switch (fmt) {
    case 1: case 10: case 11: case 12: case 13: case 14: bytes=1;break;
    case 20: case 22: case 23: case 24: case 25:
    case 30: case 32: case 33: case 34:
    case 40: case 41: case 42: case 43: bytes=2;break;
    case 103: case 104: case 105:
    case 110: case 111: case 112: case 113: case 114: case 115:
    case 552: case 553: bytes=8;break;
    case 123: case 124: case 125: bytes=16;break;
    /* Mainstream BGRA8/RGBA8/RGBA10 and R32/RG16 formats. */
    case 53: case 54: case 55:
    case 60: case 61: case 62: case 63: case 64: case 65:
    case 70: case 71: case 72: case 73: case 74: case 75:
    case 80: case 81: case 90: case 91: case 92: case 93: case 94:
    case 101: case 102:
    case 554: case 555: bytes=4;break;
    default:return 0;
    }
    *bw=1; *bh=1; *b=bytes; return 1;
}

/* device_pitch=0 means packed blocks; for a linear format it MUST be the
 * pitch the PanVK bridge reports. page must be a nonzero power of two.
 * Offset calculations and page rounding fail on overflow. */
static inline int s9_tex_plan(uint32_t fmt, uint64_t width, uint64_t height,
                              uint64_t device_pitch, uint64_t page, s9_texture_layout *out) {
    uint32_t bw,bh,bytes;
    uint64_t blocks_w,blocks_h,min_row,row,raw,padded;
    if (!out || !width || !height || !page || (page & (page-1)) ||
        !s9_tex_block(fmt,&bw,&bh,&bytes)) return 0;
    if (!s9_tex_add(width,bw-1,&blocks_w) ||
        !s9_tex_add(height,bh-1,&blocks_h)) return 0;
    blocks_w/=bw; blocks_h/=bh;
    if (!s9_tex_mul(blocks_w,bytes,&min_row)) return 0;
    if (bw==1 && bh==1 && !device_pitch) return 0;
    row=device_pitch?device_pitch:min_row;
    if (row<min_row || (row%bytes)!=0 || !s9_tex_mul(row,blocks_h,&raw) ||
        !s9_tex_add(raw,page-1,&padded))return 0;
    padded&=~(page-1);
    if (padded>SIZE_MAX || row>SIZE_MAX) return 0;
    out->row_bytes=row;
    out->block_rows=blocks_h;
    out->size=padded;
    out->alignment=page;
    out->block_width=bw;
    out->block_height=bh;
    out->bytes_per_block=bytes;
    out->width=width;out->height=height;
    return 1;
}

/* Verify the region fits within the computed allocation; callers also need
 * to check external linearBytesPerRow (whose allocation size we do not own). */
static inline int s9_tex_region(const s9_texture_layout *p,
                                uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                                uint64_t *offset, uint64_t *row_bytes, uint64_t *rows) {
    uint64_t block_x,block_y,block_w,block_h,cell,beg,end,last;
    if (!p || !w || !h || !p->block_width || !p->block_height ||
        x>=p->width || y>=p->height || w>p->width-x || h>p->height-y ||
        (x%p->block_width) || (y%p->block_height)) return 0;
    /* Partial compressed blocks may only touch the actual image edge,
     * not interior texels or padded row bytes. */
    if ((w%p->block_width && w!=p->width-x) ||
        (h%p->block_height && h!=p->height-y)) return 0;
    block_x=x/p->block_width; block_y=y/p->block_height;
    if (!s9_tex_add(w,p->block_width-1,&block_w) ||
        !s9_tex_add(h,p->block_height-1,&block_h))return 0;
    block_w/=p->block_width;block_h/=p->block_height;
    if (!s9_tex_mul(block_x,p->bytes_per_block,&cell) ||
        !s9_tex_mul(block_w,p->bytes_per_block,&end) ||
        !s9_tex_add(cell,end,&last) || last>p->row_bytes ||
        !s9_tex_add(block_y,block_h,&last) || last>p->block_rows ||
        !s9_tex_mul(block_y,p->row_bytes,&beg) ||
        !s9_tex_add(beg,cell,&beg) ||
        !s9_tex_mul(block_h-1,p->row_bytes,&last) ||
        !s9_tex_add(beg,last,&last) ||
        !s9_tex_add(last,end,&last) || last>p->size) return 0;
    if (offset) *offset=beg;
    if (row_bytes) *row_bytes=end;
    if (rows) *rows=block_h;
    return 1;
}
/* The allocation length is not supplied by this SPI. Check arithmetic and
 * optional per-image stride, without claiming to know the external buffer size. */
static inline int s9_tex_linear_span(uint64_t offset,uint64_t pitch,uint64_t bytes,
                                      uint64_t rows,uint64_t image) {
    uint64_t span,end;
    if (!rows || !bytes || pitch<bytes ||
        !s9_tex_mul(rows-1,pitch,&span) || !s9_tex_add(span,bytes,&span) ||
        !s9_tex_add(offset,span,&end) || end>SIZE_MAX) return 0;
    return !image || span<=image;
}
#endif
