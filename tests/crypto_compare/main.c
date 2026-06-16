/*
 * crypto_compare main.c
 *
 * Exercises the symmetric-cipher surface of rsa32.lib / xrsa.lib
 * (rc4, des, tripledes, CBC, desparityonkey) with deterministic inputs and
 * prints hex results. The same binary is linked once against rsa32.lib (ref)
 * and once against the source xrsa.lib; compare-crypto.ps1 diffs the output.
 */

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <rc4.h>
#include <des.h>
#include <tripldes.h>
#include <modes.h>

static unsigned int g_seed;

static unsigned int Rnd(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0xff;
}

static void Fill(unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char)Rnd();
}

static void Dump(const char *label, const unsigned char *d, int n)
{
    int i;
    printf("%s:", label);
    for (i = 0; i < n; i++)
        printf("%02X", d[i]);
    printf("\n");
}

int __cdecl main(void)
{
    /* ---- RC4 ---- */
    {
        RC4_KEYSTRUCT ks;
        unsigned char key[16];
        unsigned char buf[64];

        g_seed = 1;
        Fill(key, 16);
        Fill(buf, 64);
        rc4_key(&ks, 16, key);
        rc4(&ks, 64, buf);
        Dump("rc4", buf, 64);
    }

    /* ---- RC4 with non-multiple-of-4 length and odd key ---- */
    {
        RC4_KEYSTRUCT ks;
        unsigned char key[5];
        unsigned char buf[37];

        g_seed = 7;
        Fill(key, 5);
        Fill(buf, 37);
        rc4_key(&ks, 5, key);
        rc4(&ks, 37, buf);
        Dump("rc4_odd", buf, 37);
    }

    /* ---- DES ECB ---- */
    {
        DESTable t;
        unsigned char key[8];
        unsigned char pt[8];
        unsigned char ct[8];
        unsigned char rt[8];

        g_seed = 2;
        Fill(key, 8);
        Fill(pt, 8);
        deskey(&t, key);
        des(ct, pt, &t, ENCRYPT);
        des(rt, ct, &t, DECRYPT);
        Dump("des_enc", ct, 8);
        Dump("des_dec", rt, 8);
    }

    /* ---- Triple DES (3-key EDE) ---- */
    {
        DES3TABLE t;
        unsigned char key[24];
        unsigned char pt[8];
        unsigned char ct[8];
        unsigned char rt[8];

        g_seed = 3;
        Fill(key, 24);
        Fill(pt, 8);
        tripledes3key(&t, key);
        tripledes(ct, pt, &t, ENCRYPT);
        tripledes(rt, ct, &t, DECRYPT);
        Dump("des3_enc", ct, 8);
        Dump("des3_dec", rt, 8);
    }

    /* ---- DES-CBC over 4 blocks ---- */
    {
        DESTable t;
        unsigned char key[8];
        unsigned char iv[8];
        unsigned char fb[8];
        unsigned char in[32];
        unsigned char out[32];
        unsigned char dec[32];
        int b;

        g_seed = 4;
        Fill(key, 8);
        Fill(iv, 8);
        Fill(in, 32);
        deskey(&t, key);

        memcpy(fb, iv, 8);
        for (b = 0; b < 32; b += 8)
            CBC(des, 8, out + b, in + b, &t, ENCRYPT, fb);
        Dump("cbc_enc", out, 32);

        memcpy(fb, iv, 8);
        for (b = 0; b < 32; b += 8)
            CBC(des, 8, dec + b, out + b, &t, DECRYPT, fb);
        Dump("cbc_dec", dec, 32);
    }

    /* ---- desparityonkey ---- */
    {
        unsigned char key[24];

        g_seed = 5;
        Fill(key, 24);
        desparityonkey(key, 24);
        Dump("desparity", key, 24);
    }

    return 0;
}
