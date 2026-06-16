#include "bridge.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAUNCH_TIMEOUT_MS 120000
#define REBOOT_TIMEOUT_MS 120000
#define GOUSER_TIMEOUT_MS 60000
#define XBDM_NOXBOX 0x82DB0108u

static PDMN_SESSION g_psess;
static HANDLE g_hBreakEvent;
static HANDLE g_hExecPendingEvent;
static DWORD g_dwExecState = DMN_EXEC_START;
static DWORD g_dwMainThread;
static DWORD g_dwStoppedThread;
static PVOID g_pvStoppedAddr;
static PVOID g_pvModuleBase;
static BOOL g_fConnected;
static BOOL g_fLaunched;
static BOOL g_fShutdown;
static BOOL g_fAwaitingTitleThread;
static BOOL g_fSeenTitleMod;
static BOOL g_fLaunchStopped;
static BOOL g_fThreadStopped;
static char g_szLaunchBase[64];
static CRITICAL_SECTION g_csEmit;

#define MAX_PENDING_BP 64
#define MAX_ACTIVE_BP 64

typedef struct {
    char file[MAX_PATH];
    DWORD line;
} PENDING_BP;

static PENDING_BP g_rgPendingBp[MAX_PENDING_BP];
static int g_cPendingBp;
static PVOID g_rgActiveBp[MAX_ACTIVE_BP];
static BOOL g_rgbHwBp[MAX_ACTIVE_BP];
static char g_rgBpFile[MAX_ACTIVE_BP][MAX_PATH];
static DWORD g_rgBpLine[MAX_ACTIVE_BP];
static BOOL g_rgbBpHasSource[MAX_ACTIVE_BP];
static int g_cActiveBp;
static BOOL g_fNeedRearmHwBps;
static BOOL g_fStepActive;
static BOOL g_fAutoRunResume;
static BOOL g_fAutoRunLaunch;

static BOOL IsTitleAddress(PVOID pvAddr);
static HRESULT InstallBreakpointFileLine(LPCSTR szFile, DWORD dwLine, PVOID *ppvOut);
static HRESULT ResumeAllStoppedThreads(void);

static void RemoveActiveBreakpointAt(PVOID pvAddr)
{
    int i, j;

    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgActiveBp[i] == pvAddr) {
            for (j = i + 1; j < g_cActiveBp; ++j) {
                g_rgActiveBp[j - 1] = g_rgActiveBp[j];
                g_rgbHwBp[j - 1] = g_rgbHwBp[j];
                g_rgBpLine[j - 1] = g_rgBpLine[j];
                g_rgbBpHasSource[j - 1] = g_rgbBpHasSource[j];
                memcpy(g_rgBpFile[j - 1], g_rgBpFile[j], sizeof g_rgBpFile[j]);
            }
            --g_cActiveBp;
            return;
        }
    }
}

static int CountHardwareBreakpoints(void)
{
    int i, n = 0;

    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgbHwBp[i])
            ++n;
    }
    return n;
}

static BOOL EvictOldestHardwareBreakpoint(void)
{
    int i;

    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgbHwBp[i]) {
            PVOID pv = g_rgActiveBp[i];
            DmSetDataBreakpoint(pv, DMBREAK_NONE, 1);
            RemoveActiveBreakpointAt(pv);
            BridgeLog("Evicted hardware BP at %p (Xbox allows 4 execute HW breakpoints)", pv);
            return TRUE;
        }
    }
    return FALSE;
}

static void ClearActiveBreakpoints(void)
{
    int i;
    DWORD dwType;
    for (i = 0; i < g_cActiveBp; ++i) {
        if (SUCCEEDED(DmIsBreakpoint(g_rgActiveBp[i], &dwType)) &&
            dwType == DMBREAK_EXECUTE)
            DmSetDataBreakpoint(g_rgActiveBp[i], DMBREAK_NONE, 1);
        else
            DmRemoveBreakpoint(g_rgActiveBp[i]);
    }
    g_cActiveBp = 0;
}

static void QueuePendingBreakpoint(LPCSTR szFile, DWORD dwLine)
{
    int i;
    if (!szFile || !dwLine)
        return;
    for (i = 0; i < g_cPendingBp; ++i) {
        if (g_rgPendingBp[i].line == dwLine && _stricmp(g_rgPendingBp[i].file, szFile) == 0)
            return;
    }
    if (g_cPendingBp >= MAX_PENDING_BP)
        return;
    strncpy(g_rgPendingBp[g_cPendingBp].file, szFile, sizeof g_rgPendingBp[g_cPendingBp].file - 1);
    g_rgPendingBp[g_cPendingBp].file[sizeof g_rgPendingBp[g_cPendingBp].file - 1] = 0;
    g_rgPendingBp[g_cPendingBp].line = dwLine;
    ++g_cPendingBp;
}

static PVOID NormalizeBreakpointAddress(PVOID pvAddr)
{
    ULONG_PTR addr = (ULONG_PTR)pvAddr;
    ULONG_PTR pdbBase;
    ULONG_PTR kitBase;

    if (!pvAddr || !g_pvModuleBase || !SymbolsGetPdbBase())
        return pvAddr;
    pdbBase = (ULONG_PTR)SymbolsGetPdbBase();
    kitBase = (ULONG_PTR)g_pvModuleBase;
    if (addr >= pdbBase && addr < pdbBase + 0x100000 &&
        (addr < kitBase || addr >= kitBase + 0x100000)) {
        return SymbolsRelocateAddress(pvAddr, g_pvModuleBase);
    }
    return pvAddr;
}

static BOOL IsKitBreakpointAddress(PVOID pvAddr)
{
    ULONG_PTR addr = (ULONG_PTR)pvAddr;

    if (!pvAddr)
        return FALSE;
    if (g_pvModuleBase)
        return IsTitleAddress(pvAddr);
    /* Without a loaded module base, reject obvious PDB-only addresses. */
    return addr < 0x00400000 || addr >= 0x00600000;
}

static void RecordActiveBreakpointSource(int idx, LPCSTR szFile, DWORD dwLine)
{
    if (idx < 0 || idx >= MAX_ACTIVE_BP)
        return;
    if (szFile && dwLine) {
        strncpy(g_rgBpFile[idx], szFile, sizeof g_rgBpFile[idx] - 1);
        g_rgBpFile[idx][sizeof g_rgBpFile[idx] - 1] = 0;
        g_rgBpLine[idx] = dwLine;
        g_rgbBpHasSource[idx] = TRUE;
    } else {
        g_rgBpFile[idx][0] = 0;
        g_rgBpLine[idx] = 0;
        g_rgbBpHasSource[idx] = FALSE;
    }
}

static HRESULT InstallBreakpointAtEx(PVOID pvAddr, LPCSTR szFile, DWORD dwLine)
{
    HRESULT hr;
    int i;

    if (!pvAddr)
        return E_INVALIDARG;
    pvAddr = NormalizeBreakpointAddress(pvAddr);
    if (!IsKitBreakpointAddress(pvAddr)) {
        BridgeLog("Breakpoint address %p is not in title image (module base %p)", pvAddr, g_pvModuleBase);
        return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
    }
    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgActiveBp[i] == pvAddr)
            return S_OK;
    }

    /* Primary path: soft INT3 breakpoints via kernel BPL (128+ per page in dmbreak.c).
     * Do NOT probe DmGetMemory for 0xCC — Watson/VS2003 trust DmSetBreakpoint; the
     * INT3 patch is applied on the kit and may not be visible via host memory read. */
    hr = DmSetBreakpoint(pvAddr);
    if (SUCCEEDED(hr)) {
        if (g_cActiveBp < MAX_ACTIVE_BP) {
            g_rgActiveBp[g_cActiveBp] = pvAddr;
            g_rgbHwBp[g_cActiveBp] = FALSE;
            RecordActiveBreakpointSource(g_cActiveBp, szFile, dwLine);
            ++g_cActiveBp;
        }
        return S_OK;
    }

    /* Rare fallback when soft set fails (e.g. BPL allocation). Hardware slots: 4. */
    if (CountHardwareBreakpoints() >= 4 && !EvictOldestHardwareBreakpoint())
        return E_OUTOFMEMORY;
    hr = DmSetDataBreakpoint(pvAddr, DMBREAK_EXECUTE, 1);
    if (FAILED(hr)) {
        BridgeLog("Breakpoint failed at %p: soft and hw install failed (hw=0x%08lX)",
            pvAddr, (unsigned long)hr);
        if (hr == E_OUTOFMEMORY)
            return HRESULT_FROM_WIN32(ERROR_NO_MORE_USER_HANDLES);
        return hr;
    }
    if (g_cActiveBp < MAX_ACTIVE_BP) {
        g_rgActiveBp[g_cActiveBp] = pvAddr;
        g_rgbHwBp[g_cActiveBp] = TRUE;
        RecordActiveBreakpointSource(g_cActiveBp, szFile, dwLine);
        ++g_cActiveBp;
    }
    BridgeLog("Using hardware execute BP at %p (soft DmSetBreakpoint failed)", pvAddr);
    return S_OK;
}

static HRESULT InstallBreakpointAt(PVOID pvAddr)
{
    return InstallBreakpointAtEx(pvAddr, NULL, 0);
}

static void ReapplyActiveBreakpoints(void)
{
    char rgFile[MAX_ACTIVE_BP][MAX_PATH];
    DWORD rgLine[MAX_ACTIVE_BP];
    BOOL rgbSource[MAX_ACTIVE_BP];
    int i, n;

    if (!g_pvModuleBase || g_cActiveBp <= 0)
        return;
    n = g_cActiveBp;
    for (i = 0; i < n; ++i) {
        rgbSource[i] = g_rgbBpHasSource[i];
        rgLine[i] = g_rgBpLine[i];
        if (rgbSource[i])
            strncpy(rgFile[i], g_rgBpFile[i], sizeof rgFile[i] - 1);
        else
            rgFile[i][0] = 0;
        rgFile[i][sizeof rgFile[i] - 1] = 0;
    }
    ClearActiveBreakpoints();
    for (i = 0; i < n; ++i) {
        if (rgbSource[i] && rgLine[i])
            InstallBreakpointFileLine(rgFile[i], rgLine[i], NULL);
    }
}

static BOOL IsActiveHardwareBreakpoint(PVOID pvAddr)
{
    int i;
    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgActiveBp[i] == pvAddr)
            return g_rgbHwBp[i];
    }
    return FALSE;
}

static void RearmHardwareBreakpoints(void)
{
    int i;
    for (i = 0; i < g_cActiveBp; ++i) {
        if (g_rgbHwBp[i])
            DmSetDataBreakpoint(g_rgActiveBp[i], DMBREAK_EXECUTE, 1);
    }
    g_fNeedRearmHwBps = FALSE;
}

static void BypassStoppedHardwareBreakpoint(void)
{
    if (!g_pvStoppedAddr || !IsActiveHardwareBreakpoint(g_pvStoppedAddr))
        return;
    DmSetDataBreakpoint(g_pvStoppedAddr, DMBREAK_NONE, 1);
    g_fNeedRearmHwBps = TRUE;
}

static PVOID SoftBreakpointAddress(void)
{
    DWORD dwType = DMBREAK_NONE;
    ULONG_PTR addr;

    if (!g_pvStoppedAddr)
        return NULL;
    if (SUCCEEDED(DmIsBreakpoint(g_pvStoppedAddr, &dwType)) && dwType == DMBREAK_FIXED)
        return g_pvStoppedAddr;
    addr = (ULONG_PTR)g_pvStoppedAddr;
    if (addr > 0 &&
        SUCCEEDED(DmIsBreakpoint((PVOID)(addr - 1), &dwType)) && dwType == DMBREAK_FIXED)
        return (PVOID)(addr - 1);
    return NULL;
}

static BOOL IsStoppedAtSoftwareBreakpoint(void)
{
    return SoftBreakpointAddress() != NULL;
}

static void HaltAfterStepFailure(void)
{
    if (SUCCEEDED(DmStop())) {
        g_fThreadStopped = TRUE;
        g_fLaunchStopped = TRUE;
        g_dwExecState = DMN_EXEC_STOP;
    }
}

static HRESULT InstallBreakpointFileLine(LPCSTR szFile, DWORD dwLine, PVOID *ppvOut)
{
    HRESULT hr;
    PVOID pvAddr = NULL;

    hr = SymbolsLineToAddress(szFile, dwLine, &pvAddr);
    if (FAILED(hr))
        return hr;
    if (ppvOut)
        *ppvOut = pvAddr;
    hr = InstallBreakpointAtEx(pvAddr, szFile, dwLine);
    return hr;
}

static HRESULT ApplyPendingBreakpoints(void)
{
    int i;
    HRESULT hr = S_OK;
    HRESULT hrOne;

    for (i = 0; i < g_cPendingBp; ++i) {
        hrOne = InstallBreakpointFileLine(g_rgPendingBp[i].file, g_rgPendingBp[i].line, NULL);
        if (FAILED(hrOne))
            hr = hrOne;
    }
    g_cPendingBp = 0;
    return hr;
}

static void SetLaunchBaseFromTitle(LPCSTR szTitle)
{
    char sz[MAX_PATH];
    strncpy(sz, szTitle, sizeof sz - 1);
    sz[sizeof sz - 1] = 0;
    strncpy(g_szLaunchBase, sz, sizeof g_szLaunchBase - 1);
    g_szLaunchBase[sizeof g_szLaunchBase - 1] = 0;
    {
        char *dot = strrchr(g_szLaunchBase, '.');
        if (dot)
            *dot = 0;
    }
    _strlwr(g_szLaunchBase);
}

static BOOL ModuleMatchesLaunchTitle(LPCSTR szName)
{
    char buf[MAX_PATH];
    const char *base;
    if (!g_szLaunchBase[0] || !szName)
        return FALSE;
    strncpy(buf, szName, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    _strlwr(buf);
    base = strrchr(buf, '\\');
    if (!base)
        base = strrchr(buf, '/');
    base = base ? base + 1 : buf;
    return strstr(base, g_szLaunchBase) != NULL;
}

void BridgeEmit(const char *szJson)
{
    EnterCriticalSection(&g_csEmit);
    fputs(szJson, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    LeaveCriticalSection(&g_csEmit);
}

void BridgeLog(const char *szFmt, ...)
{
    va_list ap;
    va_start(ap, szFmt);
    vfprintf(stderr, szFmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void EmitResult(int nId, BOOL fOk, const char *szExtra)
{
    char buf[BRIDGE_LINE_MAX];
    if (szExtra)
        sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":%s,%s}", nId, fOk ? "true" : "false", szExtra);
    else
        sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":%s}", nId, fOk ? "true" : "false");
    BridgeEmit(buf);
}

static void EmitEvent(const char *szName, const char *szFields)
{
    char buf[BRIDGE_LINE_MAX];
    if (szFields)
        sprintf(buf, "{\"type\":\"event\",\"event\":\"%s\",%s}", szName, szFields);
    else
        sprintf(buf, "{\"type\":\"event\",\"event\":\"%s\"}", szName);
    BridgeEmit(buf);
}

static const char *NotifyName(ULONG n)
{
    switch (n) {
    case DM_BREAK: return "break";
    case DM_DATABREAK: return "break";
    case DM_SINGLESTEP: return "singlestep";
    case DM_CREATETHREAD: return "createthread";
    case DM_MODLOAD: return "modload";
    case DM_DEBUGSTR: return "debugstr";
    case DM_EXCEPTION: return "exception";
    case DM_ASSERT: return "assert";
    case DM_RIP: return "rip";
    case DM_EXEC: return "exec";
    default: return "notify";
    }
}

#define DM_NOTIFY_CODE(n) ((n) & DM_NOTIFICATIONMASK)

static DWORD __stdcall SessionNotify(ULONG dwNotification, DWORD_PTR dwParam)
{
    char fields[BRIDGE_LINE_MAX];
    ULONG code = DM_NOTIFY_CODE(dwNotification);

    switch (code) {
    case DM_CREATETHREAD:
        {
            PDMN_CREATETHREAD pct = (PDMN_CREATETHREAD)dwParam;
            if (g_fAwaitingTitleThread && !g_fSeenTitleMod)
                break;
            if (!g_dwMainThread)
                g_dwMainThread = pct->ThreadId;
            g_dwStoppedThread = pct->ThreadId;
            sprintf(fields, "\"threadId\":%lu,\"startAddress\":\"0x%p\",\"stopped\":%s",
                pct->ThreadId, pct->StartAddress, (dwNotification & DM_STOPTHREAD) ? "true" : "false");
            EmitEvent(NotifyName(code), fields);
            if (dwNotification & DM_STOPTHREAD) {
                g_fLaunchStopped = TRUE;
                g_fThreadStopped = TRUE;
            }
            if (g_fAwaitingTitleThread && g_fSeenTitleMod && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
        }
        break;
    case DM_BREAK:
    case DM_DATABREAK:
    case DM_SINGLESTEP:
        {
            DWORD dwThreadId;
            PVOID pvAddr;
            if (code == DM_DATABREAK) {
                PDMN_DATABREAK pdbr = (PDMN_DATABREAK)dwParam;
                dwThreadId = pdbr->ThreadId;
                pvAddr = pdbr->Address;
            } else {
                PDMN_BREAK pbr = (PDMN_BREAK)dwParam;
                dwThreadId = pbr->ThreadId;
                pvAddr = pbr->Address;
            }
            if (dwNotification & DM_STOPTHREAD) {
                g_fLaunchStopped = TRUE;
                g_fThreadStopped = TRUE;
            }
            g_dwStoppedThread = dwThreadId;
            g_pvStoppedAddr = pvAddr;
            sprintf(fields, "\"threadId\":%lu,\"address\":\"0x%p\",\"stopped\":%s",
                dwThreadId, pvAddr, (dwNotification & DM_STOPTHREAD) ? "true" : "false");
            if (!g_fStepActive && !g_fAutoRunResume)
                EmitEvent(NotifyName(code), fields);
            if (g_fAwaitingTitleThread && g_fSeenTitleMod && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
            else if (!g_fAwaitingTitleThread && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
        }
        break;
    case DM_MODLOAD:
        {
            PDMN_MODLOAD pm = (PDMN_MODLOAD)dwParam;
            PVOID pvOldBase = g_pvModuleBase;
            BOOL fTitleMod = ModuleMatchesLaunchTitle(pm->Name);

            if (fTitleMod || (g_fAwaitingTitleThread && !g_szLaunchBase[0])) {
                g_pvModuleBase = pm->BaseAddress;
                SymbolsSetModuleBase(g_pvModuleBase);
            } else if (!g_pvModuleBase) {
                g_pvModuleBase = pm->BaseAddress;
                SymbolsSetModuleBase(g_pvModuleBase);
            }
            {
                char evBuf[BRIDGE_LINE_MAX];
                int pos = sprintf(evBuf, "{\"type\":\"event\",\"event\":\"modload\",\"name\":");
                JsonAppendEscaped(evBuf, BRIDGE_LINE_MAX, &pos, pm->Name);
                if (pos < BRIDGE_LINE_MAX - 64)
                    pos += sprintf(evBuf + pos, ",\"baseAddress\":\"0x%p\",\"size\":%lu}", pm->BaseAddress, pm->Size);
                BridgeEmit(evBuf);
            }
            if (g_fAwaitingTitleThread && fTitleMod) {
                g_fSeenTitleMod = TRUE;
                if (g_fAutoRunLaunch && g_hBreakEvent)
                    SetEvent(g_hBreakEvent);
            }
            if (g_pvModuleBase && g_pvModuleBase != pvOldBase) {
                BridgeLog("Module base updated %p -> %p (%s)", pvOldBase, g_pvModuleBase, pm->Name);
                if (g_cPendingBp > 0)
                    ApplyPendingBreakpoints();
                if (g_cActiveBp > 0)
                    ReapplyActiveBreakpoints();
            }
        }
        break;
    case DM_DEBUGSTR:
        {
            PDMN_DEBUGSTR pds = (PDMN_DEBUGSTR)dwParam;
            char evBuf[BRIDGE_LINE_MAX];
            char snippet[512];
            int len = (int)pds->Length;
            int pos;

            if (len >= (int)sizeof snippet)
                len = (int)sizeof snippet - 1;
            memcpy(snippet, pds->String, len);
            snippet[len] = 0;
            pos = sprintf(evBuf, "{\"type\":\"event\",\"event\":\"debugstr\",\"threadId\":%lu,\"text\":", pds->ThreadId);
            JsonAppendEscaped(evBuf, BRIDGE_LINE_MAX, &pos, snippet);
            if (pos < BRIDGE_LINE_MAX - 2) {
                evBuf[pos++] = '}';
                evBuf[pos] = 0;
                BridgeEmit(evBuf);
            }
        }
        break;
    case DM_RIP:
        EmitEvent("terminated", "\"reason\":\"rip\"");
        break;
    case DM_EXEC:
        g_dwExecState = (DWORD)dwParam;
        sprintf(fields, "\"state\":%lu", (DWORD)dwParam);
        EmitEvent(NotifyName(code), fields);
        if (dwParam == DMN_EXEC_START && g_fNeedRearmHwBps)
            RearmHardwareBreakpoints();
        if (dwParam == DMN_EXEC_PENDING && g_hExecPendingEvent)
            SetEvent(g_hExecPendingEvent);
        if (g_fAwaitingTitleThread && g_fSeenTitleMod && !g_dwMainThread &&
            dwParam == DMN_EXEC_REBOOT && g_hBreakEvent)
            SetEvent(g_hBreakEvent);
        break;
    default:
        sprintf(fields, "\"code\":%lu,\"stopped\":%s", code,
            (dwNotification & DM_STOPTHREAD) ? "true" : "false");
        if (!g_fAutoRunResume)
            EmitEvent(NotifyName(code), fields);
        if (code == DM_EXCEPTION && (dwNotification & DM_STOPTHREAD)) {
            PDMN_EXCEPTION pex = (PDMN_EXCEPTION)dwParam;
            g_dwStoppedThread = pex->ThreadId;
            g_pvStoppedAddr = pex->Address;
            g_fThreadStopped = TRUE;
            if (g_hBreakEvent)
                SetEvent(g_hBreakEvent);
        }
        break;
    }
    return DM_NOTIFICATIONMASK;
}

static HRESULT EnsureSession(void)
{
    HRESULT hr;
    if (g_psess)
        return S_OK;
    hr = DmOpenNotificationSession(DM_PERSISTENT, &g_psess);
    if (FAILED(hr))
        return hr;
    hr = DmNotify(g_psess, DM_CREATETHREAD, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_BREAK, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_DATABREAK, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_SINGLESTEP, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_MODLOAD, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_DEBUGSTR, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_EXCEPTION, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_ASSERT, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_RIP, SessionNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(g_psess, DM_EXEC, SessionNotify);
    return hr;
}

HRESULT SessionInit(void)
{
    InitializeCriticalSection(&g_csEmit);
    g_hBreakEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hBreakEvent)
        return HRESULT_FROM_WIN32(GetLastError());
    g_hExecPendingEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hExecPendingEvent)
        return HRESULT_FROM_WIN32(GetLastError());
    DmUseSharedConnection(TRUE);
    BridgeEmit("{\"type\":\"event\",\"event\":\"ready\"}");
    return S_OK;
}

BOOL SessionIsActive(void)
{
    return !g_fShutdown;
}

static HRESULT RebootToDashboard(void)
{
    HRESULT hr;

    DmStopOn(DMSTOP_CREATETHREAD | DMSTOP_FCE | DMSTOP_DEBUGSTR, FALSE);
    (void)DmStop();
    if (g_fConnected) {
        DmConnectDebugger(FALSE);
        g_fConnected = FALSE;
    }
    ClearActiveBreakpoints();
    g_cPendingBp = 0;
    g_fLaunched = FALSE;
    g_fAwaitingTitleThread = FALSE;
    g_fSeenTitleMod = FALSE;
    g_fLaunchStopped = FALSE;
    g_fThreadStopped = FALSE;
    g_dwMainThread = 0;
    g_dwStoppedThread = 0;
    g_pvStoppedAddr = NULL;
    BridgeLog("Rebooting to dashboard (WARM, debug enabled)");
    hr = DmReboot(DMBOOT_WARM);
    if (FAILED(hr))
        BridgeLog("Dashboard reboot failed: 0x%08lX", (unsigned long)hr);
    return hr;
}

void SessionShutdown(void)
{
    g_fShutdown = TRUE;
    if (g_fConnected) {
        DmConnectDebugger(FALSE);
        g_fConnected = FALSE;
    }
    if (g_psess) {
        DmCloseNotificationSession(g_psess);
        g_psess = NULL;
    }
    SymbolsUnload();
    DmUseSharedConnection(FALSE);
    if (g_hBreakEvent) {
        CloseHandle(g_hBreakEvent);
        g_hBreakEvent = NULL;
    }
    if (g_hExecPendingEvent) {
        CloseHandle(g_hExecPendingEvent);
        g_hExecPendingEvent = NULL;
    }
    DeleteCriticalSection(&g_csEmit);
}

/* RXDKNeighborhood uses "reboot warm wait" (not stop) before debug launch. */
static HRESULT EnsureLaunchReboot(BOOL fReboot)
{
    HRESULT hr;

    if (!fReboot) {
        return S_OK;
    }

    if (g_dwExecState == DMN_EXEC_PENDING) {
        return S_OK;
    }

    ResetEvent(g_hExecPendingEvent);
    g_dwExecState = DMN_EXEC_REBOOT;
    BridgeLog("Rebooting Xbox (WARM|WAIT)...");
    hr = DmReboot(DMBOOT_WARM | DMBOOT_WAIT);
    if (FAILED(hr))
        return hr;
    if (WaitForSingleObject(g_hExecPendingEvent, REBOOT_TIMEOUT_MS) != WAIT_OBJECT_0)
        return XBDM_CONNECTIONLOST;
    if (g_dwExecState != DMN_EXEC_PENDING)
        return E_FAIL;
    return S_OK;
}

/* Debug launch needs pending-exec (see samples/xbox-launch/main.c). */
static HRESULT EnsurePendingExec(void)
{
    HRESULT hr;

    if (g_dwExecState == DMN_EXEC_PENDING)
        return S_OK;

    ResetEvent(g_hExecPendingEvent);
    g_dwExecState = DMN_EXEC_REBOOT;
    BridgeLog("Rebooting Xbox to pending exec (STOP|WARM)...");
    hr = DmReboot(DMBOOT_STOP | DMBOOT_WARM);
    if (FAILED(hr))
        return hr;
    if (WaitForSingleObject(g_hExecPendingEvent, REBOOT_TIMEOUT_MS) != WAIT_OBJECT_0)
        return XBDM_CONNECTIONLOST;
    if (g_dwExecState != DMN_EXEC_PENDING)
        return E_FAIL;
    return S_OK;
}

static HRESULT RecycleNotificationSession(void)
{
    if (g_psess) {
        DmCloseNotificationSession(g_psess);
        g_psess = NULL;
    }
    DmUseSharedConnection(FALSE);
    DmUseSharedConnection(TRUE);
    return EnsureSession();
}

DWORD SessionGetMainThread(void) { return g_dwMainThread; }
PVOID SessionGetModuleBase(void) { return g_pvModuleBase; }

static void LogKitState(const char *szTag);
static BOOL IsThreadStoppedOnKit(DWORD dwThread);
static BOOL AnyThreadStopped(void);
static HRESULT RunUntilNotStopped(void);
static HRESULT ResumeStoppedThread(BOOL fException);

static void EnsureTrailingSlash(char *szDir, size_t cchDir)
{
    size_t n;

    if (!szDir || cchDir < 2)
        return;
    n = strlen(szDir);
    if (n > 0 && n + 2 < cchDir && szDir[n - 1] != '\\') {
        szDir[n] = '\\';
        szDir[n + 1] = 0;
    }
}

static void PickMainThreadFromList(void)
{
    DWORD rg[64];
    DWORD c = 64;

    if (g_dwMainThread)
        return;
    if (SUCCEEDED(DmGetThreadList(rg, &c)) && c > 0)
        g_dwMainThread = rg[0];
}

static void LogThreadSnapshot(const char *szTag, BOOL fResumeAfter)
{
    XBDM_CONTEXT ctx;
    char szFile[MAX_PATH], szFunc[256];
    DWORD dwLine;
    BOOL fHalted = FALSE;

    if (!g_dwMainThread)
        return;
    if (!IsThreadStoppedOnKit(g_dwMainThread)) {
        if (SUCCEEDED(DmHaltThread(g_dwMainThread)))
            fHalted = TRUE;
        Sleep(50);
    }
    if (!IsThreadStoppedOnKit(g_dwMainThread))
        return;
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (SUCCEEDED(DmGetThreadContext(g_dwMainThread, &ctx))) {
        BridgeLog("%s: EIP=0x%08lX", szTag, (unsigned long)ctx.Eip);
        if (SUCCEEDED(SymbolsAddressToLine((PVOID)(ULONG_PTR)ctx.Eip, szFile, sizeof szFile, &dwLine, szFunc, sizeof szFunc)))
            BridgeLog("%s: %s:%lu %s", szTag, szFile, (unsigned long)dwLine, szFunc[0] ? szFunc : "?");
    }
    if (fHalted && fResumeAfter) {
        if (SUCCEEDED(DmContinueThread(g_dwMainThread, FALSE)))
            DmGo();
    }
}

static HRESULT ClearBootWaitIfPending(void)
{
    HRESULT hr;

    if (g_dwExecState != DMN_EXEC_PENDING) {
        return S_OK;
    }
    BridgeLog("Kit DMN_EXEC_PENDING — brief debugger connect to clear boot wait");
    hr = DmConnectDebugger(TRUE);
    if (FAILED(hr)) {
        return hr;
    }
    g_fConnected = TRUE;
    DmConnectDebugger(FALSE);
    g_fConnected = FALSE;
    return S_OK;
}

static HRESULT CmdLaunch(const char *szJson, int nId)
{
    HRESULT hr;
    char szDir[MAX_PATH], szTitle[MAX_PATH], szCmd[MAX_PATH], szConsole[256], extra[128];
    BOOL fReboot = FALSE;
    BOOL fAutoRun = FALSE;
    DWORD dwTimeout = LAUNCH_TIMEOUT_MS;

    szCmd[0] = 0;
    if (!JsonGetString(szJson, "dir", szDir, sizeof szDir) ||
        !JsonGetString(szJson, "title", szTitle, sizeof szTitle)) {
        EmitResult(nId, FALSE, "\"error\":\"missing dir/title\"");
        return E_INVALIDARG;
    }
    JsonGetString(szJson, "cmdline", szCmd, sizeof szCmd);
    JsonGetBool(szJson, "reboot", &fReboot);
    JsonGetBool(szJson, "autoRun", &fAutoRun);
    JsonGetDword(szJson, "timeout", &dwTimeout);
    if (JsonGetString(szJson, "console", szConsole, sizeof szConsole))
        DmSetXboxName(szConsole);

    hr = EnsureSession();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"notify session\"");
        return hr;
    }

    if (fAutoRun) {
        if (!fReboot && g_dwExecState == DMN_EXEC_PENDING) {
            hr = S_OK;
        } else {
            hr = EnsureLaunchReboot(fReboot);
        }
    } else if (g_dwExecState != DMN_EXEC_PENDING) {
        hr = EnsurePendingExec();
    } else {
        hr = S_OK;
    }
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"pendingExec\"");
        return hr;
    }

    hr = RecycleNotificationSession();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"notifyRecycle\"");
        return hr;
    }

    EnsureTrailingSlash(szDir, sizeof szDir);

    if (fAutoRun) {
        /* Running with no breakpoints: avoid debugger/cmdline launch — D3D CreateDevice
         * can hang after InitHardware when the host debugger is connected. */
        BridgeLog("Launch autoRun: clean start (no debugger connect)");
        hr = ClearBootWaitIfPending();
        if (FAILED(hr)) {
            EmitResult(nId, FALSE, "\"error\":\"clearBootWait\"");
            return hr;
        }
        hr = DmSetTitle(szDir, szTitle, NULL);
        if (FAILED(hr)) {
            EmitResult(nId, FALSE, "\"error\":\"setTitle\"");
            return hr;
        }
        BridgeLog("SetTitle dir=%s title=%s cmdline=<none>", szDir, szTitle);

        g_dwMainThread = 0;
        g_pvModuleBase = NULL;
        g_cPendingBp = 0;
        ClearActiveBreakpoints();
        g_fAwaitingTitleThread = TRUE;
        g_fAutoRunLaunch = TRUE;
        g_fSeenTitleMod = FALSE;
        g_fLaunchStopped = FALSE;
        g_fThreadStopped = FALSE;
        SetLaunchBaseFromTitle(szTitle);
        ResetEvent(g_hBreakEvent);

        hr = DmGo();
        if (FAILED(hr)) {
            g_fAutoRunLaunch = FALSE;
            g_fAwaitingTitleThread = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"go\"");
            return hr;
        }
        if (WaitForSingleObject(g_hBreakEvent, dwTimeout) != WAIT_OBJECT_0) {
            g_fAutoRunLaunch = FALSE;
            g_fAwaitingTitleThread = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"launchTimeout\"");
            return E_FAIL;
        }
        g_fAutoRunLaunch = FALSE;
        g_fAwaitingTitleThread = FALSE;
        PickMainThreadFromList();
        g_fLaunched = TRUE;
        sprintf(extra, "\"threadId\":%lu,\"moduleBase\":\"0x%p\",\"running\":true",
            g_dwMainThread, g_pvModuleBase);
        BridgeLog("Launch autoRun: title running (clean)");
        EmitResult(nId, TRUE, extra);
        return S_OK;
    }

    /* Debug launch: match samples/xbox-launch — stop at entry, then connect debugger. */
    hr = DmSetTitle(szDir, szTitle, szCmd[0] ? szCmd : "");
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"setTitle\"");
        return hr;
    }
    BridgeLog("SetTitle dir=%s title=%s cmdline=%s", szDir, szTitle,
        szCmd[0] ? szCmd : "<empty-debug>");

    hr = DmSetInitialBreakpoint();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"initialBreakpoint\"");
        return hr;
    }

    hr = DmStopOn(DMSTOP_CREATETHREAD, TRUE);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"stopOn\"");
        return hr;
    }

    g_dwMainThread = 0;
    g_pvModuleBase = NULL;
    g_cPendingBp = 0;
    ClearActiveBreakpoints();
    g_fAwaitingTitleThread = TRUE;
    g_fSeenTitleMod = FALSE;
    g_fLaunchStopped = FALSE;
    g_fThreadStopped = FALSE;
    SetLaunchBaseFromTitle(szTitle);
    ResetEvent(g_hBreakEvent);

    hr = DmGo();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"go\"");
        return hr;
    }

    if (WaitForSingleObject(g_hBreakEvent, dwTimeout) != WAIT_OBJECT_0) {
        g_fAwaitingTitleThread = FALSE;
        EmitResult(nId, FALSE, "\"error\":\"launchTimeout\"");
        return E_FAIL;
    }
    g_fAwaitingTitleThread = FALSE;

    if (!g_dwMainThread) {
        EmitResult(nId, FALSE, "\"error\":\"titleRebooted\"");
        return E_FAIL;
    }
    if (!g_dwStoppedThread)
        g_dwStoppedThread = g_dwMainThread;

    if (!g_fLaunchStopped) {
        hr = DmStop();
        if (FAILED(hr))
            BridgeLog("DmStop after launch wait failed: 0x%08lX", (unsigned long)hr);
    }

    hr = DmConnectDebugger(TRUE);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"connectDebugger\"");
        return hr;
    }
    g_fConnected = TRUE;

    /* Nostop createthread; ignore first-chance exceptions in XAPI/CRT during startup. */
    hr = DmStopOn(DMSTOP_CREATETHREAD | DMSTOP_FCE | DMSTOP_DEBUGSTR, FALSE);
    if (FAILED(hr))
        BridgeLog("DmStopOn(FALSE) failed: 0x%08lX", (unsigned long)hr);

    g_fLaunched = TRUE;
    g_fThreadStopped = TRUE;
    g_fLaunchStopped = TRUE;

    hr = ApplyPendingBreakpoints();
    if (FAILED(hr))
        BridgeLog("ApplyPendingBreakpoints failed: 0x%08lX", (unsigned long)hr);

    LogKitState("launch stopped for debug");
    sprintf(extra, "\"threadId\":%lu,\"moduleBase\":\"0x%p\"", g_dwMainThread, g_pvModuleBase);
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdAttach(int nId)
{
    HRESULT hr = EnsureSession();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"notify session\"");
        return hr;
    }
    hr = DmConnectDebugger(TRUE);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"connectDebugger\"");
        return hr;
    }
    g_fConnected = TRUE;
    (void)DmStopOn(DMSTOP_CREATETHREAD | DMSTOP_FCE | DMSTOP_DEBUGSTR, FALSE);
    EmitResult(nId, TRUE, NULL);
    return S_OK;
}

static BOOL IsThreadStoppedOnKit(DWORD dwThread)
{
    DM_THREADSTOP dmts;

    if (!dwThread)
        return FALSE;
    return SUCCEEDED(DmIsThreadStopped(dwThread, &dmts));
}

static ULONG GetThreadStopReason(DWORD dwThread)
{
    DM_THREADSTOP dmts;

    if (!dwThread || !SUCCEEDED(DmIsThreadStopped(dwThread, &dmts)))
        return (ULONG)-1;
    return dmts.NotifiedReason;
}

static BOOL ShouldContinueAsException(DWORD dwThread)
{
    ULONG reason = GetThreadStopReason(dwThread);

    if (reason == (ULONG)-1)
        return FALSE;
    return reason == DM_BREAK || reason == DM_DATABREAK || reason == DM_SINGLESTEP;
}

static BOOL GetMainThreadEip(ULONG_PTR *pEip)
{
    XBDM_CONTEXT ctx;

    if (!g_dwMainThread || !pEip)
        return FALSE;
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (FAILED(DmGetThreadContext(g_dwMainThread, &ctx)))
        return FALSE;
    *pEip = ctx.Eip;
    return TRUE;
}

static BOOL IsSoftBreakpointAt(PVOID pvAddr)
{
    DWORD dwType = DMBREAK_NONE;

    if (!pvAddr)
        return FALSE;
    if (SUCCEEDED(DmIsBreakpoint(pvAddr, &dwType)) && dwType == DMBREAK_FIXED)
        return TRUE;
    return FALSE;
}

static BOOL StoppedAtActiveBreakpoint(void)
{
    ULONG_PTR addr;
    int i;

    if (!g_pvStoppedAddr)
        return FALSE;
    addr = (ULONG_PTR)g_pvStoppedAddr;
    for (i = 0; i < g_cActiveBp; ++i) {
        ULONG_PTR bp = (ULONG_PTR)g_rgActiveBp[i];
        if (bp == addr || bp + 1 == addr)
            return TRUE;
    }
    return IsSoftBreakpointAt(g_pvStoppedAddr) ||
        (addr > 0 && IsSoftBreakpointAt((PVOID)(addr - 1)));
}

static BOOL SyncStoppedStateFromKit(void)
{
    ULONG_PTR eip = 0;
    int i;

    if (!g_dwMainThread || !IsThreadStoppedOnKit(g_dwMainThread))
        return FALSE;
    if (!GetMainThreadEip(&eip) || !eip)
        return FALSE;

    g_dwStoppedThread = g_dwMainThread;
    g_fThreadStopped = TRUE;

    if (IsSoftBreakpointAt((PVOID)eip)) {
        g_pvStoppedAddr = (PVOID)eip;
        return TRUE;
    }
    if (eip > 0 && IsSoftBreakpointAt((PVOID)(eip - 1))) {
        g_pvStoppedAddr = (PVOID)(eip - 1);
        return TRUE;
    }
    for (i = 0; i < g_cActiveBp; ++i) {
        ULONG_PTR bp = (ULONG_PTR)g_rgActiveBp[i];
        if (bp == eip || bp + 1 == eip) {
            g_pvStoppedAddr = g_rgActiveBp[i];
            return TRUE;
        }
    }
    g_pvStoppedAddr = (PVOID)eip;
    return TRUE;
}

static void LogKitState(const char *szTag)
{
    DWORD rg[64];
    DWORD c = 64;
    DWORD i;
    DM_THREADSTOP dmts;
    XBDM_CONTEXT ctx;

    BridgeLog(
        "%s: main=%lu stopped=%lu base=%p lastAddr=%p exec=%lu flags(thread=%d launch=%d) activeBp=%d pendingBp=%d",
        szTag,
        g_dwMainThread,
        g_dwStoppedThread,
        g_pvModuleBase,
        g_pvStoppedAddr,
        (unsigned long)g_dwExecState,
        g_fThreadStopped,
        g_fLaunchStopped,
        g_cActiveBp,
        g_cPendingBp);
    if (g_dwMainThread && IsThreadStoppedOnKit(g_dwMainThread)) {
        ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SUCCEEDED(DmGetThreadContext(g_dwMainThread, &ctx)))
            BridgeLog("%s: main thread EIP=0x%08lX (stopped)", szTag, (unsigned long)ctx.Eip);
    }
    if (SUCCEEDED(DmGetThreadList(rg, &c))) {
        BridgeLog("%s: %lu thread(s) on kit", szTag, (unsigned long)c);
        for (i = 0; i < c; ++i) {
            if (SUCCEEDED(DmIsThreadStopped(rg[i], &dmts)))
                BridgeLog("%s: thread %lu STOPPED", szTag, rg[i]);
        }
    } else {
        BridgeLog("%s: DmGetThreadList failed", szTag);
    }
}

static HRESULT ResumeStoppedThread(BOOL fException)
{
    HRESULT hr;
    DWORD dwThread;
    BOOL fNeedContinue;

    dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    if (!dwThread)
        return S_OK;
    fNeedContinue = g_fThreadStopped || g_fLaunchStopped || IsThreadStoppedOnKit(dwThread);
    if (!fNeedContinue)
        return S_OK;
    hr = DmContinueThread(dwThread, fException);
    if (FAILED(hr))
        return hr;
    g_fThreadStopped = FALSE;
    g_fLaunchStopped = FALSE;
    return S_OK;
}

static BOOL AnyThreadStopped(void)
{
    DWORD rg[64];
    DWORD c = 64;
    DWORD i;

    if (!SUCCEEDED(DmGetThreadList(rg, &c)))
        return g_fThreadStopped || g_fLaunchStopped;
    for (i = 0; i < c; ++i) {
        if (IsThreadStoppedOnKit(rg[i]))
            return TRUE;
    }
    return FALSE;
}

static HRESULT RunUntilNotStopped(void)
{
    HRESULT hr = S_OK;
    int attempt;

    g_fAutoRunResume = TRUE;
    for (attempt = 0; attempt < 16 && AnyThreadStopped(); ++attempt) {
        BridgeLog("RunUntilNotStopped attempt %d (main reason=%lu)", attempt,
            (unsigned long)GetThreadStopReason(g_dwMainThread));
        hr = ResumeAllStoppedThreads();
        if (FAILED(hr))
            break;
        ResetEvent(g_hBreakEvent);
        hr = DmGo();
        if (FAILED(hr))
            break;
        if (g_dwMainThread && IsThreadStoppedOnKit(g_dwMainThread))
            DmResumeThread(g_dwMainThread);
        if (WaitForSingleObject(g_hBreakEvent, 2000) == WAIT_OBJECT_0) {
            BridgeLog("RunUntilNotStopped: break at %p thread=%lu", g_pvStoppedAddr, g_dwStoppedThread);
            if (g_pvModuleBase && g_pvStoppedAddr && IsTitleAddress(g_pvStoppedAddr))
                break;
            ResetEvent(g_hBreakEvent);
        } else if (!AnyThreadStopped()) {
            break;
        }
    }
    if (AnyThreadStopped()) {
        BridgeLog("RunUntilNotStopped: still stopped after %d attempts, forcing exception continue", attempt);
        hr = ResumeStoppedThread(TRUE);
        if (SUCCEEDED(hr))
            hr = DmGo();
        if (SUCCEEDED(hr) && g_dwMainThread)
            DmResumeThread(g_dwMainThread);
    }
    g_fAutoRunResume = FALSE;
    g_fThreadStopped = FALSE;
    g_fLaunchStopped = FALSE;
    LogKitState("RunUntilNotStopped done");
    return hr;
}

static HRESULT ResumeAllStoppedThreads(void)
{
    DWORD rg[64];
    DWORD c = 64;
    DWORD i;
    HRESULT hr = S_OK;
    HRESULT hrOne;
    int nContinued = 0;

    if (SUCCEEDED(DmGetThreadList(rg, &c))) {
        for (i = 0; i < c; ++i) {
            BOOL fException;

            if (!IsThreadStoppedOnKit(rg[i]))
                continue;
            fException = g_fAutoRunResume || ShouldContinueAsException(rg[i]) ||
                IsStoppedAtSoftwareBreakpoint();
            hrOne = DmContinueThread(rg[i], fException);
            if (FAILED(hrOne)) {
                BridgeLog("DmContinueThread(%lu,%s) failed: 0x%08lX reason=%lu",
                    rg[i], fException ? "EXCEPTION" : "", (unsigned long)hrOne,
                    (unsigned long)GetThreadStopReason(rg[i]));
            } else {
                BridgeLog("DmContinueThread(%lu,%s) ok reason=%lu",
                    rg[i], fException ? "EXCEPTION" : "",
                    (unsigned long)GetThreadStopReason(rg[i]));
                ++nContinued;
            }
            if (FAILED(hrOne))
                hr = hrOne;
        }
    }
    if (!nContinued) {
        hrOne = ResumeStoppedThread(FALSE);
        if (FAILED(hrOne))
            hr = hrOne;
    }
    g_fThreadStopped = FALSE;
    g_fLaunchStopped = FALSE;
    BridgeLog("ResumeAllStoppedThreads continued %d thread(s)", nContinued);
    return hr;
}

static BOOL TryGetCallReturnAddress(DWORD dwThread, PVOID *ppvRet)
{
    XBDM_CONTEXT ctx;
    BYTE code[16];
    DWORD cbRead = 0;
    ULONG_PTR eip;
    BYTE modrm;
    int mod, rm, reg, len;

    if (!ppvRet)
        return FALSE;
    *ppvRet = NULL;
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (FAILED(DmGetThreadContext(dwThread, &ctx)))
        return FALSE;
    eip = ctx.Eip;
    if (FAILED(DmGetMemory((LPBYTE)eip, sizeof code, code, &cbRead)) || cbRead < 2)
        return FALSE;

    if (code[0] == 0xE8) {
        if (cbRead < 5)
            return FALSE;
        *ppvRet = (PVOID)(eip + 5);
        return TRUE;
    }

    if (code[0] != 0xFF)
        return FALSE;
    modrm = code[1];
    reg = (modrm >> 3) & 7;
    if (reg != 2)
        return FALSE;
    mod = modrm >> 6;
    rm = modrm & 7;
    len = 2;
    if (mod == 3) {
        len = 2;
    } else if (mod == 0 && rm == 5) {
        len = 6;
    } else if (mod == 1) {
        len = rm == 4 ? 4 : 3;
    } else if (mod == 2) {
        len = rm == 4 ? 7 : 6;
    } else if (mod == 0 && rm == 4) {
        if (cbRead < 3)
            return FALSE;
        if ((code[2] & 7) == 5)
            len = 7;
        else
            len = 3;
    } else {
        len = 2;
    }
    if ((DWORD)len > cbRead)
        return FALSE;
    *ppvRet = (PVOID)(eip + len);
    return TRUE;
}

static BOOL IsTitleAddress(PVOID pvAddr)
{
    ULONG_PTR addr = (ULONG_PTR)pvAddr;
    ULONG_PTR base = (ULONG_PTR)g_pvModuleBase;
    /* Title image size from modload is typically ~0x46000 for this sample. */
    if (!base || addr < base || addr >= base + 0x50000)
        return FALSE;
    return TRUE;
}

static HRESULT CmdGo(int nId)
{
    HRESULT hr;
    BOOL fAtSoftBp = IsStoppedAtSoftwareBreakpoint();

    if (fAtSoftBp)
        hr = ResumeStoppedThread(TRUE);
    else
        hr = ResumeAllStoppedThreads();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"continueThread\"");
        return hr;
    }
    BypassStoppedHardwareBreakpoint();
    hr = DmGo();
    if (SUCCEEDED(hr))
        LogKitState("go");
    else
        BridgeLog("go: DmGo failed: 0x%08lX", (unsigned long)hr);
    EmitResult(nId, SUCCEEDED(hr), SUCCEEDED(hr) ? NULL : "\"error\":\"go\"");
    return hr;
}

static HRESULT LeaveTitleRunning(int nId)
{
    HRESULT hr;

    hr = ResumeAllStoppedThreads();
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"continueThread\"");
        return hr;
    }
    BypassStoppedHardwareBreakpoint();
    hr = DmGo();
    if (FAILED(hr)) {
        BridgeLog("LeaveTitleRunning: DmGo failed: 0x%08lX", (unsigned long)hr);
        EmitResult(nId, FALSE, "\"error\":\"go\"");
        return hr;
    }
    if (AnyThreadStopped()) {
        if (SyncStoppedStateFromKit() && StoppedAtActiveBreakpoint()) {
            char extra[128];
            LogKitState("leaveRunning sync breakpoint");
            sprintf(extra, "\"threadId\":%lu,\"address\":\"0x%p\"", g_dwStoppedThread, g_pvStoppedAddr);
            EmitResult(nId, TRUE, extra);
            return S_OK;
        }
        LogKitState("leaveRunning still stopped");
        EmitResult(nId, FALSE, "\"error\":\"stillStopped\"");
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    LogKitState("leaveRunning");
    EmitResult(nId, TRUE, "\"running\":true");
    return S_OK;
}

static HRESULT CmdGoUser(int nId)
{
    HRESULT hr;
    int attempt;
    char extra[128];

    g_fAutoRunResume = TRUE;
    for (attempt = 0; attempt < 12; ++attempt) {
        hr = ResumeAllStoppedThreads();
        if (FAILED(hr)) {
            g_fAutoRunResume = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"continueThread\"");
            return hr;
        }
        ResetEvent(g_hBreakEvent);
        hr = DmGo();
        if (FAILED(hr)) {
            g_fAutoRunResume = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"go\"");
            return hr;
        }
        if (WaitForSingleObject(g_hBreakEvent, GOUSER_TIMEOUT_MS) != WAIT_OBJECT_0) {
            if (SyncStoppedStateFromKit() && StoppedAtActiveBreakpoint() &&
                IsTitleAddress(g_pvStoppedAddr)) {
                LogKitState("goUser sync stop after timeout");
                sprintf(extra, "\"threadId\":%lu,\"address\":\"0x%p\"", g_dwStoppedThread, g_pvStoppedAddr);
                g_fAutoRunResume = FALSE;
                EmitResult(nId, TRUE, extra);
                return S_OK;
            }
            BridgeLog("goUser: wait timed out; ensuring title is running");
            g_fAutoRunResume = FALSE;
            return LeaveTitleRunning(nId);
        }
        if (g_pvModuleBase && IsTitleAddress(g_pvStoppedAddr))
            break;
        if (SyncStoppedStateFromKit() && IsTitleAddress(g_pvStoppedAddr))
            break;
        /* CRT/XAPI stops before the title module base is known, or in system code. */
        BridgeLog("Skipping non-title break at %p (base=%p attempt=%d)",
            g_pvStoppedAddr, g_pvModuleBase, attempt);
    }
    if (g_pvModuleBase && g_pvStoppedAddr && IsTitleAddress(g_pvStoppedAddr)) {
        LogKitState("goUser hit breakpoint");
        sprintf(extra, "\"threadId\":%lu,\"address\":\"0x%p\"", g_dwStoppedThread, g_pvStoppedAddr);
        g_fAutoRunResume = FALSE;
        EmitResult(nId, TRUE, extra);
        return S_OK;
    }
    BridgeLog("goUser: no title breakpoint; leaving title running");
    g_fAutoRunResume = FALSE;
    return LeaveTitleRunning(nId);
}

static HRESULT CmdWaitBreak(const char *szJson, int nId)
{
    DWORD dwTimeout = 30000;
    DWORD dwWait;
    char extra[128];

    JsonGetDword(szJson, "timeout", &dwTimeout);
    ResetEvent(g_hBreakEvent);
    dwWait = WaitForSingleObject(g_hBreakEvent, dwTimeout);
    if (dwWait != WAIT_OBJECT_0) {
        EmitResult(nId, FALSE, "\"error\":\"waitTimeout\"");
        return E_FAIL;
    }
    sprintf(extra, "\"threadId\":%lu,\"address\":\"0x%p\"", g_dwStoppedThread, g_pvStoppedAddr);
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdStop(int nId)
{
    HRESULT hr = DmStop();
    EmitResult(nId, SUCCEEDED(hr), NULL);
    return hr;
}

static HRESULT CmdStep(const char *szJson, int nId)
{
    HRESULT hr;
    DWORD dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    XBDM_CONTEXT ctx;
    PVOID pvSoftBp = NULL;
    PVOID pvStepOverRet = NULL;
    BOOL fRemovedSoftBp = FALSE;
    BOOL fTempStepOverBp = FALSE;
    BOOL fOver = FALSE;

    JsonGetDword(szJson, "threadId", &dwThread);
    JsonGetBool(szJson, "over", &fOver);
    g_fStepActive = TRUE;

    /* Stepping from a soft INT3 stop: EXCEPTION continue runs ResumeAfterBreakpoint,
     * then FBreakTrace swallows the resulting SINGLESTEP notify (dmbreak.c). Remove
     * the soft BP, set TF ourselves, and reinstall after the step completes. */
    pvSoftBp = SoftBreakpointAddress();
    if (pvSoftBp) {
        hr = DmRemoveBreakpoint(pvSoftBp);
        if (FAILED(hr)) {
            g_fStepActive = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"removeBreakpoint\"");
            return hr;
        }
        RemoveActiveBreakpointAt(pvSoftBp);
        fRemovedSoftBp = TRUE;
    }

    if (fOver && TryGetCallReturnAddress(dwThread, &pvStepOverRet)) {
        hr = InstallBreakpointAt(pvStepOverRet);
        if (FAILED(hr)) {
            if (fRemovedSoftBp)
                InstallBreakpointAt(pvSoftBp);
            g_fStepActive = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"installBreakpoint\"");
            return hr;
        }
        fTempStepOverBp = TRUE;
        BridgeLog("Step over call: temp BP at %p", pvStepOverRet);
    } else {
        ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        hr = DmGetThreadContext(dwThread, &ctx);
        if (FAILED(hr)) {
            if (fRemovedSoftBp)
                InstallBreakpointAt(pvSoftBp);
            g_fStepActive = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"getContext\"");
            return hr;
        }
        ctx.EFlags |= 0x100; /* TF */
        hr = DmSetThreadContext(dwThread, &ctx);
        if (FAILED(hr)) {
            if (fRemovedSoftBp)
                InstallBreakpointAt(pvSoftBp);
            g_fStepActive = FALSE;
            EmitResult(nId, FALSE, "\"error\":\"setContext\"");
            return hr;
        }
    }

    ResetEvent(g_hBreakEvent);
    hr = ResumeStoppedThread(FALSE);
    if (FAILED(hr)) {
        if (fTempStepOverBp && pvStepOverRet) {
            DmRemoveBreakpoint(pvStepOverRet);
            RemoveActiveBreakpointAt(pvStepOverRet);
        }
        if (fRemovedSoftBp)
            InstallBreakpointAt(pvSoftBp);
        g_fStepActive = FALSE;
        EmitResult(nId, FALSE, "\"error\":\"continueThread\"");
        return hr;
    }
    BypassStoppedHardwareBreakpoint();
    hr = DmGo();
    if (FAILED(hr)) {
        HaltAfterStepFailure();
        if (fTempStepOverBp && pvStepOverRet) {
            DmRemoveBreakpoint(pvStepOverRet);
            RemoveActiveBreakpointAt(pvStepOverRet);
        }
        if (fRemovedSoftBp)
            InstallBreakpointAt(pvSoftBp);
        g_fStepActive = FALSE;
        EmitResult(nId, FALSE, "\"error\":\"go\"");
        return hr;
    }
    if (WaitForSingleObject(g_hBreakEvent, 30000) != WAIT_OBJECT_0) {
        BridgeLog("Step timeout thread=%lu lastAddr=%p softBp=%p over=%d", dwThread, g_pvStoppedAddr, pvSoftBp, fOver);
        HaltAfterStepFailure();
        if (fTempStepOverBp && pvStepOverRet) {
            DmRemoveBreakpoint(pvStepOverRet);
            RemoveActiveBreakpointAt(pvStepOverRet);
        }
        if (fRemovedSoftBp)
            InstallBreakpointAt(pvSoftBp);
        if (g_fNeedRearmHwBps)
            RearmHardwareBreakpoints();
        g_fStepActive = FALSE;
        EmitResult(nId, FALSE, "\"error\":\"stepTimeout\"");
        return E_FAIL;
    }
    if (fTempStepOverBp && pvStepOverRet) {
        DmRemoveBreakpoint(pvStepOverRet);
        RemoveActiveBreakpointAt(pvStepOverRet);
    }
    if (fRemovedSoftBp)
        InstallBreakpointAt(pvSoftBp);
    if (g_fNeedRearmHwBps)
        RearmHardwareBreakpoints();
    g_fThreadStopped = TRUE;
    g_fStepActive = FALSE;
    {
        char extra[128];
        sprintf(extra, "\"threadId\":%lu,\"address\":\"0x%p\"", g_dwStoppedThread, g_pvStoppedAddr);
        EmitResult(nId, TRUE, extra);
    }
    return S_OK;
}

static HRESULT CmdSymbolInfo(int nId)
{
    char diag[512];
    char extra[640];
    HRESULT hr = SymbolsDiag(diag, sizeof diag);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"symbolInfo\"");
        return hr;
    }
    sprintf(extra, "\"diag\":\"%s\",\"moduleBase\":\"0x%p\"", diag, g_pvModuleBase);
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdResolveLine(const char *szJson, int nId)
{
    char szFile[MAX_PATH];
    DWORD dwLine = 0;
    PVOID pvAddr = NULL;
    char extra[128];
    HRESULT hr;

    if (!JsonGetString(szJson, "file", szFile, sizeof szFile) ||
        !JsonGetDword(szJson, "line", &dwLine)) {
        EmitResult(nId, FALSE, "\"error\":\"missing file/line\"");
        return E_INVALIDARG;
    }
    hr = SymbolsResolveLine(szFile, dwLine, &pvAddr);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"resolveLine\"");
        return hr;
    }
    sprintf(extra, "\"address\":\"0x%p\",\"moduleBase\":\"0x%p\"", pvAddr, g_pvModuleBase);
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdSetBreakpoint(const char *szJson, int nId)
{
    HRESULT hr;
    PVOID pvAddr = NULL;
    char szFile[MAX_PATH];
    DWORD dwLine = 0;
    char extra[192];
    BOOL fQueue = TRUE;

    JsonGetBool(szJson, "queue", &fQueue);
    JsonGetString(szJson, "file", szFile, sizeof szFile);
    JsonGetDword(szJson, "line", &dwLine);

    if (JsonGetPtr(szJson, "address", &pvAddr)) {
        pvAddr = NormalizeBreakpointAddress(pvAddr);
        if (!IsKitBreakpointAddress(pvAddr)) {
            if (fQueue && szFile[0] && dwLine) {
                QueuePendingBreakpoint(szFile, dwLine);
                sprintf(extra, "\"address\":\"0x%p\",\"pending\":true", pvAddr);
                EmitResult(nId, TRUE, extra);
                return S_OK;
            }
            sprintf(extra, "\"address\":\"0x%p\"", pvAddr);
            EmitResult(nId, FALSE, "\"error\":\"badAddress\"");
            return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        }
        hr = InstallBreakpointAtEx(pvAddr, szFile[0] ? szFile : NULL, dwLine);
    } else if (szFile[0] && dwLine) {
        if (!g_pvModuleBase && fQueue) {
            QueuePendingBreakpoint(szFile, dwLine);
            hr = SymbolsResolveLine(szFile, dwLine, &pvAddr);
            if (FAILED(hr)) {
                EmitResult(nId, FALSE, "\"error\":\"resolveLine\"");
                return hr;
            }
            sprintf(extra, "\"address\":\"0x%p\",\"pending\":true", pvAddr);
            EmitResult(nId, TRUE, extra);
            return S_OK;
        }
        hr = InstallBreakpointFileLine(szFile, dwLine, &pvAddr);
    } else {
        EmitResult(nId, FALSE, "\"error\":\"missing address or file/line\"");
        return E_INVALIDARG;
    }

    if (pvAddr) {
        DWORD dwBpType = DMBREAK_NONE;
        BOOL fArmed = SUCCEEDED(hr) && SUCCEEDED(DmIsBreakpoint(pvAddr, &dwBpType)) &&
            dwBpType != DMBREAK_NONE;
        sprintf(extra, "\"address\":\"0x%p\",\"armed\":%s", pvAddr, fArmed ? "true" : "false");
    } else {
        strcpy(extra, "\"address\":\"0x00000000\",\"armed\":false");
    }
    if (FAILED(hr) && hr == E_OUTOFMEMORY)
        strcat(extra, ",\"error\":\"hwBpFull\"");
    else if (FAILED(hr) && hr == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS))
        strcat(extra, ",\"error\":\"badAddress\"");
    else if (FAILED(hr) && pvAddr)
        strcat(extra, ",\"error\":\"installBreakpoint\"");
    else if (FAILED(hr))
        strcat(extra, ",\"error\":\"resolveLine\"");
    EmitResult(nId, SUCCEEDED(hr), extra);
    return hr;
}

static HRESULT CmdClearBreakpoints(int nId)
{
    ClearActiveBreakpoints();
    g_cPendingBp = 0;
    EmitResult(nId, TRUE, NULL);
    return S_OK;
}

static HRESULT CmdRemoveBreakpoint(const char *szJson, int nId)
{
    PVOID pvAddr = NULL;
    DWORD dwType = 0;
    HRESULT hr;
    if (!JsonGetPtr(szJson, "address", &pvAddr)) {
        EmitResult(nId, FALSE, "\"error\":\"missing address\"");
        return E_INVALIDARG;
    }
    if (SUCCEEDED(DmIsBreakpoint(pvAddr, &dwType)) && dwType == DMBREAK_EXECUTE)
        hr = DmSetDataBreakpoint(pvAddr, DMBREAK_NONE, 1);
    else
        hr = DmRemoveBreakpoint(pvAddr);
    if (SUCCEEDED(hr))
        RemoveActiveBreakpointAt(pvAddr);
    EmitResult(nId, SUCCEEDED(hr), NULL);
    return hr;
}

static HRESULT CmdIsBreakpoint(const char *szJson, int nId)
{
    PVOID pvAddr = NULL;
    DWORD dwType = 0;
    HRESULT hr;
    char extra[64];

    if (!JsonGetPtr(szJson, "address", &pvAddr)) {
        EmitResult(nId, FALSE, "\"error\":\"missing address\"");
        return E_INVALIDARG;
    }
    hr = DmIsBreakpoint(pvAddr, &dwType);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"isBreakpoint\"");
        return hr;
    }
    sprintf(extra, "\"type\":%lu", (unsigned long)dwType);
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdGetMemory(const char *szJson, int nId)
{
    PVOID pvAddr = NULL;
    DWORD cb = 16;
    BYTE rgb[64];
    DWORD cbRet = 0;
    HRESULT hr;
    char buf[BRIDGE_LINE_MAX];
    char hex[160];
    int i, pos;

    if (!JsonGetPtr(szJson, "address", &pvAddr)) {
        EmitResult(nId, FALSE, "\"error\":\"missing address\"");
        return E_INVALIDARG;
    }
    JsonGetDword(szJson, "length", &cb);
    if (!cb || cb > sizeof rgb)
        cb = sizeof rgb;

    hr = DmGetMemory((LPCVOID)pvAddr, cb, rgb, &cbRet);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"getMemory\"");
        return hr;
    }

    pos = 0;
    for (i = 0; i < (int)cbRet && pos < (int)sizeof hex - 3; ++i)
        pos += sprintf(hex + pos, "%02x", rgb[i]);
    hex[pos] = 0;
    sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":true,\"bytes\":\"%s\",\"length\":%lu}",
        nId, hex, (unsigned long)cbRet);
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdGetThreads(int nId)
{
    HRESULT hr;
    DWORD rg[64];
    DWORD c = 64;
    char buf[BRIDGE_LINE_MAX];
    int i, pos;

    hr = DmGetThreadList(rg, &c);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"getThreadList\"");
        return hr;
    }
    pos = sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":true,\"threads\":[", nId);
    for (i = 0; i < (int)c && pos < (int)sizeof buf - 32; ++i) {
        if (i)
            buf[pos++] = ',';
        pos += sprintf(buf + pos, "%lu", rg[i]);
    }
    pos += sprintf(buf + pos, "]}");
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdGetStack(const char *szJson, int nId)
{
    DWORD dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    XBDM_CONTEXT ctx;
    HRESULT hr;
    char buf[BRIDGE_LINE_MAX];
    char szFile[MAX_PATH], szFunc[256];
    DWORD dwLine;
    int i, pos;
    DWORD64 addr;

    JsonGetDword(szJson, "threadId", &dwThread);
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    hr = DmGetThreadContext(dwThread, &ctx);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"getContext\"");
        return hr;
    }

    pos = sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":true,\"frames\":[", nId);
    addr = ctx.Eip;
    for (i = 0; i < 8; ++i) {
        if (pos >= BRIDGE_LINE_MAX - 256)
            break;
        if (i)
            buf[pos++] = ',';
        szFile[0] = szFunc[0] = 0;
        dwLine = 0;
        SymbolsAddressToLine((PVOID)(ULONG_PTR)addr, szFile, sizeof szFile, &dwLine, szFunc, sizeof szFunc);
        if (pos < BRIDGE_LINE_MAX - 64)
            pos += sprintf(buf + pos, "{\"index\":%d,\"address\":\"0x%I64x\",\"name\":", i, addr);
        JsonAppendEscaped(buf, BRIDGE_LINE_MAX, &pos, szFunc[0] ? szFunc : "???");
        if (pos < BRIDGE_LINE_MAX - 8) {
            pos += sprintf(buf + pos, ",\"file\":");
            JsonAppendEscaped(buf, BRIDGE_LINE_MAX, &pos, szFile);
            pos += sprintf(buf + pos, ",\"line\":%lu}", (unsigned long)dwLine);
        }
        /* walk stack frame: read ebp and return address */
        {
            DWORD cb;
            DWORD ebp = ctx.Ebp;
            DWORD ret;
            if (FAILED(DmGetMemory((LPCVOID)(ULONG_PTR)(ebp + 4), 4, &ret, &cb)) || cb != 4)
                break;
            if (FAILED(DmGetMemory((LPCVOID)(ULONG_PTR)ebp, 4, &ebp, &cb)) || cb != 4)
                break;
            addr = ret;
        }
    }
    sprintf(buf + pos, "]}");
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdGetVariables(const char *szJson, int nId)
{
    char szScope[32];
    DWORD dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    XBDM_CONTEXT ctx;
    HRESULT hr;
    char buf[8192];
    char varBuf[7680];
    int pos, varPos, nVars;

    if (!JsonGetString(szJson, "scope", szScope, sizeof szScope)) {
        EmitResult(nId, FALSE, "\"error\":\"missing scope\"");
        return E_INVALIDARG;
    }
    JsonGetDword(szJson, "threadId", &dwThread);
    ZeroMemory(&ctx, sizeof ctx);
    /* Globals come from the linker .map only — no thread context or kit memory reads. */
    if (_stricmp(szScope, "globals") != 0) {
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        hr = DmGetThreadContext(dwThread, &ctx);
        if (FAILED(hr)) {
            EmitResult(nId, FALSE, "\"error\":\"getContext\"");
            return hr;
        }
    }

    pos = sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":true,\"variables\":[", nId);
    if (_stricmp(szScope, "registers") == 0) {
        static const struct { LPCSTR name; DWORD value; } regs[] = {
            { "EAX", 0 }, { "EBX", 0 }, { "ECX", 0 }, { "EDX", 0 },
            { "ESI", 0 }, { "EDI", 0 }, { "EBP", 0 }, { "ESP", 0 },
            { "EIP", 0 }, { "EFLAGS", 0 },
        };
        int i;
        char szVal[32];
        for (i = 0; i < 10; ++i) {
            DWORD v;
            switch (i) {
            case 0: v = ctx.Eax; break;
            case 1: v = ctx.Ebx; break;
            case 2: v = ctx.Ecx; break;
            case 3: v = ctx.Edx; break;
            case 4: v = ctx.Esi; break;
            case 5: v = ctx.Edi; break;
            case 6: v = ctx.Ebp; break;
            case 7: v = ctx.Esp; break;
            case 8: v = ctx.Eip; break;
            default: v = ctx.EFlags; break;
            }
            if (i)
                buf[pos++] = ',';
            sprintf(szVal, "0x%08lX", (unsigned long)v);
            if (pos >= (int)sizeof buf - 96)
                break;
            buf[pos++] = '{';
            pos += sprintf(buf + pos, "\"name\":");
            JsonAppendEscaped(buf, sizeof buf, &pos, regs[i].name);
            buf[pos++] = ',';
            pos += sprintf(buf + pos, "\"value\":");
            JsonAppendEscaped(buf, sizeof buf, &pos, szVal);
            if (pos < (int)sizeof buf - 2)
                buf[pos++] = '}';
        }
    } else {
        varBuf[0] = 0;
        varPos = 0;
        nVars = 0;
        SymbolsEmitVariablesJson(szScope, &ctx, varBuf, sizeof varBuf, &nVars);
        if (varBuf[0] && pos + (int)strlen(varBuf) < (int)sizeof buf - 4)
            strcpy(buf + pos, varBuf), pos += (int)strlen(varBuf);
        (void)nVars;
    }
    sprintf(buf + pos, "]}");
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdEvaluate(const char *szJson, int nId)
{
    char szExpr[256];
    DWORD dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    XBDM_CONTEXT ctx;
    HRESULT hr;
    char szValue[128];
    char extra[320];

    if (!JsonGetString(szJson, "expression", szExpr, sizeof szExpr)) {
        EmitResult(nId, FALSE, "\"error\":\"missing expression\"");
        return E_INVALIDARG;
    }
    JsonGetDword(szJson, "threadId", &dwThread);
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    hr = DmGetThreadContext(dwThread, &ctx);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"getContext\"");
        return hr;
    }
    {
        char szErr[64];
        szErr[0] = 0;
        hr = SymbolsEvaluate(szExpr, &ctx, szValue, sizeof szValue, szErr, sizeof szErr);
        if (FAILED(hr)) {
            if (szErr[0])
                sprintf(extra, "\"error\":\"%s\"", szErr);
            else
                strcpy(extra, "\"error\":\"evaluate\"");
            EmitResult(nId, FALSE, extra);
            return hr;
        }
    }
    extra[0] = 0;
    {
        int pos = sprintf(extra, "\"value\":");
        JsonAppendEscaped(extra, sizeof extra, &pos, szValue);
    }
    EmitResult(nId, TRUE, extra);
    return S_OK;
}

static HRESULT CmdGetMembers(const char *szJson, int nId)
{
    char szBase[256];
    DWORD dwThread = g_dwStoppedThread ? g_dwStoppedThread : g_dwMainThread;
    XBDM_CONTEXT ctx;
    HRESULT hr;
    char buf[BRIDGE_LINE_MAX];
    char varBuf[BRIDGE_LINE_MAX - 128];
    int pos, nVars;

    if (!JsonGetString(szJson, "name", szBase, sizeof szBase)) {
        EmitResult(nId, FALSE, "\"error\":\"missing name\"");
        return E_INVALIDARG;
    }
    JsonGetDword(szJson, "threadId", &dwThread);
    ZeroMemory(&ctx, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    hr = DmGetThreadContext(dwThread, &ctx);
    if (FAILED(hr)) {
        EmitResult(nId, FALSE, "\"error\":\"getContext\"");
        return hr;
    }

    varBuf[0] = 0;
    nVars = 0;
    SymbolsEmitMembersJson(szBase, &ctx, varBuf, sizeof varBuf, &nVars);
    pos = sprintf(buf, "{\"type\":\"result\",\"id\":%d,\"success\":true,\"variables\":[", nId);
    if (varBuf[0])
        strcpy(buf + pos, varBuf), pos += (int)strlen(varBuf);
    (void)nVars;
    sprintf(buf + pos, "]}");
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdDiag(int nId)
{
    DWORD rg[64];
    DWORD c = 64;
    char buf[BRIDGE_LINE_MAX];
    int i, pos;
    XBDM_CONTEXT ctx;
    ULONG_PTR mainEip = 0;

    if (g_dwMainThread && IsThreadStoppedOnKit(g_dwMainThread)) {
        ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SUCCEEDED(DmGetThreadContext(g_dwMainThread, &ctx)))
            mainEip = ctx.Eip;
    }

    if (g_dwMainThread && IsThreadStoppedOnKit(g_dwMainThread) && !g_pvStoppedAddr)
        SyncStoppedStateFromKit();

    pos = sprintf(
        buf,
        "{\"type\":\"result\",\"id\":%d,\"success\":true,"
        "\"mainThread\":%lu,\"stoppedThread\":%lu,"
        "\"moduleBase\":\"0x%p\",\"stoppedAddr\":\"0x%p\","
        "\"mainEip\":\"0x%08lX\",\"execState\":%lu,"
        "\"threadStopped\":%s,\"launchStopped\":%s,"
        "\"mainStoppedOnKit\":%s,\"launched\":%s,\"connected\":%s,"
        "\"activeBreakpoints\":%d,\"pendingBreakpoints\":%d,"
        "\"threads\":[",
        nId,
        g_dwMainThread,
        g_dwStoppedThread,
        g_pvModuleBase,
        g_pvStoppedAddr,
        (unsigned long)mainEip,
        (unsigned long)g_dwExecState,
        g_fThreadStopped ? "true" : "false",
        g_fLaunchStopped ? "true" : "false",
        (g_dwMainThread && IsThreadStoppedOnKit(g_dwMainThread)) ? "true" : "false",
        g_fLaunched ? "true" : "false",
        g_fConnected ? "true" : "false",
        g_cActiveBp,
        g_cPendingBp);

    if (SUCCEEDED(DmGetThreadList(rg, &c))) {
        DM_THREADSTOP dmts;
        for (i = 0; i < (int)c && pos < (int)sizeof buf - 48; ++i) {
            BOOL fStop = SUCCEEDED(DmIsThreadStopped(rg[i], &dmts));
            if (i)
                buf[pos++] = ',';
            pos += sprintf(buf + pos, "{\"id\":%lu,\"stopped\":%s}", rg[i], fStop ? "true" : "false");
        }
    }
    pos += sprintf(buf + pos, "]}");
    if (pos >= (int)sizeof buf)
        pos = (int)sizeof buf - 1;
    buf[pos] = 0;
    BridgeEmit(buf);
    return S_OK;
}

static HRESULT CmdLoadSymbols(const char *szJson, int nId)
{
    char szExe[MAX_PATH], szPdb[MAX_PATH], szMap[MAX_PATH];
    HRESULT hr;
    if (!JsonGetString(szJson, "exe", szExe, sizeof szExe)) {
        EmitResult(nId, FALSE, "\"error\":\"missing exe\"");
        return E_INVALIDARG;
    }
    if (!JsonGetString(szJson, "pdb", szPdb, sizeof szPdb))
        strncpy(szPdb, szExe, sizeof szPdb - 1);
    if (!JsonGetString(szJson, "map", szMap, sizeof szMap))
        szMap[0] = 0;
    hr = SymbolsLoad(szExe, szPdb, szMap[0] ? szMap : NULL);
    EmitResult(nId, SUCCEEDED(hr), NULL);
    return hr;
}

HRESULT SessionHandleCommand(const char *szJson, int nId)
{
    char szCmd[64];

    if (!JsonGetString(szJson, "cmd", szCmd, sizeof szCmd)) {
        EmitResult(nId, FALSE, "\"error\":\"missing cmd\"");
        return E_INVALIDARG;
    }

    if (_stricmp(szCmd, "ping") == 0) {
        EmitResult(nId, TRUE, "\"pong\":true");
        return S_OK;
    }
    if (_stricmp(szCmd, "launch") == 0)
        return CmdLaunch(szJson, nId);
    if (_stricmp(szCmd, "attach") == 0)
        return CmdAttach(nId);
    if (_stricmp(szCmd, "go") == 0)
        return CmdGo(nId);
    if (_stricmp(szCmd, "goUser") == 0)
        return CmdGoUser(nId);
    if (_stricmp(szCmd, "waitBreak") == 0)
        return CmdWaitBreak(szJson, nId);
    if (_stricmp(szCmd, "stop") == 0)
        return CmdStop(nId);
    if (_stricmp(szCmd, "step") == 0)
        return CmdStep(szJson, nId);
    if (_stricmp(szCmd, "setBreakpoint") == 0)
        return CmdSetBreakpoint(szJson, nId);
    if (_stricmp(szCmd, "resolveLine") == 0)
        return CmdResolveLine(szJson, nId);
    if (_stricmp(szCmd, "symbolInfo") == 0)
        return CmdSymbolInfo(nId);
    if (_stricmp(szCmd, "clearBreakpoints") == 0)
        return CmdClearBreakpoints(nId);
    if (_stricmp(szCmd, "removeBreakpoint") == 0)
        return CmdRemoveBreakpoint(szJson, nId);
    if (_stricmp(szCmd, "isBreakpoint") == 0)
        return CmdIsBreakpoint(szJson, nId);
    if (_stricmp(szCmd, "getMemory") == 0)
        return CmdGetMemory(szJson, nId);
    if (_stricmp(szCmd, "getThreads") == 0)
        return CmdGetThreads(nId);
    if (_stricmp(szCmd, "getStack") == 0)
        return CmdGetStack(szJson, nId);
    if (_stricmp(szCmd, "getVariables") == 0)
        return CmdGetVariables(szJson, nId);
    if (_stricmp(szCmd, "evaluate") == 0)
        return CmdEvaluate(szJson, nId);
    if (_stricmp(szCmd, "getMembers") == 0)
        return CmdGetMembers(szJson, nId);
    if (_stricmp(szCmd, "loadSymbols") == 0)
        return CmdLoadSymbols(szJson, nId);
    if (_stricmp(szCmd, "diag") == 0)
        return CmdDiag(nId);
    if (_stricmp(szCmd, "shutdown") == 0) {
        BOOL fRebootDashboard = TRUE;
        JsonGetBool(szJson, "rebootDashboard", &fRebootDashboard);
        if (fRebootDashboard)
            (void)RebootToDashboard();
        EmitResult(nId, TRUE, NULL);
        SessionShutdown();
        return S_OK;
    }

    EmitResult(nId, FALSE, "\"error\":\"unknown cmd\"");
    return E_INVALIDARG;
}
