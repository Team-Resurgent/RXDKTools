#include "precomp.h"
#include <stdio.h>

HRESULT DmTranslateErrorA(HRESULT hr, LPSTR lpBuffer, int nBufferMax)
{
    if (!lpBuffer || nBufferMax <= 0)
        return E_INVALIDARG;
    sprintf(lpBuffer, "ERR:0x%08X", hr);
    return XBDM_NOERR;
}

HRESULT DmTranslateErrorW(HRESULT hr, LPWSTR lpBuffer, int nBufferMax)
{
    if (!lpBuffer || nBufferMax <= 0)
        return E_INVALIDARG;
    swprintf(lpBuffer, nBufferMax, L"ERR:0x%08X", hr);
    return XBDM_NOERR;
}
