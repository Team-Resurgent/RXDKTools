Host-tool **project sources** and `.vcxproj` files live under `src/`.

## Layout

- `../shared/` (repo root) — code and headers used by multiple projects
  - `include/` — `xboxdbg.h`, crypto headers, XBE image types
  - `common/` — static-link helpers (`dm_error_shim.c`, `xboxdbg_static_init.c`, shell folder helpers)
- `xboxdbg/` — static debug monitor client (`xbdbgs.lib`)
- `xbfile/` — shared file helpers
- `xbcp/`, `xbdir/`, `xbecopy/`, `xbmkdir/` — CLI file tools
- `xbox-launch/` — launch helper
- `xbshlext/` — shell extension (`out/bin/x64/Release/xbshlext.dll`)
- `xbWatson/` — break notification UI (`xbdbgs.lib`)
- `imagebld/` — Xbox Image File Builder (`imagebld.exe`; includes `xcbase.c` / `umkm.h` for XBE signing)
- `xrsa/` — source-built RSA/crypto library (`xrsa.lib`)
- `xboxdbg-bridge/` — JSON debug host for DAP (`out/bin/x64/Release/xboxdbg-bridge.exe`)

Each component folder includes its `.vcxproj` and optional `.props`. Shared output paths and roots (`IncludeRoot`, `CommonRoot`) live in `XboxTools.props`. Every project also imports `XboxTools.SharedItems.props` so `shared/include` and `shared/common` appear in Solution Explorer (set `SharedCommonCompile` in a project to compile specific common sources).
