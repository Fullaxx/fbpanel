# Installation

## Requirements

| Dependency  | Minimum version | Notes                          |
|-------------|-----------------|--------------------------------|
| GTK+ 2.0    | 2.17            | Development headers required   |
| GLib 2      | 2.4.46          | Earlier versions may work      |
| CMake       | 3.5.2           | 3.20+ required for presets     |
| X11         | any             | libx11-dev / libX11-devel      |
| GModule 2   | any             | Part of GLib; needed for dlopen|

---

## Build types

Four standard CMake build types are supported.  The default is `Release`.

| Build type      | Optimisation | Debug symbols | Strip on install | Package name         |
|-----------------|:------------:|:-------------:|:----------------:|----------------------|
| `Release`       | `-O2`        | no            | yes              | `fbpanel`            |
| `MinSizeRel`    | `-Os`        | no            | yes              | `fbpanel`            |
| `RelWithDebInfo`| `-O2`        | full (`-g3`)  | **no**           | `fbpanel-dbgsym`     |
| `Debug`         | none (`-O0`) | full (`-g3`)  | **no**           | `fbpanel-dbg`        |

Debug and RelWithDebInfo packages install binaries **with symbols embedded**,
so `gdb`, `addr2line`, and crash-report tools can produce useful stack traces.

---

## Quick build — release (no install)

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
```

---

## System install from source

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
sudo make -C build install
```

---

## Build a release .deb package

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
cd build && cpack -G DEB
# Produces: fbpanel_<version>_<arch>.deb
sudo dpkg -i fbpanel_*.deb
```

---

## Build a debug .deb package (fbpanel-dbg)

Use this when you need to run fbpanel under GDB or analyse a crash with
full symbol information.  The package name is `fbpanel-dbg` so it does not
conflict with an installed `fbpanel` release package.

```bash
cmake -B build-debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_INSTALL_PREFIX=/usr
make -C build-debug -j$(nproc)
cd build-debug && cpack -G DEB
# Produces: fbpanel-dbg_<version>_<arch>.deb
sudo dpkg -i fbpanel-dbg_*.deb
```

Compiler flags applied: `-O0 -g3 -Wall -Wextra`

---

## Build a RelWithDebInfo .deb package (fbpanel-dbgsym)

Optimised build that still carries full debug symbols.  Useful for profiling
or for reproducing performance-sensitive bugs under a debugger with realistic
performance characteristics.  The package name is `fbpanel-dbgsym`.

```bash
cmake -B build-relwithdebinfo \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX=/usr
make -C build-relwithdebinfo -j$(nproc)
cd build-relwithdebinfo && cpack -G DEB
# Produces: fbpanel-dbgsym_<version>_<arch>.deb
sudo dpkg -i fbpanel-dbgsym_*.deb
```

Compiler flags applied: `-O2 -g3 -DNDEBUG`

---

## Using CMake presets (CMake 3.20+)

`CMakePresets.json` defines `release`, `debug`, and `relwithdebinfo` presets
that set the build directory, build type, and install prefix automatically.

```bash
# List available presets
cmake --list-presets

# Configure + build (release)
cmake --preset release
cmake --build --preset release

# Configure + build (debug)
cmake --preset debug
cmake --build --preset debug

# Package a debug .deb using the preset build directory
cd build-debug && cpack -G DEB
```

> **Note:** CMake presets require CMake ≥ 3.20.  On older systems (Ubuntu 20.04,
> Debian 11) use the explicit `-DCMAKE_BUILD_TYPE=...` flag shown above instead.

---

## Customising compiler flags

Override the default flags for any build type at configure time:

```bash
# Custom debug flags (e.g. add AddressSanitizer)
cmake -B build-asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS_DEBUG="-O0 -g3 -fsanitize=address -fno-omit-frame-pointer" \
      -DCMAKE_INSTALL_PREFIX=/usr
make -C build-asan -j$(nproc)
```

See [docs/DEBUGGING.md](docs/DEBUGGING.md) for more debugging and profiling
workflows.
