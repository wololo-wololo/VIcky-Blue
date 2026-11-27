# VIcky-Blue

[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20Linux%20x64%20%7C%20macOS%20x86__64-0078D4.svg)](#system-requirements)
[![Status](https://img.shields.io/badge/status-early%20development-orange.svg)](#current-status)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

**VIcky-Blue is a single-title project.** It is a fork of
[KytyPS5](https://github.com/KytyPS5/KytyPS5), a free and open-source PlayStation 5 translation
layer written in C++, and it exists to get one specific PlayStation 5 game running. It is not a
general-purpose emulator and does not try to be one.

> [!IMPORTANT]
> **The target title is never named in this repository.** Not in code, comments, commit
> messages, branch names, pull requests, issues, discussions, logs or screenshots. Refer to it
> as "the target title". See [CONTRIBUTING.md](CONTRIBUTING.md); the rule is enforced
> automatically and by the maintainers.

## What single-title means here

- **One game decides everything.** Work is prioritised by what the target title needs to boot,
  render and run. A fix is in scope when it moves that title forward.
- **Other games are out of scope.** They may work, since the upstream code is still here, but
  compatibility with them is not tested, not tracked and not a reason to accept or reject a
  change. Regressions in other titles are acceptable when the target title benefits.
- **Title-specific workarounds are welcome.** Temporary hacks, skips and debug switches that
  would not be accepted upstream can land here, as long as they are clearly marked as such.
- **General-purpose improvements belong upstream.** If a change helps every game, please
  contribute it to [KytyPS5](https://github.com/KytyPS5/KytyPS5), where it reaches more people.
  For general emulation, compatibility lists and support, use upstream.

> [!IMPORTANT]
> VIcky-Blue is not affiliated with Sony Interactive Entertainment, PlayStation, or the
> publisher or developer of any game. The project does not distribute games or copyrighted
> system software, and **no game files are accepted in this repository**. Use only game files
> that you have obtained legally.

## Current Status

**Stage: reaches the intro sequence, slowly and with an incomplete image.** The target title boots,
shows its loading logo at full speed and then starts rendering the 3D intro scene. The image is
dark and partly missing, and the intro runs at about 0-1 fps on a first boot while shaders are
compiled.

Known state:

- The intro only starts with three temporary debug workarounds, none of them a fix:
  - `KYTY_RAW_PATCH=847c4d7:7413:9090,847c411:0f83cd000000:909090909090` disables the title's
    main-loop watchdog, which otherwise ends the title after a 30 s stall. First-boot shader
    compilation (about 750 shaders in one burst, ~36 ms each) stalls the main loop for longer
    ([#14](https://github.com/wololo-wololo/VIcky-Blue/issues/14)).
  - `KYTY_SKIP_SHADERS=05dc60d7ff9e4fce` drops one compute shader that makes the host GPU report
    `VK_ERROR_DEVICE_LOST` on its larger dispatches. Vulkan validation and GPU-assisted validation
    report nothing for it. `KYTY_GPU_FAULT_TRACE=1` is the trace that identified it
    ([#13](https://github.com/wololo-wololo/VIcky-Blue/issues/13)).
  - `--shader-cache` keeps compiled shaders between boots. A dump without `sce_sys/param.json`
    has no title id and stores them under the name `UNKNOWN`
    ([#27](https://github.com/wololo-wololo/VIcky-Blue/issues/27)). The first boot still compiles
    everything; with a complete cache the warm-up before the first frame took 30 s.
- Compute dispatches whose shader resources cannot be tracked or specialized are skipped by a
  temporary workaround ([#2](https://github.com/wololo-wololo/VIcky-Blue/issues/2)). Draws used
  to skip on specialization failures too
  ([#15](https://github.com/wololo-wololo/VIcky-Blue/issues/15)), which was part of why the image
  was incomplete.
  - GPU-selected descriptors read with typed (`tbuffer_load_format_x/xy/xyz/xyzw`) loads now
    translate. In a boot to the intro the number of distinct skipped shaders went from 231 to
    26 (4 resource tracking failures, 22 specialization failures). What is left on the tracking
    side is a store or atomic on a GPU-selected descriptor
    ([#2](https://github.com/wololo-wololo/VIcky-Blue/issues/2)).
  - Raw indexed stores and 32-bit atomic add/or on GPU-selected descriptors with a bounded key,
    and descriptors whose base is picked by a scalar branch on a value read through the user-data
    pointer, now track. A boot to the intro no longer reports any resource tracking failure; what
    is left are the specialization failures
    ([#3](https://github.com/wololo-wololo/VIcky-Blue/issues/3)).
  - The indirect buffer tables that failed specialization (store records, tables of 256 or more
    candidates, data rows next to descriptors) now materialize. In a 5-minute run into play no
    compute dispatch is skipped; the 9 shaders still skipped are draws with image atomics on
    formats 5 and 64, which specialization does not support yet
    ([#15](https://github.com/wololo-wololo/VIcky-Blue/issues/15), [#23](https://github.com/wololo-wololo/VIcky-Blue/issues/23)). The shaders that were skipped
    are large (about 363k SPIR-V words on average, 131 of 195 over 400k), so a first boot takes
    much longer to compile them ([#14](https://github.com/wololo-wololo/VIcky-Blue/issues/14)).
  - Indirect buffer tables of up to 524288 rows now specialize, stride-one byte-bounds
    descriptors are accepted as candidates for one-byte formats (SInt8 and UInt8 stores), and a
    cached program whose descriptors later stop specializing is skipped with a log line instead
    of aborting. Two of the shaders named in
    [#3](https://github.com/wololo-wololo/VIcky-Blue/issues/3) are still skipped; the remaining
    reasons are atomic image formats ([#15](https://github.com/wololo-wololo/VIcky-Blue/issues/15)),
    formatted stores on wider strides, and pointer-table probes that read unmapped rows. A run
    with these changes stayed in the intro for eight minutes without a guest fault; an earlier
    version of the byte-bounds change let 2- and 4-byte formats address unaligned words and ended
    the title with an audio decoder fault after two to three minutes.
  - Image atomics on 8- and 16-bit images and 64-bit atomics on 32_32Float images now specialize
    ([#23](https://github.com/wololo-wololo/VIcky-Blue/issues/23), [#33](https://github.com/wololo-wololo/VIcky-Blue/issues/33)).
    The 8- and 16-bit images run their atomics on an R32_UINT shadow image that is filled before
    each draw or dispatch and written back after it; this costs two extra compute copies per such
    draw. A run into the intro no longer reports an "atomic image descriptor ... uses unsupported
    format" skip. Multisampled 8- and 16-bit atomic images are still rejected by specialization.
    A run into the intro sequence now hits zero `TEMPORARY DRAW SKIP` or
    `TEMPORARY COMPUTE DISPATCH SKIP` lines, and graphics resource specialization failures are
    fatal again ([#15](https://github.com/wololo-wololo/VIcky-Blue/issues/15)); the compute
    specialization skip stays a non-fatal workaround.
- Two system library functions the title calls are not implemented yet
  ([#5](https://github.com/wololo-wololo/VIcky-Blue/issues/5), [#6](https://github.com/wololo-wololo/VIcky-Blue/issues/6)).
- The title needs `KYTY_LARGE_MEMORY=1` set to get enough guest memory ([#9](https://github.com/wololo-wololo/VIcky-Blue/issues/9)).

Resolved since the last status: the shader decode abort right after the loading logo
([#1](https://github.com/wololo-wololo/VIcky-Blue/issues/1), fixed in `f5d84d2b`), which was the previous boot blocker, and the GPU flip
events that never arrived ([#7](https://github.com/wololo-wololo/VIcky-Blue/issues/7)), which went away with it.

Windows is where the fork is developed and tested. The Linux and macOS code paths are inherited
from upstream and are not tested here.

## Help wanted

Everything that needs doing is tracked as an issue, grouped into milestones:

- [Open issues](https://github.com/wololo-wololo/VIcky-Blue/issues), with [`blocker`](https://github.com/wololo-wololo/VIcky-Blue/labels/blocker) marking what stops progress
  right now and [`good first issue`](https://github.com/wololo-wololo/VIcky-Blue/labels/good%20first%20issue) marking easy ways in
- [Milestones](https://github.com/wololo-wololo/VIcky-Blue/milestones): first frame, menus, in-game

Each issue says what the goal is, where to look in the code and when it counts as done, and has
a branch ready to work on. [CONTRIBUTING.md](CONTRIBUTING.md#joining-in) explains how to pick
one up.

## Bugs and Issues

Report only problems that affect the target title, include the commit you tested, and scrub the
title's name and any dump paths from logs and screenshots before attaching them. Never attach
game files.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. In short:

- never name the target title or its series, anywhere;
- never commit, attach or link game files, dumps, firmware or keys;
- AI-assisted contributions are allowed.

Keep each pull request focused on one problem the target title hits. Describe the exact
problem and show evidence of what the change fixes, such as a scrubbed log excerpt. Changes
with no benefit to the target title should go upstream instead.

### Formatting

Set up the clang-format hook after cloning.

Install `pre-commit` using the method appropriate for your platform:

- **Arch Linux / CachyOS:** `sudo pacman -S pre-commit`
- **Other Linux / macOS / Windows:** `python -m pip install pre-commit`

Then install the Git hook:

```bash
python -m pre_commit install --install-hooks
```

It formats staged `.cpp`, `.h`, and `.inc` files in `src`.

## Developer Information

The PS5 graphics architecture is based on AMD RDNA 2. Use AMD's
[RDNA 2 Instruction Set Architecture Reference Guide (document 70648)](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
as the primary instruction-encoding reference when working on shader decoding and recompilation.

Important areas of the codebase:

- [`src/graphics/shader/recompiler`](src/graphics/shader/recompiler) — instruction decoding,
  intermediate representation, control flow, resource tracking, and SPIR-V emission
- [`src/graphics/guest_gpu`](src/graphics/guest_gpu) — PS5 (Prospero) GPU formats and command processing
- [`src/graphics/host_gpu`](src/graphics/host_gpu) — Vulkan host backend and resource management
- [`tests`](tests) — focused memory, shader, and resource-tracking regression tests

The renderer targets Vulkan 1.3. Keep shader changes aligned with both the RDNA 2 ISA semantics and
the Vulkan/SPIR-V validation rules.

## Building

### System requirements

- Windows 10 version 1803, a current Linux distribution, or macOS on Apple Silicon
- A 64-bit x86 processor (on macOS, an Apple Silicon processor with Rosetta 2)
- A Vulkan 1.3-capable GPU with current drivers (on macOS, Vulkan is provided by the bundled
  MoltenVK)

### Build requirements (Windows)

- Git
- CMake 3.22.1 or newer
- Ninja
- Visual Studio 2022 or Build Tools 2022 with the **Desktop development with C++** workload and
  **C++ Clang tools for Windows** component
- Qt 6 for MSVC 2022 64-bit, including Concurrent, Network, and Widgets
- [glslang](https://github.com/KhronosGroup/glslang/releases) (`glslang` or `glslangValidator`) on `PATH`
- Python 3 on `PATH`; the bundled SPIRV-Tools runs it at configure time

The Microsoft C++ compiler (`cl.exe`) is not supported; use `clang-cl`.

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** (or the equivalent Developer
PowerShell), change to the repository root, and initialize the dependencies:

```powershell
git submodule update --init --recursive
```

Configure the project. Replace the Qt path with the version installed on your system:

```powershell
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
```

Build the launcher and stage a runnable installation:

```powershell
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

The finished application and its runtime dependencies will be placed in
`_Build/windows/install`.

### Building on Linux

Install the toolchain and the libraries the bundled SDL3 needs. Without the audio, Wayland, and
udev development packages, SDL3 quietly configures itself without those backends, and the resulting
build has no working audio or gamepad hotplug support:

```bash
sudo apt-get install --no-install-recommends \
  clang lld ninja-build cmake git glslang-tools python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxrandr-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libwayland-dev wayland-protocols
```

Qt 6 (Concurrent, Network, and Widgets) is required for the launcher. Use your distribution's
packages (`qt6-base-dev`) or an official Qt installation.

```bash
git submodule update --init --recursive

cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

The install step copies the Qt libraries and plugins next to the binaries, so
`_Build/linux/install` runs without a matching system Qt installation. FFmpeg is linked statically
from the pinned [KytyPS5 FFmpeg core](https://github.com/KytyPS5/ext-ffmpeg-core)
release, including VP9 and WebM support. System FFmpeg packages are not required.

To build `kyty_emulator` and the `kyty_tests` target without Qt, use a separate build directory:

```bash
git submodule update --init --recursive

cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF

cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests --parallel
```

As on Windows, the MSVC compiler is not used; Clang is required. `cl.exe` is rejected at configure
time.

The CMake source root is the repository root.

### Building on NixOS

A development shell provides Clang, CMake, Ninja, Qt 6, the Vulkan headers, and the SDL3 backend
libraries. Enter it and configure exactly as on other Linux distributions; the shell exports
`CMAKE_PREFIX_PATH` and `QT_PLUGIN_PATH`, so the `-DCMAKE_PREFIX_PATH="$Qt6_DIR"` argument is not
needed:

```bash
nix-shell # or: nix develop
git submodule update --init --recursive

cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

The configure step downloads the FFmpeg prebuilts and the `xbyak`, `zydis`, `zstd`, and ZArchive
sources, so it needs network access; a fully sandboxed `nix build` would require vendoring those
inputs. A Vulkan 1.3 driver must be available at runtime (on NixOS,
`hardware.graphics.enable = true`).

### Building on Fedora and Fedora Atomic (Bazzite, Bluefin, Silverblue)

On regular Fedora, install dependencies on the host. On Fedora Atomic variants such as Bazzite,
Bluefin and Silverblue, use [Distrobox](https://distrobox.it/) and run the install and build steps
inside the container. Keep the checkout in your home directory so it is shared with the container.
Add `--nvidia` to the create command for hosts using the proprietary NVIDIA driver:

```bash
distrobox create --name kyty-build --image registry.fedoraproject.org/fedora:latest
distrobox enter kyty-build
```

Install the toolchain, Qt 6 and SDL3 dependencies:

```bash
sudo dnf install -y \
  clang lld ninja-build cmake git glslang python3 pkgconf-pkg-config \
  mesa-libGL-devel libX11-devel libXcursor-devel libXext-devel libXfixes-devel \
  libXi-devel libXrandr-devel libXScrnSaver-devel libXtst-devel libxkbcommon-devel \
  alsa-lib-devel pulseaudio-libs-devel systemd-devel dbus-devel \
  wayland-devel wayland-protocols-devel qt6-qtbase-devel
```

From the repository root, follow the launcher configure, build and install steps in
[Building on Linux](#building-on-linux), omitting `-DCMAKE_PREFIX_PATH="$Qt6_DIR"`; CMake finds
Fedora's packaged Qt 6 automatically.

Run `./_Build/linux/install/kyty_launcher` on regular Fedora or inside the container. To launch a
container build from the host, leave the container with `exit`, then run from the repository root:

```bash
distrobox enter kyty-build -- ./_Build/linux/install/kyty_launcher
```

Qt libraries and plugins are bundled. You can also launch directly on a Fedora Atomic host
if its other system libraries are compatible with the container build.

### Building on macOS

macOS builds target x86-64 and run under Rosetta 2 on Apple Silicon, so the PS5's x86-64 game
code executes through the same translation layer as the emulator itself. Prebuilt archives are
attached to releases; the steps below are for building from source.

Requirements:

- An Apple Silicon Mac with Rosetta 2 installed (`softwareupdate --install-rosetta`)
- Xcode (or the Command Line Tools)
- Homebrew packages: `brew install cmake ninja glslang python`
- Qt 6 (Concurrent, Network, and Widgets) with x86-64 support. The official Qt installation is
  universal and works; Homebrew's Qt is arm64-only and will not link

```bash
git submodule update --init --recursive

cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/macos --target launcher --parallel
cmake --install _Build/macos --prefix _Build/macos/install
```

The build re-signs `kyty_emulator` with the JIT entitlements it needs to execute translated
guest code; no manual signing step is required. When the launcher is built, the installation
also produces `_Build/macos/install/KytyPS5.app` — double-click to launch the GUI.
A flat `kyty_emulator` is kept for CLI usage.

Vulkan comes from MoltenVK. Download `MoltenVK-macos.tar` from the
[MoltenVK releases](https://github.com/KhronosGroup/MoltenVK/releases), then copy
`MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` next to the flat `kyty_emulator`
(and, for the bundle, into `KytyPS5.app/Contents/Frameworks/`) and sign it with an ad hoc signature:

```bash
codesign --force --sign - _Build/macos/install/libMoltenVK.dylib
# For the bundle (if present):
codesign --force --sign - _Build/macos/install/KytyPS5.app/Contents/Frameworks/libMoltenVK.dylib
codesign --force --sign - _Build/macos/install/KytyPS5.app
```

Release archives already include a signed `libMoltenVK.dylib` (both flat and inside the bundle).

### Regression tests

Build every regression executable and run the registered tests with:

```powershell
cmake --build _Build/windows --target kyty_tests
ctest --test-dir _Build/windows --output-on-failure
```

Use `_Build/linux` instead of `_Build/windows` for a Linux build.

### Visual Studio Code

A ready-made Visual Studio Code setup is included in [`.vscode`](.vscode). It configures CMake
Tools to build the project with Ninja and `clang-cl` and provides launch profiles for both
`kyty_launcher.exe` and `kyty_emulator.exe`. It is Windows-only: VS Code settings cannot select a
compiler per platform, so on Linux configure from the command line as shown above.

Before using it:

1. Install the **CMake Tools** and **C/C++** extensions in Visual Studio Code.
2. Update `CMAKE_PREFIX_PATH` in [`.vscode/settings.json`](.vscode/settings.json) to point to your
   Qt 6 MSVC installation.
3. Update the `--game` path in [`.vscode/launch.json`](.vscode/launch.json) for the
   **Debug kyty_emulator** profile.
4. Open the repository in an x64 Visual Studio developer environment, configure the CMake project,
   and select a launch profile from **Run and Debug**.

## Running

Update your graphics driver before reporting rendering problems.

To use the graphical launcher:

```powershell
.\_Build\windows\install\kyty_launcher.exe
```

```bash
./_Build/linux/install/kyty_launcher
```

```bash
open _Build/macos/install/KytyPS5.app  # or double-click in Finder
```

On first launch, add one or more game folders in the global settings. The launcher searches those
folders recursively for game directories containing `eboot.bin` and ZArchive (`.zar`) game dumps
whose archive root contains `eboot.bin`. Select a detected game and run it from the game list.
ZArchive dumps are mounted read-only and streamed directly; they do not need to be extracted first.

Official release builds include a built-in updater. Leave **Check for updates on startup** enabled,
or click **Check for updates**, then choose **Install Update**. The launcher downloads the package
for your platform from GitHub, checks its SHA-256 checksum, and restarts after installation. Close
any running games first. Settings, saves, patches, and other files outside the release package are
preserved; replacement failures restore the previous files. The installation folder must be writable.
If a package cannot be installed automatically, the updater offers the GitHub release page.
Source builds and builds from forks do not replace themselves with official releases.

The emulator can also be started directly with a legally obtained game directory, ELF file, or
ZArchive dump:

```powershell
.\_Build\windows\install\kyty_emulator.exe --game "D:\Games\ExampleGame"
.\_Build\windows\install\kyty_emulator.exe --game "D:\Games\ExampleGame.zar"
```

```bash
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame"
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame.zar"
```

On macOS, the adjacent flat or app-bundled `libMoltenVK.dylib` is found automatically; no
environment variable is required:

```bash
./_Build/macos/install/kyty_emulator --game "/games/ExampleGame"
```

To override the Vulkan loader, set `SDL_VULKAN_LIBRARY`:

```bash
SDL_VULKAN_LIBRARY=/path/to/libMoltenVK.dylib ./kyty_emulator --game "/games/ExampleGame"
```

Run `kyty_emulator --help` to see the available graphics, logging, validation, profiling, and
debugging options.

### AI Use

AI tools may be used freely, including for writing code, tests, documentation, commit messages
and pull-request descriptions. Contributors must understand, review and test everything they
submit and remain responsible for its correctness. Everything else in
[CONTRIBUTING.md](CONTRIBUTING.md) applies to AI-assisted work as well.

## License

VIcky-Blue, like KytyPS5, is licensed under the [GNU General Public License version 2](LICENSE)
(`GPL-2.0-only`).

This project is based on the original [Kyty](https://github.com/InoriRus/Kyty), which was released
under the MIT License. Kyty's original copyright and license notice are preserved in
[`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt). Third-party components remain subject to the
licenses included with those components.

## Special Thanks

- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — KytyPS5 is based on a heavily modified version
  of the original Kyty project.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for understanding PS4
  memory behavior, GPU resource aliasing, cache coherency,
  and the AVPlayer implementation.
