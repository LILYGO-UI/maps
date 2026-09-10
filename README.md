# LILYGO Maps

**English** | [简体中文](README.zh-CN.md)

LILYGO Maps is an online map viewer for LILYGO UI, built with C++17,
[LILYGO UI AppKit](https://github.com/LILYGO-UI/cm0-appkit) and LVGL 9.5.
It opens near your approximate public-IP location, lets you browse and search
OpenStreetMap, and caches downloaded tiles on the device for reuse.

The project provides a CM0 ARM64 Linux build and an SDL2 desktop simulator
for development and testing.

## Preview

<p>
  <img src="assets/previews/maps.png" alt="Map centered on the current IP location" width="240">
  <img src="assets/previews/maps-search.png" alt="Address search result for Shanghai Pudong" width="240">
</p>

## Features

- **Map browsing:** drag to pan and use zoom controls to explore OpenStreetMap.
- **Startup location:** locate the device by public IP, with fallback providers
  and a world view when lookup fails. Return to the last successful IP location
  with the location button.
- **Address search:** find places with OpenStreetMap Nominatim, including Chinese
  and English address queries.
- **Local tile cache:** reuse downloaded tiles, view cache usage, and clear the
  cache after confirmation. Previously cached areas remain available offline.
- **Responsive interface:** portrait and landscape layouts, an on-screen keyboard,
  and map attribution.
- **Background work:** location lookup, search, and tile downloads run on worker
  threads to keep the interface responsive.

IP location is approximate and can be affected by a VPN or proxy. New tiles,
location lookup, and address search require internet access. See
[services and cache configuration](docs/02-services-and-cache.md) for details.

## Development Environment

Project identity, version, and package metadata come from [`lpm.toml`](lpm.toml).
[`CMakePresets.json`](CMakePresets.json) defines the `host-simulator` and
`cm0-cross` configurations. Run all project commands from the repository root.

### Get the Source

```sh
git clone --recurse-submodules https://github.com/LILYGO-UI/lilygo-ui-maps.git
cd lilygo-ui-maps
```

For an existing checkout, initialize the pinned dependencies:

```sh
git submodule update --init --recursive
```

AppKit is consumed directly from `third_party/cm0-appkit`; a separate AppKit
installation is not required for the simulator. Its bundled LVGL 9.5.0 archive
is used by default, with network fetching disabled in both presets.

### Host Dependencies

Use CMake 3.21 or later (including CTest and CPack), Python 3.11 or later,
and a C/C++ compiler with C++17 support. The presets use Unix Makefiles;
AppKit also requires `patch` to apply its bundled LVGL patches.
SDL2 supplies the host display, FreeType supplies font rendering,
and json-c parses service responses. Network requests use the `curl` executable,
so libcurl development headers are not required.

#### Debian 12 / Ubuntu 24.04 or Later

```sh
sudo apt update
sudo apt install build-essential cmake git pkg-config patch libsdl2-dev \
  libfreetype6-dev libjson-c-dev curl ca-certificates python3 python3-venv python3-pip
```

On older distributions, upgrade CMake and Python to the minimum versions above.

#### macOS

Install the Xcode Command Line Tools if needed and dependencies through
[Homebrew](https://brew.sh/). Complete the Homebrew shell setup so that `brew`
and its installed tools are on `PATH`:

```sh
xcode-select --install
brew install git cmake pkg-config patch sdl2 freetype json-c curl python
```

#### LPM

CMake invokes `lpm metadata --format cmake` during configuration. Install LPM
in a project-local environment using Python 3.11 or later:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install "git+https://github.com/LILYGO-UI/lpm.git"
lpm --version
```

Reactivate the environment with `. .venv/bin/activate` in each new terminal
before running `lpm`. CMake searches `.venv/bin` as well as `PATH`. An existing
LPM installation can also be selected with
`-DLPM_EXECUTABLE=/absolute/path/to/lpm` during configuration.

Check the host environment before building:

```sh
cmake --version
python --version
pkg-config --modversion sdl2 freetype2 json-c
lpm metadata --format cmake
```

### Device Cross Compiler

Skip this step when only building the desktop simulator. Device builds need
a separate AArch64 Linux cross compiler on the development machine.

**Debian / Ubuntu:**

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

**Apple Silicon macOS:**

```sh
brew install aarch64-unknown-linux-gnu
```

The BSP toolchain expects these executable names on `PATH`:

```sh
aarch64-linux-gnu-gcc --version
aarch64-linux-gnu-g++ --version
```

See [CM0BspBuilder](https://github.com/LILYGO-UI/CM0BspBuilder) for SDK host
and compiler requirements. The BSP supplies target headers and libraries but
does not install the host cross compiler.

## Build and Test

After completing the environment setup, use LPM from the repository root.
It reads the build presets and package output directory from `lpm.toml`.

### Desktop Simulator

```sh
lpm start --foreground
```

This configures and builds the simulator, then starts it in the foreground.
To reuse an existing build, run `lpm start --no-build --foreground`.
The Debug build has tests enabled and is stored in `build/host-simulator`.
Running the application requires a graphical desktop session. Online operation
also requires working DNS, internet access, `curl` on `PATH`, and CA certificates.
See [services and cache](docs/02-services-and-cache.md) for offline operation.

### Tests

Configure and build the host simulator, then run all tests:

```sh
lpm test
```

| Test | Coverage |
| --- | --- |
| `maps-core` | Projection, date-line wrapping, visible tiles, PNG validation, and cache operations |
| `maps-location` | IP provider fallback, invalid responses, and HTTP protocol restrictions |
| `maps-view-model` | User commands and asynchronous state updates |
| `maps-render-portrait` | Rendering and main interactions at `568 x 1232` |
| `maps-render-landscape` | Rendering and main interactions at `1232 x 568` |

Render tests write `maps-portrait.ppm` and `maps-landscape.ppm` under
`build/host-simulator`. Inspect both after UI changes; passing assertions alone
does not verify the visual layout. Follow the
[UI development guidelines](docs/01-user-interface.md) for UI acceptance requirements.

### CM0 Device Build

The device preset targets CM0 Raspberry Pi OS on AArch64, uses a Release build,
disables host tests, and sets the installation prefix to `/usr`.
Cross-compile and create the Debian package:

```sh
lpm pack
```

The first `lpm pack` (or `cmake --preset cm0-cross`) uses the
[toolchain wrapper](cmake/cm0-bsp-toolchain.cmake) to download CM0 BSP
`0.1.0`, verifies its pinned SHA-256 checksum, and extracts the sysroot to
`.cache/cm0-bsp/0.1.0/sysroot`. The first download requires access to GitHub;
later configurations reuse this cache. To change
the cache location, pass `-DCM0_BSP_CACHE_DIR=/absolute/path/to/cache` when
configuring. No sibling CM0BspBuilder checkout is required.

Cross compilation needs target libraries and headers, including json-c,
FreeType, and DRM, in the SDK sysroot. Host libraries cannot substitute for
missing target dependencies.

CPack writes an ARM64 `.deb` to `dist/`. The package installs the executable,
launcher entry and icon, desktop entry, AppStream metadata, and shared-font
license notices. The target system must provide the declared dependencies,
including `lilygo-ui-appkit-dev >= 0.1.0`, json-c, `curl`, and CA certificates.
AppKit supplies shared runtime fonts on the device.

<details>
<summary>Underlying CMake commands</summary>

LPM wraps the following steps. You can also run them directly when debugging
the build configuration; LPM is still required to read project metadata.

Host build and tests:

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

Device build and packaging:

```sh
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

</details>

### Troubleshooting

| Symptom | Action |
| --- | --- |
| `lpm is required to read project metadata` | Install LPM in `.venv`, put it on `PATH`, or set `LPM_EXECUTABLE`. |
| `LilyGoUI` or bundled LVGL cannot be found | Initialize the pinned AppKit submodule with `git submodule update --init --recursive`. |
| SDL2, FreeType, or json-c is missing | Install the host development packages; for cross builds, check the target sysroot. |
| Homebrew libraries are not found | Configure with `cmake --preset host-simulator -DCMAKE_PREFIX_PATH="$(brew --prefix)"`. |
| BSP download fails | Check access to GitHub releases and available disk space, then rerun configuration. |
| Map tiles or location do not load | Check `curl`, connectivity, service availability, and `LILYGO_MAPS_DISABLE_NETWORK`; see [runtime configuration](docs/02-services-and-cache.md). |

## Documentation

| Guide | Contents |
| --- | --- |
| [Services and cache](docs/02-services-and-cache.md) | Online providers, environment variables, cache paths, and offline behavior |
| [Architecture overview](docs/00-overview.md) (Chinese) | Module boundaries, worker threads, data flow, and cache consistency |
| [UI development guidelines](docs/01-user-interface.md) (Chinese) | Layout, typography, ownership, and UI validation requirements |

## Acknowledgments

Map data is provided by [OpenStreetMap contributors](https://www.openstreetmap.org/copyright),
tiles by [OpenStreetMap.de](https://www.openstreetmap.de/), and address search by
[Nominatim](https://nominatim.org/). The interface uses LILYGO UI AppKit and
[LVGL](https://lvgl.io/). Follow the relevant providers' usage policies when
running or distributing the application.
