Sidecar for macOS (11 Big Sur or newer; Apple silicon and Intel)
================================================================

Install: drag Sidecar.app onto the Applications folder.

First launch: Sidecar is not signed with an Apple Developer ID or notarized (it's a
free hobby project), so macOS blocks a plain double-click. Instead:

  * Right-click (or Control-click) Sidecar in Applications, choose Open, then Open again.
  * On macOS 15 Sequoia and newer: open it once, then go to System Settings >
    Privacy & Security, scroll down and click "Open Anyway" next to Sidecar.
  * Or, in Terminal:  xattr -dr com.apple.quarantine /Applications/Sidecar.app

macOS then remembers the choice. Allow microphone access when asked: it's how macOS
lets Sidecar read your audio interface's inputs.

Audio: CoreAudio. Choose the device in SETUP > AUDIO.
