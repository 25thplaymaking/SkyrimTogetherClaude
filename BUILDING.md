# Building SkyrimTogetherClaude

This fork builds with **xmake**, not CMake. Everything below is Windows/x64; the
server also builds for Linux via Docker (see `docs/deploy/grain-silo.md`).

**Always build through `Tools\build-env.cmd`.** A bare `xmake` invocation fails on
this machine for three separate reasons, each documented below. The wrapper
normalises all three so local builds match CI.

---

## Prerequisites

| Component | Required | Notes |
|---|---|---|
| **xmake** | **2.9.8** | Pinned. CI uses 2.9.8 (`.github/workflows/windows.yml`). |
| **VS 2022 BuildTools** | MSVC 14.44.35207 + Windows SDK 10.0.26100.0 | Must include the "Desktop development with C++" workload. |
| **Node** | 20+ | UI only. |
| **pnpm** | 9 | UI only. |

### Installing xmake 2.9.8

`winget` only carries xmake 3.x, which is a major version with breaking changes
against this project's `set_xmakever("2.8.5")`. Install 2.9.8 side-by-side:

```powershell
$url = "https://github.com/xmake-io/xmake/releases/download/v2.9.8/xmake-v2.9.8.win64.exe"
Invoke-WebRequest -Uri $url -OutFile "$env:TEMP\xmake-2.9.8.exe"
Start-Process -FilePath "$env:TEMP\xmake-2.9.8.exe" `
              -ArgumentList '/S','/D=C:\Users\Bryce\Tools\xmake-2.9.8' -Wait -NoNewWindow
```

`Tools\build-env.cmd` invokes that absolute path, so a different xmake on `PATH`
is harmless.

---

## Build

```bash
# Configure (first run downloads and builds all dependencies -- allow ~20 min)
cmd //c "Tools\build-env.cmd f -p windows --arch=x64 --mode=releasedbg --yes"

# Build
cmd //c "Tools\build-env.cmd -y"
```

Artifacts land in `build/windows/x64/releasedbg/`:

| Artifact | What it is |
|---|---|
| `SkyrimTogether.exe` | Client launcher |
| `STServer.dll` | Server implementation |
| `SkyrimTogetherServer.exe` | Server host |
| `TPProcess.exe` | CEF overlay worker |
| `EarlyLoad.dll` | Early game hook |
| `TPTests.exe` and friends | Test binaries |

Valid `--mode` values are `debug`, `releasedbg`, and `release`. CI ships
`release`; use `releasedbg` for development.

### Building the UI

```bash
pnpm --prefix Code/skyrim_ui/ install
pnpm --prefix Code/skyrim_ui/ deploy:production
cp -r Code/skyrim_ui/dist/UI build/windows/x64/releasedbg/
```

---

## Tests

```bash
build/windows/x64/releasedbg/TPTests.exe
```

### Known-failing suites (pre-existing, not regressions)

Two suites fail on a clean checkout of `v1.8.0` before any of our changes.
Do not treat them as breakage you caused.

| Suite | Status | Why |
|---|---|---|
| `TPTests` | **28 assertions / 5 cases — passing** | Encoding and serialization. This is the suite Phase 0 work extends. |
| `Console_Tests` | **6 tests — passing** | |
| `Resources_Tests` | **13 tests — passing** | |
| `ESLoader_Tests` | **8 tests — all failing** | `BuildRecordCollection` returns false because `ESLoader::LoadFiles()` is commented out upstream (`ESLoader.cpp:44-51`) and the tests expect real Skyrim ESM records. The `0xC0000005` crashes cascade from the null collection. Re-enabling the loader is **Phase 4** scope. |
| `TiltedReverse_Tests` | **1 test — failing** | Submodule's own "load the reverse dll" test, SIGSEGV. Unrelated to this tree. |

---

## The three gotchas the wrapper fixes

Recorded so nobody rediscovers them.

### 1. Auto-detection picks a headerless compiler

`vswhere` reports two installs. **VS 18 Community** is newer, so xmake and
`cc-rs`-style detectors select it — but its C++ workload has no headers
(`VC\Tools\MSVC\14.51.36231\include` does not exist) and its `vcvars64.bat` sets
`INCLUDE`/`LIB` **empty**, so even its own developer shell is unusable.

```
fatal error C1083: Cannot open include file: 'stdarg.h'
```

The wrapper calls the **VS 2022 BuildTools** `vcvars64.bat` explicitly and aborts
if `INCLUDE` comes back empty.

### 2. Git Bash breaks DirectXTK's shader build

Git Bash exports `NoDefaultCurrentDirectoryInExePath=1`, and it survives into
`cmd.exe`. That disables cmd's normal "search the current directory first"
behaviour. DirectXTK's custom build rule invokes `CompileShaders.cmd` by bare
name, so the build dies with:

```
'CompileShaders.cmd' is not recognized as an internal or external command
error MSB8066: Custom build for '...CompileShaders.cmd' exited with code 1
```

CI never hits this because it runs from PowerShell, where the variable is unset.
The wrapper clears it.

### 3. xmake detects the wrong platform under Git Bash

Git ships `C:\Program Files\Git\mingw64`, so xmake auto-detects platform `mingw`
and then reports `gamenetworkingsockets`, `mem` and `sentry-native` as
unsupported. **Always pass `-p windows` explicitly** when configuring. CI does
not need this because `windows-latest` defaults correctly.

### Bonus: writing batch wrappers

`cmd` expands `%VAR%` when it *parses* an `if (...)` block, before evaluating the
condition. An unquoted `echo %VCVARS%` inside such a block injects
`C:\Program Files (x86)\...`, whose `)` closes the block early:

```
\Microsoft was unexpected at this time.
```

Quote every path echoed inside a block, and prefer `if not defined VAR` over
`if "%VAR%"==""`.

---

## Submodules

The four `Libraries/*` submodules are **pinned** to the commits recorded by
`v1.8.0`. CI checks out those pins with
`git submodule update --init --force --recursive`.

**Never run `git submodule update --remote`.** Doing so silently advances all
four to upstream `master`, so your local build and CI compile different
dependency code. To verify you are at the pins:

```bash
git submodule status   # no line may start with '+'
```

To restore them:

```bash
git submodule update --init --force --recursive
```
