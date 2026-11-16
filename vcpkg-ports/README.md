# vcpkg overlay ports

Ports here replace the vcpkg registry's port of the same name for this
project (`vcpkg-configuration.overlay-ports` in `vcpkg.json`). Each one is the
port from the project's `builtin-baseline` with the smallest change the server
needs; keep it in step with the baseline when the baseline moves.

| Port | Change from the baseline port | Why |
| --- | --- | --- |
| `x265` | Configured with `-DHIGH_BIT_DEPTH=ON` | The full-size output proxy is 10-bit HEVC Main10, and x265 is its software encoder where NVENC cannot open. The registry builds the 8-bit library, which refuses 10-bit frames. |

x265 is GPL-2.0-or-later. With it, FFmpeg is built with `gpl`, so the FFmpeg
libraries the package ships are GPL; see `packaging/windows/README.txt`.
