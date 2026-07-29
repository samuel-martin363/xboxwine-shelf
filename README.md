# XboxWine Shelf v0.2 — local game folders, no USB required

This is a source patch kit for the experimental `worleydl/Boxedwine-uwp`
`uwp-compat` branch.

The important change in v0.2 is that games are transferred once from a PC and
stored in XboxWine Shelf's own Xbox app storage. After a transfer finishes, the
PC can be shut down. A USB drive is not required.

## What this version adds

- Controller-first Xbox game library.
- **Add Folder** screen with a local-network receiver on port `24872`.
- Windows PC uploader with a normal folder picker.
- Recursive `.exe` discovery and 32-bit/64-bit PE identification.
- Automatic ranking of the most likely game executable.
- Best-effort keyboard-control discovery from configuration files and binary
  strings.
- Per-game Xbox controller mapping editor, including right-stick mouse/digital modes.
- Games and controller profiles stored in the app's `LocalState\Games` folder.
- Optional legacy removable-storage scanning remains in the code.

## Important status

This is still an experimental source milestone. It has not been compiled or
run on a physical Xbox from this environment because Microsoft UWP/Xbox build
tools require Windows, Visual Studio, and access to the console.

The existing UWP BoxedWine fork is experimental too. Expect compilation fixes,
Xbox-specific debugging, and game-specific compatibility work.

BoxedWine currently targets 16-bit and 32-bit Windows software. This project
does not bypass Steam DRM, launchers, anti-cheat, licensing checks, or other
access controls.

# Build

## 1. Install Visual Studio 2022 components

Install:

- Universal Windows Platform development
- C++ Universal Windows Platform tools
- MSVC v143 x64/x86 build tools
- Windows 10 SDK 10.0.19041.0

## 2. Download and patch the UWP fork

Extract this kit and open PowerShell in its folder:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\bootstrap.ps1
```

The script downloads the experimental fork, applies this source patch, and
downloads the official starter Wine filesystem. The solution will be at:

```text
work\Boxedwine-uwp-uwp-compat\project\msvc\BoxedWine\BoxedWine.sln
```

## 3. Create the Xbox package

Open the solution in Visual Studio:

1. Select `Release | x64`.
2. Set `uwp` as the startup project.
3. Build the solution.
4. Right-click `uwp`.
5. Choose **Publish / Create App Packages**.
6. Choose **Sideloading**.
7. Build an x64 package.
8. Keep the `.msixbundle` and every file in `Dependencies\x64`.

Full instructions: `docs\INSTALL-ON-XBOX.md`.

# Add a whole game folder without USB

## On Xbox

1. Start XboxWine Shelf.
2. Press **Y — Add Folder**.
3. Leave that screen open.
4. Note the displayed address, for example `192.168.1.45:24872`.

## On PC

Double-click:

```text
run-uploader.bat
```

Or run:

```powershell
py xboxwine_uploader.py
```

Then:

1. Enter the Xbox IP shown on the TV.
2. Choose the complete game folder.
3. The uploader recursively finds all `.exe` files.
4. It identifies x86 versus x64 executables and selects the most likely game.
5. It scans likely control/configuration data for keyboard keys.
6. Review or change the selected executable.
7. Select **Send folder to Xbox**.
8. On Xbox, press **View** to cycle through every discovered executable later.

The uploader ZIPs the contents temporarily and sends them directly to the app.
The ZIP and its generated manifest are stored inside XboxWine Shelf's local app
storage. The temporary PC ZIP is deleted afterward.

You can also use the command line:

```powershell
py xboxwine_uploader.py `
  --xbox 192.168.1.45 `
  --folder "C:\Games\Example Game" `
  --exe "ExampleGame.exe" `
  --title "Example Game"
```

# Controller customization

Highlight a game and press **X — Controls**.

- Up/down chooses an Xbox control.
- Left/right or A cycles the assigned keyboard/mouse input.
- Keys detected by the uploader appear first.
- X restores the default profile.
- Menu toggles the right stick between mouse mode and four customizable directions.
- Y saves.
- B cancels.

Default profile:

| Xbox input | Windows input |
|---|---|
| Left stick | WASD |
| D-pad | Arrow keys |
| A | Space |
| B | Escape |
| X | E |
| Y | Q |
| LB | Shift |
| RB | Ctrl |
| Menu | Enter |
| View | Tab |
| Right trigger | Left mouse button |
| Left trigger | Right mouse button |
| Right stick | Mouse movement by default; Menu switches to four custom directions |

## About automatic key detection

The uploader scans common configuration formats and readable strings embedded
in `.exe`/`.dll` files. This can discover many controls, but no static scanner
can guarantee every key used by every game. Some games encrypt settings,
generate bindings at runtime, use DirectInput/XInput only, or never store key
names in readable form.

The control editor therefore includes letters, numbers, punctuation, modifiers, arrows, navigation keys, F1–F24, numpad keys, mouse buttons, and wheel actions even
when detection finds nothing. A deeper future milestone can instrument the
BoxedWine/Wine input path and log which virtual keys a running game queries.

# Local storage notes

Imported games consume Xbox Developer Mode storage. Increase the app's
allocated storage in Dev Home when necessary. Removing or resetting the app can
also remove its local games, so keep the original folders on your PC.

# Current limitations

- No native guest XInput/DirectInput controller yet; this version emits keyboard
  and mouse input.
- No 64-bit Windows guest support.
- No automatic game installers.
- No DRM or launcher bypass.
- No guarantee that a detected `.exe` is compatible with BoxedWine.
- Static key discovery is best-effort, not perfect runtime introspection.
- No cover-art download or editing yet.

# License

BoxedWine is GPL-2.0. This patch is GPL-2.0-or-later. When distributing a
combined binary, provide the complete corresponding source and retain upstream
copyright and license notices.
