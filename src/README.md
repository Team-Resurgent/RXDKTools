Host-tool **sources** and **project files** live in this repository.



- `include/` — `xboxdbg.h` and crypto headers

- `xboxdbg/` — static debug monitor client (`xbdbgs.lib`)

- `xbfile/` — shared file helpers

- `xbcp/`, `xbdir/`, `xbecopy/`, `xbmkdir/` — CLI file tools

- `xbox-launch/` — launch helper

- `xbshlext/` — shell extension (`out/bin/x64/Release/xbshlext.dll`)

- `xbWatson/` — break notification UI (`xbdbgs.lib`)

- `imagebld/` — Xbox Image File Builder (`imagebld.exe`)

- `crypto/` — `xcbase.c`, `umkm.h` (imagebld RSA helpers)

- `xboxdbg-bridge/` — JSON debug host for DAP (`out/bin/x64/Release/xboxdbg-bridge.exe`)

- `common/` — static-link helpers (`dm_error_shim.c`, `xboxdbg_static_init.c`, shell folder helpers)

Each component folder includes its `.vcxproj` and optional `.props`. Shared output paths live in `src/XboxNeighborhood.props`.

