MEDIA SERVER 0.1.0 - WINDOWS X64 TEST BUILD
===========================================

This is an early test build. Extract the complete folder before launching; the
DLL files beside media-server.exe are required.

OPEN THE EDITOR

Double-click media-server.exe and press OPEN EDITOR at the bottom of the
window. The editor opens as this machine's Director, listening for render
machines on port 45060.

QUICK START FROM POWERSHELL

1. Open PowerShell in this folder.
2. Run:

     powershell -ExecutionPolicy Bypass -File .\scripts\run-editor.ps1

   This opens the operator editor as the Director and prints what each render
   machine needs to connect. Close the editor window to stop.

You can also launch the editor directly from a prompt:

     .\media-server.exe --role=director --playback --edit `
       --director-listen=0.0.0.0:45060 `
       --show-id=0000000000000001000000000000000B `
       --director-epoch=0000000000000003000000000000000C

TWO OR MORE MACHINES

Copy and extract this same package on every machine, then use the two halves
of the split setup. Both scripts find media-server.exe beside this folder, so
nothing else needs configuring.

1. On the control machine:

     .\scripts\run-editor.ps1

   It opens the editor, listens for nodes, and prints this machine's address
   and the suggested node command. Render-node control uses the password in
   File > Preferences > Render nodes. Omit a password on both sides if none
   is configured.

2. On each render machine:

     .\scripts\run-output-node.ps1 -DirectorHost <control-machine-ip> `
       -NodeIndex 0

   Add -Password <your-render-node-password> when one is set in the editor.

   Use a different -NodeIndex per render machine. Add -Fullscreen for a real
   wall, or -Windowed while setting things up. It prints the values to enter in
   Hardware Map.

3. Back in the editor, open Hardware Map and add a Render Server with the node
   index, the render machine's address, and control port 47840 + the node
   index. You can also drag the machine from DETECTED ON NETWORK onto the
   map. Output Monitor can then START / RESTART / STOP that machine.

Alternatively, double-click media-server.exe on a render machine. Enter the
control machine's numeric address, node index and matching render-node
password, then choose SAVE AND START. Existing per-user settings are reused
when the new executable starts; CHANGE SETTINGS edits them.

RECONNECT / RESTART TEST

1. Stop the old render-machine agent and its output process. Extract this
   complete package into a new folder, then start the new executable or node
   launcher. Update a shortcut or startup entry if it points at the old folder.
2. Use this same build on the control machine and every render machine. Keep
   the Director address, port and show ID consistent with the saved node setup.
3. Leave a remote node running, close only the editor, and reopen the editor
   with the same project. Output Monitor shows CONNECTING, CATCHING UP, then
   LIVE; expand its row for acknowledged and target control frames.
4. Exercise STOP, START and RESTART from Output Monitor. New output windows
   initialize to black; reconnection holds the last complete image until an
   approved replacement is available. An old connection's lease can take up
   to fifteen seconds to clear before a persistent duplicate is reported.

The ZIP contains application files, not projects or source media. Existing
projects and per-user configuration are kept separately. Physical LED-wall
verification is still required for these output changes.

Notes:

- Windows Firewall must allow the Director port (45060) inbound on the control
  machine, and the lifecycle port (TCP 47840 + node index) and discovery port
  (UDP 47839) inbound on each render machine. The
  scripts print the netsh command; they do not change firewall rules
  themselves. Run it from an elevated prompt.
- Host and -DirectorHost require numeric IPv4/IPv6 addresses, not host names.
- The render-node password must match between the editor and each render
  machine. It is a machine preference, not a project property.
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
- Saved fullscreen output bindings apply the requested Windows display mode
  and fail readiness when that exact mode is unavailable.
- This package is unsigned. Windows SmartScreen may show an Unknown Publisher
  warning on machines that have not seen the executable before.

CRASH AND HANG REPORTS

media-server.pdb and media-bake.pdb are the debug symbols for exactly these
executables. Keep them with the folder: a dump file (Task Manager > Details >
right-click media-server.exe > Create memory dump file) can only be read
against the symbols of the build that wrote it. Send the dump together with
the logs from %LOCALAPPDATA%\MediaServer\logs.

LICENCES

The FFmpeg libraries in this folder are built with x265 for software HEVC
encoding, which places them under the GNU General Public License rather than
the LGPL. The licences of every bundled library are in licenses\.

For the complete command-line reference:

  .\media-server.exe --help
