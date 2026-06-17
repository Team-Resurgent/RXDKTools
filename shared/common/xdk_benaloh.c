/*
 * Portable BenalohModExp (replaces prebuilt rsa32.lib for xbdbgs).
 * Only BenalohModExp is required by xboxdbg secure.c DH key exchange.
 */
#include <windows.h>
#include <benaloh.h>
#include <string.h>

#define XDK_BENALOH_MAX_LEN 32

static void mp_zero(DWORD* a, DWORD len)
{
    memset(a, 0, len * sizeof(DWORD));
}

static void mp_copy(DWORD* dst, const DWORD* src, DWORD len)
{
    memcpy(dst, src, len * sizeof(DWORD));
}

static int mp_is_zero(const DWORD* a, DWORD len)
{
    DWORD i;
    for (i = 0; i < len; ++i) {
        if (a[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int mp_cmp(const DWORD* a, const DWORD* b, DWORD len)
{
    DWORD i;
    for (i = len; i-- > 0;) {
        if (a[i] > b[i]) {
            return 1;
        }
        if (a[i] < b[i]) {
            return -1;
        }
    }
    return 0;
}

static DWORD mp_norm_len(const DWORD* a, DWORD len)
{
    while (len > 0 && a[len - 1] == 0) {
        --len;
    }
    return len;
}

static void mp_mul(
    const DWORD* a,
    const DWORD* b,
    DWORD* prod,
    DWORD len)
{
    DWORD i;
    DWORD j;
    mp_zero(prod, len * 2);
    for (i = 0; i < len; ++i) {
        unsigned __int64 carry = 0;
        for (j = 0; j < len; ++j) {
            unsigned __int64 t =
                (unsigned __int64)a[i] * (unsigned __int64)b[j] +
                (unsigned __int64)prod[i + j] + carry;
            prod[i + j] = (DWORD)t;
            carry = t >> 32;
        }
        prod[i + len] = (DWORD)carry;
    }
}

static void mp_shl(DWORD* a, DWORD* hi, DWORD len)
{
    DWORD i;
    DWORD carry = 0;
    DWORD next_carry;
    for (i = 0; i < len; ++i) {
        next_carry = (a[i] >> 31) & 1;
        a[i] = (a[i] << 1) | carry;
        carry = next_carry;
    }
    *hi = carry;
}

static void mp_sub(DWORD* a, const DWORD* b, DWORD len)
{
    DWORD i;
    unsigned __int64 borrow = 0;
    for (i = 0; i < len; ++i) {
        unsigned __int64 av = (unsigned __int64)a[i];
        unsigned __int64 bv = (unsigned __int64)b[i] + borrow;
        if (av < bv) {
            a[i] = (DWORD)(av + (1ULL << 32) - bv);
            borrow = 1;
        } else {
            a[i] = (DWORD)(av - bv);
            borrow = 0;
        }
    }
}

static void mp_mod(DWORD* r, DWORD rlen, const DWORD* m, DWORD mlen)
{
    DWORD nlen;
    DWORD mtop;
    DWORD shift;
    DWORD t[XDK_BENALOH_MAX_LEN * 2];
    DWORD qbit;

    rlen = mp_norm_len(r, rlen);
    mlen = mp_norm_len(m, mlen);
    if (mlen == 0) {
        return;
    }
    if (rlen < mlen) {
        return;
    }
    if (rlen == mlen && mp_cmp(r, m, mlen) < 0) {
        return;
    }

    nlen = rlen;
    mtop = mlen - 1;

    while (nlen > mlen || (nlen == mlen && mp_cmp(r, m, mlen) >= 0)) {
        shift = 0;
        if (nlen > mlen) {
            shift = (nlen - mlen) * 32;
            DWORD msb = r[nlen - 1];
            while (shift && ((msb >> ((shift - 1) & 31)) & 1) == 0) {
                --shift;
            }
        } else {
            DWORD msb_diff = r[mtop] ^ m[mtop];
            if (msb_diff) {
                DWORD bit = 31;
                while (bit && ((msb_diff >> bit) == 0)) {
                    --bit;
                }
                shift = bit;
            }
        }

        mp_zero(t, mlen + 1);
        mp_copy(t, m, mlen);
        qbit = 0;
        while (shift--) {
            DWORD hi = 0;
            mp_shl(t, &hi, mlen);
            if (hi) {
                t[mlen] = hi;
            }
            ++qbit;
        }

        while (mp_cmp(r, t, nlen) >= 0) {
            mp_sub(r, t, nlen);
            while (nlen > 0 && r[nlen - 1] == 0) {
                --nlen;
            }
        }
    }
}

static void mp_mod_mul(
    DWORD* a,
    const DWORD* b,
    const DWORD* mod,
    DWORD len,
    DWORD* scratch)
{
    mp_mul(a, b, scratch, len);
    mp_mod(scratch, len * 2, mod, len);
    mp_copy(a, scratch, len);
}

BOOL BenalohModExp(LPDWORD A, LPDWORD B, LPDWORD C, LPDWORD D, DWORD len)
{
    DWORD result[XDK_BENALOH_MAX_LEN];
    DWORD scratch[XDK_BENALOH_MAX_LEN * 2];
    DWORD exp_word;
    DWORD bit;
    int word;
    int top_word;

    if (len == 0 || len > XDK_BENALOH_MAX_LEN) {
        return FALSE;
    }
    if (mp_is_zero(D, len)) {
        return FALSE;
    }

    mp_zero(result, len);
    result[0] = 1;

    top_word = (int)len - 1;
    while (top_word > 0 && C[top_word] == 0) {
        --top_word;
    }

    for (word = top_word; word >= 0; --word) {
        exp_word = C[word];
        for (bit = 0; bit < 32; ++bit) {
            mp_mod_mul(result, result, D, len, scratch);
            if (exp_word & 0x80000000u) {
                mp_mod_mul(result, B, D, len, scratch);
            }
            exp_word <<= 1;
        }
    }

    mp_copy(A, result, len);
    return TRUE;
}
