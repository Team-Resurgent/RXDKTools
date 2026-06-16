/*
 * xrsa_modes.c
 *
 * Source replacement for rsa32 rsa32_modes.obj (CBC).
 *
 * Generic cipher-block-chaining wrapper around a block cipher. Processes one
 * block (dwBlockLen bytes) per call, maintaining the chaining value in the
 * caller-supplied feedback register. Handles in-place operation (output may
 * alias input or feedback) via local scratch buffers.
 */

#include <windows.h>
#include <modes.h>

#define CBC_MAX_BLOCKLEN 32

void
RSA32API
CBC(
    void   RSA32API Cipher(BYTE *, BYTE *, void *, int),
    DWORD  dwBlockLen,
    BYTE   *output,
    BYTE   *input,
    void   *keyTable,
    int    op,
    BYTE   *feedback
    )
{
    BYTE work[CBC_MAX_BLOCKLEN];
    BYTE saved[CBC_MAX_BLOCKLEN];
    DWORD i;

    if (dwBlockLen > CBC_MAX_BLOCKLEN)
        return;

    if (op == ENCRYPT)
    {
        for (i = 0; i < dwBlockLen; i++)
            work[i] = (BYTE)(input[i] ^ feedback[i]);

        Cipher(output, work, keyTable, ENCRYPT);

        for (i = 0; i < dwBlockLen; i++)
            feedback[i] = output[i];
    }
    else
    {
        for (i = 0; i < dwBlockLen; i++)
            saved[i] = input[i];

        Cipher(work, input, keyTable, DECRYPT);

        for (i = 0; i < dwBlockLen; i++)
            output[i] = (BYTE)(work[i] ^ feedback[i]);

        for (i = 0; i < dwBlockLen; i++)
            feedback[i] = saved[i];
    }
}
