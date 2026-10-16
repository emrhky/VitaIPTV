/* QR code of https://github.com/emrhky/VitaIPTV (version 3, error correction M), made with OpenCV's
 * QRCodeEncoder and read back with its QRCodeDetector (tests/run_qr_test.py). One row per entry, the
 * leftmost module in the highest of the 29 bits; drawn on white with a 4-module quiet zone. */
#ifndef QR_GITHUB_H
#define QR_GITHUB_H
#include <stdint.h>

#define QR_GITHUB_N 29
static const uint32_t QR_GITHUB[QR_GITHUB_N] = {
    0x1FC3EF7F, 0x104AF441, 0x175DA65D, 0x1750695D,
    0x17553F5D, 0x10520D41, 0x1FD5557F, 0x001D5100,
    0x17C30A7C, 0x069BEB71, 0x1E6FE510, 0x1215910A,
    0x04757C0C, 0x0C163FF1, 0x17EC299C, 0x132173F2,
    0x1BF432AC, 0x1B21F2F5, 0x154EE974, 0x1413A5C2,
    0x19667BF7, 0x00143D1F, 0x1FC9375C, 0x105B6313,
    0x175809F6, 0x175C730F, 0x175033FE, 0x1048E5AA,
    0x1FDA6A74,
};

#endif
