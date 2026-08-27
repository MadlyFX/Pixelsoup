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

TWO OR MORE MACHINES

Copy and extract this same package on every machine, then use the two halves
of the split setup. Both scripts find media-server.exe beside this folder, so
nothing else needs configuring.

1. On the control machine:

     .\scripts\run-editor.ps1

   It opens the editor, listens for nodes, and prints this machine's address
   plus the shared lifecycle secret. Copy that whole command it suggests.

2. On each render machine:

     .\scripts\run-output-node.ps1 -DirectorHost <control-machine-ip> `
       -NodeIndex 0 -Secret <secret-from-step-1>

   Use a different -NodeIndex per render machine. Add -Fullscreen for a real
   wall, or -Windowed while setting things up. It prints the values to enter in
   Hardware Map.

3. Back in the editor, open Hardware Map and add a Render Server with the node
   index, the render machine's address, and control port 47840. Output Monitor
   can then START / RESTART / STOP that machine.

Notes:

- Windows Firewall must allow the Director port (45060) inbound on the control
  machine and the lifecycle port (47840) inbound on each render machine. The
  scripts print the netsh command; they do not change firewall rules
  themselves. Run it from an elevated prompt.
- Host and -DirectorHost require numeric IPv4/IPv6 addresses, not host names.
- The lifecycle secret must be identical everywhere. Never store it in the
  project file or share it publicly.
- Imported media and UV/model assets are currently path references, not packed
  into the project. Every render machine must be able to reach identical files
  at the authored path (a shared UNC path is the simplest test setup).
- Add -Direct to run-output-node.ps1 to start the render process without the
  lifecycle agent. Output Monitor cannot control a node started that way, but
  it is one fewer moving part when first proving connectivity.
- Add -DryRun to either script to see the exact command line without starting
  anything.

OTHER INCLUDED DEMOS

  .\scripts\run-scene-demo.ps1   Two synchronized scene outputs
  .\scripts\run-wall-demo.ps1    Two synchronized feed-mapped outputs

REQUIREMENTS / LIMITATIONS

- Windows 10 or 11 x64 and a Direct3D 12-capable GPU/driver.
- Hardware output refresh fields are currently project metadata; Windows display
  mode selection is not yet performed by the application.
- This package is unsigned. Windows SmartScreen may show an Unknown Publisher
  warning on machines that have not seen the executable before.

For the complete command-line reference:

  .\media-server.exe --help
