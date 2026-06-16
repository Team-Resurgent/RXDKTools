/*
 * xrsa_des.c
 *
 * Source replacement for the rsa32 DES object files:
 *   rsa32_deskey.obj  (deskey, desparityonkey, desreducekey,
 *                      deskeyexpand, tripledes2key, tripledes3key)
 *   rsa32_desfast.obj (des)
 *   rsa32_desport.obj (tripledes)
 *
 * This is the Eric Young / SSLeay "libdes" (a.k.a. OpenSSL DES_encrypt1)
 * implementation. The S-box/permutation table (Spbox == des_SPtrans) and the
 * key-schedule selection table (Sel == des_skb) were extracted verbatim from
 * rsa32_spb.obj, and the rotate schedule (double_shift) and nibble popcount
 * table (DESParityTable) from rsa32_deskey.obj, so the output matches the
 * original rsa32.lib bit-for-bit.
 */

#include <windows.h>
#include <des.h>
#include <tripldes.h>
#include <modes.h>

/* Tables extracted from rsa32 (defined in xrsa_des_tables.c). */
extern const unsigned long Spbox[512];          /* des_SPtrans[8][64] */
extern const unsigned long Sel[512];            /* des_skb[8][64]     */
extern const unsigned char double_shift[16];
extern const unsigned char DESParityTable[16];

#define SP(box)  (&Spbox[(box) * 64])
#define SKB(box) (&Sel[(box) * 64])

/* rotate right by n (libdes ROTATE) */
#define ROTR(a, n) (((a) >> (n)) | ((a) << (32 - (n))))
/* rotate left by n */
#define ROTL(a, n) (((a) << (n)) | ((a) >> (32 - (n))))

#define PERM_OP(a, b, t, n, m)            \
    do {                                  \
        (t) = ((((a) >> (n)) ^ (b)) & (m)); \
        (b) ^= (t);                       \
        (a) ^= ((t) << (n));              \
    } while (0)

static __inline DWORD load_le32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static __inline void store_le32(BYTE *p, DWORD v)
{
    p[0] = (BYTE)(v);
    p[1] = (BYTE)(v >> 8);
    p[2] = (BYTE)(v >> 16);
    p[3] = (BYTE)(v >> 24);
}

/*
 * Build the encrypt key schedule (16 rounds x 2 longs) into the DESTable.
 * Direction is selected at cipher time via the op argument to des().
 */
void RSA32API deskey(DESTable *pTable, unsigned char *key)
{
    DWORD c;
    DWORD d;
    DWORD t;
    DWORD s;
    DWORD u;
    DWORD *k = &pTable->keytab[0][0];
    int i;

    c = load_le32(key);
    d = load_le32(key + 4);

    PERM_OP(d, c, t, 4, 0x0f0f0f0fUL);

    /* HPERM_OP(c, t, -2, 0xcccc0000) */
    t = ((c << 18) ^ c) & 0xcccc0000UL;
    c = c ^ t ^ (t >> 18);
    /* HPERM_OP(d, t, -2, 0xcccc0000) */
    t = ((d << 18) ^ d) & 0xcccc0000UL;
    d = d ^ t ^ (t >> 18);

    PERM_OP(d, c, t, 1, 0x55555555UL);
    PERM_OP(c, d, t, 8, 0x00ff00ffUL);
    PERM_OP(d, c, t, 1, 0x55555555UL);

    d = (((d & 0x000000ffUL) << 16) |
          (d & 0x0000ff00UL) |
         ((d & 0x00ff0000UL) >> 16) |
         ((c & 0xf0000000UL) >> 4));
    c &= 0x0fffffffUL;

    for (i = 0; i < 16; i++)
    {
        if (double_shift[i])
        {
            c = ((c >> 2) | (c << 26));
            d = ((d >> 2) | (d << 26));
        }
        else
        {
            c = ((c >> 1) | (c << 27));
            d = ((d >> 1) | (d << 27));
        }
        c &= 0x0fffffffUL;
        d &= 0x0fffffffUL;

        s = SKB(0)[ (c)        & 0x3f] |
            SKB(1)[((c >>  6) & 0x03) | ((c >>  7) & 0x3c)] |
            SKB(2)[((c >> 13) & 0x0f) | ((c >> 14) & 0x30)] |
            SKB(3)[((c >> 20) & 0x01) | ((c >> 21) & 0x06) | ((c >> 22) & 0x38)];

        t = SKB(4)[ (d)        & 0x3f] |
            SKB(5)[((d >>  7) & 0x03) | ((d >>  8) & 0x3c)] |
            SKB(6)[ (d >> 15) & 0x3f] |
            SKB(7)[((d >> 21) & 0x0f) | ((d >> 22) & 0x30)];

        /* table contains pre-rotated values; cook into the schedule */
        u = (s & 0x0000ffffUL) | (t << 16);
        *k++ = ROTL(u, 2);
        u = (s >> 16) | (t & 0xffff0000UL);
        *k++ = ROTL(u, 6);
    }
}

#define D_ENCRYPT(LL, R, S)                                  \
    do {                                                     \
        u = (R) ^ s[(S)];                                    \
        t = (R) ^ s[(S) + 1];                                \
        t = ROTR(t, 4);                                      \
        (LL) ^=                                              \
            SP(0)[(u >>  2) & 0x3f] ^                        \
            SP(2)[(u >> 10) & 0x3f] ^                        \
            SP(4)[(u >> 18) & 0x3f] ^                        \
            SP(6)[(u >> 26) & 0x3f] ^                        \
            SP(1)[(t >>  2) & 0x3f] ^                        \
            SP(3)[(t >> 10) & 0x3f] ^                        \
            SP(5)[(t >> 18) & 0x3f] ^                        \
            SP(7)[(t >> 26) & 0x3f];                         \
    } while (0)

void RSA32API des(BYTE *pbOut, BYTE *pbIn, void *key, int op)
{
    register DWORD l;
    register DWORD r;
    register DWORD t;
    register DWORD u;
    const DWORD *s = (const DWORD *)key;
    int i;

    /* r = data[0], l = data[1] (OpenSSL DES_encrypt1 convention) */
    r = load_le32(pbIn);
    l = load_le32(pbIn + 4);

    /* Initial permutation -- IP(r, l) */
    PERM_OP(l, r, t, 4, 0x0f0f0f0fUL);
    PERM_OP(r, l, t, 16, 0x0000ffffUL);
    PERM_OP(l, r, t, 2, 0x33333333UL);
    PERM_OP(r, l, t, 8, 0x00ff00ffUL);
    PERM_OP(l, r, t, 1, 0x55555555UL);

    /* The SPtrans table is rotated 1 bit right, compensate here. */
    r = ROTR(r, 29);
    l = ROTR(l, 29);

    if (op)
    {
        for (i = 0; i < 32; i += 8)
        {
            D_ENCRYPT(l, r, i + 0);
            D_ENCRYPT(r, l, i + 2);
            D_ENCRYPT(l, r, i + 4);
            D_ENCRYPT(r, l, i + 6);
        }
    }
    else
    {
        for (i = 30; i > 0; i -= 8)
        {
            D_ENCRYPT(l, r, i - 0);
            D_ENCRYPT(r, l, i - 2);
            D_ENCRYPT(l, r, i - 4);
            D_ENCRYPT(r, l, i - 6);
        }
    }

    l = ROTR(l, 3);
    r = ROTR(r, 3);

    /* Final permutation -- FP(r, l) */
    PERM_OP(r, l, t, 1, 0x55555555UL);
    PERM_OP(l, r, t, 8, 0x00ff00ffUL);
    PERM_OP(r, l, t, 2, 0x33333333UL);
    PERM_OP(l, r, t, 16, 0x0000ffffUL);
    PERM_OP(r, l, t, 4, 0x0f0f0f0fUL);

    /* data[0] = l, data[1] = r */
    store_le32(pbOut, l);
    store_le32(pbOut + 4, r);
}

void RSA32API tripledes(BYTE *pbOut, BYTE *pbIn, void *pKey, int op)
{
    DES3TABLE *p = (DES3TABLE *)pKey;
    BYTE tmp[8];
    BYTE tmp2[8];

    if (op == ENCRYPT)
    {
        des(tmp,  pbIn, &p->keytab1, ENCRYPT);
        des(tmp2, tmp,  &p->keytab2, DECRYPT);
        des(pbOut, tmp2, &p->keytab3, ENCRYPT);
    }
    else
    {
        des(tmp,  pbIn, &p->keytab3, DECRYPT);
        des(tmp2, tmp,  &p->keytab2, ENCRYPT);
        des(pbOut, tmp2, &p->keytab1, DECRYPT);
    }
}

void RSA32API tripledes2key(PDES3TABLE pDES3Table, BYTE *pbKey)
{
    deskey(&pDES3Table->keytab1, pbKey);
    deskey(&pDES3Table->keytab2, pbKey + 8);
    pDES3Table->keytab3 = pDES3Table->keytab1;
}

void RSA32API tripledes3key(PDES3TABLE pDES3Table, BYTE *pbKey)
{
    deskey(&pDES3Table->keytab1, pbKey);
    deskey(&pDES3Table->keytab2, pbKey + 8);
    deskey(&pDES3Table->keytab3, pbKey + 16);
}

void RSA32API desparityonkey(BYTE *pbKey, DWORD cbKey)
{
    DWORD i;

    for (i = 0; i < cbKey; i++)
    {
        BYTE b = pbKey[i];
        unsigned int bits = DESParityTable[b >> 4] + DESParityTable[b & 0x0f];
        if ((bits & 1) == 0)
            pbKey[i] = b ^ 1;
    }
}

void RSA32API desreducekey(BYTE *key)
{
    key[0] &= 0x0f;
    key[2] &= 0x0f;
    key[4] &= 0x0f;
    key[6] &= 0x0f;
}

void RSA32API deskeyexpand(BYTE *pbKey, BYTE *pbExpanded_key)
{
    pbExpanded_key[0] = (BYTE)(pbKey[0] >> 4);
    pbExpanded_key[1] = (BYTE)((pbKey[0] << 3) | (pbKey[1] >> 5));
    pbExpanded_key[2] = (BYTE)((pbKey[1] >> 2) & 0x0f);
    pbExpanded_key[3] = (BYTE)((pbKey[1] << 5) | (pbKey[2] >> 3));
    pbExpanded_key[4] = (BYTE)(pbKey[2] & 0x0f);
    pbExpanded_key[5] = (BYTE)((pbKey[2] << 7) | (pbKey[3] >> 1));
    pbExpanded_key[6] = (BYTE)(((pbKey[4] >> 6) | (pbKey[3] << 2)) & 0x0f);
    pbExpanded_key[7] = (BYTE)(pbKey[4] << 1);
}
