Sidecar for Linux (x86_64)
==========================

Running
-------
AppImage:  chmod +x Sidecar-*.AppImage && ./Sidecar-*.AppImage
Tarball:   tar xzf Sidecar-*-Linux-x86_64.tar.gz && ./Sidecar-*-Linux-x86_64/Sidecar

Built on Ubuntu 24.04, so it needs glibc 2.38 or newer (Ubuntu 24.04+, Debian 13+,
Fedora 39+ and similar). The AppImage needs FUSE (fusermount3, package "fuse3"), or run
it with --appimage-extract-and-run.

Runtime libraries
-----------------
Sidecar uses the system's audio and font libraries; they are installed on almost every
desktop. If it doesn't start, install them (Debian/Ubuntu package names):

  sudo apt install libasound2t64 libfreetype6 libfontconfig1 libx11-6 libxext6 \
                   libxrandr2 libxinerama1 libxcursor1 libxrender1 libxcomposite1

  (libasound2 instead of libasound2t64 before Ubuntu 24.04 / Debian 13.)

Audio
-----
ALSA works out of the box. For JACK (or PipeWire's JACK layer, pipewire-jack), install
libjack-jackd2-0 or pipewire-jack; Sidecar loads libjack at run time when it's there.
Choose the driver and device in SETUP > AUDIO. For low latency, add yourself to the
"audio" group (or use a realtime-enabled PipeWire/JACK setup).

Menu entry (tarball)
--------------------
  mkdir -p ~/.local/bin ~/.local/share/applications ~/.local/share/icons/hicolor/256x256/apps
  cp Sidecar SidecarPluginHost ~/.local/bin/
  cp sidecar.desktop ~/.local/share/applications/
  cp sidecar.png ~/.local/share/icons/hicolor/256x256/apps/

(~/.local/bin must be on your PATH for the menu entry's "Exec=Sidecar". SidecarPluginHost,
the plugin sandbox helper, must stay next to Sidecar.)
