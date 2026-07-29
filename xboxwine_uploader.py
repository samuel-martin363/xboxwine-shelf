#!/usr/bin/env python3
"""XboxWine Shelf folder uploader.

Selects a Windows game folder, discovers likely executable files, scans likely
keyboard controls, packages the folder, and sends it to XboxWine Shelf over the
local network. No third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import atexit
import gc
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import struct
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from urllib.request import Request, urlopen
import zipfile

APP_VERSION = "0.2.9.3"
PORT = 24872
MAGIC = b"XWUP2\0\0\0"
CONNECT_TIMEOUT_SECONDS = 12
TRANSFER_TIMEOUT_SECONDS = 20 * 60
TRANSFER_CHUNK_BYTES = 256 * 1024
MAX_SCAN_BYTES = 96 * 1024 * 1024
VC_REDIST_X86_URL = "https://aka.ms/vc14/vc_redist.x86.exe"

BAD_EXE_WORDS = {
    "unins", "uninstall", "setup", "install", "installer", "updater",
    "update", "crashhandler", "crashreport", "reporter", "vcredist",
    "dxsetup", "dotnet", "unitycrashhandler", "steamservice", "launcher",
    "config", "configuration", "server", "benchmark",
}

TEXT_EXTENSIONS = {
    ".ini", ".cfg", ".conf", ".json", ".xml", ".txt", ".lua", ".js",
    ".cs", ".yml", ".yaml", ".toml", ".properties", ".input",
    ".controls", ".bind", ".bindings",
}
BINARY_SCAN_EXTENSIONS = {".exe", ".dll"}

KEYS = [
    *(chr(code) for code in range(ord("A"), ord("Z") + 1)),
    *(str(number) for number in range(10)),
    "SPACE", "ENTER", "ESCAPE", "TAB", "BACKSPACE",
    "SHIFT", "LSHIFT", "RSHIFT", "CTRL", "LCTRL", "RCTRL",
    "ALT", "LALT", "RALT",
    "UP", "DOWN", "LEFT", "RIGHT", "HOME", "END", "PAGEUP",
    "PAGEDOWN", "INSERT", "DELETE", "CAPSLOCK", "NUMLOCK", "SCROLLLOCK",
    "MINUS", "EQUALS", "COMMA", "PERIOD", "SLASH", "SEMICOLON",
    "QUOTE", "LEFTBRACKET", "RIGHTBRACKET", "BACKSLASH", "GRAVE",
    *(f"F{number}" for number in range(1, 25)),
    *(f"NUMPAD{number}" for number in range(10)),
    "NUMPAD_PLUS", "NUMPAD_MINUS", "NUMPAD_MULTIPLY", "NUMPAD_DIVIDE",
    "NUMPAD_ENTER", "NUMPAD_PERIOD",
]

DEFAULT_BINDINGS = {
    "A": "SPACE", "B": "ESCAPE", "X": "E", "Y": "Q",
    "LB": "SHIFT", "RB": "CTRL", "LT": "MOUSE_RIGHT",
    "RT": "MOUSE_LEFT", "VIEW": "TAB", "MENU": "ENTER",
    "LS_CLICK": "NONE", "RS_CLICK": "NONE",
    "DPAD_UP": "UP", "DPAD_DOWN": "DOWN",
    "DPAD_LEFT": "LEFT", "DPAD_RIGHT": "RIGHT",
    "LS_UP": "W", "LS_DOWN": "S", "LS_LEFT": "A", "LS_RIGHT": "D",
    "RS_UP": "NONE", "RS_DOWN": "NONE", "RS_LEFT": "NONE", "RS_RIGHT": "NONE",
}



def safe_remove_tree(path: Path, attempts: int = 12) -> None:
    """Remove temporary files without turning antivirus locks into app errors."""
    for attempt in range(attempts):
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            gc.collect()
            time.sleep(0.15 * (attempt + 1))
        except OSError:
            gc.collect()
            time.sleep(0.15 * (attempt + 1))
    # Leave the folder for Windows temp cleanup rather than crashing at exit.


def make_work_folder() -> Path:
    folder = Path(tempfile.mkdtemp(prefix="xboxwine-"))
    atexit.register(safe_remove_tree, folder)
    return folder


def download_file(url: str, output: Path, progress=None) -> None:
    request = Request(
        url,
        headers={"User-Agent": f"XboxWine-Uploader/{APP_VERSION}"},
    )

    try:
        with urlopen(request, timeout=120) as response, output.open("wb") as stream:
            total = int(response.headers.get("Content-Length") or 0)
            completed = 0

            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break

                stream.write(chunk)
                completed += len(chunk)

                if progress:
                    progress(
                        "Downloading VC++ x86 runtime",
                        completed,
                        max(total, 1),
                    )
    except (TimeoutError, socket.timeout) as error:
        raise RuntimeError(
            "The Microsoft VC++ runtime download timed out. "
            "Uncheck the runtime option and retry the game transfer. "
            "This timeout happened before connecting to the Xbox."
        ) from error
    except OSError as error:
        raise RuntimeError(
            f"The VC++ runtime could not be downloaded: {error}"
        ) from error


def find_runtime_installers(root: Path) -> dict[str, tuple[Path, str]]:
    """Find runtime installers already shipped with a game folder."""
    found: dict[str, tuple[Path, str]] = {}
    for path in root.rglob("*.exe"):
        name = path.name.lower()
        lowered = str(path).lower()
        if name == "dxsetup.exe" and "directx" in lowered:
            found.setdefault("directx", (path, "/silent"))
        elif name in {"oalinst.exe", "openal.exe"}:
            found.setdefault("openal", (path, "/S"))
        elif "dotnetfx" in name or name.startswith("ndp"):
            found.setdefault("dotnet", (path, "/q /norestart"))
    return found

def pe_architecture(path: Path) -> str:
    """Return x86, x64, arm, arm64, or unknown from a PE header."""
    try:
        with path.open("rb") as stream:
            if stream.read(2) != b"MZ":
                return "not-pe"
            stream.seek(0x3C)
            pe_offset_data = stream.read(4)
            if len(pe_offset_data) != 4:
                return "unknown"
            pe_offset = struct.unpack("<I", pe_offset_data)[0]
            stream.seek(pe_offset)
            if stream.read(4) != b"PE\0\0":
                return "unknown"
            machine_data = stream.read(2)
            if len(machine_data) != 2:
                return "unknown"
            machine = struct.unpack("<H", machine_data)[0]
    except OSError:
        return "unknown"

    return {
        0x014C: "x86",
        0x8664: "x64",
        0x01C0: "arm",
        0xAA64: "arm64",
    }.get(machine, f"machine-{machine:04x}")


def executable_score(path: Path, root: Path, architecture: str) -> int:
    relative = path.relative_to(root)
    stem = path.stem.lower()
    folder_name = root.name.lower().replace(" ", "")
    compact_stem = stem.replace(" ", "").replace("_", "").replace("-", "")
    score = 0

    if architecture == "x86":
        score += 100
    elif architecture == "x64":
        score -= 80
    elif architecture == "not-pe":
        score -= 100

    depth = len(relative.parts) - 1
    score += max(0, 40 - depth * 10)
    if compact_stem == folder_name:
        score += 90
    elif compact_stem in folder_name or folder_name in compact_stem:
        score += 45

    lowered = str(relative).lower()
    if any(word in stem or word in lowered for word in BAD_EXE_WORDS):
        score -= 120
    if any(part.lower() in {"redist", "redistributable", "support", "tools"}
           for part in relative.parts[:-1]):
        score -= 80

    try:
        size = path.stat().st_size
        score += min(30, int(size / (2 * 1024 * 1024)))
    except OSError:
        pass
    return score


def discover_executables(root: Path) -> list[tuple[Path, str, int]]:
    candidates: list[tuple[Path, str, int]] = []
    for path in root.rglob("*.exe"):
        if not path.is_file():
            continue
        architecture = pe_architecture(path)
        score = executable_score(path, root, architecture)
        candidates.append((path, architecture, score))
    candidates.sort(key=lambda item: (-item[2], len(item[0].relative_to(root).parts),
                                      str(item[0]).lower()))
    return candidates


def extract_strings(data: bytes) -> str:
    ascii_strings = re.findall(rb"[ -~]{4,}", data)
    utf16_strings = re.findall(rb"(?:[ -~]\x00){4,}", data)
    decoded: list[str] = []
    for value in ascii_strings:
        decoded.append(value.decode("ascii", errors="ignore"))
    for value in utf16_strings:
        decoded.append(value.decode("utf-16le", errors="ignore"))
    return "\n".join(decoded)


def read_for_key_scan(path: Path) -> str:
    try:
        size = path.stat().st_size
        if path.suffix.lower() in TEXT_EXTENSIONS:
            if size > 4 * 1024 * 1024:
                return ""
            data = path.read_bytes()
            for encoding in ("utf-8", "utf-16", "cp1252"):
                try:
                    return data.decode(encoding)
                except UnicodeDecodeError:
                    continue
            return data.decode("utf-8", errors="ignore")

        if path.suffix.lower() in BINARY_SCAN_EXTENSIONS:
            if size > 24 * 1024 * 1024:
                with path.open("rb") as stream:
                    data = stream.read(8 * 1024 * 1024)
                    stream.seek(max(0, size - 4 * 1024 * 1024))
                    data += stream.read(4 * 1024 * 1024)
            else:
                data = path.read_bytes()
            return extract_strings(data)
    except OSError:
        pass
    return ""


def key_patterns(key: str) -> list[re.Pattern[str]]:
    aliases = {
        "SPACE": ["SPACE", "SPACEBAR", "VK_SPACE", "SDLK_SPACE", "KEY_SPACE"],
        "ENTER": ["ENTER", "RETURN", "VK_RETURN", "SDLK_RETURN", "KEY_ENTER"],
        "ESCAPE": ["ESCAPE", "ESC", "VK_ESCAPE", "SDLK_ESCAPE", "KEY_ESCAPE"],
        "CTRL": ["CTRL", "CONTROL", "LCTRL", "RCTRL", "VK_CONTROL"],
        "SHIFT": ["SHIFT", "LSHIFT", "RSHIFT", "VK_SHIFT"],
        "ALT": ["ALT", "LALT", "RALT", "VK_MENU"],
        "UP": ["UP", "UPARROW", "ARROWUP", "VK_UP", "KEY_UP"],
        "DOWN": ["DOWN", "DOWNARROW", "ARROWDOWN", "VK_DOWN", "KEY_DOWN"],
        "LEFT": ["LEFT", "LEFTARROW", "ARROWLEFT", "VK_LEFT", "KEY_LEFT"],
        "RIGHT": ["RIGHT", "RIGHTARROW", "ARROWRIGHT", "VK_RIGHT", "KEY_RIGHT"],
    }.get(key, [key])

    patterns: list[re.Pattern[str]] = []
    for alias in aliases:
        escaped = re.escape(alias)
        if len(key) == 1 and key.isalnum():
            patterns.extend([
                re.compile(rf"\b(?:VK|KEY|SDLK|KEYCODE|BIND|INPUT)[_ .:=\-]*{escaped}\b", re.I),
                re.compile(rf"[\"']{escaped}[\"']\s*[:=]", re.I),
                re.compile(rf"[:=]\s*[\"']?{escaped}[\"']?(?:\s|,|;|$)", re.I),
            ])
        else:
            patterns.append(re.compile(rf"(?<![A-Z0-9]){escaped}(?![A-Z0-9])", re.I))
    return patterns


COMPILED_KEY_PATTERNS = {key: key_patterns(key) for key in KEYS}


def detect_keys(root: Path) -> list[str]:
    combined_parts: list[str] = []
    scanned_bytes = 0
    preferred_names = ("input", "control", "bind", "key", "setting", "config")

    files = [path for path in root.rglob("*") if path.is_file()]
    files.sort(key=lambda path: (
        0 if any(word in path.name.lower() for word in preferred_names) else 1,
        path.suffix.lower() not in TEXT_EXTENSIONS,
        str(path).lower(),
    ))

    for path in files:
        if path.suffix.lower() not in TEXT_EXTENSIONS | BINARY_SCAN_EXTENSIONS:
            continue
        try:
            size = path.stat().st_size
        except OSError:
            continue
        if scanned_bytes >= MAX_SCAN_BYTES:
            break
        text = read_for_key_scan(path)
        if text:
            combined_parts.append(text)
            scanned_bytes += min(size, 24 * 1024 * 1024)

    combined = "\n".join(combined_parts)
    found: list[str] = []

    # Common movement clusters are often stored as simple single-letter values.
    if re.search(r"\bWASD\b", combined, re.I):
        found.extend(["W", "A", "S", "D"])
    if re.search(r"\b(?:ARROW\s*KEYS|CURSOR\s*KEYS)\b", combined, re.I):
        found.extend(["UP", "DOWN", "LEFT", "RIGHT"])

    for key in KEYS:
        if key in found:
            continue
        if any(pattern.search(combined) for pattern in COMPILED_KEY_PATTERNS[key]):
            found.append(key)

    priority = [
        "W", "A", "S", "D", "UP", "DOWN", "LEFT", "RIGHT", "SPACE",
        "ENTER", "ESCAPE", "E", "Q", "Z", "X", "C", "SHIFT", "CTRL",
        "ALT", "TAB",
    ]
    order = {key: index for index, key in enumerate(priority)}
    found.sort(key=lambda key: (order.get(key, 999), KEYS.index(key)))
    return found


def choose_default_bindings(detected: list[str]) -> dict[str, str]:
    bindings = dict(DEFAULT_BINDINGS)
    keys = set(detected)

    if not {"W", "A", "S", "D"}.issubset(keys) and \
            {"UP", "DOWN", "LEFT", "RIGHT"}.issubset(keys):
        bindings.update({
            "LS_UP": "UP", "LS_DOWN": "DOWN",
            "LS_LEFT": "LEFT", "LS_RIGHT": "RIGHT",
        })

    if "SPACE" not in keys:
        for candidate in ("Z", "X", "C", "ENTER"):
            if candidate in keys:
                bindings["A"] = candidate
                break

    if "ESCAPE" not in keys and "X" in keys and bindings["A"] != "X":
        bindings["B"] = "X"

    action_candidates = [key for key in ("E", "Z", "X", "C", "Q", "F") if key in keys]
    if action_candidates:
        bindings["X"] = action_candidates[0]
    if len(action_candidates) > 1:
        bindings["Y"] = action_candidates[1]
    return bindings


def normalize_wine_path(relative: Path) -> str:
    return "c:\\xboxwine\\" + str(relative).replace("/", "\\")


def make_manifest(
    title: str,
    executable: Path,
    candidates: list[tuple[Path, str, int]],
    detected: list[str],
    root: Path,
    include_vc_runtime: bool = False,
) -> str:
    bindings = choose_default_bindings(detected)
    candidate_paths = [normalize_wine_path(path.relative_to(root)) for path, _, _ in candidates]
    candidate_architectures = [architecture for _, architecture, _ in candidates]
    runtime_installers = find_runtime_installers(root)
    selected_architecture = next(
        (architecture for path, architecture, _ in candidates if path == executable),
        pe_architecture(executable),
    )
    lines = [
        "# Generated by XboxWine Folder Uploader",
        f"title={title}",
        "zip=game.zip",
        f"exe={normalize_wine_path(executable.relative_to(root))}",
        f"architecture={selected_architecture}",
        "candidate_exes=" + "|".join(candidate_paths),
        "candidate_architectures=" + "|".join(candidate_architectures),
        "detected_keys=" + ",".join(detected),
        "arguments=",
        "fullscreen=aspect",
        "resolution=1280x720",
        "controller_bridge=keyboard_mouse",
        "right_stick_mouse=true",
        "mouse_speed=14",
        "stick_deadzone=9000",
        "extra_boxedwine_args=",
    ]
    if include_vc_runtime:
        lines.append(
            "runtime_vc14_x86="
            "c:\\xboxwine\\__xboxwine_runtimes\\vc_redist.x86.exe"
        )
    for runtime_name, (runtime_path, _arguments) in runtime_installers.items():
        lines.append(
            f"runtime_{runtime_name}="
            f"{normalize_wine_path(runtime_path.relative_to(root))}"
        )
    lines.extend(f"bind_{name.lower()}={value}" for name, value in bindings.items())
    return "\n".join(lines) + "\n"


def zip_folder(
    root: Path,
    output: Path,
    progress=None,
    extra_files: dict[Path, str] | None = None,
) -> None:
    files = [path for path in root.rglob("*") if path.is_file()]
    extras = extra_files or {}
    total = sum(path.stat().st_size for path in files)
    total += sum(path.stat().st_size for path in extras)
    completed = 0
    with zipfile.ZipFile(
        output,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=6,
        allowZip64=True,
    ) as archive:
        for path in files:
            archive.write(path, path.relative_to(root))
            completed += path.stat().st_size
            if progress:
                progress("Packing folder", completed, max(total, 1))
        for source, archive_name in extras.items():
            archive.write(source, archive_name)
            completed += source.stat().st_size
            if progress:
                progress("Packing runtime files", completed, max(total, 1))


def send_game(
    xbox_ip: str,
    port: int,
    manifest: str,
    archive_path: Path,
    progress=None,
) -> str:
    metadata = manifest.encode("utf-8")
    archive_size = archive_path.stat().st_size
    header = struct.pack("<8sIQ", MAGIC, len(metadata), archive_size)

    if progress:
        progress("Connecting to Xbox", 0, 1)

    connection: socket.socket | None = None

    try:
        last_error: OSError | None = None

        for attempt in range(1, 4):
            try:
                connection = socket.create_connection(
                    (xbox_ip, port),
                    timeout=CONNECT_TIMEOUT_SECONDS,
                )
                break
            except (TimeoutError, socket.timeout, ConnectionRefusedError, OSError) as error:
                last_error = error

                if attempt < 3:
                    if progress:
                        progress(
                            f"Xbox connection attempt {attempt} failed; retrying",
                            attempt,
                            3,
                        )
                    time.sleep(1.5)

        if connection is None:
            detail = f" ({last_error})" if last_error else ""
            raise RuntimeError(
                "Could not connect to XboxWine at "
                f"{xbox_ip}:{port}{detail}. "
                "Keep XboxWine open in the foreground on its transfer screen, "
                "confirm the displayed IP address has not changed, and make sure "
                "the PC and Xbox are on the same local network."
            )

        connection.settimeout(TRANSFER_TIMEOUT_SECONDS)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

        try:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass

        if progress:
            progress("Sending upload header", 0, 1)

        try:
            connection.sendall(header)
            connection.sendall(metadata)
        except (TimeoutError, socket.timeout) as error:
            raise RuntimeError(
                "Connected to XboxWine, but the Xbox timed out while reading "
                "the upload header. Return to the transfer screen and retry."
            ) from error
        except OSError as error:
            raise RuntimeError(
                f"The Xbox connection closed while sending the upload header: {error}"
            ) from error

        sent = 0

        try:
            with archive_path.open("rb") as stream:
                while True:
                    chunk = stream.read(TRANSFER_CHUNK_BYTES)
                    if not chunk:
                        break

                    connection.sendall(chunk)
                    sent += len(chunk)

                    if progress:
                        progress(
                            "Sending game folder to Xbox",
                            sent,
                            max(archive_size, 1),
                        )
        except (TimeoutError, socket.timeout) as error:
            raise RuntimeError(
                "The Xbox stopped receiving the game folder for 20 minutes. "
                f"Sent {sent:,} of {archive_size:,} bytes. "
                "Keep XboxWine open in the foreground and prevent the Xbox "
                "from returning to Dev Home during the transfer."
            ) from error
        except OSError as error:
            raise RuntimeError(
                "The network connection was interrupted while sending the game "
                f"folder after {sent:,} of {archive_size:,} bytes: {error}"
            ) from error

        if progress:
            progress("Waiting for Xbox to save the upload", 0, 1)

        try:
            reply_bytes = connection.recv(4096)
        except (TimeoutError, socket.timeout) as error:
            raise RuntimeError(
                "The complete folder was sent, but XboxWine did not reply within "
                "20 minutes. Read the status message shown on the Xbox transfer "
                "screen; it may be stuck saving the archive."
            ) from error
        except OSError as error:
            raise RuntimeError(
                f"The Xbox closed the connection before sending a result: {error}"
            ) from error

        reply = reply_bytes.decode("utf-8", errors="replace").strip()

        if not reply:
            raise RuntimeError(
                "The Xbox closed the connection without returning an upload result."
            )

        if not reply.startswith("OK"):
            raise RuntimeError(reply)

        return reply
    finally:
        if connection is not None:
            try:
                connection.close()
            except OSError:
                pass


class UploaderApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(f"XboxWine Folder Uploader v{APP_VERSION}")
        self.root.minsize(720, 500)
        self.folder: Path | None = None
        self.candidates: list[tuple[Path, str, int]] = []
        self.detected: list[str] = []
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.send_thread: threading.Thread | None = None
        self.sending = False
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        frame = ttk.Frame(root, padding=18)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="Xbox address").grid(row=0, column=0, sticky="w", pady=6)
        self.ip_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.ip_var).grid(row=0, column=1, sticky="ew", pady=6)
        ttk.Label(frame, text=":24872").grid(row=0, column=2, sticky="w")

        ttk.Label(frame, text="Game folder").grid(row=1, column=0, sticky="w", pady=6)
        self.folder_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.folder_var, state="readonly").grid(
            row=1, column=1, sticky="ew", pady=6
        )
        ttk.Button(frame, text="Choose folder…", command=self.choose_folder).grid(
            row=1, column=2, padx=(8, 0), pady=6
        )

        ttk.Label(frame, text="Game title").grid(row=2, column=0, sticky="w", pady=6)
        self.title_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.title_var).grid(
            row=2, column=1, columnspan=2, sticky="ew", pady=6
        )

        ttk.Label(frame, text="Executable").grid(row=3, column=0, sticky="w", pady=6)
        self.exe_var = tk.StringVar()
        self.exe_combo = ttk.Combobox(frame, textvariable=self.exe_var, state="readonly")
        self.exe_combo.grid(row=3, column=1, sticky="ew", pady=6)
        self.exe_combo.bind("<<ComboboxSelected>>", self.on_executable_changed)

        self.architecture_var = tk.StringVar(value="Architecture: not scanned")
        ttk.Label(frame, textvariable=self.architecture_var).grid(
            row=3,
            column=2,
            sticky="w",
            padx=(8, 0),
            pady=6,
        )

        self.include_vc_var = tk.BooleanVar(value=False)
        self.runtime_checkbox = ttk.Checkbutton(
            frame,
            text="Include Microsoft VC++ 2015-2022 x86 runtime",
            variable=self.include_vc_var,
        )
        self.runtime_checkbox.grid(
            row=4,
            column=1,
            columnspan=2,
            sticky="w",
            pady=(4, 6),
        )

        ttk.Label(frame, text="Detected keys").grid(row=5, column=0, sticky="nw", pady=6)
        self.keys_text = tk.Text(frame, height=6, wrap="word", state="disabled")
        self.keys_text.grid(row=5, column=1, columnspan=2, sticky="nsew", pady=6)
        frame.rowconfigure(5, weight=1)

        self.status_var = tk.StringVar(value="Open XboxWine v0.2.9.2, press Y, then choose a folder here.")
        ttk.Label(frame, textvariable=self.status_var, wraplength=650).grid(
            row=6, column=0, columnspan=3, sticky="ew", pady=(14, 6)
        )
        self.progress = ttk.Progressbar(frame, maximum=100)
        self.progress.grid(row=7, column=0, columnspan=3, sticky="ew", pady=6)

        self.send_button = ttk.Button(frame, text="Send folder to Xbox", command=self.start_send)
        self.send_button.grid(row=8, column=0, columnspan=3, sticky="ew", pady=(12, 0))

        ttk.Label(
            frame,
            text=f"Uploader v{APP_VERSION} • XWUP2 • 20-minute LAN timeout",
        ).grid(
            row=9,
            column=0,
            columnspan=3,
            sticky="e",
            pady=(8, 0),
        )

        self.root.after(100, self.poll_events)

    def on_executable_changed(self, _event=None) -> None:
        index = self.exe_combo.current()

        if index < 0 or index >= len(self.candidates):
            self.architecture_var.set("Architecture: unknown")
            self.include_vc_var.set(False)
            self.runtime_checkbox.state(["disabled"])
            return

        _path, architecture, _score = self.candidates[index]
        self.architecture_var.set(f"Architecture: {architecture}")

        if architecture == "x86":
            self.runtime_checkbox.state(["!disabled"])
        else:
            self.include_vc_var.set(False)
            self.runtime_checkbox.state(["disabled"])

    def choose_folder(self) -> None:
        selected = filedialog.askdirectory(title="Choose the complete game folder")
        if not selected:
            return
        self.folder = Path(selected)
        self.folder_var.set(str(self.folder))
        self.title_var.set(self.folder.name)
        self.status_var.set("Scanning executables and likely keyboard controls…")
        self.send_button.state(["disabled"])
        threading.Thread(target=self.scan_worker, daemon=True).start()

    def scan_worker(self) -> None:
        assert self.folder is not None
        try:
            candidates = discover_executables(self.folder)
            detected = detect_keys(self.folder)
            self.events.put(("scan_done", (candidates, detected)))
        except Exception as error:  # GUI boundary
            self.events.put(("error", str(error)))

    def start_send(self) -> None:
        if self.folder is None or not self.candidates:
            messagebox.showerror("XboxWine", "Choose a folder containing an .exe first.")
            return
        xbox_ip = self.ip_var.get().strip()
        xbox_ip = re.sub(r"^https?://", "", xbox_ip, flags=re.IGNORECASE)
        xbox_ip = xbox_ip.split("/", 1)[0]
        xbox_ip = xbox_ip.rsplit(":", 1)[0].strip()

        if not xbox_ip:
            messagebox.showerror("XboxWine", "Enter the Xbox IP shown in the transfer screen.")
            return
        index = self.exe_combo.current()
        if index < 0:
            index = 0
        executable = self.candidates[index][0]
        title = self.title_var.get().strip() or self.folder.name
        self.send_button.state(["disabled"])
        self.sending = True
        self.send_thread = threading.Thread(
            target=self.send_worker,
            args=(
                xbox_ip,
                title,
                executable,
                bool(self.include_vc_var.get()),
            ),
            daemon=False,
        )
        self.send_thread.start()

    def send_worker(
        self,
        xbox_ip: str,
        title: str,
        executable: Path,
        include_vc_runtime: bool,
    ) -> None:
        assert self.folder is not None
        work_folder = make_work_folder()
        try:
            extras: dict[Path, str] = {}
            selected_architecture = pe_architecture(executable)
            include_vc_runtime = include_vc_runtime and selected_architecture == "x86"

            if include_vc_runtime:
                vc_runtime = work_folder / "vc_redist.x86.exe"
                download_file(
                    VC_REDIST_X86_URL,
                    vc_runtime,
                    self.post_progress,
                )
                extras[vc_runtime] = (
                    "__xboxwine_runtimes/vc_redist.x86.exe"
                )

            manifest = make_manifest(
                title,
                executable,
                self.candidates,
                self.detected,
                self.folder,
                include_vc_runtime=include_vc_runtime,
            )
            archive = work_folder / "game.zip"
            zip_folder(
                self.folder,
                archive,
                self.post_progress,
                extra_files=extras,
            )
            send_game(
                xbox_ip,
                PORT,
                manifest,
                archive,
                self.post_progress,
            )
            self.events.put(("done", title))
        except Exception as error:  # GUI boundary
            self.events.put(("error", str(error)))
        finally:
            safe_remove_tree(work_folder)
            self.events.put(("send_finished", None))

    def on_close(self) -> None:
        if self.sending:
            messagebox.showwarning(
                "XboxWine",
                "Packing or transfer is still running. Wait for it to finish "
                "before closing the uploader.",
            )
            return
        self.root.destroy()

    def post_progress(self, stage: str, done: int, total: int) -> None:
        self.events.put(("progress", (stage, done, total)))

    def poll_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "scan_done":
                    self.candidates, self.detected = value  # type: ignore[misc]
                    labels = [
                        f"{path.relative_to(self.folder)}  [{arch}; score {score}]"
                        for path, arch, score in self.candidates
                    ]
                    self.exe_combo["values"] = labels
                    if labels:
                        self.exe_combo.current(0)
                        self.on_executable_changed()
                        self.status_var.set(
                            f"Found {len(labels)} executable(s). The best candidate is selected."
                        )
                        self.send_button.state(["!disabled"])
                    else:
                        self.architecture_var.set("Architecture: none")
                        self.include_vc_var.set(False)
                        self.runtime_checkbox.state(["disabled"])
                        self.status_var.set("No .exe files were found in that folder.")
                    self.keys_text.configure(state="normal")
                    self.keys_text.delete("1.0", "end")
                    self.keys_text.insert(
                        "1.0",
                        ", ".join(self.detected) if self.detected else
                        "No confident keys found. The Xbox control editor will still offer all common keys."
                    )
                    self.keys_text.configure(state="disabled")
                elif kind == "progress":
                    stage, done, total = value  # type: ignore[misc]
                    percent = max(0.0, min(100.0, done * 100.0 / total))
                    self.progress["value"] = percent
                    self.status_var.set(f"{stage}: {percent:.1f}%")
                elif kind == "send_finished":
                    self.sending = False
                    self.send_thread = None
                elif kind == "done":
                    self.progress["value"] = 100
                    self.status_var.set(
                        f"{value} is stored on the Xbox. Return to the library and open Controls before playing."
                    )
                    self.send_button.state(["!disabled"])
                    messagebox.showinfo("XboxWine", f"Transferred {value} successfully.")
                elif kind == "error":
                    self.status_var.set(str(value))
                    self.send_button.state(["!disabled"])
                    messagebox.showerror("XboxWine", str(value))
        except queue.Empty:
            pass
        self.root.after(100, self.poll_events)


def cli_main(args: argparse.Namespace) -> int:
    folder = Path(args.folder).resolve()
    if not folder.is_dir():
        raise SystemExit(f"Folder does not exist: {folder}")
    candidates = discover_executables(folder)
    if not candidates:
        raise SystemExit("No .exe files were found.")

    executable: Path
    if args.exe:
        executable = folder / args.exe
        if not executable.is_file():
            raise SystemExit(f"Executable does not exist: {executable}")
    else:
        executable = candidates[0][0]

    detected = detect_keys(folder)
    title = args.title or folder.name
    print(f"Selected executable: {executable.relative_to(folder)}")
    print("Detected keys:", ", ".join(detected) or "none")
    manifest = make_manifest(title, executable, candidates, detected, folder)

    def progress(stage: str, done: int, total: int) -> None:
        print(f"\r{stage}: {done * 100.0 / total:5.1f}%", end="", flush=True)

    work_folder = make_work_folder()
    try:
        archive = work_folder / "game.zip"
        zip_folder(folder, archive, progress)
        print()
        send_game(args.xbox, args.port, manifest, archive, progress)
    finally:
        safe_remove_tree(work_folder)
        print()
    print("Transfer complete.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbox", help="Xbox IP address")
    parser.add_argument("--folder", help="Complete game folder")
    parser.add_argument("--exe", help="Executable path relative to the game folder")
    parser.add_argument("--title", help="Library title")
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    if args.xbox and args.folder:
        return cli_main(args)

    root = tk.Tk()
    UploaderApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
