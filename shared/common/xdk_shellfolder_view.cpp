/*
 * Load SHCreateShellFolderView from shell32.dll (private export; no shell32p.lib).
 */
#include <windows.h>
#include <shlobj.h>

typedef HRESULT (WINAPI* PFN_SHCreateShellFolderView)(const SFV_CREATE* pcsfv, IShellView** ppsv);

HRESULT XdkCreateShellFolderView(const SFV_CREATE* pcsfv, IShellView** ppsv)
{
    static PFN_SHCreateShellFolderView pfn = nullptr;
    if (!pfn) {
        HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
        if (!shell32) {
            shell32 = LoadLibraryW(L"shell32.dll");
        }
        if (shell32) {
            pfn = reinterpret_cast<PFN_SHCreateShellFolderView>(
                GetProcAddress(shell32, "SHCreateShellFolderView"));
            if (!pfn) {
                pfn = reinterpret_cast<PFN_SHCreateShellFolderView>(
                    GetProcAddress(shell32, MAKEINTRESOURCEA(256)));
            }
        }
    }
    if (!pfn) {
        return E_FAIL;
    }
    return pfn(pcsfv, ppsv);
}
