# Contributing to Sidecar

Thanks for helping. Sidecar is a hobby project, so replies may take a while, but bug
reports, tests with real audio interfaces and controllers, and pull requests are all
welcome.

## Reporting bugs and ideas

Use the [issue templates](https://github.com/Audbol/Live-Daw/issues/new/choose).
For a bug, say which OS, audio interface, driver, sample rate and buffer size you
used, and what the event log (SETUP > LOG) shows. Security problems: see
[SECURITY.md](SECURITY.md).

## Building and testing

See [Building from source](README.md#building-from-source). Before opening a pull
request, run:

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure

cd rust
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
```

The C++ build must stay free of warnings (JUCE's recommended warning flags are on).
CI builds and tests Windows, macOS and Linux, and runs the tests under
AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Code guidelines

- **The audio thread is sacred:** no allocations, locks, file or network I/O, or
  logging on it. Structural changes go through the engine's lock-free swaps. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- DSP, mixing and session logic belong in the Rust crates (`rust/`); the C++ side
  hosts devices and plugins and draws the UI. The C ABI is in
  `rust/crates/livedaw-ffi/include/livedaw_engine.h`.
- Add tests for behaviour you add or fix (Rust tests next to the code or in
  `tests/`, C++ tests in `tests/`).
- Rust: `rustfmt` with the repository's `rust/rustfmt.toml`, clippy clean, no new
  third-party crates without discussing it first. C++: follow the surrounding style
  (JUCE style, C++20).
- Follow the WING: screens, names and behaviour should match the console. Where
  Sidecar must differ, say so in the user guide or the known limitations.
- Keep the docs in step: `README.md`, `docs/USER_GUIDE.md`, `docs/ARCHITECTURE.md`
  and, for limitations, the start-up notice (`src/ui/StartupNotice.cpp`).

## Clean-room rule

Sidecar emulates the WING from public information only: the WING's manuals and
protocol documents as references, open-source projects (with their licences
respected and credited in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), and
observing the console's behaviour.

- **Never add Behringer or Music Tribe firmware, binaries, software (such as WING
  Edit), decompiled or disassembled code, manual pages, screenshots, logos or other
  artwork** to the repository, and don't base code on them.
- Parameter names, paths and ranges needed for interoperability are fine; say where
  they come from.
- Code or data from other projects needs a compatible licence (AGPLv3-compatible)
  and an entry in `THIRD_PARTY_NOTICES.md`.

## Licence

By contributing, you agree that your contributions are licensed under the GNU AGPLv3,
the licence of the project ([LICENSE](LICENSE)).
