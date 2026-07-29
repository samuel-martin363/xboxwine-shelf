#!/usr/bin/env python3
"""Find likely 32-bit Windows games without launching them.

No third-party modules are required. The scanner reads PE headers only, scores
likely game executables, and can export results to CSV.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import os
from pathlib import Path
import queue
import re
import struct
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Iterable

MACHINE_NAMES = {
    0x014C: "x86",
    0x8664: "x64",
    0x01C0: "arm",
    0xAA64: "arm64",
}

SKIP_DIRECTORY_NAMES = {
    "$recycle.bin",
    "system volume information",
    "windows",
    "winsxs",
    "system32",
    "syswow64",
    "node_modules",
    ".git",
    ".cache",
}

BAD_EXECUTABLE_WORDS = {
    "unins",
    "uninstall",
    "setup",
    "install",
    "installer",
    "updater",
    "update",
    "crashhandler",
    "crashreport",
    "reporter",
    "vcredist",
    "dxsetup",
    "dotnet",
    "unitycrashhandler",
    "steamservice",
    "benchmark",
    "config",
    "configuration",
    "server",
    "helper",
    "service",
}

GAME_MARKERS = {
    "UnityPlayer.dll": "Unity",
    "GameAssembly.dll": "Unity IL2CPP",
    "steam_api.dll": "Steamworks",
    "steam_api64.dll": "Steamworks",
    "data.win": "GameMaker",
    "nw.dll": "NW.js",
    "package.nw": "NW.js",
    "rpg_core.js": "RPG Maker",
}


@dataclass(frozen=True)
class Result:
    path: Path
    architecture: str
    score: int
    engine: str
    size_bytes: int
    subsystem: str


def read_pe(path: Path) -> tuple[str, str]:
    """Return architecture and subsystem from a PE header."""
    try:
        with path.open("rb") as stream:
            if stream.read(2) != b"MZ":
                return "not-pe", "unknown"

            stream.seek(0x3C)
            raw_offset = stream.read(4)
            if len(raw_offset) != 4:
                return "unknown", "unknown"

            pe_offset = struct.unpack("<I", raw_offset)[0]
            stream.seek(pe_offset)

            if stream.read(4) != b"PE\0\0":
                return "unknown", "unknown"

            machine_raw = stream.read(2)
            if len(machine_raw) != 2:
                return "unknown", "unknown"

            machine = struct.unpack("<H", machine_raw)[0]
            architecture = MACHINE_NAMES.get(machine, f"machine-{machine:04x}")

            # COFF header is 20 bytes total. We already read Machine (2 bytes).
            stream.seek(pe_offset + 24)
            optional_magic_raw = stream.read(2)
            if len(optional_magic_raw) != 2:
                return architecture, "unknown"

            optional_magic = struct.unpack("<H", optional_magic_raw)[0]
            if optional_magic == 0x10B:
                subsystem_offset = pe_offset + 24 + 68
            elif optional_magic == 0x20B:
                subsystem_offset = pe_offset + 24 + 88
            else:
                return architecture, "unknown"

            stream.seek(subsystem_offset)
            subsystem_raw = stream.read(2)
            if len(subsystem_raw) != 2:
                return architecture, "unknown"

            subsystem = struct.unpack("<H", subsystem_raw)[0]
            subsystem_name = {
                2: "GUI",
                3: "Console",
                9: "Windows CE",
                10: "EFI",
            }.get(subsystem, f"Subsystem {subsystem}")

            return architecture, subsystem_name
    except (OSError, ValueError, struct.error):
        return "unreadable", "unknown"


def detect_engine(executable: Path) -> str:
    folder = executable.parent
    try:
        names = {child.name.lower(): child for child in folder.iterdir()}
    except OSError:
        return "Unknown"

    detected: list[str] = []
    for marker, engine in GAME_MARKERS.items():
        if marker.lower() in names and engine not in detected:
            detected.append(engine)

    stem = executable.stem
    if (folder / f"{stem}_Data").is_dir() and "Unity" not in detected:
        detected.append("Unity")

    return ", ".join(detected) if detected else "Unknown"


def likely_game_score(path: Path, architecture: str, engine: str) -> int:
    name = path.stem.lower()
    lowered_path = str(path).lower()
    score = 0

    if architecture == "x86":
        score += 100
    elif architecture == "x64":
        score -= 30
    else:
        score -= 100

    if engine != "Unknown":
        score += 80

    if any(word in name or word in lowered_path for word in BAD_EXECUTABLE_WORDS):
        score -= 140

    try:
        size = path.stat().st_size
    except OSError:
        size = 0

    if size >= 512 * 1024:
        score += 10
    if size >= 2 * 1024 * 1024:
        score += 15
    if size >= 10 * 1024 * 1024:
        score += 10

    parent_compact = re.sub(r"[^a-z0-9]", "", path.parent.name.lower())
    stem_compact = re.sub(r"[^a-z0-9]", "", name)
    if parent_compact and stem_compact:
        if parent_compact == stem_compact:
            score += 70
        elif parent_compact in stem_compact or stem_compact in parent_compact:
            score += 35

    if any(
        part.lower() in {"redist", "_commonredist", "support", "tools"}
        for part in path.parts
    ):
        score -= 80

    return score


def walk_executables(root: Path, cancelled: threading.Event) -> Iterable[Path]:
    def on_error(_: OSError) -> None:
        return

    for current, directories, filenames in os.walk(root, onerror=on_error):
        if cancelled.is_set():
            return

        directories[:] = [
            directory
            for directory in directories
            if directory.lower() not in SKIP_DIRECTORY_NAMES
        ]

        for filename in filenames:
            if cancelled.is_set():
                return
            if filename.lower().endswith(".exe"):
                yield Path(current) / filename


def scan_folder(
    root: Path,
    cancelled: threading.Event,
    progress=None,
) -> list[Result]:
    results: list[Result] = []
    checked = 0

    for path in walk_executables(root, cancelled):
        checked += 1
        architecture, subsystem = read_pe(path)

        if progress and checked % 25 == 0:
            progress(checked, path)

        if architecture != "x86":
            continue

        engine = detect_engine(path)
        score = likely_game_score(path, architecture, engine)

        try:
            size = path.stat().st_size
        except OSError:
            size = 0

        results.append(
            Result(
                path=path,
                architecture=architecture,
                score=score,
                engine=engine,
                size_bytes=size,
                subsystem=subsystem,
            )
        )

    results.sort(
        key=lambda item: (
            -item.score,
            str(item.path).lower(),
        )
    )
    return results


def steam_library_roots() -> list[Path]:
    candidates = [
        Path(os.environ.get("PROGRAMFILES(X86)", "")) / "Steam",
        Path(os.environ.get("PROGRAMFILES", "")) / "Steam",
    ]
    roots: list[Path] = []

    for steam in candidates:
        common = steam / "steamapps" / "common"
        if common.is_dir() and common not in roots:
            roots.append(common)

        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if not vdf.is_file():
            continue

        try:
            text = vdf.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue

        for raw in re.findall(r'"path"\s+"([^"]+)"', text):
            library = Path(raw.replace("\\\\", "\\")) / "steamapps" / "common"
            if library.is_dir() and library not in roots:
                roots.append(library)

    return roots


def common_roots() -> list[Path]:
    home = Path.home()
    candidates = [
        *steam_library_roots(),
        Path(os.environ.get("PROGRAMFILES", "")) / "Epic Games",
        Path(os.environ.get("PROGRAMFILES(X86)", "")) / "GOG Galaxy" / "Games",
        Path("C:/GOG Games"),
        home / "Downloads",
        home / "Desktop",
        home / "Documents",
    ]

    roots: list[Path] = []
    for candidate in candidates:
        if candidate.is_dir() and candidate not in roots:
            roots.append(candidate)
    return roots


class ScannerApp:
    def __init__(self, window: tk.Tk) -> None:
        self.window = window
        self.window.title("XboxWine 32-bit Game Finder")
        self.window.minsize(980, 580)

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.cancelled = threading.Event()
        self.results: list[Result] = []
        self.scan_thread: threading.Thread | None = None

        outer = ttk.Frame(window, padding=14)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(2, weight=1)

        toolbar = ttk.Frame(outer)
        toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 10))

        ttk.Button(
            toolbar,
            text="Choose folder",
            command=self.choose_folder,
        ).pack(side="left", padx=(0, 8))

        ttk.Button(
            toolbar,
            text="Scan common game folders",
            command=self.scan_common,
        ).pack(side="left", padx=(0, 8))

        self.stop_button = ttk.Button(
            toolbar,
            text="Stop",
            command=self.stop_scan,
            state="disabled",
        )
        self.stop_button.pack(side="left", padx=(0, 8))

        self.export_button = ttk.Button(
            toolbar,
            text="Export CSV",
            command=self.export_csv,
            state="disabled",
        )
        self.export_button.pack(side="left")

        self.status = tk.StringVar(value="Choose a folder to scan.")
        ttk.Label(outer, textvariable=self.status).grid(
            row=1,
            column=0,
            sticky="ew",
            pady=(0, 8),
        )

        columns = ("score", "engine", "subsystem", "size", "path")
        self.table = ttk.Treeview(
            outer,
            columns=columns,
            show="headings",
            selectmode="browse",
        )
        self.table.heading("score", text="Game score")
        self.table.heading("engine", text="Likely engine")
        self.table.heading("subsystem", text="Type")
        self.table.heading("size", text="EXE size")
        self.table.heading("path", text="32-bit executable")

        self.table.column("score", width=90, anchor="center")
        self.table.column("engine", width=130)
        self.table.column("subsystem", width=90)
        self.table.column("size", width=100, anchor="e")
        self.table.column("path", width=540)

        scrollbar = ttk.Scrollbar(
            outer,
            orient="vertical",
            command=self.table.yview,
        )
        self.table.configure(yscrollcommand=scrollbar.set)

        self.table.grid(row=2, column=0, sticky="nsew")
        scrollbar.grid(row=2, column=1, sticky="ns")

        ttk.Label(
            outer,
            text=(
                "High scores are more likely to be the main game executable. "
                "The script reads headers only and never launches a file."
            ),
        ).grid(row=3, column=0, sticky="w", pady=(10, 0))

        self.window.after(100, self.poll_events)

    def choose_folder(self) -> None:
        selected = filedialog.askdirectory(title="Choose a game-library folder")
        if selected:
            self.start_scan([Path(selected)])

    def scan_common(self) -> None:
        roots = common_roots()
        if not roots:
            messagebox.showinfo(
                "XboxWine finder",
                "No common game-library folders were detected.",
            )
            return
        self.start_scan(roots)

    def start_scan(self, roots: list[Path]) -> None:
        if self.scan_thread and self.scan_thread.is_alive():
            return

        self.cancelled.clear()
        self.results.clear()
        self.table.delete(*self.table.get_children())
        self.stop_button.configure(state="normal")
        self.export_button.configure(state="disabled")

        def worker() -> None:
            combined: list[Result] = []
            try:
                for root in roots:
                    if self.cancelled.is_set():
                        break

                    self.events.put(("status", f"Scanning {root}"))

                    found = scan_folder(
                        root,
                        self.cancelled,
                        lambda count, path: self.events.put(
                            (
                                "status",
                                f"Checked {count:,} executables — {path.name}",
                            )
                        ),
                    )
                    combined.extend(found)

                deduplicated = {
                    str(result.path).lower(): result
                    for result in combined
                }
                final = sorted(
                    deduplicated.values(),
                    key=lambda item: (-item.score, str(item.path).lower()),
                )
                self.events.put(("done", final))
            except Exception as error:
                self.events.put(("error", str(error)))

        self.scan_thread = threading.Thread(target=worker, daemon=True)
        self.scan_thread.start()

    def stop_scan(self) -> None:
        self.cancelled.set()
        self.status.set("Stopping…")

    def poll_events(self) -> None:
        try:
            while True:
                event, value = self.events.get_nowait()

                if event == "status":
                    self.status.set(str(value))

                elif event == "done":
                    self.results = list(value)
                    self.populate()
                    self.stop_button.configure(state="disabled")
                    self.export_button.configure(
                        state="normal" if self.results else "disabled"
                    )
                    self.status.set(
                        f"Found {len(self.results):,} 32-bit executables."
                    )

                elif event == "error":
                    self.stop_button.configure(state="disabled")
                    self.status.set("Scan failed.")
                    messagebox.showerror("XboxWine finder", str(value))
        except queue.Empty:
            pass

        self.window.after(100, self.poll_events)

    def populate(self) -> None:
        self.table.delete(*self.table.get_children())

        for result in self.results:
            mib = result.size_bytes / (1024 * 1024)
            self.table.insert(
                "",
                "end",
                values=(
                    result.score,
                    result.engine,
                    result.subsystem,
                    f"{mib:.1f} MB",
                    str(result.path),
                ),
            )

    def export_csv(self) -> None:
        output = filedialog.asksaveasfilename(
            title="Save results",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv")],
            initialfile="xboxwine-32bit-games.csv",
        )
        if not output:
            return

        with Path(output).open("w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                ["score", "architecture", "engine", "type", "exe_size", "path"]
            )
            for result in self.results:
                writer.writerow(
                    [
                        result.score,
                        result.architecture,
                        result.engine,
                        result.subsystem,
                        result.size_bytes,
                        result.path,
                    ]
                )

        messagebox.showinfo("XboxWine finder", f"Saved:\n{output}")


def cli() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "folder",
        nargs="?",
        type=Path,
        help="Folder to scan. Omit it to open the graphical interface.",
    )
    parser.add_argument("--csv", type=Path, help="Optional CSV output path.")
    args = parser.parse_args()

    if args.folder is None:
        window = tk.Tk()
        ScannerApp(window)
        window.mainloop()
        return 0

    cancelled = threading.Event()
    results = scan_folder(
        args.folder,
        cancelled,
        lambda count, path: print(
            f"\rChecked {count:,}: {path.name:<50}",
            end="",
            flush=True,
        ),
    )
    print()

    for result in results:
        print(
            f"[{result.score:>4}] {result.engine:<14} "
            f"{result.subsystem:<10} {result.path}"
        )

    if args.csv:
        with args.csv.open("w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                ["score", "architecture", "engine", "type", "exe_size", "path"]
            )
            for result in results:
                writer.writerow(
                    [
                        result.score,
                        result.architecture,
                        result.engine,
                        result.subsystem,
                        result.size_bytes,
                        result.path,
                    ]
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(cli())
