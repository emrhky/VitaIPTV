/*
 * H.264 SPS without "bitstream_restriction": the decoder must then assume the level's largest picture
 * buffer (720p at level 3.1: 5 pictures) and gives no picture before it is full. The Vita's decoder ran out
 * of picture memory on such streams ("Decoder produced no picture", e.g. a 720p25 channel with 2 reference
 * pictures, IBBP). The restriction is added with max_dec_frame_buffering = num_reorder_frames =
 * max_num_ref_frames, which the standard allows and which these streams keep; an SPS that has it is left
 * as it is.
 */
#include "spsfix.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t nbits, pos; int err; } Rd;

static uint32_t rd_u(Rd *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        if (b->pos >= b->nbits) { b->err = 1; return 0; }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t rd_ue(Rd *b)
{
    int zeros = 0;
    while (!b->err && rd_u(b, 1) == 0) { if (++zeros > 31) { b->err = 1; return 0; } }
    if (b->err) return 0;
    return zeros ? (((uint32_t)1 << zeros) - 1) + rd_u(b, zeros) : 0;
}

static void rd_se(Rd *b) { (void)rd_ue(b); }

static void skip_scaling(Rd *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && !b->err; j++) {
        if (next != 0) {
            uint32_t k = rd_ue(b);
            int d = (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
            next = (last + d + 256) % 256;
        }
        last = next ? next : last;
    }
}

static void skip_hrd(Rd *b)
{
    uint32_t cnt = rd_ue(b);
    rd_u(b, 4); rd_u(b, 4);
    for (uint32_t i = 0; i <= cnt && i < 32 && !b->err; i++) { rd_ue(b); rd_ue(b); rd_u(b, 1); }
    rd_u(b, 5); rd_u(b, 5); rd_u(b, 5); rd_u(b, 5);
}

typedef struct { uint8_t *p; size_t cap, pos; int err; } Wr;

static void wr_u(Wr *w, uint32_t v, int n)
{
    while (n-- > 0) {
        size_t byte = w->pos >> 3;
        if (byte >= w->cap) { w->err = 1; return; }
        if ((w->pos & 7) == 0) w->p[byte] = 0;
        if ((v >> n) & 1) w->p[byte] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

static void wr_ue(Wr *w, uint32_t v)
{
    uint32_t x = v + 1;
    int len = 0;
    for (uint32_t t = x; t > 1; t >>= 1) len++;
    wr_u(w, 0, len);
    wr_u(w, x, len + 1);
}

size_t sps_add_restriction(const uint8_t *nal, size_t len, uint8_t *out, size_t cap, int *reorder)
{
    uint8_t rbsp[512], wb[540];
    size_t n = 0;
    if (len < 4 || (nal[0] & 0x1F) != 7) return 0;
    for (size_t i = 1; i < len; i++) {
        if (n >= sizeof rbsp) return 0;
        if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
        rbsp[n++] = nal[i];
    }
    /* the payload ends before the stop bit (rbsp_trailing_bits); trailing zero bytes are padding */
    while (n && rbsp[n - 1] == 0) n--;
    if (!n) return 0;
    int last = 0;
    while (!((rbsp[n - 1] >> last) & 1)) last++;
    size_t payload_bits = n * 8 - (size_t)last - 1;

    Rd b = { rbsp, payload_bits, 0, 0 };
    int profile = (int)rd_u(&b, 8);
    rd_u(&b, 16);                                           /* constraint flags, level */
    rd_ue(&b);                                              /* sps id */
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135: {
        uint32_t chroma = rd_ue(&b);
        if (chroma == 3) rd_u(&b, 1);
        rd_ue(&b); rd_ue(&b); rd_u(&b, 1);
        if (rd_u(&b, 1))
            for (int i = 0; i < (chroma != 3 ? 8 : 12) && !b.err; i++)
                if (rd_u(&b, 1)) skip_scaling(&b, i < 6 ? 16 : 64);
        break;
    }
    default: break;
    }
    rd_ue(&b);                                              /* log2_max_frame_num - 4 */
    uint32_t poc = rd_ue(&b);
    if (poc == 0) rd_ue(&b);
    else if (poc == 1) {
        rd_u(&b, 1); rd_se(&b); rd_se(&b);
        uint32_t cnt = rd_ue(&b);
        for (uint32_t i = 0; i < cnt && i < 256 && !b.err; i++) rd_se(&b);
    }
    uint32_t refs = rd_ue(&b);
    rd_u(&b, 1);                                            /* gaps_in_frame_num_allowed */
    rd_ue(&b); rd_ue(&b);                                   /* size in macroblocks */
    if (!rd_u(&b, 1)) rd_u(&b, 1);                          /* frame_mbs_only, mb_adaptive */
    rd_u(&b, 1);                                            /* direct_8x8_inference */
    if (rd_u(&b, 1)) { rd_ue(&b); rd_ue(&b); rd_ue(&b); rd_ue(&b); }
    if (b.err || refs > 16) return 0;
    size_t vui_flag_pos = b.pos;
    size_t keep;                                            /* bits copied as they are */
    int vui = (int)rd_u(&b, 1);
    if (b.err) return 0;
    if (!vui) keep = vui_flag_pos;
    else {
        if (rd_u(&b, 1) && rd_u(&b, 8) == 255) rd_u(&b, 32);   /* aspect ratio */
        if (rd_u(&b, 1)) rd_u(&b, 1);                           /* overscan */
        if (rd_u(&b, 1)) { rd_u(&b, 5); if (rd_u(&b, 1)) rd_u(&b, 24); }   /* video signal type */
        if (rd_u(&b, 1)) { rd_ue(&b); rd_ue(&b); }              /* chroma location */
        if (rd_u(&b, 1)) { rd_u(&b, 32); rd_u(&b, 32); rd_u(&b, 1); }   /* timing */
        int nal_hrd = (int)rd_u(&b, 1);
        if (nal_hrd) skip_hrd(&b);
        int vcl_hrd = (int)rd_u(&b, 1);
        if (vcl_hrd) skip_hrd(&b);
        if (nal_hrd || vcl_hrd) rd_u(&b, 1);                    /* low_delay_hrd */
        rd_u(&b, 1);                                            /* pic_struct_present */
        if (b.err) return 0;
        keep = b.pos;
        if (rd_u(&b, 1)) return 0;                              /* the restriction is there already */
        if (b.err) return 0;
    }

    uint32_t dpb = refs ? refs : 1;
    Wr w = { wb, sizeof wb, 0, 0 };
    Rd c = { rbsp, payload_bits, 0, 0 };
    for (size_t i = 0; i < keep; i++) wr_u(&w, rd_u(&c, 1), 1);
    if (!vui) {
        wr_u(&w, 1, 1);                                     /* vui_parameters_present */
        wr_u(&w, 0, 1); wr_u(&w, 0, 1); wr_u(&w, 0, 1); wr_u(&w, 0, 1); wr_u(&w, 0, 1);   /* aspect .. timing */
        wr_u(&w, 0, 1); wr_u(&w, 0, 1);                     /* nal / vcl hrd */
        wr_u(&w, 0, 1);                                     /* pic_struct_present */
    }
    wr_u(&w, 1, 1);                                         /* bitstream_restriction */
    wr_u(&w, 1, 1);                                         /* motion_vectors_over_pic_boundaries */
    wr_ue(&w, 2); wr_ue(&w, 1); wr_ue(&w, 16); wr_ue(&w, 16);   /* the standard's values when absent */
    wr_ue(&w, dpb);                                         /* max_num_reorder_frames */
    wr_ue(&w, dpb);                                         /* max_dec_frame_buffering */
    wr_u(&w, 1, 1);                                         /* stop bit */
    while (w.pos & 7) wr_u(&w, 0, 1);
    if (w.err) return 0;
    size_t wn = w.pos >> 3;

    /* NAL header, then the payload with emulation prevention */
    size_t o = 0;
    if (cap < 1) return 0;
    out[o++] = nal[0];
    int zeros = 0;
    for (size_t i = 0; i < wn; i++) {
        if (zeros >= 2 && wb[i] <= 3) {
            if (o >= cap) return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        out[o++] = wb[i];
        zeros = wb[i] == 0 ? zeros + 1 : 0;
    }
    if (reorder) *reorder = (int)dpb;
    return o;
}
