# Security policy

## Supported versions

Sidecar is a beta hobby project. Fixes go into the latest release only.

## Reporting a vulnerability

Please **don't open a public issue** for a security problem. Report it privately
through GitHub: on the repository's **Security** tab, choose **Report a
vulnerability** (<https://github.com/Audbol/Live-Daw/security/advisories/new>).
If that isn't available, open an issue that only asks for a private contact, without
details.

Please include the Sidecar version, your OS, what an attacker can do and how to
reproduce it. You'll get an answer as soon as possible; as this is a spare-time
project, that may take a few days.

## What to keep in mind

- The **WING remote servers** (SETUP > REMOTE > WING REMOTE, off unless you enable
  them) speak the WING's network protocols, which have no authentication or
  encryption: anyone who can reach the computer on UDP 2222 / 2223 and TCP 2222 can
  control the console. Enable them only on a trusted network and keep them behind a
  firewall.
- **Plugins** (VST3, AU, LV2) run inside Sidecar with your user's rights. Only load
  plugins you trust.
- Show, preset, snapshot and audio files are parsed by Sidecar; a file that crashes
  or misleads the parser is a bug worth reporting.
