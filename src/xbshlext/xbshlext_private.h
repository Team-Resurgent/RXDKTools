/* Minimal private shell definitions needed by xbshlext (extracted from leak sdkp headers). */
#ifndef __XBSHLEXT_PRIVATE_H__
#define __XBSHLEXT_PRIVATE_H__

#include <windows.h>
#include <propidl.h>

#define PID_STG_DIRECTORY               ((PROPID) 0x00000002)
#define PID_STG_STORAGETYPE             ((PROPID) 0x00000004)
#define PID_STG_NAME                    ((PROPID) 0x0000000a)
#define PID_STG_SIZE                    ((PROPID) 0x0000000c)
#define PID_STG_ATTRIBUTES              ((PROPID) 0x0000000d)
#define PID_STG_WRITETIME               ((PROPID) 0x0000000e)

#define SFVM_MERGEMENU            1
#define SFVM_INVOKECOMMAND        2
#define SFVM_GETHELPTEXT          3
#define SFVM_INITMENUPOPUP        7
#define SFVM_PRERELEASE          12
#define SFVM_WINDOWCREATED       15
#define SFVM_REFRESH             17
#define SFVM_GETDETAILSOF        23
#define SFVM_COLUMNCLICK         24
#define SFVM_DEFITEMCOUNT        26
#define SFVM_UNMERGEMENU         28
#define SFVM_UPDATESTATUSBAR     31
#define SFVM_BACKGROUNDENUM      32
#define SFVM_DIDDRAGDROP         36
#define SFVM_DONTCUSTOMIZE       56
#define SFVM_GETZONE             58

#define DVM_MERGEMENU           SFVM_MERGEMENU
#define DVM_INVOKECOMMAND       SFVM_INVOKECOMMAND
#define DVM_GETHELPTEXT         SFVM_GETHELPTEXT
#define DVM_INITMENUPOPUP       SFVM_INITMENUPOPUP
#define DVM_RELEASE             SFVM_PRERELEASE
#define DVM_WINDOWCREATED       SFVM_WINDOWCREATED
#define DVM_REFRESH             SFVM_REFRESH
#define DVM_GETDETAILSOF        SFVM_GETDETAILSOF
#define DVM_COLUMNCLICK         SFVM_COLUMNCLICK
#define DVM_DEFITEMCOUNT        SFVM_DEFITEMCOUNT
#define DVM_UNMERGEMENU         SFVM_UNMERGEMENU
#define DVM_UPDATESTATUSBAR     SFVM_UPDATESTATUSBAR
#define DVM_BACKGROUNDENUM      SFVM_BACKGROUNDENUM
#define DVM_DIDDRAGDROP         SFVM_DIDDRAGDROP

EXTERN_C const IID IID_IShellFolderView;
EXTERN_C const IID IID_IAsyncOperation;

#define CMF_DVFILE 0x00010000

#if defined(_WIN64)
inline LONG_PTR XdkSetDlgUser(HWND hwnd, LONG_PTR val)
{
    return SetWindowLongPtr(hwnd, DWLP_USER, val);
}
inline LONG_PTR XdkGetDlgUser(HWND hwnd)
{
    return GetWindowLongPtr(hwnd, DWLP_USER);
}
inline LONG_PTR XdkSetDlgMsgResult(HWND hwnd, LONG_PTR val)
{
    return SetWindowLongPtr(hwnd, DWLP_MSGRESULT, val);
}
#else
inline LONG XdkSetDlgUser(HWND hwnd, LONG_PTR val)
{
    return SetWindowLong(hwnd, DWL_USER, (LONG)val);
}
inline LONG XdkGetDlgUser(HWND hwnd)
{
    return GetWindowLong(hwnd, DWL_USER);
}
inline LONG XdkSetDlgMsgResult(HWND hwnd, LONG_PTR val)
{
    return SetWindowLong(hwnd, DWL_MSGRESULT, (LONG)val);
}
#endif

#endif
