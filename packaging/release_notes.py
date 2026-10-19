#!/usr/bin/env python3
"""Writes the GitHub release notes for a tag to stdout.

    python3 packaging/release_notes.py <tag> [<commit>]

The notes start with CHANGELOG.md's section for the version, if there is one
(a heading like "## [0.1.0]", "## 0.1.0 - 2026-10-19" or "## v0.1.0"), then the
per-platform install instructions, the disclaimer and the licence.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def changelog_section(version):
    path = ROOT / "CHANGELOG.md"
    if not path.is_file():
        return None
    heading = re.compile(r"^##\s+\[?v?" + re.escape(version) + r"\]?(\s|$)")
    lines, taking = [], False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            if taking:
                break
            taking = bool(heading.match(line))
            continue
        if taking:
            lines.append(line)
    text = "\n".join(lines).strip()
    return text or None


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: release_notes.py <tag> [<commit>]")
    tag = sys.argv[1]
    commit = sys.argv[2][:7] if len(sys.argv) > 2 else ""
    version = tag[1:] if tag[:1] in "vV" else tag
    built = f", built and tested by CI from {commit}" if commit else ""

    intro = changelog_section(version) or (
        f"Sidecar {version}{built}: a live mixing console for Windows, macOS and Linux "
        "that emulates the Behringer WING. See README.md for what it does."
    )

    print(f"""{intro}

## Download and install

| Platform | File |
| --- | --- |
| Windows 10/11 (64-bit) | `Sidecar-{version}-Windows-x64-Setup.exe` (installer) or `Sidecar-{version}-Windows-x64.zip` (portable) |
| macOS 11+ (Apple silicon and Intel) | `Sidecar-{version}-macOS.dmg` |
| Linux x86_64 (glibc 2.38+, e.g. Ubuntu 24.04) | `Sidecar-{version}-Linux-x86_64.AppImage` or `Sidecar-{version}-Linux-x86_64.tar.gz` |

`SHA256SUMS.txt` lists the files' checksums (`sha256sum -c SHA256SUMS.txt --ignore-missing`).

**Windows:** run the installer (Start-menu entry and uninstaller), or unzip the portable build
and run `Sidecar.exe`. No runtime to install. The files aren't code-signed, so SmartScreen may
warn; choose *More info > Run anyway*. Audio: ASIO (your interface's driver, ASIO4ALL or
FlexASIO) and WASAPI.

**macOS:** open the disk image and drag Sidecar to Applications. Sidecar isn't notarized, so the
first time right-click it and choose *Open* (on macOS 15+: try to open it, then *System Settings >
Privacy & Security > Open Anyway*), or run `xattr -dr com.apple.quarantine /Applications/Sidecar.app`.
Allow microphone access so it can read your interface's inputs. Audio: CoreAudio.

**Linux:** `chmod +x` the AppImage and run it (needs FUSE; otherwise add `--appimage-extract-and-run`),
or unpack the tarball and run `./Sidecar`. It uses the system's ALSA, FreeType, Fontconfig and X11
libraries; JACK (or pipewire-jack) is used when installed. See README-LINUX.txt in the tarball.

Choose the audio driver and device in SETUP > AUDIO.

## About

Sidecar is an independent hobby project, made for fun and practice. It is not
affiliated with or endorsed by Behringer or Music Tribe; WING is their trademark.
It tries hard to emulate the WING faithfully, but it is incomplete and can differ
from the real console (the start-up notice lists how). Don't rely on it for a
critical live show without testing it first.

Sidecar is free software under the GNU Affero General Public License v3 (see LICENSE;
third-party licences in THIRD_PARTY_NOTICES.md).""")


if __name__ == "__main__":
    main()
