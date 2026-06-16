/*
 * xrsa_rc4.c
 *
 * Source replacement for rsa32 rsa32_rc4fast.obj (rc4 / rc4_key).
 *
 * Standard ARCFOUR (RC4) stream cipher, matching the rsa32 implementation
 * bit-for-bit. The key control structure stores the 256-byte state table
 * followed by the i and j stream indices (see RC4_KEYSTRUCT in rc4.h).
 */

#include <windows.h>
#include <rc4.h>

void RSA32API rc4_key(struct RC4_KEYSTRUCT *pKS, unsigned int dwLen, unsigned char *pbKey)
{
    unsigned char *S = pKS->S;
    unsigned int i;
    unsigned int j;
    unsigned int k;

    for (i = 0; i < 256; i++)
        S[i] = (unsigned char)i;

    pKS->i = 0;
    pKS->j = 0;

    if (dwLen == 0)
        return;

    j = 0;
    k = 0;
    for (i = 0; i < 256; i++)
    {
        unsigned char si = S[i];
        j = (j + pbKey[k] + si) & 0xff;
        S[i] = S[j];
        S[j] = si;
        if (++k == dwLen)
            k = 0;
    }
}

void RSA32API rc4(struct RC4_KEYSTRUCT *pKS, unsigned int dwLen, unsigned char *pbuf)
{
    unsigned char *S = pKS->S;
    unsigned int i = pKS->i;
    unsigned int j = pKS->j;
    unsigned int n;

    for (n = 0; n < dwLen; n++)
    {
        unsigned char si;
        unsigned char sj;

        i = (i + 1) & 0xff;
        si = S[i];
        j = (j + si) & 0xff;
        sj = S[j];
        S[i] = sj;
        S[j] = si;
        pbuf[n] ^= S[(si + sj) & 0xff];
    }

    pKS->i = (unsigned char)i;
    pKS->j = (unsigned char)j;
}
