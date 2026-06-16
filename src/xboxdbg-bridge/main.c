#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __cdecl main(void)
{
    char szLine[BRIDGE_LINE_MAX];
    int nId = 0;

    if (FAILED(SessionInit()))
        return 1;

    while (fgets(szLine, sizeof szLine, stdin)) {
        char *nl = strchr(szLine, '\n');
        if (nl)
            *nl = 0;
        if (szLine[0] == 0)
            continue;
        if (!JsonGetDword(szLine, "id", (DWORD *)&nId))
            nId = 0;
        SessionHandleCommand(szLine, nId);
        if (!SessionIsActive())
            break;
    }

    SessionShutdown();
    return 0;
}
