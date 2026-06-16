#include "bridge.h"
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IMAGE_NT_SIGNATURE
#define IMAGE_NT_SIGNATURE 0x00004550
#endif

#pragma comment(lib, "dbghelp.lib")

static BOOL g_fSymInit;
static PVOID g_pvModuleBase;
static DWORD64 g_dw64PdbBase;
static char g_szLoadedModule[MAX_PATH];
static char g_szMapPath[MAX_PATH];
static DWORD g_dwMapLinkBase;

static BOOL CALLBACK SymCb(HANDLE hProcess, ULONG ActionCode, ULONG64 CallbackData, ULONG64 UserContext)
{
    (void)hProcess; (void)ActionCode; (void)CallbackData; (void)UserContext;
    return FALSE;
}

static HANDLE SymbolsProcess(void)
{
    /* Watson / VC6 offline symbol loaders use a pseudo handle, not GetCurrentProcess(). */
    return (HANDLE)(ULONG_PTR)1;
}

static void SymbolsDefaultMapPath(LPCSTR szExePath, char *szMap, int cchMap)
{
    char *dot;
    if (!szExePath || !szMap || cchMap <= 0)
        return;
    strncpy(szMap, szExePath, cchMap - 1);
    szMap[cchMap - 1] = 0;
    dot = strrchr(szMap, '.');
    if (dot)
        strcpy(dot, ".map");
    else
        strncat(szMap, ".map", cchMap - 1 - (int)strlen(szMap));
}

static BOOL SymbolsReadMapLinkBase(LPCSTR szMapPath, DWORD *pdwBase)
{
    FILE *fp;
    char line[128];
    unsigned base = 0;

    if (!szMapPath || !pdwBase)
        return FALSE;
    fp = fopen(szMapPath, "r");
    if (!fp)
        return FALSE;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, " Preferred load address is %x", &base) == 1 ||
            sscanf(line, " Preferred load address is %X", &base) == 1) {
            fclose(fp);
            *pdwBase = base;
            return TRUE;
        }
    }
    fclose(fp);
    return FALSE;
}

HRESULT SymbolsLoad(LPCSTR szExePath, LPCSTR szPdbPath, LPCSTR szMapPath)
{
    char szSearch[MAX_PATH];
    char szModule[MAX_PATH];
    char szMap[MAX_PATH];
    char *slash;
    HANDLE hProc = SymbolsProcess();
    DWORD64 base;
    DWORD dwSize = 0;
    HANDLE hFile;
    DWORD cbRead;

    SymbolsUnload();

    if (!g_fSymInit) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        if (!SymInitialize(hProc, NULL, FALSE))
            return HRESULT_FROM_WIN32(GetLastError());
        g_fSymInit = TRUE;
    }

    strncpy(szSearch, szExePath, sizeof szSearch - 1);
    szSearch[sizeof szSearch - 1] = 0;
    slash = strrchr(szSearch, '\\');
    if (!slash)
        slash = strrchr(szSearch, '/');
    if (slash)
        *slash = 0;
    SymSetSearchPath(hProc, szSearch);

    hFile = CreateFileA(szExePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS32 nth;
        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        if (ReadFile(hFile, &dos, sizeof dos, &cbRead, NULL) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            SetFilePointer(hFile, dos.e_lfanew, NULL, FILE_BEGIN);
            if (ReadFile(hFile, &nth, sizeof nth, &cbRead, NULL) && nth.Signature == IMAGE_NT_SIGNATURE)
                dwSize = nth.OptionalHeader.SizeOfImage;
        }
        CloseHandle(hFile);
    }

    strncpy(szModule, szExePath, sizeof szModule - 1);
    szModule[sizeof szModule - 1] = 0;
    slash = strrchr(szModule, '\\');
    if (!slash)
        slash = strrchr(szModule, '/');
    if (slash)
        memmove(szModule, slash + 1, strlen(slash + 1) + 1);

    base = SymLoadModuleEx(hProc, NULL, szModule, szPdbPath, 0x400000, dwSize, NULL, 0);
    if (!base)
        base = SymLoadModuleEx(hProc, NULL, szExePath, szPdbPath, 0x400000, dwSize, NULL, 0);
    if (!base)
        return HRESULT_FROM_WIN32(GetLastError());

    strncpy(g_szLoadedModule, szModule, sizeof g_szLoadedModule - 1);
    g_szLoadedModule[sizeof g_szLoadedModule - 1] = 0;
    g_dw64PdbBase = base;

    g_szMapPath[0] = 0;
    g_dwMapLinkBase = 0x400000;
    if (szMapPath && szMapPath[0])
        strncpy(szMap, szMapPath, sizeof szMap - 1);
    else
        SymbolsDefaultMapPath(szExePath, szMap, sizeof szMap);
    szMap[sizeof szMap - 1] = 0;
    if (GetFileAttributesA(szMap) != INVALID_FILE_ATTRIBUTES) {
        strncpy(g_szMapPath, szMap, sizeof g_szMapPath - 1);
        g_szMapPath[sizeof g_szMapPath - 1] = 0;
        if (!SymbolsReadMapLinkBase(g_szMapPath, &g_dwMapLinkBase))
            g_dwMapLinkBase = 0x400000;
    }

    return S_OK;
}

void SymbolsUnload(void)
{
    if (g_fSymInit) {
        SymCleanup(SymbolsProcess());
        g_fSymInit = FALSE;
    }
    g_pvModuleBase = NULL;
    g_dw64PdbBase = 0;
    g_szLoadedModule[0] = 0;
    g_szMapPath[0] = 0;
    g_dwMapLinkBase = 0x400000;
}

void SymbolsSetModuleBase(PVOID pvBase)
{
    g_pvModuleBase = pvBase;
}

PVOID SymbolsRelocateAddress(PVOID pvPdbAddr, PVOID pvModuleBase)
{
    ULONG_PTR pdb = (ULONG_PTR)pvPdbAddr;
    ULONG_PTR base = (ULONG_PTR)pvModuleBase;
    ULONG_PTR pdbBase = (ULONG_PTR)g_dw64PdbBase;
    if (!base || !pdbBase)
        return pvPdbAddr;
    return (PVOID)(base + (pdb - pdbBase));
}

static void SymbolsNormalizePath(LPCSTR szIn, char *szOut, int cchOut)
{
    int i, j = 0;
    if (!szIn || !szOut || cchOut <= 0)
        return;
    while (*szIn == ' ' || *szIn == '\t')
        ++szIn;
    if (strncmp(szIn, "file:///", 8) == 0)
        szIn += 8;
    else if (strncmp(szIn, "file://", 7) == 0)
        szIn += 7;
    for (i = 0; szIn[i] && j < cchOut - 1; ++i) {
        char c = szIn[i];
        if (c == '/')
            c = '\\';
        szOut[j++] = c;
    }
    szOut[j] = 0;
}

static BOOL FileBaseMatches(LPCSTR szRequested, LPCSTR szPdbPath)
{
    char req[MAX_PATH];
    char got[MAX_PATH];
    const char *base;

    if (!szRequested || !szPdbPath)
        return FALSE;
    SymbolsNormalizePath(szRequested, req, sizeof req);
    SymbolsNormalizePath(szPdbPath, got, sizeof got);
    base = strrchr(req, '\\');
    base = base ? base + 1 : req;
    {
        const char *gotBase = strrchr(got, '\\');
        gotBase = gotBase ? gotBase + 1 : got;
        if (_stricmp(base, gotBase) == 0)
            return TRUE;
    }
    return strstr(got, base) != NULL || strstr(req, got) != NULL;
}

static BOOL LookupLineFromName(HANDLE hProc, LPCSTR szMod, LPCSTR szPath, DWORD dwLine, IMAGEHLP_LINE64 *pLine)
{
    DWORD dwDisp32;

    if (!szPath || !pLine)
        return FALSE;
    return SymGetLineFromName64(hProc, szMod, szPath, dwLine, &dwDisp32, pLine) != FALSE;
}

static BOOL LookupLineExact(HANDLE hProc, LPCSTR szFile, DWORD dwLine, DWORD64 *pAddr)
{
    char szPath[MAX_PATH];
    char szNorm[MAX_PATH];
    IMAGEHLP_LINE64 line;
    LPCSTR szTry;
    LPCSTR szMod;
    int imod;

    SymbolsNormalizePath(szFile, szNorm, sizeof szNorm);
    szTry = szNorm;
    strncpy(szPath, szTry, sizeof szPath - 1);
    szPath[sizeof szPath - 1] = 0;

    for (imod = 0; imod < 2; ++imod) {
        szMod = (imod == 0 && g_szLoadedModule[0]) ? g_szLoadedModule : NULL;
        if (imod == 0 && !szMod)
            continue;
        if (LookupLineFromName(hProc, szMod, szPath, dwLine, &line))
            goto found;
        szTry = szNorm;
        {
            const char *slash = strrchr(szTry, '\\');
            if (slash)
                szTry = slash + 1;
            slash = strrchr(szTry, '/');
            if (slash)
                szTry = slash + 1;
            strncpy(szPath, szTry, sizeof szPath - 1);
            szPath[sizeof szPath - 1] = 0;
            if (LookupLineFromName(hProc, szMod, szPath, dwLine, &line))
                goto found;
        }
    }
    return FALSE;

found:
    *pAddr = line.Address;
    return TRUE;
}

static BOOL LookupLineFromFunction(HANDLE hProc, LPCSTR szFunc, LPCSTR szFile, DWORD dwLine, DWORD64 *pAddr)
{
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    IMAGEHLP_LINE64 line;
    DWORD dwDisp32;
    DWORD64 bestAddr = 0;
    BOOL fFound = FALSE;

    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;
    if (!SymFromName(hProc, (LPSTR)szFunc, pSym))
        return FALSE;
    if (!SymGetLineFromAddr64(hProc, pSym->Address, &dwDisp32, &line))
        return FALSE;
    do {
        if (line.LineNumber == dwLine && FileBaseMatches(szFile, line.FileName) && line.Address) {
            if (line.Address >= bestAddr) {
                bestAddr = line.Address;
                fFound = TRUE;
            }
        }
    } while (SymGetLineNext64(hProc, &line));
    ZeroMemory(&line, sizeof line);
    line.SizeOfStruct = sizeof line;
    if (SymGetLineFromAddr64(hProc, pSym->Address, &dwDisp32, &line)) {
        do {
            if (line.LineNumber == dwLine && FileBaseMatches(szFile, line.FileName) && line.Address) {
                if (line.Address >= bestAddr) {
                    bestAddr = line.Address;
                    fFound = TRUE;
                }
            }
        } while (SymGetLinePrev64(hProc, &line));
    }
    if (fFound) {
        *pAddr = bestAddr;
        return TRUE;
    }
    return FALSE;
}

static BOOL LookupLineNearest(HANDLE hProc, LPCSTR szFile, DWORD dwLine, DWORD64 *pAddr, DWORD *pdwFoundLine)
{
    char szNorm[MAX_PATH];
    char szBase[MAX_PATH];
    IMAGEHLP_LINE64 line;
    DWORD dwDisp32;
    DWORD delta;
    int i;
    DWORD bestDelta = 9999;
    DWORD64 bestAddr = 0;
    DWORD bestLine = 0;
    static const char *rgszFuncs[] = {
        "_main", "main", "wmain", "InitD3D", "InitVB", "InitTime",
        "UpdateTime", "Update", "Render", NULL
    };

    for (delta = 1; delta <= 12; ++delta) {
        DWORD tryNum;

        tryNum = dwLine + delta;
        if (LookupLineExact(hProc, szFile, tryNum, pAddr)) {
            if (pdwFoundLine)
                *pdwFoundLine = tryNum;
            return TRUE;
        }
        if (dwLine > delta) {
            tryNum = dwLine - delta;
            if (LookupLineExact(hProc, szFile, tryNum, pAddr)) {
                if (pdwFoundLine)
                    *pdwFoundLine = tryNum;
                return TRUE;
            }
        }
    }

    for (i = 0; rgszFuncs[i]; ++i) {
        if (LookupLineFromFunction(hProc, rgszFuncs[i], szFile, dwLine, pAddr)) {
            if (pdwFoundLine)
                *pdwFoundLine = dwLine;
            return TRUE;
        }
    }

    SymbolsNormalizePath(szFile, szNorm, sizeof szNorm);
    {
        const char *slash = strrchr(szNorm, '\\');
        strncpy(szBase, slash ? slash + 1 : szNorm, sizeof szBase - 1);
        szBase[sizeof szBase - 1] = 0;
    }

    for (i = 0; rgszFuncs[i]; ++i) {
        UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;

        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSym->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromName(hProc, (LPSTR)rgszFuncs[i], pSym))
            continue;
        if (!SymGetLineFromAddr64(hProc, pSym->Address, &dwDisp32, &line))
            continue;
        do {
            if (FileBaseMatches(szFile, line.FileName) && line.Address) {
                delta = (line.LineNumber > dwLine) ? (line.LineNumber - dwLine) : (dwLine - line.LineNumber);
                if (delta < bestDelta) {
                    bestDelta = delta;
                    bestAddr = line.Address;
                    bestLine = line.LineNumber;
                }
            }
        } while (SymGetLineNext64(hProc, &line));
    }

    if (bestDelta <= 12 && bestAddr) {
        *pAddr = bestAddr;
        if (pdwFoundLine)
            *pdwFoundLine = bestLine;
        return TRUE;
    }
    return FALSE;
}

static HRESULT SymbolsLookupLine(LPCSTR szFile, DWORD dwLine, DWORD64 *pAddr)
{
    HANDLE hProc = SymbolsProcess();
    DWORD foundLine = dwLine;

    if (!g_fSymInit || !g_dw64PdbBase || !pAddr)
        return E_FAIL;

    if (LookupLineExact(hProc, szFile, dwLine, pAddr))
        return S_OK;
    if (LookupLineFromFunction(hProc, "_main", szFile, dwLine, pAddr))
        return S_OK;
    if (LookupLineFromFunction(hProc, "main", szFile, dwLine, pAddr))
        return S_OK;
    if (LookupLineNearest(hProc, szFile, dwLine, pAddr, &foundLine))
        return S_OK;

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT SymbolsLineToAddress(LPCSTR szFile, DWORD dwLine, PVOID *ppvAddr)
{
    DWORD64 addr;
    HRESULT hr;

    if (!g_pvModuleBase)
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    hr = SymbolsLookupLine(szFile, dwLine, &addr);
    if (FAILED(hr))
        return hr;
    *ppvAddr = SymbolsRelocateAddress((PVOID)(ULONG_PTR)addr, g_pvModuleBase);
    return S_OK;
}

HRESULT SymbolsResolveLine(LPCSTR szFile, DWORD dwLine, PVOID *ppvAddr)
{
    DWORD64 addr;
    HRESULT hr;

    hr = SymbolsLookupLine(szFile, dwLine, &addr);
    if (FAILED(hr))
        return hr;
    *ppvAddr = (PVOID)(ULONG_PTR)addr;
    if (g_pvModuleBase)
        *ppvAddr = SymbolsRelocateAddress(*ppvAddr, g_pvModuleBase);
    return S_OK;
}

DWORD64 SymbolsGetPdbBase(void)
{
    return g_dw64PdbBase;
}

HRESULT SymbolsDiag(char *szOut, int cchOut)
{
    HANDLE hProc = SymbolsProcess();
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    IMAGEHLP_MODULE64 mod;
    IMAGEHLP_LINE64 line;
    DWORD dwDisp32;
    int pos = 0;

    if (!szOut || cchOut <= 0)
        return E_INVALIDARG;
    szOut[0] = 0;

    ZeroMemory(&mod, sizeof mod);
    mod.SizeOfStruct = sizeof mod;
    if (g_dw64PdbBase)
        SymGetModuleInfo64(hProc, g_dw64PdbBase, &mod);

    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;
    pos += sprintf(szOut + pos, "pdbBase=0x%I64x symType=%lu", g_dw64PdbBase, (unsigned long)mod.SymType);
    if (SymFromName(hProc, "_main", pSym)) {
        pos += sprintf(szOut + pos, " _main=0x%I64x", pSym->Address);
        ZeroMemory(&line, sizeof line);
        line.SizeOfStruct = sizeof line;
        if (SymGetLineFromAddr64(hProc, pSym->Address, &dwDisp32, &line))
            pos += sprintf(szOut + pos, " line=%lu file=%s", line.LineNumber, line.FileName);
    }
    ZeroMemory(&line, sizeof line);
    line.SizeOfStruct = sizeof line;
    if (SymGetLineFromAddr64(hProc, 0x4043b0, &dwDisp32, &line))
        pos += sprintf(szOut + pos, " addr4043b0=line%lu:%s", line.LineNumber, line.FileName);
    return S_OK;
}

HRESULT SymbolsAddressToLine(PVOID pvAddr, char *szFile, int cchFile, DWORD *pdwLine, char *szFunc, int cchFunc)
{
    HANDLE hProc = SymbolsProcess();
    DWORD64 disp;
    DWORD dwDisp32;
    IMAGEHLP_LINE64 line;
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    ULONG_PTR addr = (ULONG_PTR)pvAddr;
    ULONG_PTR pdbAddr;

    if (!g_fSymInit)
        return E_FAIL;

    if (g_pvModuleBase && g_dw64PdbBase)
        pdbAddr = (ULONG_PTR)g_dw64PdbBase + (addr - (ULONG_PTR)g_pvModuleBase);
    else
        pdbAddr = addr;

    if (szFunc && cchFunc > 0) {
        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSym->MaxNameLen = MAX_SYM_NAME;
        if (SymFromAddr(hProc, pdbAddr, &disp, pSym))
            strncpy(szFunc, pSym->Name, cchFunc - 1);
        else
            szFunc[0] = 0;
        szFunc[cchFunc - 1] = 0;
    }

    ZeroMemory(&line, sizeof line);
    line.SizeOfStruct = sizeof line;
    if (SymGetLineFromAddr64(hProc, pdbAddr, &dwDisp32, &line)) {
        strncpy(szFile, line.FileName, cchFile - 1);
        szFile[cchFile - 1] = 0;
        *pdwLine = line.LineNumber;
        return S_OK;
    }

    if (szFile && cchFile > 0)
        szFile[0] = 0;
    *pdwLine = 0;
    return S_FALSE;
}

#ifndef SYMFLAG_REGISTER
#define SYMFLAG_REGISTER 0x0008
#define SYMFLAG_REGREL   0x0010
#endif
#ifndef SYMFLAG_GLOBAL
#define SYMFLAG_GLOBAL   0x02000000
#endif
/* DbgHelp uses ModBase on map-only fallbacks; Address is already a runtime VA. */
#define BRIDGE_MAP_RUNTIME_MODBASE 1

#define SYM_TAG_FUNCTION 5
#define SYM_TAG_DATA 7
#define SYM_TAG_BLOCK 6
#define SYM_TAG_POINTERTYPE 8
#define SYM_TAG_UDT 11
#define SYM_TAG_BASECLASS 12
#define SYM_TAG_ARRAYTYPE 3
#define VT_R4 10
#define MAX_VARS_JSON 32
#define MAX_EMITTED_RANGES 96

typedef struct {
    DWORD offset;
    DWORD length;
} EMITTED_RANGE;

typedef struct {
    EMITTED_RANGE ranges[MAX_EMITTED_RANGES];
    DWORD count;
} EMITTED_COVERAGE;

typedef struct {
    char *buf;
    int cchBuf;
    int pos;
    int count;
    int maxVars;
    XBDM_CONTEXT *ctx;
    BOOL fLocals;
    BOOL fGlobals;
    BOOL fStop;
    int cEnumExamined;
    char szEmitted[MAX_VARS_JSON][MAX_SYM_NAME];
    int cEmitted;
} VAR_JSON_CTX;

#define MAP_NODE_OFF_LEFT    0
#define MAP_NODE_OFF_PARENT  4
#define MAP_NODE_OFF_RIGHT   8
#define MAP_NODE_OFF_COLOR   12
#define MAP_NODE_OFF_ISNIL   13
#define MAP_NODE_OFF_MYVAL   16
#define MAP_TREE_MAX_STEPS   64

static DWORD RegValueFromContext(PXBDM_CONTEXT ctx, DWORD reg)
{
    if (!ctx)
        return 0;
    switch (reg) {
    case 0: return ctx->Eax;
    case 1: return ctx->Ecx;
    case 2: return ctx->Edx;
    case 3: return ctx->Ebx;
    case 4: return ctx->Esp;
    case 5: return ctx->Ebp;
    case 6: return ctx->Esi;
    case 7: return ctx->Edi;
    default: return 0;
    }
}

static BOOL ReadXboxDword(PVOID pvAddr, DWORD *pdwOut)
{
    DWORD cb = 0;
    if (!pvAddr || !pdwOut)
        return FALSE;
    return SUCCEEDED(DmGetMemory((LPCVOID)pvAddr, 4, (LPBYTE)pdwOut, &cb)) && cb == 4;
}

static BOOL ReadXboxByte(PVOID pvAddr, BYTE *pbOut)
{
    DWORD cb = 0;
    if (!pvAddr || !pbOut)
        return FALSE;
    return SUCCEEDED(DmGetMemory((LPCVOID)pvAddr, 1, pbOut, &cb)) && cb == 1;
}

static BOOL MapPtrIsSentinel(DWORD myhead, DWORD ptr)
{
    BYTE isnil = 0;

    if (!ptr || ptr == myhead)
        return TRUE;
    if (!ReadXboxByte((PVOID)(ULONG_PTR)(ptr + MAP_NODE_OFF_ISNIL), &isnil))
        return TRUE;
    return isnil != 0;
}

static BOOL ReadXboxQword(ULONG_PTR addr, DWORD *pdwLo, DWORD *pdwHi)
{
    return ReadXboxDword((PVOID)addr, pdwLo) && ReadXboxDword((PVOID)(addr + 4), pdwHi);
}

static BOOL IsVarEmitted(const VAR_JSON_CTX *pj, LPCSTR szName)
{
    int i;

    if (!pj || !szName || !szName[0])
        return FALSE;
    for (i = 0; i < pj->cEmitted; ++i) {
        if (_stricmp(pj->szEmitted[i], szName) == 0)
            return TRUE;
    }
    return FALSE;
}

static void MarkVarEmitted(VAR_JSON_CTX *pj, LPCSTR szName)
{
    if (!pj || !szName || !szName[0] || pj->cEmitted >= pj->maxVars)
        return;
    strncpy(pj->szEmitted[pj->cEmitted], szName, MAX_SYM_NAME - 1);
    pj->szEmitted[pj->cEmitted][MAX_SYM_NAME - 1] = 0;
    ++pj->cEmitted;
}

static BOOL IsFloatMemberName(LPCSTR szName)
{
    if (!szName || !szName[0])
        return FALSE;
    if ((szName[0] == 'x' || szName[0] == 'y' || szName[0] == 'z' || szName[0] == 'w') && !szName[1])
        return TRUE;
    if (szName[0] == 'f' && (szName[1] == 'S' || szName[1] == 'E'))
        return TRUE;
    if (szName[0] == 'f' && szName[1] >= '0' && szName[1] <= '9')
        return TRUE;
    if (szName[0] == '_' && szName[1] >= '0' && szName[1] <= '9')
        return TRUE;
    return FALSE;
}

static void CoverageAdd(EMITTED_COVERAGE *cov, DWORD offset, DWORD length)
{
    if (!cov || !length || cov->count >= MAX_EMITTED_RANGES)
        return;
    cov->ranges[cov->count].offset = offset;
    cov->ranges[cov->count].length = length;
    ++cov->count;
}

static BOOL CoverageOverlaps(const EMITTED_COVERAGE *cov, DWORD offset, DWORD length)
{
    DWORD i;
    DWORD end = offset + length;

    if (!cov || !length)
        return FALSE;
    for (i = 0; i < cov->count; ++i) {
        DWORD rEnd = cov->ranges[i].offset + cov->ranges[i].length;
        if (offset < rEnd && end > cov->ranges[i].offset)
            return TRUE;
    }
    return FALSE;
}

static BOOL IsLayoutChildTag(DWORD tag)
{
    return tag == SYM_TAG_DATA || tag == SYM_TAG_BLOCK || tag == SYM_TAG_UDT ||
        tag == SYM_TAG_BASECLASS || tag == SYM_TAG_ARRAYTYPE;
}

static BOOL IsSpuriousMemberName(LPCSTR szName)
{
    if (!szName || !szName[0])
        return TRUE;
    if (szName[0] == '?' || szName[0] == '$')
        return TRUE;
    if (strncmp(szName, "operator", 8) == 0)
        return TRUE;
    if (strstr(szName, "::"))
        return TRUE;
    return FALSE;
}

static BOOL IsFloatTypeId(DWORD typeId)
{
    DWORD baseType = 0;
    HANDLE hProc = SymbolsProcess();

    if (!typeId || !g_dw64PdbBase)
        return FALSE;
    if (SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_BASETYPE, &baseType))
        return baseType == VT_R4;
    return FALSE;
}

static void FormatScalarValue(char *szOut, int cch, DWORD dw, LPCSTR szName)
{
    if (!szOut || cch <= 0)
        return;
    if (IsFloatMemberName(szName)) {
        union { DWORD d; float f; } u;
        u.d = dw;
        sprintf(szOut, "%g (0x%08lX)", (double)u.f, (unsigned long)dw);
    } else if (szName && ((szName[0] == 'g' && szName[1] == '_') || (szName[0] == 'p' && szName[1] != 0)))
        sprintf(szOut, "0x%08lX", (unsigned long)dw);
    else
        sprintf(szOut, "%ld (0x%08lX)", (long)dw, (unsigned long)dw);
}

static void FormatMemberValue(char *szOut, int cch, DWORD dw, LPCSTR szName, DWORD fieldTypeId)
{
    if (!szOut || cch <= 0)
        return;
    if (IsFloatTypeId(fieldTypeId) || IsFloatMemberName(szName)) {
        union { DWORD d; float f; } u;
        u.d = dw;
        sprintf(szOut, "%g (0x%08lX)", (double)u.f, (unsigned long)dw);
    } else {
        FormatScalarValue(szOut, cch, dw, szName);
    }
}

static BOOL IsPointerTypeId(DWORD typeId)
{
    DWORD tag = 0;
    HANDLE hProc = SymbolsProcess();

    if (!typeId || !g_dw64PdbBase)
        return FALSE;
    if (SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_SYMTAG, &tag))
        return tag == SYM_TAG_POINTERTYPE;
    return FALSE;
}

static BOOL IsGlobalPointerName(LPCSTR szName)
{
    if (!szName || !szName[0])
        return FALSE;
    if (szName[0] == 'g' && szName[1] == '_')
        return TRUE;
    if (szName[0] == 'p' && szName[1] != 0 && szName[1] != '_')
        return TRUE;
    return FALSE;
}

static BOOL FormatGlobalScalar(PSYMBOL_INFO pSym, ULONG_PTR addr, char *szOut, int cchOut)
{
    DWORD dw = 0;

    if (!pSym || !szOut || cchOut <= 0)
        return FALSE;
    if (!ReadXboxDword((PVOID)addr, &dw))
        return FALSE;
    if (IsPointerTypeId(pSym->TypeIndex) || IsGlobalPointerName(pSym->Name))
        FormatScalarValue(szOut, cchOut, dw, pSym->Name);
    else
        FormatMemberValue(szOut, cchOut, dw, pSym->Name, pSym->TypeIndex);
    return TRUE;
}

static BOOL FormatGlobalQword(PSYMBOL_INFO pSym, ULONG_PTR addr, char *szOut, int cchOut)
{
    DWORD lo = 0, hi = 0;

    if (!pSym || !szOut || cchOut <= 0)
        return FALSE;
    if (!ReadXboxQword(addr, &lo, &hi))
        return FALSE;
    sprintf(szOut, "{Low=%lu (0x%08lX), High=%lu (0x%08lX)}",
        (unsigned long)lo, (unsigned long)lo, (unsigned long)hi, (unsigned long)hi);
    return TRUE;
}

static BOOL IsRuntimeTitleAddress(ULONG_PTR addr)
{
    ULONG_PTR base = (ULONG_PTR)g_pvModuleBase;
    if (!base || addr < base || addr >= base + 0x01000000)
        return FALSE;
    return TRUE;
}

static BOOL CALLBACK EnumVarsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext);

static BOOL IsTitleMapObject(LPCSTR szObj)
{
    size_t len;
    /* Title TU objects are "main.obj"; libs use "libcmt:foo.obj"; linker uses "<common>". */
    if (!szObj || !szObj[0] || strchr(szObj, ':'))
        return FALSE;
    len = strlen(szObj);
    return len > 4 && _stricmp(szObj + len - 4, ".obj") == 0;
}

static DWORD GuessSizeFromMangled(LPCSTR szMangled)
{
    if (!szMangled)
        return 4;
    if (strstr(szMangled, "@@3T_LARGE_INTEGER@@"))
        return 8;
    if (strstr(szMangled, "@@3U_D3DPRESENT_PARAMETERS@@"))
        return 48;
    return 4;
}

static BOOL ParseMapObjectFile(LPCSTR szLine, char *szObj, int cchObj)
{
    const char *p;
    const char *start;
    size_t len;

    if (!szLine || !szObj || cchObj <= 0)
        return FALSE;
    szObj[0] = 0;
    p = szLine + strlen(szLine);
    while (p > szLine && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t'))
        --p;
    start = p;
    while (start > szLine && start[-1] != ' ' && start[-1] != '\t')
        --start;
    len = (size_t)(p - start);
    if (!len || len >= (size_t)cchObj)
        return FALSE;
    memcpy(szObj, start, len);
    szObj[len] = 0;
    return strstr(szObj, ".obj") != NULL;
}

static BOOL ParseMapPublicLine(LPCSTR szLine, DWORD *pSect, DWORD *pLinkAddr, char *szMangled, int cchMangled, char *szObj, int cchObj)
{
    const char *p;
    char sectOff[32];
    DWORD sectOffVal = 0;
    int n;

    if (!szLine || !pSect || !pLinkAddr || !szMangled || !szObj)
        return FALSE;
    szObj[0] = 0;
    while (*szLine == ' ' || *szLine == '\t')
        ++szLine;
    if (szLine[0] < '0' || szLine[0] > '9')
        return FALSE;
    n = 0;
    while (szLine[n] && szLine[n] != ' ' && szLine[n] != '\t' && n < (int)sizeof sectOff - 1) {
        sectOff[n] = szLine[n];
        ++n;
    }
    sectOff[n] = 0;
    if (sscanf(sectOff, "%x:%x", pSect, &sectOffVal) < 1) {
        if (sscanf(sectOff, "%x", pSect) != 1)
            return FALSE;
        sectOffVal = 0;
    }
    p = szLine + n;
    while (*p == ' ' || *p == '\t')
        ++p;
    n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && n < cchMangled - 1) {
        szMangled[n] = p[n];
        ++n;
    }
    szMangled[n] = 0;
    if (!szMangled[0] || szMangled[0] == '.')
        return FALSE;
    p += n;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (sscanf(p, "%x", pLinkAddr) != 1)
        *pLinkAddr = g_dwMapLinkBase + sectOffVal;
    (void)ParseMapObjectFile(szLine, szObj, cchObj);
    return TRUE;
}

static BOOL AppendVariableJsonEx(VAR_JSON_CTX *pj, LPCSTR szName, LPCSTR szValue, BOOL fExpandable, LPCSTR szBase);

static BOOL IsMapSectionBoundary(LPCSTR szLine)
{
    if (!szLine || !szLine[0])
        return FALSE;
    return strstr(szLine, "Publics by RVA") != NULL ||
        strstr(szLine, "Static symbols") != NULL ||
        strstr(szLine, "Line numbers") != NULL ||
        strstr(szLine, "entry point at") != NULL;
}

static BOOL EmitMapGlobalSymbol(VAR_JSON_CTX *pj, DWORD sect, DWORD linkAddr, LPCSTR mangled, LPCSTR obj)
{
    char display[MAX_SYM_NAME];
    char szVal[48];
    ULONG_PTR runtimeAddr;
    DWORD size;
    BOOL fExpand;

    if (!pj || pj->fStop || !mangled || !mangled[0] || pj->cEmitted >= pj->maxVars)
        return FALSE;
    if (sect != 3)
        return FALSE;
    if (!obj[0] || !IsTitleMapObject(obj))
        return FALSE;
    if (mangled[0] == '_' && mangled[1] == '_')
        return FALSE;

    display[0] = 0;
    UnDecorateSymbolName(mangled, display, sizeof display, UNDNAME_NAME_ONLY);
    if (!display[0])
        strncpy(display, mangled, sizeof display - 1);
    display[sizeof display - 1] = 0;
    if (!display[0] || IsSpuriousMemberName(display) || IsVarEmitted(pj, display))
        return FALSE;

    size = GuessSizeFromMangled(mangled);
    runtimeAddr = 0;
    if (g_pvModuleBase)
        runtimeAddr = (ULONG_PTR)g_pvModuleBase + (ULONG_PTR)(linkAddr - g_dwMapLinkBase);

    /* List only — no DmGetMemory here (kit reads can block; evaluate/watch reads on demand). */
    if (size > 4)
        sprintf(szVal, "{%lu bytes}", (unsigned long)size);
    else if (runtimeAddr)
        sprintf(szVal, "@0x%08lX", (unsigned long)runtimeAddr);
    else
        strcpy(szVal, "-");

    fExpand = size > 4;
    if (!AppendVariableJsonEx(pj, display, szVal, fExpand, display)) {
        pj->fStop = TRUE;
        return FALSE;
    }
    MarkVarEmitted(pj, display);
    return TRUE;
}

static void EmitMapSectionGlobals(VAR_JSON_CTX *pj, FILE *fp)
{
    char line[512];
    DWORD linesScanned = 0;

    while (fgets(line, sizeof line, fp) && !pj->fStop && pj->cEmitted < pj->maxVars && linesScanned < 8000) {
        DWORD sect = 0, linkAddr = 0;
        char mangled[256], obj[128];

        ++linesScanned;
        if (IsMapSectionBoundary(line))
            break;
        if (!ParseMapPublicLine(line, &sect, &linkAddr, mangled, sizeof mangled, obj, sizeof obj))
            continue;
        EmitMapGlobalSymbol(pj, sect, linkAddr, mangled, obj);
    }
}

static void EmitMapFileGlobals(VAR_JSON_CTX *pj)
{
    FILE *fp;
    char line[512];
    int pass;

    if (!pj || !g_szMapPath[0])
        return;

    fp = fopen(g_szMapPath, "r");
    if (!fp) {
        BridgeLog("Globals: map not found (%s)", g_szMapPath);
        return;
    }

    /* Pass 0 = Publics by Value, pass 1 = Static symbols (file-scope statics). */
    for (pass = 0; pass < 2 && !pj->fStop && pj->cEmitted < pj->maxVars; ++pass) {
        const char *marker = pass ? "Static symbols" : "Publics by Value";

        fseek(fp, 0, SEEK_SET);
        while (fgets(line, sizeof line, fp) && !pj->fStop && pj->cEmitted < pj->maxVars) {
            if (strstr(line, marker)) {
                EmitMapSectionGlobals(pj, fp);
                break;
            }
        }
    }
    fclose(fp);
}

static BOOL AppendVariableJsonEx(VAR_JSON_CTX *pj, LPCSTR szName, LPCSTR szValue, BOOL fExpandable, LPCSTR szBase)
{
    if (!pj || !szName || !szValue || pj->count >= pj->maxVars)
        return FALSE;
    if (pj->pos >= pj->cchBuf - 160)
        return FALSE;
    if (pj->count++)
        pj->buf[pj->pos++] = ',';
    pj->buf[pj->pos++] = '{';
    pj->pos += sprintf(pj->buf + pj->pos, "\"name\":");
    JsonAppendEscaped(pj->buf, pj->cchBuf, &pj->pos, szName);
    pj->buf[pj->pos++] = ',';
    pj->pos += sprintf(pj->buf + pj->pos, "\"value\":");
    JsonAppendEscaped(pj->buf, pj->cchBuf, &pj->pos, szValue);
    if (fExpandable) {
        pj->pos += sprintf(pj->buf + pj->pos, ",\"expandable\":true,\"base\":");
        JsonAppendEscaped(pj->buf, pj->cchBuf, &pj->pos, szBase ? szBase : szName);
    }
    if (pj->pos < pj->cchBuf - 2)
        pj->buf[pj->pos++] = '}';
    pj->buf[pj->pos] = 0;
    return TRUE;
}

static BOOL AppendVariableJson(VAR_JSON_CTX *pj, LPCSTR szName, LPCSTR szValue)
{
    return AppendVariableJsonEx(pj, szName, szValue, FALSE, NULL);
}

static DWORD ResolveNestedTypeId(DWORD childId, DWORD fieldType, DWORD childTag);

static BOOL EmitScalarMember(ULONG_PTR fieldAddr, ULONG_PTR rootAddr, DWORD length, DWORD fieldTypeId,
    LPCSTR szName, VAR_JSON_CTX *ctx, EMITTED_COVERAGE *cov);

static void EmitStructMembersRecursive(DWORD typeId, ULONG_PTR structAddr, ULONG_PTR rootAddr,
    VAR_JSON_CTX *ctx, EMITTED_COVERAGE *cov, int depth);

static BOOL ResolveSymbolAddress(PSYMBOL_INFO pSym, PXBDM_CONTEXT ctx, ULONG_PTR *pAddr)
{
    if (!pSym || !pAddr)
        return FALSE;
    if (pSym->Flags & SYMFLAG_REGISTER)
        return FALSE;
    if (pSym->Flags & SYMFLAG_REGREL) {
        if (!ctx)
            return FALSE;
        *pAddr = (ULONG_PTR)ctx->Ebp + (ULONG_PTR)(LONG)pSym->Address;
        return TRUE;
    }
    if (pSym->ModBase == BRIDGE_MAP_RUNTIME_MODBASE) {
        *pAddr = (ULONG_PTR)pSym->Address;
        return TRUE;
    }
    *pAddr = (ULONG_PTR)SymbolsRelocateAddress((PVOID)(ULONG_PTR)pSym->Address, g_pvModuleBase);
    return TRUE;
}

static BOOL ResolveSymbolValue(PSYMBOL_INFO pSym, PXBDM_CONTEXT ctx, DWORD *pdwOut)
{
    ULONG_PTR addr;

    if (!pSym || !pdwOut)
        return FALSE;
    if (pSym->Flags & SYMFLAG_REGISTER) {
        *pdwOut = RegValueFromContext(ctx, pSym->Register);
        return TRUE;
    }
    if (!ResolveSymbolAddress(pSym, ctx, &addr))
        return FALSE;
    return ReadXboxDword((PVOID)addr, pdwOut);
}

static BOOL LookupD3dppMemberOffset(LPCSTR szMember, DWORD *pOffset)
{
    static const struct { LPCSTR name; DWORD off; } members[] = {
        { "BackBufferWidth", 0 },
        { "BackBufferHeight", 4 },
        { "BackBufferFormat", 8 },
        { "BackBufferCount", 12 },
        { "MultiSampleType", 16 },
        { "SwapEffect", 20 },
        { "hDeviceWindow", 24 },
        { "Windowed", 28 },
        { "EnableAutoDepthStencil", 32 },
        { "AutoDepthStencilFormat", 36 },
        { "Flags", 40 },
        { "FullScreen_RefreshRateInHz", 44 },
        { "FullScreen_PresentationInterval", 48 },
    };
    int i;

    for (i = 0; i < (int)(sizeof members / sizeof members[0]); ++i) {
        if (_stricmp(szMember, members[i].name) == 0) {
            *pOffset = members[i].off;
            return TRUE;
        }
    }
    return FALSE;
}

static ULONG_PTR PdbAddressFromRuntime(ULONG_PTR runtimeAddr)
{
    if (g_pvModuleBase && g_dw64PdbBase)
        return (ULONG_PTR)g_dw64PdbBase + (runtimeAddr - (ULONG_PTR)g_pvModuleBase);
    return runtimeAddr;
}

static void SetupSymbolContext(PXBDM_CONTEXT ctx)
{
    IMAGEHLP_STACK_FRAME frame;

    if (!g_fSymInit || !ctx)
        return;
    ZeroMemory(&frame, sizeof frame);
    frame.InstructionOffset = PdbAddressFromRuntime(ctx->Eip);
    frame.FrameOffset = ctx->Ebp;
    frame.StackOffset = ctx->Esp;
    SymSetContext(SymbolsProcess(), &frame, NULL);
}

static DWORD GetUdtTypeIndex(DWORD typeIndex)
{
    HANDLE hProc = SymbolsProcess();
    DWORD udtType = 0;

    if (!typeIndex || !g_dw64PdbBase)
        return typeIndex;
    if (SymGetTypeInfo(hProc, g_dw64PdbBase, typeIndex, TI_GET_TYPE, &udtType) && udtType)
        return udtType;
    return typeIndex;
}

static DWORD GetTypeByteSize(DWORD typeId)
{
    HANDLE hProc = SymbolsProcess();
    DWORD length = 0;

    if (!typeId || !g_dw64PdbBase)
        return 0;
    if (SymGetTypeInfo(hProc, g_dw64PdbBase, GetUdtTypeIndex(typeId), TI_GET_LENGTH, &length))
        return length;
    return 0;
}

static DWORD LookupTypeIndexByName(LPCSTR szName)
{
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    HANDLE hProc = SymbolsProcess();

    if (!szName || !szName[0] || !g_dw64PdbBase)
        return 0;
    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;
    if (!SymGetTypeFromName(hProc, g_dw64PdbBase, (PCHAR)szName, pSym))
        return 0;
    return GetUdtTypeIndex(pSym->TypeIndex);
}

static DWORD ResolveNestedTypeId(DWORD childId, DWORD fieldType, DWORD childTag)
{
    /* SymTagBaseClass: ChildId is already the base-class type index (MSDN). */
    if (childTag == SYM_TAG_BASECLASS)
        return GetUdtTypeIndex(childId);
    if (fieldType)
        return GetUdtTypeIndex(fieldType);
    if (childTag == SYM_TAG_UDT || childTag == SYM_TAG_ARRAYTYPE)
        return GetUdtTypeIndex(childId);
    return childId;
}

static BOOL EmitScalarMember(ULONG_PTR fieldAddr, ULONG_PTR rootAddr, DWORD length, DWORD fieldTypeId,
    LPCSTR szName, VAR_JSON_CTX *ctx, EMITTED_COVERAGE *cov)
{
    DWORD dw = 0;
    DWORD rootOff;
    char szVal[64];

    if (!ctx || !szName || !szName[0] || ctx->count >= ctx->maxVars)
        return FALSE;
    if (length != 4 || fieldAddr < rootAddr)
        return FALSE;
    rootOff = (DWORD)(fieldAddr - rootAddr);
    if (cov && CoverageOverlaps(cov, rootOff, length))
        return FALSE;
    if (IsVarEmitted(ctx, szName))
        return FALSE;
    if (!ReadXboxDword((PVOID)fieldAddr, &dw))
        return FALSE;
    FormatMemberValue(szVal, sizeof szVal, dw, szName, fieldTypeId);
    if (!AppendVariableJson(ctx, szName, szVal))
        return FALSE;
    MarkVarEmitted(ctx, szName);
    if (cov)
        CoverageAdd(cov, rootOff, length);
    return TRUE;
}

/* DbgHelp TI_GET_SYMNAME returns a WCHAR*; treating it as char* yields single-letter names. */
static BOOL GetTypeSymName(DWORD typeId, char *szOut, int cchOut)
{
    WCHAR *wsz = NULL;
    HANDLE hProc = SymbolsProcess();

    if (!szOut || cchOut <= 0)
        return FALSE;
    szOut[0] = 0;
    if (!g_dw64PdbBase)
        return FALSE;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_SYMNAME, &wsz) || !wsz)
        return FALSE;
    if (wsz[0] && !wsz[1] && wsz[0] < 128) {
        szOut[0] = (char)wsz[0];
        szOut[1] = 0;
        return TRUE;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, wsz, -1, szOut, cchOut, NULL, NULL))
        return FALSE;
    return szOut[0] != 0;
}

static BOOL FindMemberOffsetInType(DWORD typeIndex, LPCSTR szMember, DWORD baseOff, DWORD *pOffset, int depth)
{
    HANDLE hProc = SymbolsProcess();
    DWORD childCount = 0;
    TI_FINDCHILDREN_PARAMS *pParams = NULL;
    ULONG allocSize;
    DWORD i;
    BOOL found = FALSE;

    if (!pOffset || !szMember || !szMember[0] || depth > 6)
        return FALSE;

    typeIndex = GetUdtTypeIndex(typeIndex);
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeIndex, TI_GET_CHILDRENCOUNT, &childCount) || !childCount)
        return FALSE;

    allocSize = sizeof(TI_FINDCHILDREN_PARAMS) + sizeof(ULONG) * (childCount - 1);
    pParams = (TI_FINDCHILDREN_PARAMS *)malloc(allocSize);
    if (!pParams)
        return FALSE;
    ZeroMemory(pParams, allocSize);
    pParams->Count = childCount;
    pParams->Start = 0;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeIndex, TI_FINDCHILDREN, pParams)) {
        free(pParams);
        return FALSE;
    }

    {
        DWORD layoutOff = 0;
        for (i = 0; i < childCount; ++i) {
            char szChildName[MAX_SYM_NAME];
            DWORD offset = 0;
            DWORD childTag = 0;
            DWORD fieldType = 0;
            DWORD length = 0;
            BOOL hasOffset;

            if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_SYMTAG, &childTag))
                continue;
            if (!IsLayoutChildTag(childTag))
                continue;
            hasOffset = SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_OFFSET, &offset) != FALSE;
            if (!hasOffset)
                offset = layoutOff;
            if (!GetTypeSymName(pParams->ChildId[i], szChildName, sizeof szChildName))
                szChildName[0] = 0;
            SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_TYPE, &fieldType);

            if (childTag == SYM_TAG_DATA) {
                if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_LENGTH, &length))
                    length = 4;
                if (_stricmp(szChildName, szMember) == 0) {
                    *pOffset = baseOff + offset;
                    found = TRUE;
                    break;
                }
                layoutOff = offset + length;
            } else {
                DWORD nestedType = ResolveNestedTypeId(pParams->ChildId[i], fieldType, childTag);
                if (FindMemberOffsetInType(nestedType, szMember, baseOff + offset, pOffset, depth + 1)) {
                    found = TRUE;
                    break;
                }
                {
                    DWORD nestedSize = GetTypeByteSize(nestedType);
                    if (nestedSize)
                        layoutOff = offset + nestedSize;
                }
            }
        }
    }
    free(pParams);
    return found;
}

static BOOL FindMemberOffset(DWORD typeIndex, LPCSTR szMember, DWORD *pOffset)
{
    if (!pOffset)
        return FALSE;
    *pOffset = 0;
    return FindMemberOffsetInType(typeIndex, szMember, 0, pOffset, 0);
}

static BOOL TypeHasSymTag(DWORD typeId, DWORD tag)
{
    DWORD actual = 0;
    HANDLE hProc = SymbolsProcess();

    if (!typeId || !g_dw64PdbBase)
        return FALSE;
    return SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_SYMTAG, &actual) && actual == tag;
}

static BOOL TypeIsArray(DWORD typeId)
{
    return TypeHasSymTag(typeId, SYM_TAG_ARRAYTYPE);
}

static DWORD ResolveArrayTypeId(DWORD typeId)
{
    DWORD fieldType = 0;

    if (!typeId)
        return 0;
    if (TypeIsArray(typeId))
        return typeId;
    if (g_dw64PdbBase &&
        SymGetTypeInfo(SymbolsProcess(), g_dw64PdbBase, typeId, TI_GET_TYPE, &fieldType) &&
        TypeIsArray(fieldType))
        return fieldType;
    return 0;
}

static BOOL TryReadArrayLayout(DWORD typeId, DWORD *pElemCount, DWORD *pElemSize)
{
    HANDLE hProc = SymbolsProcess();
    DWORD elemCount = 0;
    DWORD elemType = 0;
    DWORD elemLen = 0;

    typeId = ResolveArrayTypeId(typeId);
    if (!typeId || !pElemCount || !pElemSize || !g_dw64PdbBase)
        return FALSE;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_COUNT, &elemCount) || !elemCount)
        return FALSE;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_TYPE, &elemType) || !elemType)
        return FALSE;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, elemType, TI_GET_LENGTH, &elemLen) || !elemLen)
        elemLen = 4;
    if (elemCount > 256)
        return FALSE;
    *pElemCount = elemCount;
    *pElemSize = elemLen;
    return TRUE;
}

static BOOL TypeNameLooksLikeVector(LPCSTR szTypeName)
{
    const char *p;

    if (!szTypeName || !szTypeName[0])
        return FALSE;
    p = strstr(szTypeName, "vector");
    if (!p)
        return FALSE;
    if (p > szTypeName && (p[-1] == ':' || p[-1] == '>'))
        return TRUE;
    if (p == szTypeName || p[-1] == ' ' || p[-1] == '<')
        return TRUE;
    return strstr(szTypeName, "vector<") != NULL || strstr(szTypeName, "vector>") != NULL;
}

static DWORD GuessVectorElemSize(LPCSTR szTypeName)
{
    const char *p;

    if (!szTypeName)
        return 4;
    p = strstr(szTypeName, "vector<");
    if (!p)
        p = strstr(szTypeName, "vector");
    if (!p)
        return 4;
    if (p[6] == '<')
        p += 7;
    else
        p += 6;
    while (*p == ' ')
        ++p;
    if (strncmp(p, "unsigned int", 12) == 0)
        return 4;
    if (strncmp(p, "int", 3) == 0 && (p[3] == ',' || p[3] == '>'))
        return 4;
    if (strncmp(p, "long", 4) == 0)
        return 4;
    if (strncmp(p, "float", 5) == 0)
        return 4;
    if (strncmp(p, "double", 6) == 0)
        return 8;
    if (strncmp(p, "char", 4) == 0 && (p[4] == ',' || p[4] == '>'))
        return 1;
    return 4;
}

static BOOL VectorCountFromPointers(DWORD first, DWORD last, DWORD elemSize, DWORD *pCount)
{
    if (!pCount || !elemSize)
        return FALSE;
    if (first == 0 && last == 0) {
        *pCount = 0;
        return TRUE;
    }
    if (last < first || ((last - first) % elemSize) != 0)
        return FALSE;
    *pCount = (last - first) / elemSize;
    return *pCount <= 256;
}

static BOOL ResolveVectorPointerOffsets(DWORD typeId, DWORD objSize, DWORD *pOffFirst, DWORD *pOffLast)
{
    DWORD offMyval2 = 0;

    if (!pOffFirst || !pOffLast)
        return FALSE;
    if (FindMemberOffset(typeId, "_Myfirst", pOffFirst) &&
        FindMemberOffset(typeId, "_Mylast", pOffLast))
        return TRUE;
    if (FindMemberOffset(typeId, "_Myval2", &offMyval2)) {
        *pOffFirst = offMyval2;
        *pOffLast = offMyval2 + 4;
        return TRUE;
    }
    /* 32-bit MSVC std::vector with empty-base allocator: _Vector_val {_Myfirst,_Mylast,_Myend}. */
    if (objSize == 12) {
        *pOffFirst = 0;
        *pOffLast = 4;
        return TRUE;
    }
    return FALSE;
}

static BOOL TryReadVectorLayout(DWORD typeId, ULONG_PTR baseAddr, DWORD objSize, DWORD *pFirst, DWORD *pLast,
    DWORD *pElemSize, DWORD *pCount)
{
    DWORD offFirst = 0, offLast = 0;
    DWORD first = 0, last = 0;
    char szTypeName[MAX_SYM_NAME];
    DWORD elemSize, count;
    BOOL likelyVector = FALSE;

    if (!pFirst || !pLast || !pElemSize || !pCount)
        return FALSE;
    *pCount = 0;
    typeId = GetUdtTypeIndex(typeId);
    szTypeName[0] = 0;
    if (GetTypeSymName(typeId, szTypeName, sizeof szTypeName))
        likelyVector = TypeNameLooksLikeVector(szTypeName);
    if (!likelyVector && objSize == 12 && !ResolveArrayTypeId(typeId))
        likelyVector = TRUE;
    if (!likelyVector)
        return FALSE;
    if (!ResolveVectorPointerOffsets(typeId, objSize, &offFirst, &offLast))
        return FALSE;
    if (!ReadXboxDword((PVOID)(baseAddr + offFirst), &first) ||
        !ReadXboxDword((PVOID)(baseAddr + offLast), &last))
        return FALSE;
    elemSize = GuessVectorElemSize(szTypeName);
    if (!elemSize)
        return FALSE;
    if (!VectorCountFromPointers(first, last, elemSize, &count))
        return FALSE;
    *pFirst = first;
    *pLast = last;
    *pElemSize = elemSize;
    *pCount = count;
    return TRUE;
}

static BOOL EmitStdVectorMembers(DWORD typeId, ULONG_PTR baseAddr, DWORD objSize, VAR_JSON_CTX *ctx)
{
    DWORD first, last, elemSize, count, i;
    char szVal[64];
    char szIdx[16];

    if (!ctx || !TryReadVectorLayout(typeId, baseAddr, objSize, &first, &last, &elemSize, &count))
        return FALSE;
    sprintf(szVal, "%lu", (unsigned long)count);
    if (!AppendVariableJson(ctx, "size", szVal))
        return FALSE;
    for (i = 0; i < count && ctx->count < ctx->maxVars; ++i) {
        DWORD dw = 0;
        sprintf(szIdx, "[%lu]", (unsigned long)i);
        if (!ReadXboxDword((PVOID)(ULONG_PTR)(first + i * elemSize), &dw))
            continue;
        FormatScalarValue(szVal, sizeof szVal, dw, szIdx);
        AppendVariableJson(ctx, szIdx, szVal);
    }
    return ctx->count > 0;
}

static BOOL TypeNameLooksLikeMap(LPCSTR szTypeName)
{
    const char *p;

    if (!szTypeName || !szTypeName[0])
        return FALSE;
    p = strstr(szTypeName, "map");
    if (!p)
        return FALSE;
    if (p > szTypeName && (p[-1] == ':' || p[-1] == '>'))
        return TRUE;
    if (p == szTypeName || p[-1] == ' ' || p[-1] == '<')
        return TRUE;
    return strstr(szTypeName, "map<") != NULL || strstr(szTypeName, "map>") != NULL;
}

static BOOL ResolveMapPointerOffsets(DWORD typeId, DWORD objSize, BOOL fFastMap, DWORD *pOffHead, DWORD *pOffSize)
{
    if (!pOffHead || !pOffSize)
        return FALSE;
    if (fFastMap && objSize == 8) {
        *pOffHead = 0;
        *pOffSize = 4;
        return TRUE;
    }
    if (FindMemberOffset(typeId, "_Myhead", pOffHead) &&
        FindMemberOffset(typeId, "_Mysize", pOffSize))
        return TRUE;
    if (objSize == 8) {
        *pOffHead = 0;
        *pOffSize = 4;
        return TRUE;
    }
    return FALSE;
}

static BOOL TryReadMapLayout(DWORD typeId, ULONG_PTR baseAddr, DWORD objSize, DWORD *pHead, DWORD *pSize)
{
    DWORD offHead = 0, offSize = 0;
    DWORD head = 0, size = 0;
    char szTypeName[MAX_SYM_NAME];
    BOOL likelyMap = FALSE;

    if (!pHead || !pSize)
        return FALSE;
    typeId = GetUdtTypeIndex(typeId);
    szTypeName[0] = 0;
    if (GetTypeSymName(typeId, szTypeName, sizeof szTypeName))
        likelyMap = TypeNameLooksLikeMap(szTypeName);
    if (!likelyMap && !FindMemberOffset(typeId, "_Myhead", &offHead))
        return FALSE;
    if (!ResolveMapPointerOffsets(typeId, objSize, likelyMap, &offHead, &offSize))
        return FALSE;
    if (!ReadXboxDword((PVOID)(baseAddr + offHead), &head) ||
        !ReadXboxDword((PVOID)(baseAddr + offSize), &size))
        return FALSE;
    if (size > 256)
        return FALSE;
    *pHead = head;
    *pSize = size;
    return TRUE;
}

static BOOL MapTreeMinimum(DWORD myhead, DWORD *pNode)
{
    DWORD left = 0;

    if (!pNode || !myhead)
        return FALSE;
    if (!ReadXboxDword((PVOID)(ULONG_PTR)(myhead + MAP_NODE_OFF_LEFT), &left))
        return FALSE;
    if (MapPtrIsSentinel(myhead, left)) {
        *pNode = 0;
        return TRUE;
    }
    *pNode = left;
    return TRUE;
}

static BOOL MapTreeMinChild(DWORD myhead, DWORD subtree, DWORD *pNode)
{
    DWORD left = 0;
    DWORD steps = 0;

    if (!pNode || MapPtrIsSentinel(myhead, subtree))
        return FALSE;
    *pNode = subtree;
    for (steps = 0; steps < MAP_TREE_MAX_STEPS; ++steps) {
        if (!ReadXboxDword((PVOID)(ULONG_PTR)(subtree + MAP_NODE_OFF_LEFT), &left))
            return FALSE;
        if (MapPtrIsSentinel(myhead, left))
            return TRUE;
        subtree = left;
    }
    return FALSE;
}

static BOOL MapTreeSuccessor(DWORD myhead, DWORD node, DWORD *pNext)
{
    DWORD right = 0;
    DWORD parent = 0;
    DWORD parentRight = 0;
    DWORD steps = 0;

    if (!pNext || !node || MapPtrIsSentinel(myhead, node))
        return FALSE;
    if (!ReadXboxDword((PVOID)(ULONG_PTR)(node + MAP_NODE_OFF_RIGHT), &right))
        return FALSE;
    if (!MapPtrIsSentinel(myhead, right))
        return MapTreeMinChild(myhead, right, pNext);

    for (steps = 0; steps < MAP_TREE_MAX_STEPS; ++steps) {
        if (!ReadXboxDword((PVOID)(ULONG_PTR)(node + MAP_NODE_OFF_PARENT), &parent))
            return FALSE;
        if (MapPtrIsSentinel(myhead, parent)) {
            *pNext = 0;
            return TRUE;
        }
        if (!ReadXboxDword((PVOID)(ULONG_PTR)(parent + MAP_NODE_OFF_RIGHT), &parentRight))
            return FALSE;
        if (node != parentRight) {
            *pNext = parent;
            return TRUE;
        }
        node = parent;
    }
    return FALSE;
}

static BOOL EmitRawDwordArray(ULONG_PTR baseAddr, DWORD byteSize, VAR_JSON_CTX *ctx)
{
    DWORD count, i, dw;
    char szVal[64];
    char szIdx[16];

    if (!ctx || byteSize < 4 || (byteSize % 4) != 0 || byteSize > 1024)
        return FALSE;
    count = byteSize / 4;
    sprintf(szVal, "%lu", (unsigned long)count);
    if (!AppendVariableJson(ctx, "size", szVal))
        return FALSE;
    for (i = 0; i < count && ctx->count < ctx->maxVars; ++i) {
        if (!ReadXboxDword((PVOID)(baseAddr + i * 4), &dw))
            continue;
        sprintf(szIdx, "[%lu]", (unsigned long)i);
        FormatScalarValue(szVal, sizeof szVal, dw, szIdx);
        AppendVariableJson(ctx, szIdx, szVal);
    }
    return ctx->count > 0;
}

static BOOL EmitStdMapMembers(DWORD typeId, ULONG_PTR baseAddr, DWORD objSize, VAR_JSON_CTX *ctx)
{
    DWORD myhead = 0, size = 0, node = 0, emitted = 0, key = 0, val = 0;
    char szVal[64];
    char szIdx[32];
    const DWORD kKeySize = 4;

    if (!ctx || !TryReadMapLayout(typeId, baseAddr, objSize, &myhead, &size))
        return FALSE;
    sprintf(szVal, "%lu", (unsigned long)size);
    if (!AppendVariableJson(ctx, "size", szVal))
        return FALSE;
    if (!size || !myhead)
        return ctx->count > 0;
    if (!MapTreeMinimum(myhead, &node) || !node || node == myhead)
        return ctx->count > 0;
    do {
        if (emitted >= size || emitted >= 256 || ctx->count >= ctx->maxVars)
            break;
        if (!ReadXboxDword((PVOID)(ULONG_PTR)(node + MAP_NODE_OFF_MYVAL), &key) ||
            !ReadXboxDword((PVOID)(ULONG_PTR)(node + MAP_NODE_OFF_MYVAL + kKeySize), &val))
            break;
        sprintf(szIdx, "[%ld]", (long)key);
        FormatScalarValue(szVal, sizeof szVal, val, szIdx);
        if (!AppendVariableJson(ctx, szIdx, szVal))
            break;
        ++emitted;
        if (!MapTreeSuccessor(myhead, node, &node) || !node || node == myhead)
            break;
    } while (1);
    return ctx->count > 0;
}

static BOOL FormatLocalAggregateSummary(DWORD typeId, ULONG_PTR addr, DWORD objSize, char *szVal, int cchVal)
{
    DWORD elemCount = 0, elemSize = 0;
    DWORD mapHead = 0, mapSize = 0;
    DWORD first = 0, last = 0, vecCount = 0;

    if (!szVal || cchVal <= 0)
        return FALSE;
    if (TryReadArrayLayout(typeId, &elemCount, &elemSize)) {
        sprintf(szVal, "array[%lu]", (unsigned long)elemCount);
        return TRUE;
    }
    if (TryReadMapLayout(typeId, addr, objSize, &mapHead, &mapSize)) {
        sprintf(szVal, "map size=%lu", (unsigned long)mapSize);
        return TRUE;
    }
    if (TryReadVectorLayout(typeId, addr, objSize, &first, &last, &elemSize, &vecCount)) {
        sprintf(szVal, "vector size=%lu", (unsigned long)vecCount);
        return TRUE;
    }
    if (objSize >= 4 && (objSize % 4) == 0 && objSize <= 1024) {
        char szTypeName[MAX_SYM_NAME];
        szTypeName[0] = 0;
        GetTypeSymName(GetUdtTypeIndex(typeId), szTypeName, sizeof szTypeName);
        if (!TypeNameLooksLikeMap(szTypeName) && !TypeNameLooksLikeVector(szTypeName)) {
            sprintf(szVal, "array[%lu]", (unsigned long)(objSize / 4));
            return TRUE;
        }
    }
    sprintf(szVal, "{%lu bytes}", (unsigned long)objSize);
    return TRUE;
}

static BOOL TypeLooksLikeStdAggregate(DWORD typeId, DWORD objSize)
{
    char szTypeName[MAX_SYM_NAME];

    if (ResolveArrayTypeId(typeId))
        return TRUE;
    szTypeName[0] = 0;
    GetTypeSymName(GetUdtTypeIndex(typeId), szTypeName, sizeof szTypeName);
    if (TypeNameLooksLikeVector(szTypeName) || TypeNameLooksLikeMap(szTypeName))
        return TRUE;
    if (objSize == 8 && TypeNameLooksLikeMap(szTypeName))
        return TRUE;
    if (objSize == 12 && TypeNameLooksLikeVector(szTypeName))
        return TRUE;
    return FALSE;
}

static BOOL FindFrameLocalByName(DWORD typeId, LPCSTR szName, DWORD *pTypeIndex, DWORD *pOffset, DWORD *pLength,
    int depth)
{
    HANDLE hProc = SymbolsProcess();
    DWORD childCount = 0;
    DWORD tag = 0;
    TI_FINDCHILDREN_PARAMS *pParams = NULL;
    DWORD i;
    BOOL found = FALSE;

    if (!szName || !szName[0] || !pTypeIndex || !pOffset || !pLength || depth > 6 || !g_dw64PdbBase)
        return FALSE;

    if (SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_SYMTAG, &tag)) {
        if (tag == SYM_TAG_DATA || tag == SYM_TAG_ARRAYTYPE) {
            char szChild[MAX_SYM_NAME];
            DWORD offset = 0;
            DWORD length = 0;
            DWORD fieldType = 0;

            if (GetTypeSymName(typeId, szChild, sizeof szChild) && _stricmp(szChild, szName) == 0) {
                if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_OFFSET, &offset))
                    offset = 0;
                if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_LENGTH, &length))
                    length = 4;
                SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_TYPE, &fieldType);
                *pOffset = offset;
                *pLength = length;
                if (tag == SYM_TAG_ARRAYTYPE)
                    *pTypeIndex = typeId;
                else
                    *pTypeIndex = fieldType ? fieldType : typeId;
                return TRUE;
            }
        }
    }

    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_CHILDRENCOUNT, &childCount) || !childCount)
        return FALSE;

    {
        ULONG allocSize = sizeof(TI_FINDCHILDREN_PARAMS) + sizeof(ULONG) * (childCount - 1);
        pParams = (TI_FINDCHILDREN_PARAMS *)malloc(allocSize);
        if (!pParams)
            return FALSE;
        ZeroMemory(pParams, allocSize);
        pParams->Count = childCount;
        pParams->Start = 0;
        if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_FINDCHILDREN, pParams))
            goto done;

        for (i = 0; i < childCount && !found; ++i) {
            DWORD childTag = 0;
            if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_SYMTAG, &childTag))
                continue;
            if (childTag == SYM_TAG_DATA || childTag == SYM_TAG_ARRAYTYPE)
                found = FindFrameLocalByName(pParams->ChildId[i], szName, pTypeIndex, pOffset, pLength, depth + 1);
            else if (childTag == SYM_TAG_BLOCK || childTag == 5)
                found = FindFrameLocalByName(pParams->ChildId[i], szName, pTypeIndex, pOffset, pLength, depth + 1);
        }
    }
done:
    if (pParams)
        free(pParams);
    return found;
}

static BOOL ParseMemberExpr(LPCSTR szExpr, char *szBase, int cchBase, char *szMember, int cchMember, BOOL *pfDeref)
{
    const char *sep = NULL;
    const char *dot = NULL;
    const char *arrow = NULL;
    int baseLen;

    if (!szExpr || !szBase || !szMember || !pfDeref)
        return FALSE;
    szBase[0] = szMember[0] = 0;
    *pfDeref = FALSE;

    while (*szExpr == ' ' || *szExpr == '\t')
        ++szExpr;
    dot = strrchr(szExpr, '.');
    arrow = strstr(szExpr, "->");
    if (dot && arrow)
        sep = (dot > arrow) ? dot : arrow;
    else
        sep = dot ? dot : arrow;

    if (!sep)
        return FALSE;
    if (sep[0] == '-' && sep[1] == '>') {
        *pfDeref = TRUE;
        baseLen = (int)(sep - szExpr);
        sep += 2;
    } else {
        baseLen = (int)(sep - szExpr);
        sep += 1;
    }
    while (baseLen > 0 && (szExpr[baseLen - 1] == ' ' || szExpr[baseLen - 1] == '\t'))
        --baseLen;
    if (baseLen <= 0 || baseLen >= cchBase || !sep[0])
        return FALSE;
    memcpy(szBase, szExpr, baseLen);
    szBase[baseLen] = 0;
    strncpy(szMember, sep, cchMember - 1);
    szMember[cchMember - 1] = 0;
    {
        char *p = szMember + strlen(szMember);
        while (p > szMember && (p[-1] == ' ' || p[-1] == '\t'))
            --p;
        *p = 0;
    }
    return szMember[0] != 0;
}

static BOOL LookupBaseSymbol(LPCSTR szBase, PXBDM_CONTEXT ctx, PSYMBOL_INFO pSym)
{
    HANDLE hProc = SymbolsProcess();
    UCHAR frameBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pFrame = (PSYMBOL_INFO)frameBuf;
    DWORD64 disp = 0;
    DWORD typeIndex = 0;
    DWORD offset = 0;
    DWORD length = 0;

    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;

    if (ctx && g_fSymInit && g_dw64PdbBase) {
        SetupSymbolContext(ctx);
        pFrame->SizeOfStruct = sizeof(SYMBOL_INFO);
        pFrame->MaxNameLen = MAX_SYM_NAME;
        if (SymFromAddr(hProc, PdbAddressFromRuntime(ctx->Eip), &disp, pFrame) &&
            FindFrameLocalByName(pFrame->TypeIndex, szBase, &typeIndex, &offset, &length, 0)) {
            strncpy(pSym->Name, szBase, MAX_SYM_NAME - 1);
            pSym->Name[MAX_SYM_NAME - 1] = 0;
            pSym->TypeIndex = typeIndex;
            pSym->Size = length;
            pSym->Tag = SYM_TAG_DATA;
            pSym->Flags = SYMFLAG_REGREL;
            pSym->Address = offset;
            pSym->Register = 0;
            pSym->ModBase = 0;
            return TRUE;
        }
    }

    if (ctx)
        SetupSymbolContext(ctx);
    return SymFromName(hProc, (LPSTR)szBase, pSym) != FALSE;
}

static BOOL TryIndexedFloatOffset(DWORD totalSize, LPCSTR szMember, DWORD *pOffset)
{
    DWORD slot = 0;
    const char *p;

    if (!szMember || !pOffset || !szMember[0])
        return FALSE;
    if (szMember[0] != 'f' || szMember[1] < '0' || szMember[1] > '9')
        return FALSE;
    slot = (DWORD)strtoul(szMember + 1, (char **)&p, 10);
    if (p == szMember + 1)
        return FALSE;
    if (slot * 4 + 4 > totalSize)
        return FALSE;
    *pOffset = slot * 4;
    return TRUE;
}

static BOOL ReadMemberValue(LPCSTR szBase, PXBDM_CONTEXT ctx, LPCSTR szMember, DWORD *pdwOut)
{
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    ULONG_PTR baseAddr = 0;
    DWORD offset = 0;

    if (!LookupBaseSymbol(szBase, ctx, pSym))
        return FALSE;
    if (!ResolveSymbolAddress(pSym, ctx, &baseAddr))
        return FALSE;
    if (!FindMemberOffset(GetUdtTypeIndex(pSym->TypeIndex), szMember, &offset)) {
        if (!FindMemberOffset(pSym->TypeIndex, szMember, &offset)) {
            if (TryIndexedFloatOffset(pSym->Size, szMember, &offset))
                (void)0;
            else if ((_stricmp(szBase, "d3dpp") == 0 || pSym->Size >= 40) &&
                LookupD3dppMemberOffset(szMember, &offset))
                (void)0;
            else
                return FALSE;
        }
    }
    return ReadXboxDword((PVOID)(baseAddr + offset), pdwOut);
}

static BOOL CALLBACK EnumVarsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext)
{
    VAR_JSON_CTX *pj = (VAR_JSON_CTX *)UserContext;
    char szVal[96];
    ULONG_PTR addr = 0;
    BOOL fAppended = FALSE;

    (void)SymbolSize;
    if (!pj || !pSymInfo || !pSymInfo->Name || !pSymInfo->Name[0])
        return TRUE;
    if (pSymInfo->Name[0] == '_' && pSymInfo->Name[1] == '_')
        return TRUE;
    if (pj->fGlobals) {
        BOOL canRead;

        if (IsSpuriousMemberName(pSymInfo->Name))
            return TRUE;
        if (IsVarEmitted(pj, pSymInfo->Name))
            return TRUE;
        if (pSymInfo->Tag != SYM_TAG_DATA)
            return TRUE;
        if (pSymInfo->Flags & (SYMFLAG_REGISTER | SYMFLAG_REGREL))
            return TRUE;
        canRead = ResolveSymbolAddress(pSymInfo, pj->ctx, &addr) != FALSE;
        if (canRead && pSymInfo->ModBase != BRIDGE_MAP_RUNTIME_MODBASE)
            canRead = IsRuntimeTitleAddress(addr);
        if (!canRead)
            addr = 0;
    }
    if (pj->fLocals) {
        if (pSymInfo->Tag != SYM_TAG_DATA && pSymInfo->Tag != SYM_TAG_BLOCK)
            return TRUE;
    } else if (!pj->fGlobals && pSymInfo->Tag != SYM_TAG_DATA) {
        return TRUE;
    }

    if (pj->fGlobals) {
        if (!addr)
            strncpy(szVal, "???", sizeof szVal);
        else if (pSymInfo->Size == 8) {
            if (!FormatGlobalQword(pSymInfo, addr, szVal, sizeof szVal))
                strncpy(szVal, "???", sizeof szVal);
        } else if (pSymInfo->Size > 4) {
            sprintf(szVal, "{%lu bytes}", (unsigned long)pSymInfo->Size);
        } else if (!FormatGlobalScalar(pSymInfo, addr, szVal, sizeof szVal)) {
            strncpy(szVal, "???", sizeof szVal);
        }
        szVal[sizeof szVal - 1] = 0;
        if (pSymInfo->Size > 4 || pSymInfo->Size == 8)
            fAppended = AppendVariableJsonEx(pj, pSymInfo->Name, szVal,
                pSymInfo->TypeIndex != 0 || pSymInfo->Size > 4, pSymInfo->Name);
        else
            fAppended = AppendVariableJson(pj, pSymInfo->Name, szVal);
        if (fAppended)
            MarkVarEmitted(pj, pSymInfo->Name);
        return pj->count < pj->maxVars;
    }

    if (pSymInfo->Size > 4 && pj->fLocals) {
        if (ResolveSymbolAddress(pSymInfo, pj->ctx, &addr))
            FormatLocalAggregateSummary(pSymInfo->TypeIndex, addr, pSymInfo->Size, szVal, sizeof szVal);
        else
            sprintf(szVal, "{%lu bytes}", (unsigned long)pSymInfo->Size);
        AppendVariableJsonEx(pj, pSymInfo->Name, szVal, TRUE, pSymInfo->Name);
        return pj->count < pj->maxVars;
    }

    {
        DWORD dw;
        if (!ResolveSymbolValue(pSymInfo, pj->ctx, &dw))
            return TRUE;
        FormatScalarValue(szVal, sizeof szVal, dw, pSymInfo->Name);
        AppendVariableJson(pj, pSymInfo->Name, szVal);
    }
    return pj->count < pj->maxVars;
}

static void EmitTypeTreeLocals(DWORD typeId, PXBDM_CONTEXT pctx, VAR_JSON_CTX *pj, int depth);

static DWORD SymbolTypeByteSize(DWORD typeIndex, DWORD symSize)
{
    HANDLE hProc = SymbolsProcess();
    DWORD length = 0;

    if (symSize)
        return symSize;
    if (typeIndex && g_dw64PdbBase &&
        SymGetTypeInfo(hProc, g_dw64PdbBase, GetUdtTypeIndex(typeIndex), TI_GET_LENGTH, &length))
        return length;
    return 0;
}

static void EmitArrayTypeLocal(DWORD typeId, PXBDM_CONTEXT pctx, VAR_JSON_CTX *pj)
{
    HANDLE hProc = SymbolsProcess();
    char szName[MAX_SYM_NAME];
    DWORD offset = 0;
    DWORD elemCount = 0;
    DWORD elemSize = 0;
    char szVal[64];

    if (!pctx || !pj || pj->count >= pj->maxVars || !g_dw64PdbBase)
        return;
    if (!GetTypeSymName(typeId, szName, sizeof szName))
        return;
    if (szName[0] == '_' && szName[1] == '_')
        return;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_OFFSET, &offset))
        offset = 0;
    if (!TryReadArrayLayout(typeId, &elemCount, &elemSize))
        return;
    sprintf(szVal, "array[%lu]", (unsigned long)elemCount);
    AppendVariableJsonEx(pj, szName, szVal, TRUE, szName);
}

static void EmitDataTypeLocal(DWORD typeId, PXBDM_CONTEXT pctx, VAR_JSON_CTX *pj)
{
    HANDLE hProc = SymbolsProcess();
    char szName[MAX_SYM_NAME];
    DWORD offset = 0;
    DWORD length = 0;
    DWORD fieldType = 0;
    DWORD dw = 0;
    char szVal[64];
    ULONG_PTR addr;

    if (!pctx || !pj || pj->count >= pj->maxVars || !g_dw64PdbBase)
        return;
    if (!GetTypeSymName(typeId, szName, sizeof szName))
        return;
    if (szName[0] == '_' && szName[1] == '_')
        return;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_OFFSET, &offset))
        offset = 0;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_LENGTH, &length))
        length = 4;
    SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_TYPE, &fieldType);

    addr = (ULONG_PTR)pctx->Ebp + (ULONG_PTR)(LONG)offset;
    if (length > 4) {
        FormatLocalAggregateSummary(fieldType ? fieldType : typeId, addr, length, szVal, sizeof szVal);
        AppendVariableJsonEx(pj, szName, szVal, TRUE, szName);
        return;
    }
    if (!ReadXboxDword((PVOID)addr, &dw))
        return;
    FormatScalarValue(szVal, sizeof szVal, dw, szName);
    AppendVariableJson(pj, szName, szVal);
}

static void EmitTypeTreeLocals(DWORD typeId, PXBDM_CONTEXT pctx, VAR_JSON_CTX *pj, int depth)
{
    HANDLE hProc = SymbolsProcess();
    DWORD childCount = 0;
    TI_FINDCHILDREN_PARAMS *pParams = NULL;
    DWORD i;
    DWORD tag = 0;

    if (!pctx || !pj || !g_dw64PdbBase || depth > 4 || pj->count >= pj->maxVars)
        return;

    if (SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_SYMTAG, &tag) && tag == SYM_TAG_DATA) {
        EmitDataTypeLocal(typeId, pctx, pj);
        return;
    }

    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_CHILDRENCOUNT, &childCount) || !childCount)
        return;

    {
        ULONG allocSize = sizeof(TI_FINDCHILDREN_PARAMS) + sizeof(ULONG) * (childCount - 1);
        pParams = (TI_FINDCHILDREN_PARAMS *)malloc(allocSize);
        if (!pParams)
            return;
        ZeroMemory(pParams, allocSize);
        pParams->Count = childCount;
        pParams->Start = 0;
        if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_FINDCHILDREN, pParams))
            goto done;

        for (i = 0; i < childCount && pj->count < pj->maxVars; ++i) {
            DWORD childTag = 0;
            if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_SYMTAG, &childTag))
                continue;
            if (childTag == SYM_TAG_DATA)
                EmitDataTypeLocal(pParams->ChildId[i], pctx, pj);
            else if (childTag == SYM_TAG_ARRAYTYPE)
                EmitArrayTypeLocal(pParams->ChildId[i], pctx, pj);
            else if (childTag == SYM_TAG_BLOCK || childTag == 5)
                EmitTypeTreeLocals(pParams->ChildId[i], pctx, pj, depth + 1);
        }
    }
done:
    if (pParams)
        free(pParams);
}

static BOOL CALLBACK EnumSupplementalLocalsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext)
{
    VAR_JSON_CTX *pj = (VAR_JSON_CTX *)UserContext;
    char szVal[96];
    ULONG_PTR addr = 0;

    (void)SymbolSize;
    if (!pj || !pSymInfo || !pSymInfo->Name || !pSymInfo->Name[0])
        return TRUE;
    if (pj->fStop || pj->count >= pj->maxVars)
        return FALSE;
    if (++pj->cEnumExamined > 128)
        return FALSE;
    if (pSymInfo->Tag != SYM_TAG_DATA)
        return TRUE;
    if (!(pSymInfo->Flags & SYMFLAG_REGREL))
        return TRUE;
    if (pSymInfo->Name[0] == '_' && pSymInfo->Name[1] == '_')
        return TRUE;
    if (IsVarEmitted(pj, pSymInfo->Name))
        return TRUE;

    if (pSymInfo->Size > 4) {
        if (ResolveSymbolAddress(pSymInfo, pj->ctx, &addr))
            FormatLocalAggregateSummary(pSymInfo->TypeIndex, addr, pSymInfo->Size, szVal, sizeof szVal);
        else
            sprintf(szVal, "{%lu bytes}", (unsigned long)pSymInfo->Size);
        AppendVariableJsonEx(pj, pSymInfo->Name, szVal, TRUE, pSymInfo->Name);
        return pj->count < pj->maxVars;
    }

    {
        DWORD dw;
        if (!ResolveSymbolValue(pSymInfo, pj->ctx, &dw))
            return TRUE;
        FormatScalarValue(szVal, sizeof szVal, dw, pSymInfo->Name);
        AppendVariableJson(pj, pSymInfo->Name, szVal);
    }
    return pj->count < pj->maxVars;
}

static void EmitLocalsFromFrame(PXBDM_CONTEXT pctx, VAR_JSON_CTX *pj)
{
    HANDLE hProc = SymbolsProcess();
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    DWORD64 disp = 0;
    ULONG_PTR pdbEip;

    if (!pctx || !pj || !g_dw64PdbBase)
        return;

    SetupSymbolContext(pctx);
    pdbEip = PdbAddressFromRuntime(pctx->Eip);
    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;
    if (!SymFromAddr(hProc, pdbEip, &disp, pSym))
        return;

    EmitTypeTreeLocals(pSym->TypeIndex, pctx, pj, 0);
    pj->cEnumExamined = 0;
    SymEnumSymbols(hProc, 0, NULL, EnumSupplementalLocalsCallback, pj);
}

HRESULT SymbolsEmitVariablesJson(LPCSTR szScope, PXBDM_CONTEXT pctx, char *szBuf, int cchBuf, int *pcVars)
{
    VAR_JSON_CTX ctx;
    BOOL fLocals = FALSE, fGlobals = FALSE;

    if (!szBuf || cchBuf <= 0)
        return E_INVALIDARG;
    if (pcVars)
        *pcVars = 0;
    if (_stricmp(szScope, "locals") == 0)
        fLocals = TRUE;
    else if (_stricmp(szScope, "globals") == 0)
        fGlobals = TRUE;
    else
        return E_INVALIDARG;

    if (fGlobals) {
        if (!g_szMapPath[0])
            return E_FAIL;
    } else if (!g_fSymInit || !g_dw64PdbBase) {
        return E_FAIL;
    }

    ZeroMemory(&ctx, sizeof ctx);
    ctx.buf = szBuf;
    ctx.cchBuf = cchBuf;
    ctx.pos = 0;
    ctx.count = 0;
    ctx.maxVars = MAX_VARS_JSON;
    ctx.ctx = pctx;
    ctx.fLocals = fLocals;
    ctx.fGlobals = fGlobals;

    szBuf[0] = 0;
    if (fGlobals) {
        EmitMapFileGlobals(&ctx);
    } else if (fLocals) {
        SetupSymbolContext(pctx);
        EmitLocalsFromFrame(pctx, &ctx);
    }
    if (pcVars)
        *pcVars = ctx.count;
    return ctx.count > 0 ? S_OK : S_FALSE;
}

static void EmitArrayMembers(DWORD arrayTypeId, ULONG_PTR arrayAddr, ULONG_PTR rootAddr,
    VAR_JSON_CTX *ctx, EMITTED_COVERAGE *cov, int depth)
{
    HANDLE hProc = SymbolsProcess();
    DWORD elemCount = 0;
    DWORD elemType = 0;
    DWORD elemLen = 0;
    DWORD elemTag = 0;
    DWORD i;

    if (!ctx || depth > 6 || !g_dw64PdbBase)
        return;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, arrayTypeId, TI_GET_COUNT, &elemCount) || !elemCount)
        return;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, arrayTypeId, TI_GET_TYPE, &elemType) || !elemType)
        return;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, elemType, TI_GET_LENGTH, &elemLen) || !elemLen)
        elemLen = 4;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, elemType, TI_GET_SYMTAG, &elemTag))
        elemTag = 0;

    for (i = 0; i < elemCount && ctx->count < ctx->maxVars; ++i) {
        char szName[32];
        ULONG_PTR elemAddr = arrayAddr + (ULONG_PTR)(i * elemLen);
        sprintf(szName, "[%lu]", (unsigned long)i);
        if (elemTag == SYM_TAG_DATA && elemLen == 4) {
            EmitScalarMember(elemAddr, rootAddr, 4, elemType, szName, ctx, cov);
        } else {
            EmitStructMembersRecursive(ResolveNestedTypeId(elemType, elemType, elemTag),
                elemAddr, rootAddr, ctx, cov, depth + 1);
        }
    }
}

static void EmitStructMembersRecursive(DWORD typeId, ULONG_PTR structAddr, ULONG_PTR rootAddr,
    VAR_JSON_CTX *ctx, EMITTED_COVERAGE *cov, int depth)
{
    HANDLE hProc = SymbolsProcess();
    DWORD childCount = 0;
    TI_FINDCHILDREN_PARAMS *pParams = NULL;
    ULONG allocSize;
    DWORD i;

    if (!ctx || depth > 8 || ctx->count >= ctx->maxVars || !g_dw64PdbBase)
        return;

    typeId = GetUdtTypeIndex(typeId);
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_GET_CHILDRENCOUNT, &childCount) || !childCount)
        return;

    allocSize = sizeof(TI_FINDCHILDREN_PARAMS) + sizeof(ULONG) * (childCount - 1);
    pParams = (TI_FINDCHILDREN_PARAMS *)malloc(allocSize);
    if (!pParams)
        return;
    ZeroMemory(pParams, allocSize);
    pParams->Count = childCount;
    pParams->Start = 0;
    if (!SymGetTypeInfo(hProc, g_dw64PdbBase, typeId, TI_FINDCHILDREN, pParams))
        goto done;

    {
        DWORD layoutOff = 0;
        for (i = 0; i < childCount && ctx->count < ctx->maxVars; ++i) {
            char szChildName[MAX_SYM_NAME];
            DWORD offset = 0;
            DWORD childTag = 0;
            DWORD fieldType = 0;
            DWORD length = 0;
            BOOL hasOffset;
            int countBefore;

            if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_SYMTAG, &childTag))
                continue;
            if (!IsLayoutChildTag(childTag))
                continue;
            hasOffset = SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_OFFSET, &offset) != FALSE;
            if (!hasOffset)
                offset = layoutOff;
            if (!GetTypeSymName(pParams->ChildId[i], szChildName, sizeof szChildName))
                szChildName[0] = 0;
            if (childTag == SYM_TAG_DATA) {
                if (!szChildName[0] || IsSpuriousMemberName(szChildName))
                    continue;
                if (szChildName[0] == '_' && szChildName[1] == '_' && szChildName[2] != 0)
                    continue;
            }
            SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_TYPE, &fieldType);

            if (childTag == SYM_TAG_ARRAYTYPE) {
                EmitArrayMembers(pParams->ChildId[i], structAddr + offset, rootAddr, ctx, cov, depth + 1);
                {
                    DWORD arraySize = GetTypeByteSize(pParams->ChildId[i]);
                    if (arraySize)
                        layoutOff = offset + arraySize;
                }
            } else if (childTag == SYM_TAG_DATA) {
                if (!SymGetTypeInfo(hProc, g_dw64PdbBase, pParams->ChildId[i], TI_GET_LENGTH, &length))
                    length = 4;
                EmitScalarMember(structAddr + offset, rootAddr, length, fieldType, szChildName, ctx, cov);
                layoutOff = offset + length;
            } else {
                DWORD nestedType = ResolveNestedTypeId(pParams->ChildId[i], fieldType, childTag);
                DWORD memberRootOff = (DWORD)(structAddr + offset - rootAddr);
                DWORD nestedSize = GetTypeByteSize(nestedType);

                /* Union alternate views (e.g. LARGE_INTEGER anonymous struct + u) share bytes. */
                if (nestedSize && CoverageOverlaps(cov, memberRootOff, nestedSize)) {
                    layoutOff = offset + nestedSize;
                    continue;
                }
                countBefore = ctx->count;
                EmitStructMembersRecursive(nestedType, structAddr + offset, rootAddr, ctx, cov, depth + 1);
                if (ctx->count == countBefore && szChildName[0] &&
                    !(nestedSize && CoverageOverlaps(cov, memberRootOff, nestedSize))) {
                    DWORD altType = LookupTypeIndexByName(szChildName);
                    if (!altType && szChildName[0] == '_')
                        altType = LookupTypeIndexByName(szChildName + 1);
                    if (altType && altType != nestedType)
                        EmitStructMembersRecursive(altType, structAddr + offset, rootAddr, ctx, cov, depth + 1);
                }
                if (nestedSize)
                    layoutOff = offset + nestedSize;
            }
        }
    }
done:
    if (pParams)
        free(pParams);
}

HRESULT SymbolsEmitMembersJson(LPCSTR szBase, PXBDM_CONTEXT pctx, char *szBuf, int cchBuf, int *pcVars)
{
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    VAR_JSON_CTX ctx;
    ULONG_PTR baseAddr = 0;
    DWORD i;

    if (!szBase || !szBuf || cchBuf <= 0)
        return E_INVALIDARG;
    if (pcVars)
        *pcVars = 0;
    if (!g_fSymInit || !g_dw64PdbBase)
        return E_FAIL;

    SetupSymbolContext(pctx);
    if (!LookupBaseSymbol(szBase, pctx, pSym))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (!ResolveSymbolAddress(pSym, pctx, &baseAddr))
        return E_FAIL;

    ZeroMemory(&ctx, sizeof ctx);
    ctx.buf = szBuf;
    ctx.cchBuf = cchBuf;
    ctx.pos = 0;
    ctx.count = 0;
    ctx.maxVars = MAX_VARS_JSON;
    ctx.ctx = pctx;
    szBuf[0] = 0;

    {
        DWORD objSize = SymbolTypeByteSize(pSym->TypeIndex, pSym->Size);
        DWORD symSize = objSize ? objSize : pSym->Size;
        DWORD arrayTypeId = ResolveArrayTypeId(pSym->TypeIndex);

        if (EmitStdVectorMembers(pSym->TypeIndex, baseAddr, objSize, &ctx)) {
            if (pcVars)
                *pcVars = ctx.count;
            return ctx.count > 0 ? S_OK : S_FALSE;
        }
        ctx.pos = 0;
        ctx.count = 0;
        szBuf[0] = 0;

        if (EmitStdMapMembers(pSym->TypeIndex, baseAddr, objSize, &ctx)) {
            if (pcVars)
                *pcVars = ctx.count;
            return ctx.count > 0 ? S_OK : S_FALSE;
        }
        ctx.pos = 0;
        ctx.count = 0;
        szBuf[0] = 0;

        if (arrayTypeId) {
            EMITTED_COVERAGE cov;
            ZeroMemory(&cov, sizeof cov);
            EmitArrayMembers(arrayTypeId, baseAddr, baseAddr, &ctx, &cov, 0);
            if (ctx.count > 0) {
                if (pcVars)
                    *pcVars = ctx.count;
                return S_OK;
            }
            ctx.pos = 0;
            ctx.count = 0;
            szBuf[0] = 0;
        }

        if (EmitRawDwordArray(baseAddr, symSize, &ctx)) {
            if (pcVars)
                *pcVars = ctx.count;
            return ctx.count > 0 ? S_OK : S_FALSE;
        }
        ctx.pos = 0;
        ctx.count = 0;
        szBuf[0] = 0;

        if (TypeLooksLikeStdAggregate(pSym->TypeIndex, symSize)) {
            if (pcVars)
                *pcVars = 0;
            return S_FALSE;
        }
    }

    {
        EMITTED_COVERAGE cov;
        char szTypeName[MAX_SYM_NAME];
        DWORD typeId;

        ZeroMemory(&cov, sizeof cov);
        typeId = GetUdtTypeIndex(pSym->TypeIndex);
        EmitStructMembersRecursive(typeId, baseAddr, baseAddr, &ctx, &cov, 0);
        if (!ctx.count && typeId != pSym->TypeIndex)
            EmitStructMembersRecursive(pSym->TypeIndex, baseAddr, baseAddr, &ctx, &cov, 0);
        if (!ctx.count && GetTypeSymName(typeId, szTypeName, sizeof szTypeName)) {
            DWORD altType = LookupTypeIndexByName(szTypeName);
            if (altType && altType != typeId)
                EmitStructMembersRecursive(altType, baseAddr, baseAddr, &ctx, &cov, 0);
        }
    }

    if (!ctx.count && _stricmp(szBase, "d3dpp") == 0) {
        static const struct { LPCSTR name; DWORD off; } members[] = {
            { "BackBufferWidth", 0 },
            { "BackBufferHeight", 4 },
            { "BackBufferFormat", 8 },
            { "BackBufferCount", 12 },
            { "MultiSampleType", 16 },
            { "SwapEffect", 20 },
            { "Windowed", 28 },
            { "EnableAutoDepthStencil", 32 },
            { "AutoDepthStencilFormat", 36 },
            { "FullScreen_RefreshRateInHz", 44 },
            { "FullScreen_PresentationInterval", 48 },
        };
        for (i = 0; i < sizeof members / sizeof members[0] && ctx.count < ctx.maxVars; ++i) {
            DWORD dw = 0;
            char szVal[64];
            if (!ReadXboxDword((PVOID)(baseAddr + members[i].off), &dw))
                continue;
            FormatScalarValue(szVal, sizeof szVal, dw, members[i].name);
            AppendVariableJson(&ctx, members[i].name, szVal);
        }
    }

    if (pcVars)
        *pcVars = ctx.count;
    return ctx.count > 0 ? S_OK : S_FALSE;
}

HRESULT SymbolsEvaluate(LPCSTR szExpr, PXBDM_CONTEXT pctx, char *szValue, int cchValue, char *szErr, int cchErr)
{
    HANDLE hProc = SymbolsProcess();
    UCHAR symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symBuf;
    DWORD dw;
    char name[MAX_SYM_NAME];
    char base[MAX_SYM_NAME];
    char member[MAX_SYM_NAME];
    BOOL fDeref = FALSE;

    if (!szExpr || !szValue || cchValue <= 0)
        return E_INVALIDARG;
    szValue[0] = 0;
    if (szErr && cchErr > 0)
        szErr[0] = 0;
    if (!g_fSymInit)
        return E_FAIL;

    strncpy(name, szExpr, sizeof name - 1);
    name[sizeof name - 1] = 0;
    {
        char *p = name;
        while (*p == ' ' || *p == '\t')
            ++p;
        strncpy(name, p, sizeof name - 1);
        name[sizeof name - 1] = 0;
    }

    if (ParseMemberExpr(name, base, sizeof base, member, sizeof member, &fDeref)) {
        if (fDeref) {
            ULONG_PTR structAddr = 0;
            DWORD offset = 0;

            if (!LookupBaseSymbol(base, pctx, pSym))
                goto not_found;
            if (!ResolveSymbolValue(pSym, pctx, &dw))
                goto read_fail;
            structAddr = (ULONG_PTR)dw;
            if (!FindMemberOffset(GetUdtTypeIndex(pSym->TypeIndex), member, &offset)) {
                if (!FindMemberOffset(pSym->TypeIndex, member, &offset) &&
                    !LookupD3dppMemberOffset(member, &offset))
                    goto member_fail;
            }
            if (!ReadXboxDword((PVOID)(structAddr + offset), &dw))
                goto read_fail;
        } else if (!ReadMemberValue(base, pctx, member, &dw)) {
            goto member_fail;
        }
        FormatMemberValue(szValue, cchValue, dw, member, 0);
        return S_OK;
    }

    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = MAX_SYM_NAME;
    if (!SymFromName(hProc, name, pSym))
        goto not_found;

    if (pSym->Flags & (SYMFLAG_REGISTER | SYMFLAG_REGREL))
        SetupSymbolContext(pctx);

    if (!ResolveSymbolValue(pSym, pctx, &dw))
        goto read_fail;
    FormatScalarValue(szValue, cchValue, dw, pSym->Name);
    return S_OK;

not_found:
    if (szErr && cchErr > 0)
        strncpy(szErr, "symbolNotFound", cchErr - 1);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

member_fail:
    if (szErr && cchErr > 0)
        strncpy(szErr, "memberNotFound", cchErr - 1);
    return E_FAIL;

read_fail:
    if (szErr && cchErr > 0)
        strncpy(szErr, "readFailed", cchErr - 1);
    return E_FAIL;
}
