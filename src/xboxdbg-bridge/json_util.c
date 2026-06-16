#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *FindKey(const char *szJson, const char *szKey)
{
    char pat[128];
    const char *p;
    sprintf(pat, "\"%s\"", szKey);
    p = strstr(szJson, pat);
    if (!p)
        return NULL;
    p = strchr(p + strlen(pat), ':');
    return p;
}

BOOL JsonGetString(const char *szJson, const char *szKey, char *szOut, int cchOut)
{
    const char *p = FindKey(szJson, szKey);
    const char *q;
    char *end;
    int cch;
    if (!p)
        return FALSE;
    p = strchr(p, '"');
    if (!p)
        return FALSE;
    ++p;
    q = strchr(p, '"');
    if (!q)
        return FALSE;
    cch = (int)(q - p);
    if (cch >= cchOut)
        cch = cchOut - 1;
    memcpy(szOut, p, cch);
    szOut[cch] = 0;
    /* unescape backslashes minimally */
    for (end = szOut; *end; ++end) {
        if (end[0] == '\\' && end[1])
            memmove(end, end + 1, strlen(end));
    }
    return TRUE;
}

BOOL JsonGetDword(const char *szJson, const char *szKey, DWORD *pdwOut)
{
    const char *p = FindKey(szJson, szKey);
    if (!p)
        return FALSE;
    ++p;
    while (*p == ' ' || *p == '\t')
        ++p;
    *pdwOut = (DWORD)strtoul(p, NULL, 0);
    return TRUE;
}

BOOL JsonGetBool(const char *szJson, const char *szKey, BOOL *pfOut)
{
    const char *p = FindKey(szJson, szKey);
    if (!p)
        return FALSE;
    if (strstr(p, "true"))
        *pfOut = TRUE;
    else if (strstr(p, "false"))
        *pfOut = FALSE;
    else
        return FALSE;
    return TRUE;
}

BOOL JsonGetPtr(const char *szJson, const char *szKey, PVOID *ppvOut)
{
    char szNum[32];
    if (!JsonGetString(szJson, szKey, szNum, sizeof szNum)) {
        DWORD dw;
        if (!JsonGetDword(szJson, szKey, &dw))
            return FALSE;
        *ppvOut = (PVOID)(ULONG_PTR)dw;
        return TRUE;
    }
    *ppvOut = (PVOID)(ULONG_PTR)_strtoui64(szNum, NULL, 0);
    return TRUE;
}

void JsonAppendEscaped(char *szOut, int cchOut, int *pPos, LPCSTR szIn)
{
    int pos;
    char c;

    if (!szOut || cchOut <= 0 || !pPos || !szIn)
        return;
    pos = *pPos;
    if (pos >= cchOut - 2)
        return;
    szOut[pos++] = '"';
    for (; *szIn && pos < cchOut - 2; ++szIn) {
        c = *szIn;
        if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t') {
            if (pos >= cchOut - 3)
                break;
            szOut[pos++] = '\\';
            if (c == '\n')
                szOut[pos++] = 'n';
            else if (c == '\r')
                szOut[pos++] = 'r';
            else if (c == '\t')
                szOut[pos++] = 't';
            else
                szOut[pos++] = c;
        } else {
            szOut[pos++] = c;
        }
    }
    szOut[pos++] = '"';
    szOut[pos] = 0;
    *pPos = pos;
}
