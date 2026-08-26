MEDIA SERVER 0.1.0 - WINDOWS X64 TEST BUILD
===========================================

This is an early test build. Extract the complete folder before launching; the
DLL files beside media-server.exe are required.

QUICK START ON ONE MACHINE

1. Open PowerShell in this folder.
2. Run:

     powershell -ExecutionPolicy Bypass -File .\scripts\run-editor-demo.ps1

   This opens the operator editor and a lifecycle-supervised output window.
   Press Enter in the PowerShell window when you want to stop the demo.

You can also launch an editor without the bundled demo node:

     .\media-server.exe --role=director --playback --edit

OTHER INCLUDED DEMOS

  .\scripts\run-scene-demo.ps1   Two synchronized scene outputs
  .\scripts\run-wall-demo.ps1    Two synchronized feed-mapped outputs

MULTI-MACHINE TESTING

- Copy and extract this same package on every Windows render machine.
- Windows Firewall must allow the Director/session and lifecycle TCP ports.
- Install the machine-local lifecycle listener as described by `--help`; its
  fixed child executable should be this package's media-server.exe.
- Use the same lifecycle secret on the Director and listeners. Never store the
  secret in the project file or share it publicly.
- Imported media and UV/model assets are currently path references, not packed
  into the project. Every render machine must be able to reach identical files
  at the authored path (a shared UNC path is the simplest test setup).
- Hardware Map Host values currently require numeric IPv4/IPv6 addresses.

REQUIREMENTS / LIMITATIONS

- Windows 10 or 11 x64 and a Direct3D 12-capable GPU/driver.
- Hardware output refresh fields are currently project metadata; Windows display
  mode selection is not yet performed by the application.
- This package is unsigned. Windows SmartScreen may show an Unknown Publisher
  warning on machines that have not seen the executable before.

For the complete command-line reference:

  .\media-server.exe --help
