# XboxWine Shelf v0.2 development notes

## Data flow

```text
PC folder picker
    -> recursive executable/key scan
    -> temporary ZIP + game.xwgame manifest
    -> TCP 24872 on the local network
    -> Xbox UWP StreamSocketListener
    -> ApplicationData.LocalFolder/Games/<title>/
       - game.zip
       - game.xwgame
    -> BoxedWine mounts game.zip as Wine folder C:\xboxwine
```

## Transfer protocol

Little-endian fixed header:

```text
8 bytes   magic: XWUP2\0\0\0
4 bytes   UTF-8 manifest length
8 bytes   ZIP length
N bytes   UTF-8 game.xwgame
M bytes   ZIP data
```

The server replies with `OK\n` or `ERROR ...\n`.

## Why local app storage

Xbox does not provide ordinary unrestricted desktop filesystem access to UWP
apps. Games are therefore copied into the app-controlled LocalState area. This
also allows them to remain on the console after the transfer PC is shut down.

## Key discovery

The PC scanner searches common text configuration formats and printable
ASCII/UTF-16 strings in executable/library files. It recognizes common names
such as `VK_SPACE`, `SDLK_w`, `KeyW`, arrows, function keys, and control words.

This is a useful heuristic rather than complete introspection. The proper
runtime milestone is to instrument guest input access:

1. Log guest calls to Win32 keyboard APIs such as `GetAsyncKeyState` and
   `GetKeyState`, or the Wine driver paths serving those calls.
2. Log DirectInput device enumeration/object queries.
3. Record requested virtual keys per executable.
4. Expose a live **Learn Controls** overlay after returning to the shelf.
5. Offer the discovered runtime set in the mapping editor.

## Native controller milestone

The current controller bridge emits SDL keyboard and mouse events. True gamepad
support requires an emulated guest joystick/XInput path, including vibration.

## Source files added

- `shelf_entry.cpp`
- `xbox_shelf.cpp/.h`
- `controller_bridge.cpp/.h`
- `transfer_server.cpp/.h`
- `xboxwine_uploader.py`

## First validation sequence

1. Compile the UWP target.
2. Launch the shelf on Xbox.
3. Verify the transfer address appears.
4. Upload a tiny folder with one x86 `.exe`.
5. Confirm files are written to LocalState.
6. Confirm the game appears after returning.
7. Save a changed controller binding and relaunch the app.
8. Confirm BoxedWine can mount the local ZIP.
9. Test a minimal 32-bit Win32 program before attempting a game.
