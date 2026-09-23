#include "tszip_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void write_le32(unsigned char *buf, size_t offset, uint32_t value) {
    buf[offset + 0] = (unsigned char)(value & 0xFF);
    buf[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
    buf[offset + 2] = (unsigned char)((value >> 16) & 0xFF);
    buf[offset + 3] = (unsigned char)((value >> 24) & 0xFF);
}

/* --------------------------------------------------------------------
 * TSZIP writing -- ported from make_tszip_v3_fixed.py. Streamed
 * textures' .tstream data gets packaged into one "<prefix>.tszip"
 * archive (matching the real format: STORED entries, MD5 extra fields,
 * absolute local-header offsets, doubled EOCD+CD front copy) rather
 * than written as loose files. Verified spec-compliant by running the
 * actual reference script's own validate_tszip() against our output
 * before being wired in here.
 * ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void make_crc32_table(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[n] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_of(const unsigned char *data, size_t len) {
    if (!crc32_table_ready) make_crc32_table();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t n = 0; n < len; n++)
        c = crc32_table[(c ^ data[n]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct { uint32_t state[4]; uint64_t count; unsigned char buffer[64]; } MD5_CTX;
static uint32_t md5_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
#define MD5_F(x,y,z) (((x)&(y))|((~x)&(z)))
#define MD5_G(x,y,z) (((x)&(z))|((y)&(~z)))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|(~z)))
#define MD5_FF(a,b,c,d,x,s,ac) { (a) += MD5_F((b),(c),(d))+(x)+(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
#define MD5_GG(a,b,c,d,x,s,ac) { (a) += MD5_G((b),(c),(d))+(x)+(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
#define MD5_HH(a,b,c,d,x,s,ac) { (a) += MD5_H((b),(c),(d))+(x)+(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }
#define MD5_II(a,b,c,d,x,s,ac) { (a) += MD5_I((b),(c),(d))+(x)+(ac); (a)=md5_rotl((a),(s)); (a)+=(b); }

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],x[16];
    for (int i=0;i<16;i++) x[i]=(uint32_t)block[i*4]|((uint32_t)block[i*4+1]<<8)|((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);
    MD5_FF(a,b,c,d,x[0],7,0xd76aa478u);MD5_FF(d,a,b,c,x[1],12,0xe8c7b756u);MD5_FF(c,d,a,b,x[2],17,0x242070dbu);MD5_FF(b,c,d,a,x[3],22,0xc1bdceeeu);
    MD5_FF(a,b,c,d,x[4],7,0xf57c0fafu);MD5_FF(d,a,b,c,x[5],12,0x4787c62au);MD5_FF(c,d,a,b,x[6],17,0xa8304613u);MD5_FF(b,c,d,a,x[7],22,0xfd469501u);
    MD5_FF(a,b,c,d,x[8],7,0x698098d8u);MD5_FF(d,a,b,c,x[9],12,0x8b44f7afu);MD5_FF(c,d,a,b,x[10],17,0xffff5bb1u);MD5_FF(b,c,d,a,x[11],22,0x895cd7beu);
    MD5_FF(a,b,c,d,x[12],7,0x6b901122u);MD5_FF(d,a,b,c,x[13],12,0xfd987193u);MD5_FF(c,d,a,b,x[14],17,0xa679438eu);MD5_FF(b,c,d,a,x[15],22,0x49b40821u);
    MD5_GG(a,b,c,d,x[1],5,0xf61e2562u);MD5_GG(d,a,b,c,x[6],9,0xc040b340u);MD5_GG(c,d,a,b,x[11],14,0x265e5a51u);MD5_GG(b,c,d,a,x[0],20,0xe9b6c7aau);
    MD5_GG(a,b,c,d,x[5],5,0xd62f105du);MD5_GG(d,a,b,c,x[10],9,0x02441453u);MD5_GG(c,d,a,b,x[15],14,0xd8a1e681u);MD5_GG(b,c,d,a,x[4],20,0xe7d3fbc8u);
    MD5_GG(a,b,c,d,x[9],5,0x21e1cde6u);MD5_GG(d,a,b,c,x[14],9,0xc33707d6u);MD5_GG(c,d,a,b,x[3],14,0xf4d50d87u);MD5_GG(b,c,d,a,x[8],20,0x455a14edu);
    MD5_GG(a,b,c,d,x[13],5,0xa9e3e905u);MD5_GG(d,a,b,c,x[2],9,0xfcefa3f8u);MD5_GG(c,d,a,b,x[7],14,0x676f02d9u);MD5_GG(b,c,d,a,x[12],20,0x8d2a4c8au);
    MD5_HH(a,b,c,d,x[5],4,0xfffa3942u);MD5_HH(d,a,b,c,x[8],11,0x8771f681u);MD5_HH(c,d,a,b,x[11],16,0x6d9d6122u);MD5_HH(b,c,d,a,x[14],23,0xfde5380cu);
    MD5_HH(a,b,c,d,x[1],4,0xa4beea44u);MD5_HH(d,a,b,c,x[4],11,0x4bdecfa9u);MD5_HH(c,d,a,b,x[7],16,0xf6bb4b60u);MD5_HH(b,c,d,a,x[10],23,0xbebfbc70u);
    MD5_HH(a,b,c,d,x[13],4,0x289b7ec6u);MD5_HH(d,a,b,c,x[0],11,0xeaa127fau);MD5_HH(c,d,a,b,x[3],16,0xd4ef3085u);MD5_HH(b,c,d,a,x[6],23,0x04881d05u);
    MD5_HH(a,b,c,d,x[9],4,0xd9d4d039u);MD5_HH(d,a,b,c,x[12],11,0xe6db99e5u);MD5_HH(c,d,a,b,x[15],16,0x1fa27cf8u);MD5_HH(b,c,d,a,x[2],23,0xc4ac5665u);
    MD5_II(a,b,c,d,x[0],6,0xf4292244u);MD5_II(d,a,b,c,x[7],10,0x432aff97u);MD5_II(c,d,a,b,x[14],15,0xab9423a7u);MD5_II(b,c,d,a,x[5],21,0xfc93a039u);
    MD5_II(a,b,c,d,x[12],6,0x655b59c3u);MD5_II(d,a,b,c,x[3],10,0x8f0ccc92u);MD5_II(c,d,a,b,x[10],15,0xffeff47du);MD5_II(b,c,d,a,x[1],21,0x85845dd1u);
    MD5_II(a,b,c,d,x[8],6,0x6fa87e4fu);MD5_II(d,a,b,c,x[15],10,0xfe2ce6e0u);MD5_II(c,d,a,b,x[6],15,0xa3014314u);MD5_II(b,c,d,a,x[13],21,0x4e0811a1u);
    MD5_II(a,b,c,d,x[4],6,0xf7537e82u);MD5_II(d,a,b,c,x[11],10,0xbd3af235u);MD5_II(c,d,a,b,x[2],15,0x2ad7d2bbu);MD5_II(b,c,d,a,x[9],21,0xeb86d391u);
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
}
static void md5_init(MD5_CTX *ctx){ctx->count=0;ctx->state[0]=0x67452301u;ctx->state[1]=0xefcdab89u;ctx->state[2]=0x98badcfeu;ctx->state[3]=0x10325476u;}
static void md5_update(MD5_CTX *ctx, const unsigned char *input, size_t inputLen) {
    size_t index=(size_t)((ctx->count>>3)&0x3F); ctx->count+=((uint64_t)inputLen<<3);
    size_t partLen=64-index, i=0;
    if (inputLen>=partLen) {
        memcpy(&ctx->buffer[index],input,partLen);
        md5_transform(ctx->state,ctx->buffer);
        for (i=partLen;i+64<=inputLen;i+=64)
            md5_transform(ctx->state,&input[i]);
        index=0;
    }
    if (i<inputLen) memcpy(&ctx->buffer[index],&input[i],inputLen-i);
}
static void md5_final(unsigned char digest[16], MD5_CTX *ctx) {
    unsigned char bits[8];
    for (int i=0;i<8;i++) bits[i]=(unsigned char)((ctx->count>>(8*i))&0xFF);
    size_t index=(size_t)((ctx->count>>3)&0x3F);
    size_t padLen=(index<56)?(56-index):(120-index);
    static const unsigned char PADDING[64]={0x80};
    md5_update(ctx,PADDING,padLen);
    md5_update(ctx,bits,8);
    for (int i=0;i<4;i++)
        for (int j=0;j<4;j++)
            digest[i*4+j]=(unsigned char)((ctx->state[i]>>(8*j))&0xFF);
}
static void md5_of(const unsigned char *data, size_t len, unsigned char digest[16]) {
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx,data,len);
    md5_final(digest,&ctx);
}

static void write_le16(unsigned char *buf, size_t offset, uint16_t value) {
    buf[offset] = (unsigned char)(value & 0xFF);
    buf[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
}

#define TSZIP_DOS_TIME 34816u
#define TSZIP_DOS_DATE 35743u
#define TSZIP_EXTRA_ID 0x464Bu


static size_t central_entry_size(size_t fname_len) { return 46 + fname_len + 23; }

/* Builds a .tszip matching make_tszip_v3_fixed.py's exact byte layout:
 * [EOCD][CD][body][CD][EOCD], STORED entries, absolute local-header
 * offsets, MD5 extra fields. Returns 0 on success. */
int build_tszip(const TszipEntry *entries, int count, const char *output_path) {
    uint32_t *crcs = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    unsigned char *md5s = (unsigned char *)malloc((size_t)count * 16);
    size_t *fname_lens = (size_t *)malloc((size_t)count * sizeof(size_t));
    size_t *body_offsets = (size_t *)malloc((size_t)count * sizeof(size_t));
    size_t body_size = 0, cd_size = 0;

    for (int i = 0; i < count; i++) {
        crcs[i] = crc32_of(entries[i].data, entries[i].size);
        md5_of(entries[i].data, entries[i].size, md5s + i * 16);
        fname_lens[i] = strlen(entries[i].name);
        body_offsets[i] = body_size;
        body_size += 30 + fname_lens[i] + entries[i].size;
        cd_size += central_entry_size(fname_lens[i]);
    }

    size_t eocd_size = 22;
    size_t front_index_size = eocd_size + cd_size;
    size_t cd_offset = front_index_size + body_size;
    size_t total_size = eocd_size + cd_size + body_size + cd_size + eocd_size;

    unsigned char eocd[22];
    write_le32(eocd, 0, 0x06054B50u);
    write_le16(eocd, 4, 0); write_le16(eocd, 6, 0);
    write_le16(eocd, 8, (uint16_t)count); write_le16(eocd, 10, (uint16_t)count);
    write_le32(eocd, 12, (uint32_t)cd_size);
    write_le32(eocd, 16, (uint32_t)cd_offset);
    write_le16(eocd, 20, 0);

    unsigned char *cd = (unsigned char *)malloc(cd_size);
    size_t cd_pos = 0;
    for (int i = 0; i < count; i++) {
        write_le32(cd, cd_pos, 0x02014B50u); cd_pos += 4;
        write_le16(cd, cd_pos, 20); cd_pos += 2;
        write_le16(cd, cd_pos, 20); cd_pos += 2;
        write_le16(cd, cd_pos, 0); cd_pos += 2;
        write_le16(cd, cd_pos, 0); cd_pos += 2;
        write_le16(cd, cd_pos, (uint16_t)TSZIP_DOS_TIME); cd_pos += 2;
        write_le16(cd, cd_pos, (uint16_t)TSZIP_DOS_DATE); cd_pos += 2;
        write_le32(cd, cd_pos, crcs[i]); cd_pos += 4;
        write_le32(cd, cd_pos, (uint32_t)entries[i].size); cd_pos += 4;
        write_le32(cd, cd_pos, (uint32_t)entries[i].size); cd_pos += 4;
        write_le16(cd, cd_pos, (uint16_t)fname_lens[i]); cd_pos += 2;
        write_le16(cd, cd_pos, 23); cd_pos += 2;
        write_le16(cd, cd_pos, 0); cd_pos += 2;
        write_le16(cd, cd_pos, 0); cd_pos += 2;
        write_le16(cd, cd_pos, 0); cd_pos += 2;
        write_le32(cd, cd_pos, 0); cd_pos += 4;
        uint32_t abs_off = (uint32_t)(front_index_size + body_offsets[i]);
        write_le32(cd, cd_pos, abs_off); cd_pos += 4;
        memcpy(cd + cd_pos, entries[i].name, fname_lens[i]); cd_pos += fname_lens[i];
        write_le16(cd, cd_pos, (uint16_t)TSZIP_EXTRA_ID); cd_pos += 2;
        write_le16(cd, cd_pos, 19); cd_pos += 2;
        memcpy(cd + cd_pos, "MD5", 3); cd_pos += 3;
        memcpy(cd + cd_pos, md5s + i * 16, 16); cd_pos += 16;
    }

    unsigned char *body = (unsigned char *)malloc(body_size > 0 ? body_size : 1);
    size_t body_pos = 0;
    for (int i = 0; i < count; i++) {
        write_le32(body, body_pos, 0x04034B50u); body_pos += 4;
        write_le16(body, body_pos, 20); body_pos += 2;
        write_le16(body, body_pos, 0); body_pos += 2;
        write_le16(body, body_pos, 0); body_pos += 2;
        write_le16(body, body_pos, (uint16_t)TSZIP_DOS_TIME); body_pos += 2;
        write_le16(body, body_pos, (uint16_t)TSZIP_DOS_DATE); body_pos += 2;
        write_le32(body, body_pos, crcs[i]); body_pos += 4;
        write_le32(body, body_pos, (uint32_t)entries[i].size); body_pos += 4;
        write_le32(body, body_pos, (uint32_t)entries[i].size); body_pos += 4;
        write_le16(body, body_pos, (uint16_t)fname_lens[i]); body_pos += 2;
        write_le16(body, body_pos, 0); body_pos += 2;
        memcpy(body + body_pos, entries[i].name, fname_lens[i]); body_pos += fname_lens[i];
        memcpy(body + body_pos, entries[i].data, entries[i].size); body_pos += entries[i].size;
    }

    unsigned char *buf = (unsigned char *)malloc(total_size);
    size_t pos = 0;
    memcpy(buf + pos, eocd, 22); pos += 22;
    memcpy(buf + pos, cd, cd_size); pos += cd_size;
    memcpy(buf + pos, body, body_size); pos += body_size;
    memcpy(buf + pos, cd, cd_size); pos += cd_size;
    memcpy(buf + pos, eocd, 22); pos += 22;

    FILE *f = fopen(output_path, "wb");
    int ok = 0;
    if (f) {
        size_t written = fwrite(buf, 1, total_size, f);
        fclose(f);
        ok = (written == total_size);
    }

    free(crcs); free(md5s); free(fname_lens); free(body_offsets);
    free(cd); free(body); free(buf);
    return ok ? 0 : -1;
}
