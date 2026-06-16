// xbdbgs static lib: EXE host init/teardown for notification support.
#include "precomp.h"

HINSTANCE hXboxdbg;

static BOOL g_fXboxdbgStaticInit;

void XboxdbgStaticInit(void)
{
    if (g_fXboxdbgStaticInit)
        return;
    hXboxdbg = GetModuleHandle(NULL);
    InitNotificationEvents();
    g_fXboxdbgStaticInit = TRUE;
}

static void __cdecl XboxdbgStaticShutdown(void)
{
    if (!g_fXboxdbgStaticInit)
        return;
    StopAllNotifications();
    DestroyNotificationEvents();
    g_fXboxdbgStaticInit = FALSE;
}

#pragma section(".CRT$XCU", read)
static void __cdecl XboxdbgStaticCtor(void)
{
    XboxdbgStaticInit();
    atexit(XboxdbgStaticShutdown);
}

//
// This object exists only to register the CRT (.CRT$XCU) initializer above, which
// initializes the DM critical sections / events (g_sci.csSharedConn, etc.) before
// any DM API is called.  Two things conspire to drop it when xbdbgs.lib is linked
// into an EXE:
//   1. The compiler (/O2) removes a *static*, unreferenced .CRT$XCU pointer (and
//      then the ctor it points at) outright -- so the initializer never even
//      reaches the .lib.
//   2. Even if kept, the linker would discard this otherwise-unreferenced object,
//      and /OPT:REF would fold the unreferenced pointer.
// Give the pointer external linkage so the compiler keeps it, and have xboxdbg.h
// force a /include of this exact symbol so the linker keeps it (and this object)
// too.  Every consumer of xboxdbg.h links xbdbgs.lib, so the symbol resolves.
//
__declspec(allocate(".CRT$XCU")) void (__cdecl *g_pXboxdbgStaticCtor)(void) = XboxdbgStaticCtor;
