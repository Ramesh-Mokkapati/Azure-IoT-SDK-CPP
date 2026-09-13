# Azure IoT SDK setup

This solution now fetches and builds the Azure IoT C SDK **automatically** the
first time you build it in Visual Studio — no manual vcpkg install, no
`VcpkgRoot` environment variable, no `vcpkg integrate install`.

## How it works

- `vcpkg.json` still declares the dependency (`azure-iot-sdk-c`).
- `Directory.Build.props` now points `VcpkgRoot` at a `vcpkg\` folder *inside
  this repo* (instead of requiring a system-wide vcpkg install / env var).
- `Directory.Build.targets` adds a pre-build step that, if the SDK headers
  aren't found yet, runs `scripts\bootstrap-vcpkg.ps1`, which:
  1. Clones `microsoft/vcpkg` into `vcpkg\` (needs Git on PATH).
  2. Bootstraps it (`bootstrap-vcpkg.bat`).
  3. Runs `vcpkg install` for the manifest, building `azure-iot-sdk-c` for the
     active triplet (`x64-windows` / `x86-windows`).
- The two sample projects link against whatever ends up in
  `vcpkg\installed\<triplet>\lib\*.lib` and copy the matching DLLs next to the
  built `.exe` after linking.

## What this means for a fresh machine

Just copy/clone the whole `Azure IoT` folder and open the `.sln` in Visual
Studio 2022 with the "Desktop development with C++" workload installed, then
Build. Requirements on the target machine:

- Visual Studio 2022 (v143 toolset) with the C++ workload
- Git (for cloning vcpkg itself)
- Internet access (vcpkg downloads/builds the SDK's dependencies)

**The first build will be slow** (it's compiling the SDK, uAMQP, uMQTT,
etc. from source — this can take 10–20+ minutes). Every build after that is
fast, since the bootstrap step short-circuits once
`vcpkg\installed\<triplet>\include\azureiot\iothub.h` exists.

## Notes

- `vcpkg\` is git-ignored — it's meant to be re-downloaded per machine, not
  committed.
- If you'd rather point at a shared/pre-existing vcpkg instance (e.g. a team
  cache), just set a `VcpkgRoot` environment variable before opening Visual
  Studio — `Directory.Build.props` will use that instead of the local clone.
