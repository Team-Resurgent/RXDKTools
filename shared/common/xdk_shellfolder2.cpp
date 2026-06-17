/*
 * IShellFolder2 / IPersistFolder2 for x64 DefView on Win10/11.
 */
#include "stdafx.h"

HRESULT CXboxFolder::GetCurFolder(LPITEMIDLIST *ppidl)
{
    if (!ppidl)
        return E_POINTER;
    *ppidl = GetPidl(CPidlUtils::PidlTypeAbsolute);
    return *ppidl ? S_OK : E_FAIL;
}

HRESULT CXboxFolder::GetDefaultSearchGUID(GUID *pguid)
{
    if (!pguid)
        return E_POINTER;
    return E_NOTIMPL;
}

HRESULT CXboxFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    if (!ppenum)
        return E_POINTER;
    *ppenum = NULL;
    return E_NOTIMPL;
}

HRESULT CXboxFolder::GetDefaultColumn(DWORD /*dwRes*/, ULONG *pSort, ULONG *pDisplay)
{
    if (pSort)
        *pSort = 0;
    if (pDisplay)
        *pDisplay = 0;
    return S_OK;
}

HRESULT CXboxFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    if (!pcsFlags)
        return E_POINTER;
    if (iColumn >= GetColumnCount())
        return E_INVALIDARG;
    *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
    return S_OK;
}

HRESULT CXboxFolder::GetDetailsEx(LPCITEMIDLIST /*pidl*/, const SHCOLUMNID * /*pscid*/, VARIANT * /*pv*/)
{
    return E_NOTIMPL;
}

HRESULT CXboxFolder::GetDetailsOf(LPCITEMIDLIST pidl, UINT iColumn, SHELLDETAILS *psd)
{
    if (!psd)
        return E_POINTER;
    if (pidl == NULL)
        return GetColumnHeaderDetails(iColumn, psd);
    return GetDetails(pidl, iColumn, psd);
}

HRESULT CXboxFolder::MapColumnToSCID(UINT iColumn, SHCOLUMNID *pscid)
{
    if (!pscid)
        return E_POINTER;
    return GetColumnSCID(iColumn, pscid);
}
