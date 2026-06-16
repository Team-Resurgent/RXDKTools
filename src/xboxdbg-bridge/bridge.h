#ifndef XBOXDBG_BRIDGE_H
#define XBOXDBG_BRIDGE_H

#include <windows.h>
#include <xboxdbg.h>

#define BRIDGE_LINE_MAX 4096

void BridgeEmit(const char *szJson);
void BridgeLog(const char *szFmt, ...);

/* json_util.c */
BOOL JsonGetString(const char *szJson, const char *szKey, char *szOut, int cchOut);
BOOL JsonGetDword(const char *szJson, const char *szKey, DWORD *pdwOut);
BOOL JsonGetBool(const char *szJson, const char *szKey, BOOL *pfOut);
BOOL JsonGetPtr(const char *szJson, const char *szKey, PVOID *ppvOut);
void JsonAppendEscaped(char *szOut, int cchOut, int *pPos, LPCSTR szIn);

/* symbols.c */
HRESULT SymbolsLoad(LPCSTR szExePath, LPCSTR szPdbPath, LPCSTR szMapPath);
void SymbolsUnload(void);
void SymbolsSetModuleBase(PVOID pvBase);
HRESULT SymbolsLineToAddress(LPCSTR szFile, DWORD dwLine, PVOID *ppvAddr);
HRESULT SymbolsResolveLine(LPCSTR szFile, DWORD dwLine, PVOID *ppvAddr);
HRESULT SymbolsAddressToLine(PVOID pvAddr, char *szFile, int cchFile, DWORD *pdwLine, char *szFunc, int cchFunc);
PVOID SymbolsRelocateAddress(PVOID pvPdbAddr, PVOID pvModuleBase);
DWORD64 SymbolsGetPdbBase(void);
HRESULT SymbolsDiag(char *szOut, int cchOut);
HRESULT SymbolsEmitVariablesJson(LPCSTR szScope, PXBDM_CONTEXT pctx, char *szBuf, int cchBuf, int *pcVars);
HRESULT SymbolsEmitMembersJson(LPCSTR szBase, PXBDM_CONTEXT pctx, char *szBuf, int cchBuf, int *pcVars);
HRESULT SymbolsEvaluate(LPCSTR szExpr, PXBDM_CONTEXT pctx, char *szValue, int cchValue, char *szErr, int cchErr);

/* session.c */
HRESULT SessionInit(void);
void SessionShutdown(void);
HRESULT SessionHandleCommand(const char *szJson, int nId);
BOOL SessionIsActive(void);
DWORD SessionGetMainThread(void);
PVOID SessionGetModuleBase(void);

#endif
