/* Launch an Xbox title via xboxdbg and wait for the initial debug break. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxdbg.h>

#define LAUNCH_TIMEOUT_MS 120000
#define REBOOT_TIMEOUT_MS 120000
#define XBDM_NOXBOX 0x82DB0108u

static HANDLE g_hBreakEvent;
static HANDLE g_hExecPendingEvent;
static DWORD g_dwMainThread;
static DWORD g_dwExecState = DMN_EXEC_START;
static BOOL g_fAwaitingTitleThread;
static BOOL g_fSeenTitleMod;
static BOOL g_fLaunchStopped;
static char g_szLaunchBase[64];

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

static void PrintHr(const char *szWhat, HRESULT hr)
{
    char szErr[256];
    if (SUCCEEDED(DmTranslateErrorA(hr, szErr, sizeof szErr)))
        fprintf(stderr, "%s failed: %s (0x%08lX)\n", szWhat, szErr, (unsigned long)hr);
    else
        fprintf(stderr, "%s failed: 0x%08lX\n", szWhat, (unsigned long)hr);
}

static const char *NotifyName(ULONG n)
{
    switch (n) {
    case DM_BREAK: return "DM_BREAK";
    case DM_DEBUGSTR: return "DM_DEBUGSTR";
    case DM_EXEC: return "DM_EXEC";
    case DM_SINGLESTEP: return "DM_SINGLESTEP";
    case DM_MODLOAD: return "DM_MODLOAD";
    case DM_MODUNLOAD: return "DM_MODUNLOAD";
    case DM_CREATETHREAD: return "DM_CREATETHREAD";
    case DM_DESTROYTHREAD: return "DM_DESTROYTHREAD";
    case DM_EXCEPTION: return "DM_EXCEPTION";
    case DM_ASSERT: return "DM_ASSERT";
    case DM_DATABREAK: return "DM_DATABREAK";
    case DM_RIP: return "DM_RIP";
    case DM_SECTIONLOAD: return "DM_SECTIONLOAD";
    case DM_SECTIONUNLOAD: return "DM_SECTIONUNLOAD";
    case DM_FIBER: return "DM_FIBER";
    default: return "DM_?";
    }
}

#define DM_NOTIFY_CODE(n) ((n) & DM_NOTIFICATIONMASK)

static DWORD __stdcall LaunchNotify(ULONG dwNotification, DWORD_PTR dwParam)
{
    ULONG code = DM_NOTIFY_CODE(dwNotification);
    printf("notify: %s%s\n", NotifyName(code), (dwNotification & DM_STOPTHREAD) ? " (stop)" : "");

    switch (code) {
    case DM_CREATETHREAD:
        {
            PDMN_CREATETHREAD pct = (PDMN_CREATETHREAD)dwParam;
            if (g_fAwaitingTitleThread && !g_fSeenTitleMod)
                break;
            if (!g_dwMainThread)
                g_dwMainThread = pct->ThreadId;
            printf("  main thread id=%lu start=%p\n", pct->ThreadId, pct->StartAddress);
            if (dwNotification & DM_STOPTHREAD)
                g_fLaunchStopped = TRUE;
            if (g_fAwaitingTitleThread && g_fSeenTitleMod && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
            else if (!g_fAwaitingTitleThread && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
        }
        break;
    case DM_BREAK:
        {
            PDMN_BREAK pbr = (PDMN_BREAK)dwParam;
            printf("  break addr=%p thread=%lu\n", pbr->Address, pbr->ThreadId);
            if (dwNotification & DM_STOPTHREAD)
                g_fLaunchStopped = TRUE;
            if (g_fAwaitingTitleThread && g_fSeenTitleMod && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
            else if (!g_fAwaitingTitleThread && g_hBreakEvent)
                SetEvent(g_hBreakEvent);
        }
        break;
    case DM_MODLOAD:
        {
            PDMN_MODLOAD pm = (PDMN_MODLOAD)dwParam;
            printf("  module %s base=%p size=%lu\n", pm->Name, pm->BaseAddress, pm->Size);
            if (g_fAwaitingTitleThread && ModuleMatchesLaunchTitle(pm->Name))
                g_fSeenTitleMod = TRUE;
        }
        break;
    case DM_EXEC:
        g_dwExecState = (DWORD)dwParam;
        printf("  exec state=%lu\n", (DWORD)dwParam);
        if (dwParam == DMN_EXEC_PENDING && g_hExecPendingEvent)
            SetEvent(g_hExecPendingEvent);
        /* DMN_EXEC_STOP is normal when the debugger halts the title; only REBOOT means dashboard return. */
        if (g_fAwaitingTitleThread && g_fSeenTitleMod && !g_dwMainThread &&
            dwParam == DMN_EXEC_REBOOT && g_hBreakEvent)
            SetEvent(g_hBreakEvent);
        break;
    case DM_EXCEPTION:
        {
            PDMN_EXCEPTION pex = (PDMN_EXCEPTION)dwParam;
            printf("  exception code=0x%08lX thread=%lu addr=%p flags=0x%lX\n",
                (unsigned long)pex->Code, pex->ThreadId, pex->Address, (unsigned long)pex->Flags);
        }
        break;
    case DM_RIP:
        printf("  rip (title terminated)\n");
        if (g_fAwaitingTitleThread && g_fSeenTitleMod && !g_dwMainThread && g_hBreakEvent)
            SetEvent(g_hBreakEvent);
        break;
    case DM_DEBUGSTR:
        {
            PDMN_DEBUGSTR pds = (PDMN_DEBUGSTR)dwParam;
            printf("  debugstr[%lu]: %.*s\n", pds->ThreadId, pds->Length, pds->String);
        }
        break;
    default:
        printf("  notification code=%lu param=0x%08lX\n", dwNotification, (unsigned long)dwParam);
        break;
    }
    return DM_NOTIFICATIONMASK;
}

static void Usage(const char *szExe)
{
    fprintf(stderr,
        "usage: %s /dir xe:\\path /title game.xbe [/cmd args] [/x console] [/reboot] [/timeout ms]\n",
        szExe);
    exit(1);
}

static HRESULT OpenNotifySession(PDMN_SESSION *ppsess)
{
    HRESULT hr;
    hr = DmOpenNotificationSession(DM_PERSISTENT, ppsess);
    if (FAILED(hr))
        return hr;
    hr = DmNotify(*ppsess, DM_EXEC, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_CREATETHREAD, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_BREAK, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_MODLOAD, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_DEBUGSTR, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_EXCEPTION, LaunchNotify);
    if (SUCCEEDED(hr))
        hr = DmNotify(*ppsess, DM_RIP, LaunchNotify);
    return hr;
}

static HRESULT EnsurePendingExec(BOOL fReboot)
{
    HRESULT hr;

    if (!fReboot && g_dwExecState == DMN_EXEC_PENDING)
        return S_OK;

    ResetEvent(g_hExecPendingEvent);
    g_dwExecState = DMN_EXEC_REBOOT;
    printf("Rebooting Xbox to pending exec (STOP|WARM)...\n");
    hr = DmReboot(DMBOOT_STOP | DMBOOT_WARM);
    if (FAILED(hr))
        return hr;
    if (WaitForSingleObject(g_hExecPendingEvent, REBOOT_TIMEOUT_MS) != WAIT_OBJECT_0)
        return XBDM_CONNECTIONLOST;
    if (g_dwExecState != DMN_EXEC_PENDING)
        return E_FAIL;
    return S_OK;
}

int __cdecl main(int argc, char **argv)
{
    HRESULT hr;
    PDMN_SESSION psess = NULL;
    const char *szDir = NULL;
    const char *szTitle = NULL;
    const char *szCmdLine = "";
    const char *szConsole = NULL;
    BOOL fReboot = TRUE;
    DWORD dwTimeout = LAUNCH_TIMEOUT_MS;
    int i;

    for (i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "/?") == 0) || (strcmp(argv[i], "-?") == 0))
            Usage(argv[0]);
        else if (_stricmp(argv[i], "/dir") == 0 && i + 1 < argc)
            szDir = argv[++i];
        else if (_stricmp(argv[i], "/title") == 0 && i + 1 < argc)
            szTitle = argv[++i];
        else if (_stricmp(argv[i], "/cmd") == 0 && i + 1 < argc)
            szCmdLine = argv[++i];
        else if (_stricmp(argv[i], "/x") == 0 && i + 1 < argc)
            szConsole = argv[++i];
        else if (_stricmp(argv[i], "/reboot") == 0)
            fReboot = TRUE;
        else if (_stricmp(argv[i], "/timeout") == 0 && i + 1 < argc)
            dwTimeout = (DWORD)strtoul(argv[++i], NULL, 10);
        else
            Usage(argv[0]);
    }

    if (!szDir || !szTitle)
        Usage(argv[0]);

    DmUseSharedConnection(TRUE);

    if (szConsole) {
        hr = DmSetXboxName(szConsole);
        if (FAILED(hr)) {
            PrintHr("DmSetXboxName", hr);
            return 1;
        }
    }

    g_hBreakEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hBreakEvent) {
        fprintf(stderr, "CreateEvent failed\n");
        return 1;
    }
    g_hExecPendingEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hExecPendingEvent) {
        fprintf(stderr, "CreateEvent failed\n");
        return 1;
    }

    hr = OpenNotifySession(&psess);
    if (FAILED(hr)) {
        if (hr == (HRESULT)XBDM_NOXBOX) {
            fprintf(stderr, "No Xbox console configured (XBDM_NOXBOX).\n");
            return 2;
        }
        PrintHr("DmOpenNotificationSession", hr);
        return 1;
    }

    if (fReboot || g_dwExecState != DMN_EXEC_PENDING) {
        hr = EnsurePendingExec(TRUE);
        if (FAILED(hr)) {
            PrintHr("EnsurePendingExec", hr);
            DmCloseNotificationSession(psess);
            return 1;
        }
        DmCloseNotificationSession(psess);
        psess = NULL;
        DmUseSharedConnection(FALSE);
        DmUseSharedConnection(TRUE);
        hr = OpenNotifySession(&psess);
        if (FAILED(hr)) {
            PrintHr("DmOpenNotificationSession(recycle)", hr);
            return 1;
        }
    }

    printf("SetTitle dir=%s title=%s\n", szDir, szTitle);
    hr = DmSetTitle(szDir, szTitle, szCmdLine);
    if (FAILED(hr)) {
        PrintHr("DmSetTitle", hr);
        DmCloseNotificationSession(psess);
        return 1;
    }

    hr = DmSetInitialBreakpoint();
    if (FAILED(hr)) {
        PrintHr("DmSetInitialBreakpoint", hr);
        DmCloseNotificationSession(psess);
        return 1;
    }

    hr = DmStopOn(DMSTOP_CREATETHREAD, TRUE);
    if (FAILED(hr)) {
        PrintHr("DmStopOn", hr);
        DmCloseNotificationSession(psess);
        return 1;
    }

    g_dwMainThread = 0;
    g_fAwaitingTitleThread = TRUE;
    g_fSeenTitleMod = FALSE;
    g_fLaunchStopped = FALSE;
    SetLaunchBaseFromTitle(szTitle);
    ResetEvent(g_hBreakEvent);

    hr = DmGo();
    if (FAILED(hr)) {
        PrintHr("DmGo", hr);
        DmCloseNotificationSession(psess);
        return 1;
    }

    printf("Waiting up to %lu ms for initial break...\n", dwTimeout);
    if (WaitForSingleObject(g_hBreakEvent, dwTimeout) != WAIT_OBJECT_0) {
        fprintf(stderr, "Timed out waiting for initial stop (createthread/break).\n");
        DmCloseNotificationSession(psess);
        return 1;
    }
    g_fAwaitingTitleThread = FALSE;

    if (!g_dwMainThread) {
        fprintf(stderr, "Title rebooted or crashed before main thread (returned to dashboard).\n");
        DmCloseNotificationSession(psess);
        return 1;
    }

    if (!g_fLaunchStopped) {
        hr = DmStop();
        if (FAILED(hr)) {
            PrintHr("DmStop", hr);
            DmCloseNotificationSession(psess);
            return 1;
        }
    }

    hr = DmConnectDebugger(TRUE);
    if (FAILED(hr)) {
        PrintHr("DmConnectDebugger", hr);
        DmCloseNotificationSession(psess);
        return 1;
    }

    printf("OK: stopped at entry (thread=%lu). Debugger connected.\n", g_dwMainThread);
    DmCloseNotificationSession(psess);
    DmUseSharedConnection(FALSE);
    return 0;
}
