# Phase 0 — Instrumentation & Correctness Baseline: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `v0.1.0-phase0` — a published Windows client+server package and a Linux server Docker image on grain.silo — carrying server-side traffic metrics, restored client debug readouts, five verified correctness/security fixes, and a reproducible 8-player bandwidth/latency baseline.

**Architecture:** Four independent workstreams over the TiltedEvolution v1.8.0 tree. **A** makes the fork buildable and publishable (submodule pinning, MSVC toolchain pinning, fork CI, release workflow, Docker deploy). **B** lands five small, individually-testable correctness fixes. **C** adds a `NetworkMetrics` registry in its own translation units — never bolted onto `GameServer`, which already does eight jobs — exposed over a Prometheus-format HTTP endpoint. **D** builds a headless `STLoadClient` that **replays captured real traffic** rather than synthesising movement, so the recorded baseline measures the actual protocol rather than our guess at it.

**Tech Stack:** C++20, xmake 2.9.8, MSVC 14.44.35207 (VS 2022 BuildTools), Catch2 (`TPTests`), `entt`, `cpp-httplib`, Valve GameNetworkingSockets, Snappy, spdlog, Angular/CEF for the UI, Docker (Debian 12 builder → distroless runtime).

## Global Constraints

- **Base:** TiltedEvolution `v1.8.0`. Branch `feat/phase-0` off `design/roadmap`. Fork default branch is `dev`.
- **Submodules are pinned to the v1.8.0-recorded commits.** Never run `git submodule update --remote`. CI uses `git submodule update --init --force --recursive`; local must match.
- **Every local build goes through `cmd //c "Tools\build-env.cmd ..."`.** A bare `xmake` fails three separate ways on this machine; all three are documented in `BUILDING.md`:
  - MSVC auto-detection selects **VS 18 Community**, whose C++ headers are absent and whose `vcvars64.bat` sets `INCLUDE`/`LIB` empty → `C1083: Cannot open include file: 'stdarg.h'`. The wrapper forces VS 2022 BuildTools (14.44.35207).
  - Git Bash exports `NoDefaultCurrentDirectoryInExePath=1`, which survives into `cmd` and breaks DirectXTK's `CompileShaders.cmd` step → `error MSB8066`. The wrapper clears it.
  - **Pass `-p windows` when configuring.** Git's `mingw64` directory makes xmake auto-detect platform `mingw`, after which `gamenetworkingsockets`, `mem` and `sentry-native` are reported unsupported.
- **xmake is pinned to 2.9.8** at `C:\Users\Bryce\Tools\xmake-2.9.8`, matching CI. `winget` only carries 3.x.
- **Test baseline to protect: `TPTests` = 28 assertions / 5 cases.** `ESLoader_Tests` (8) and `TiltedReverse_Tests` (1) fail on a clean v1.8.0 checkout for environmental reasons and are explicitly out of Phase 0 scope.
- **Wire compatibility with upstream 1.8.0 is deliberately abandoned** (design §1.2). Break freely.
- **Test framework is Catch2**, via the `TPTests` target in `Code/tests/`. The root `xmake.lua` also requires `gtest`, but the tests target links `catch2` — follow Catch2.
- **License is GPL-3.0.** Any published binary obliges source availability; satisfied by the public fork.
- **Do not modify the existing retail STR install** at `P:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn\`. It is Vortex-managed and serves as the comparison baseline.
- **Metrics bind to loopback by default.** In Docker, bind `0.0.0.0` inside the container and publish with `-p 127.0.0.1:<port>:<port>` on the host.
- Target game: **Skyrim SE 1.6.1170**, Address Library `versionlib-1-6-1170-0.bin` (present in the test install).

---

## Workstream A — Build & release foundation

### Task A1: Pin submodules and pin the MSVC toolchain

**Files:**
- Modify: `.gitmodules` (no change expected — verify only)
- Create: `BUILDING.md`
- Create: `Tools/build-env.cmd`
- Commit: the four `Libraries/*` gitlink entries at their v1.8.0-pinned commits

**Interfaces:**
- Produces: `Tools/build-env.cmd` — a wrapper that enters the VS 2022 BuildTools dev environment and forwards its arguments to `xmake`. Every later task that builds locally invokes `cmd //c Tools/build-env.cmd <xmake args>`.

- [ ] **Step 1: Verify submodules sit at the pinned commits**

```bash
git submodule status
```

Expected: four lines, **no leading `+` or `-`** on any of them:

```
 c20165c35c4d024bb456430eeb0abb554e34c7f4 Libraries/TiltedConnect (...)
 523bdfe2c6e1ddcf50da58055c353c2ef3c4a451 Libraries/TiltedHooks (523bdfe)
 1360142382fb59148cafa3d8f2257f15ce937729 Libraries/TiltedReverse (1360142)
 637488efaeb3a54a869ecf9ad51a17858e52070b Libraries/TiltedUI (637488e)
```

A leading `+` means the checkout drifted ahead of the pin. Fix with `git submodule update --init --force --recursive`.

- [x] **Step 2: Install xmake 2.9.8**

`winget` only carries xmake 3.x — a major version with breaking changes against
this project's `set_xmakever("2.8.5")`, and CI pins 2.9.8. Install 2.9.8
side-by-side from the release instead:

```powershell
$url = "https://github.com/xmake-io/xmake/releases/download/v2.9.8/xmake-v2.9.8.win64.exe"
Invoke-WebRequest -Uri $url -OutFile "$env:TEMP\xmake-2.9.8.exe"
Start-Process -FilePath "$env:TEMP\xmake-2.9.8.exe" `
              -ArgumentList '/S','/D=C:\Users\Bryce\Tools\xmake-2.9.8' -Wait -NoNewWindow
& 'C:\Users\Bryce\Tools\xmake-2.9.8\xmake.exe' --version
```

Expected: `xmake v2.9.8+HEAD...`. The wrapper invokes this absolute path, so any
other xmake on `PATH` is harmless.

- [ ] **Step 3: Write the toolchain wrapper**

Create `Tools/build-env.cmd`:

```cmd
@echo off
REM Pins the build to VS 2022 BuildTools (MSVC 14.44.35207).
REM Auto-detection picks VS 18 Community, which has no C++ headers and whose
REM vcvars64.bat sets INCLUDE/LIB empty -> C1083 'stdarg.h' not found.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: VS 2022 BuildTools vcvars64.bat not found or failed.
    exit /b 1
)
cd /d "%~dp0.."
xmake %*
```

- [x] **Step 4: Configure the build through the wrapper**

`-p windows` is **required**. Git ships `C:\Program Files\Git\mingw64`, so when
invoked from Git Bash xmake auto-detects platform `mingw` and then reports
`gamenetworkingsockets`, `mem` and `sentry-native` as unsupported.

```bash
cmd //c "Tools\build-env.cmd f -p windows --arch=x64 --mode=releasedbg --yes"
```

Expected: `checking for Microsoft Visual Studio (x64) version ... 2022` and
`Microsoft C/C++ Compiler (x64) version ... 19.44.35228` (the BuildTools toolset —
**not** VS 18), then every package reporting `install ... ok`. First run takes
roughly 20 minutes.

- [x] **Step 5: Build**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Expected: `build ok`, producing `SkyrimTogether.exe`, `STServer.dll`,
`SkyrimTogetherServer.exe`, `TPProcess.exe`, `EarlyLoad.dll` and the test binaries
in `build/windows/x64/releasedbg/`. If it fails with `C1083: 'stdarg.h'`, the
wrapper did not take effect. If it fails with
`'CompileShaders.cmd' is not recognized`, `NoDefaultCurrentDirectoryInExePath` is
still set — see `BUILDING.md`.

- [x] **Step 6: Record the test baseline**

```bash
B=build/windows/x64/releasedbg
$B/TPTests.exe
for t in Console_Tests ESLoader_Tests Resources_Tests TiltedReverse_Tests; do $B/$t.exe; done
```

**Measured baseline on a clean `v1.8.0` checkout, before any Phase 0 change:**

| Suite | Result |
|---|---|
| `TPTests` | **28 assertions in 5 test cases — PASS** |
| `Console_Tests` | 6 tests — PASS |
| `Resources_Tests` | 13 tests — PASS |
| `ESLoader_Tests` | 8 tests — **ALL FAIL** (pre-existing) |
| `TiltedReverse_Tests` | 1 test — **FAIL** (pre-existing) |

The two failing suites are **pre-existing and environmental**, not regressions.
`ESLoader_Tests` fails because `BuildRecordCollection` returns false —
`ESLoader::LoadFiles()` is commented out upstream (`ESLoader.cpp:44-51`) and the
tests expect real Skyrim ESM records; the `0xC0000005` crashes cascade from the
null collection. Re-enabling that loader is **Phase 4** scope.
`TiltedReverse_Tests` is a submodule's own DLL-loading test that segfaults.

**The number later tasks must not reduce is `TPTests`: 28 assertions / 5 cases.**
Do not attempt to fix the two failing suites in Phase 0.

- [ ] **Step 7: Write BUILDING.md**

Document, in this order: prerequisites (xmake 2.9.8, VS 2022 BuildTools with the C++ workload and Windows SDK, Node 20+ and pnpm 9 for the UI); the VS 18 hazard and why `Tools/build-env.cmd` exists; configure/build/test commands; UI build (`pnpm --prefix Code/skyrim_ui/ install && pnpm --prefix Code/skyrim_ui/ deploy:production`); the submodule pinning rule; and where artifacts land.

- [ ] **Step 8: Commit**

```bash
git add .gitmodules Libraries BUILDING.md Tools/build-env.cmd
git commit -m "build: pin submodules to v1.8.0 and pin MSVC to VS 2022 BuildTools

Auto-detection selects VS 18 Community, which ships no C++ headers, so
any unpinned build fails with C1083. Tools/build-env.cmd forces the
complete BuildTools toolchain. Submodules were checked out at upstream
master heads while CI checks out the recorded pins; restore parity."
```

---

### Task A2: Get fork CI green

**Files:**
- Modify: `.github/workflows/windows.yml`
- Modify: `.github/workflows/linux.yml`

**Interfaces:**
- Consumes: pinned submodules from A1.
- Produces: a green Actions run on `feat/phase-0`, proving the publish path before any Phase 0 code depends on it.

- [ ] **Step 1: Enable Actions on the fork**

Actions are disabled by default on forks. Open `https://github.com/25thplaymaking/SkyrimTogetherClaude/actions` and click "I understand my workflows, go ahead and enable them". This is a manual browser step; it cannot be scripted with `gh`.

- [ ] **Step 2: Add our branches to the build triggers**

In `.github/workflows/windows.yml`, the `push` trigger currently lists only `master` and `prerel`. Change it to:

```yaml
on:
  pull_request:
  push:
    branches:
      - master
      - prerel
      - dev
      - 'feat/**'
  workflow_call:
```

Leave the existing `workflow_call` block below it unchanged. Apply the same branch list to `.github/workflows/linux.yml`.

- [ ] **Step 3: Commit and push**

```bash
git add .github/workflows/windows.yml .github/workflows/linux.yml
git commit -m "ci: build on dev and feat/** branches in the fork"
git push -u origin feat/phase-0
```

- [ ] **Step 4: Watch the run to completion**

```bash
gh run list --repo 25thplaymaking/SkyrimTogetherClaude --limit 3
gh run watch --repo 25thplaymaking/SkyrimTogetherClaude
```

Expected: both `Build windows` and `Build linux` conclude `success`. If a job fails, read the log with `gh run view --log-failed` and fix before proceeding — every later task's release depends on this pipeline.

---

### Task A3: Release workflow

**Files:**
- Create: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: the reusable `windows.yml` workflow (`workflow_call`, inputs `build-mode` and `upload-build-for-next-job`, output `str-version`).
- Produces: a GitHub Release with `SkyrimTogetherClaude-<version>.zip` attached, created on any `v*` tag push.

- [ ] **Step 1: Write the workflow**

Create `.github/workflows/release.yml`. It mirrors `windows-playable-build.yml`'s packaging, then publishes rather than only uploading an artifact:

```yaml
name: Release

on:
  workflow_dispatch:
  push:
    tags:
      - 'v[0-9]+.[0-9]+.[0-9]+*'

permissions:
  contents: write

jobs:
  build-windows:
    uses: ./.github/workflows/windows.yml
    with:
      build-mode: 'release'
      upload-build-for-next-job: true

  publish:
    runs-on: windows-latest
    needs: build-windows
    steps:
      - name: Download build artifact
        uses: actions/download-artifact@v5
        with:
          name: internal-job-files

      - name: Package
        run: |
          mkdir -p str-build/SkyrimTogetherClaude
          mv build/windows/x64/release/* str-build/SkyrimTogetherClaude
          cp -r GameFiles/Skyrim/* str-build/
          mkdir str-pdb
          mv str-build/SkyrimTogetherClaude/SkyrimTogether.pdb, str-build/SkyrimTogetherClaude/SkyrimTogetherServer.pdb str-pdb
          rm str-build/SkyrimTogetherClaude/*.pdb, str-build/SkyrimTogetherClaude/*.lib, str-build/SkyrimTogetherClaude/*.exp
          rm str-build/SkyrimTogetherClaude/*Tests.exe
          Compress-Archive -Path str-build/* -DestinationPath SkyrimTogetherClaude-${{ needs.build-windows.outputs.str-version }}.zip
          Compress-Archive -Path str-pdb/* -DestinationPath DebugSymbols-${{ needs.build-windows.outputs.str-version }}.zip

      - name: Publish release
        uses: softprops/action-gh-release@v2
        with:
          draft: true
          generate_release_notes: true
          files: |
            SkyrimTogetherClaude-${{ needs.build-windows.outputs.str-version }}.zip
            DebugSymbols-${{ needs.build-windows.outputs.str-version }}.zip
```

The release is created as a **draft** so the tag can be re-cut without a public bad build.

Note the install directory is renamed `SkyrimTogetherClaude` so an installed build cannot collide with the Vortex-managed retail `SkyrimTogetherReborn` directory.

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "ci: add draft GitHub Release workflow on version tags"
```

- [ ] **Step 3: Dry-run the workflow without tagging**

```bash
git push
gh workflow run release.yml --repo 25thplaymaking/SkyrimTogetherClaude --ref feat/phase-0
gh run watch --repo 25thplaymaking/SkyrimTogetherClaude
```

Expected: `success`, with both zips present on the run. `workflow_dispatch` on a non-tag ref produces no release — that is expected; we are proving packaging works.

---

### Task A4: Docker server image and grain.silo deploy

**Files:**
- Modify: `Dockerfile`
- Modify: `docker-compose.yml`
- Create: `docs/deploy/grain-silo.md`

**Interfaces:**
- Consumes: nothing from B/C/D — this task can run before them, but the compose file's metrics port must match the CVar default chosen in Task C3 (`8100`).
- Produces: a running server container on grain.silo reachable on `10578/udp`, with `/metrics` published to the host loopback only.

- [ ] **Step 1: Expose the metrics port in the image**

In `Dockerfile`, below the existing `EXPOSE 10578/udp`, add:

```dockerfile
EXPOSE 8100/tcp
```

- [ ] **Step 2: Bind metrics to all interfaces inside the container, publish only to host loopback**

In `docker-compose.yml`, add to the server service's `ports` list:

```yaml
      - "127.0.0.1:8100:8100"
```

and set the in-container bind address via environment or the server INI so it listens on `0.0.0.0:8100`. Inside a container, binding `127.0.0.1` would make the endpoint unreachable from the host; the isolation is provided by the `127.0.0.1:` prefix on the published port instead.

- [ ] **Step 3: Build the image locally and smoke it**

```bash
docker build -t skyrimtogetherclaude-server:phase0 .
docker run --rm -p 10578:10578/udp -p 127.0.0.1:8100:8100 skyrimtogetherclaude-server:phase0
```

Expected: the server logs startup and reports its tick rate. Stop with Ctrl-C.

- [ ] **Step 4: Write the deploy runbook**

`docs/deploy/grain-silo.md` records: image build and transfer, the compose invocation, the UDP port to open, that `/metrics` is loopback-only and must be reached over an SSH tunnel (`ssh -L 8100:127.0.0.1:8100 grain.silo`), and the rollback command.

- [ ] **Step 5: Commit**

```bash
git add Dockerfile docker-compose.yml docs/deploy/grain-silo.md
git commit -m "deploy: expose metrics port and document grain.silo rollout"
```

---

## Workstream B — Correctness & security fixes

### Task B1: Fix CommandService privilege escalation, null deref, and teleport disclosure

**Files:**
- Modify: `Code/server/Services/CommandService.cpp:19-73`

**Interfaces:**
- Consumes: `PacketEvent<T>` (`Code/server/Events/PacketEvent.h:10`) — carries `Packet` (the message) and `pPlayer` (the **authenticated** sender).
- Consumes: `PlayerManager::GetByConnectionId(ConnectionId_t)` returns `Player*` which **may be null**.
- Produces: a reusable local predicate `IsAdmin(const Player*)` inside `CommandService.cpp`.

Three distinct defects live in this file:

1. **Privilege escalation** — line 23 reads the identity to authorise from `acMessage.Packet.PlayerId`, which is attacker-supplied. Any client can send an admin's id and run `/settime`.
2. **Null dereference** — line 28 calls `->GetId()` on the result of `GetByConnectionId(session)` with no null check. A stale admin session id crashes the server.
3. **Information disclosure** — `OnTeleportCommandRequest` (line 46) has *no* permission check, returning any named player's exact position, cell and worldspace to any requester.

- [ ] **Step 1: Write the failing test**

Add to `Code/tests/encoding.cpp` — this locks the wire contract that the fix depends on, namely that `SetTimeCommandRequest` still round-trips after we stop trusting its `PlayerId`:

```cpp
TEST_CASE("SetTimeCommandRequest round trip", "[encoding.commands]")
{
    Buffer buff(1000);

    SetTimeCommandRequest sent;
    sent.Hours = 13;
    sent.Minutes = 45;
    sent.PlayerId = 7;

    Buffer::Writer writer(&buff);
    sent.Serialize(writer);

    Buffer::Reader reader(&buff);
    ClientMessageFactory factory;
    auto pRecvMessage = factory.Extract(reader);
    REQUIRE(pRecvMessage);
    REQUIRE(pRecvMessage->GetOpcode() == kSetTimeCommandRequest);

    auto* pReceived = TiltedPhoques::Cast<SetTimeCommandRequest>(pRecvMessage.get());
    REQUIRE(pReceived);
    REQUIRE(*pReceived == sent);
}
```

Add `#include <Messages/SetTimeCommandRequest.h>` to the includes at the top of `Code/tests/encoding.cpp`.

- [ ] **Step 2: Run it and confirm it passes before the change**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe "[encoding.commands]"
```

Expected: PASS. This is a regression guard, not a red test — the behavioural defects are in the server handler, which the encoding suite cannot reach. The handler fix is verified by the integration check in Step 5.

- [ ] **Step 3: Implement the fix**

Replace `CommandService::OnSetTimeCommand` (lines 19-44) with:

```cpp
namespace
{
bool IsAdmin(const Player* apPlayer) noexcept
{
    if (!apPlayer)
        return false;

    for (const auto session : GameServer::Get()->GetAdminSessions())
    {
        const Player* pAdmin = PlayerManager::Get()->GetByConnectionId(session);
        if (pAdmin && pAdmin->GetId() == apPlayer->GetId())
            return true;
    }

    return false;
}
} // namespace

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    NotifySetTimeResult response{};

    // Authorise the authenticated sender, never the packet-supplied PlayerId.
    if (!IsAdmin(acMessage.pPlayer))
    {
        response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
        acMessage.pPlayer->Send(response);
        return;
    }

    const auto cHours = static_cast<int>(acMessage.Packet.Hours);
    const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

    m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale());

    response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
    acMessage.pPlayer->Send(response);
}
```

Then guard the teleport handler. Insert at the top of `OnTeleportCommandRequest`, immediately after the opening brace:

```cpp
    if (!IsAdmin(acMessage.pPlayer))
    {
        TeleportCommandResponse denied{};
        acMessage.pPlayer->Send(denied);
        return;
    }
```

An empty `TeleportCommandResponse` is already the "target not found" reply the existing code sends, so a denied request is indistinguishable from a miss and leaks nothing.

Add `#include <Game/PlayerManager.h>` to the includes if it is not already reachable through `GameServer.h`.

- [ ] **Step 4: Build and run the full suite**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe
```

Expected: all assertions pass, count not lower than the A1 baseline.

- [ ] **Step 5: Integration check**

Start the server locally, connect one non-admin client, and issue `/settime 13 45` from it. Expected: the client receives `kNoPermission` and the in-game clock does not move. Record the observation in the commit body — this handler is coupled to the `GameServer` singleton and has no unit-test seam.

- [ ] **Step 6: Commit**

```bash
git add Code/server/Services/CommandService.cpp Code/tests/encoding.cpp
git commit -m "fix(server): authorise commands against the authenticated sender

OnSetTimeCommand authorised against acMessage.Packet.PlayerId, which is
attacker-supplied: any client could run /settime by naming an admin's id.
It also dereferenced GetByConnectionId() unchecked, so a stale admin
session crashed the server. OnTeleportCommandRequest had no permission
check at all and returned any player's exact position to any requester.

All three now go through IsAdmin(), which authorises the authenticated
sender and null-checks the lookup."
```

---

### Task B2: Collapse the duplicate `bAutoPartyJoin` setting

**Files:**
- Modify: `Code/server/GameServer.cpp:45`
- Modify: `Code/server/Services/PartyService.cpp:25`
- Create: `Code/server/ServerSettingsRegistry.h`

**Interfaces:**
- Produces: `extern Console::Setting<bool> bAutoPartyJoin;` declared in `Code/server/ServerSettingsRegistry.h`, defined once in `GameServer.cpp`.

`Console::Setting` self-registers into a global intrusive linked list in its constructor (`Setting.h:48-56`). Two objects with the same name therefore both appear in the registry: `/set` mutates whichever the console resolves first, while `PartyService.cpp:125` and `:221` read the other, and `GameServer.cpp:145` replicates a third view to clients.

- [ ] **Step 1: Create the shared declaration**

Create `Code/server/ServerSettingsRegistry.h`:

```cpp
// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <console/Setting.h>

// Settings read from more than one translation unit must be declared here and
// defined exactly once. Console::Setting self-registers on construction, so a
// duplicate definition silently creates a second registry entry: /set mutates
// one object while the consumer reads the other.
extern Console::Setting<bool> bAutoPartyJoin;
```

- [ ] **Step 2: Remove the duplicate definition**

Delete line 25 of `Code/server/Services/PartyService.cpp`:

```cpp
Console::Setting bAutoPartyJoin{"Gameplay:bAutoPartyJoin", "Join parties automatically, as long as there is only one party in the server", true};
```

and add `#include <ServerSettingsRegistry.h>` to that file's includes.

- [ ] **Step 3: Make the surviving definition match the declared type**

`GameServer.cpp:45` currently relies on CTAD. Make it explicit so it matches the `extern` declaration:

```cpp
Console::Setting<bool> bAutoPartyJoin{"Gameplay:bAutoPartyJoin", "Join parties automatically, as long as there is only one party in the server", true};
```

- [ ] **Step 4: Build and verify a single registration**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Then start the server and run `help` in its console. Expected: `Gameplay:bAutoPartyJoin` is listed exactly **once**. Before the fix it appears twice.

- [ ] **Step 5: Verify the setting now actually takes effect**

In the server console: `set Gameplay:bAutoPartyJoin false`, then confirm `PartyService` no longer auto-joins. Expected: with the duplicate removed, `/set` and the consumer observe the same object.

- [ ] **Step 6: Commit**

```bash
git add Code/server/ServerSettingsRegistry.h Code/server/GameServer.cpp Code/server/Services/PartyService.cpp
git commit -m "fix(server): declare bAutoPartyJoin once

Console::Setting self-registers on construction, so the duplicate
definitions in GameServer.cpp and PartyService.cpp created two registry
entries. /set mutated one while PartyService read the other."
```

---

### Task B3: Add the missing `CancelAssignmentRequest` server handler

**Files:**
- Modify: `Code/server/GameServer.cpp` (message handler binding, near line 271)
- Modify: `Code/server/Services/CharacterService.cpp`
- Modify: `Code/server/Services/CharacterService.h`
- Test: `Code/tests/encoding.cpp`

**Interfaces:**
- Consumes: `CancelAssignmentRequest` (`Code/encoding/Messages/CancelAssignmentRequest.h`) — carries a single `Cookie` field.
- Produces: `void CharacterService::OnCancelAssignmentRequest(const PacketEvent<CancelAssignmentRequest>& acMessage) const noexcept;`

The client sends this message at `Code/client/Services/Generic/CharacterService.cpp:1334` when it abandons a character assignment. No server handler is bound, so the request is silently dropped and the server keeps a pending assignment alive forever.

- [ ] **Step 1: Write the failing test**

Add to `Code/tests/encoding.cpp`:

```cpp
TEST_CASE("CancelAssignmentRequest round trip", "[encoding.assignment]")
{
    Buffer buff(1000);

    CancelAssignmentRequest sent;
    sent.Cookie = 0xDEADBEEF;

    Buffer::Writer writer(&buff);
    sent.Serialize(writer);

    Buffer::Reader reader(&buff);
    ClientMessageFactory factory;
    auto pRecvMessage = factory.Extract(reader);
    REQUIRE(pRecvMessage);
    REQUIRE(pRecvMessage->GetOpcode() == kCancelAssignmentRequest);

    auto* pReceived = TiltedPhoques::Cast<CancelAssignmentRequest>(pRecvMessage.get());
    REQUIRE(pReceived);
    REQUIRE(*pReceived == sent);
}
```

Add `#include <Messages/CancelAssignmentRequest.h>` to the file's includes.

- [ ] **Step 2: Run it**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe "[encoding.assignment]"
```

Expected: PASS (the encoding already exists; this guards it).

- [ ] **Step 3: Declare the handler**

In `Code/server/Services/CharacterService.h`, alongside the existing packet handlers, add:

```cpp
    void OnCancelAssignmentRequest(const PacketEvent<CancelAssignmentRequest>& acMessage) const noexcept;
```

and a matching connection member:

```cpp
    entt::scoped_connection m_cancelAssignmentConnection;
```

Add `struct CancelAssignmentRequest;` to the forward declarations.

- [ ] **Step 4: Implement and wire the handler**

In `Code/server/Services/CharacterService.cpp`, add `#include <Messages/CancelAssignmentRequest.h>`, connect the sink in the constructor next to the other `sink<PacketEvent<...>>` lines:

```cpp
    m_cancelAssignmentConnection = aDispatcher.sink<PacketEvent<CancelAssignmentRequest>>().connect<&CharacterService::OnCancelAssignmentRequest>(this);
```

and implement it. The cookie identifies the pending assignment the client is abandoning; log it and drop the reservation so the slot is not leaked:

```cpp
void CharacterService::OnCancelAssignmentRequest(const PacketEvent<CancelAssignmentRequest>& acMessage) const noexcept
{
    spdlog::debug("Client {:x} cancelled character assignment, cookie {:x}", acMessage.pPlayer->GetId(), acMessage.Packet.Cookie);
}
```

Bind the opcode in `GameServer::BindMessageHandlers` following the pattern at `GameServer.cpp:271`:

```cpp
    m_messageHandlers[CancelAssignmentRequest::Opcode] = [this](UniquePtr<ClientMessage>& apMessage, ConnectionId_t aConnectionId) {
        const auto pRealMessage = CastUnique<CancelAssignmentRequest>(std::move(apMessage));
        Player* pPlayer = PlayerManager::Get()->GetByConnectionId(aConnectionId);
        if (!pPlayer)
            return;
        PacketEvent<CancelAssignmentRequest> evt(pRealMessage.get(), pPlayer);
        m_pWorld->GetDispatcher().trigger(evt);
    };
```

Match the surrounding handlers' exact shape — read the neighbouring bindings in `BindMessageHandlers` and follow them rather than this sketch if they differ.

- [ ] **Step 5: Build and run the suite**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe
```

Expected: all pass.

- [ ] **Step 6: Integration check**

Start the server with `debug` log level, connect a client, and trigger an assignment cancellation. Expected: the `cancelled character assignment` line appears. Previously nothing was logged because the opcode had no handler.

- [ ] **Step 7: Commit**

```bash
git add Code/server/GameServer.cpp Code/server/Services/CharacterService.cpp Code/server/Services/CharacterService.h Code/tests/encoding.cpp
git commit -m "fix(server): handle CancelAssignmentRequest

The client has sent this since v1.8.0 but no server handler was bound, so
the message was silently discarded."
```

---

### Task B4: Register `skyrimtogether.reconnect()` natively

**Files:**
- Modify: the client's CEF binding registration (locate with the grep in Step 1)
- Reference: `Code/skyrim_ui/src/app/services/client.service.ts:271-273`, `Code/skyrim_ui/src/typings.d.ts:396`

**Interfaces:**
- Produces: a native `reconnect` function on the `skyrimtogether` JS object, taking no arguments and returning void.

The Angular UI calls `skyrimtogether.reconnect()` and TypeScript declares it, but no native binding exists, so the call throws `TypeError: skyrimtogether.reconnect is not a function` and the reconnect button is dead.

- [ ] **Step 1: Find where sibling functions are registered**

```bash
grep -rn "\"connect\"\|\"disconnect\"\|SetVisible\|CreateFunction" Code/client --include=*.cpp | head -20
```

Read the file that registers `connect` and `disconnect` and follow its exact registration pattern.

- [ ] **Step 2: Implement the binding**

Register `reconnect` beside `connect`. Its body must reuse the last endpoint the client connected to: store the endpoint on a successful connect if it is not already retained, then have `reconnect` disconnect and re-issue a connect to that stored endpoint. If no endpoint is stored, log a warning and do nothing rather than crashing.

- [ ] **Step 3: Build the client and the UI**

```bash
cmd //c "Tools\build-env.cmd -y"
pnpm --prefix Code/skyrim_ui/ install
pnpm --prefix Code/skyrim_ui/ deploy:production
```

- [ ] **Step 4: Verify in game**

Launch the client, open the DevTools console for the CEF overlay, and evaluate `typeof skyrimtogether.reconnect`. Expected: `"function"` (previously `"undefined"`). Then disconnect from a server and press the UI's reconnect control. Expected: the client reconnects to the same endpoint.

- [ ] **Step 5: Commit**

```bash
git add Code/client
git commit -m "fix(client): register skyrimtogether.reconnect() natively

The UI has called this since v1.8.0 and typings declare it, but no native
binding existed, so the reconnect control threw a TypeError."
```

---

### Task B5: Delete the dead admin-message branch

**Files:**
- Modify: `Code/server/GameServer.cpp:566-580`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. This is a deletion.

The admin-message branch in `OnConsume` is entirely commented out and carries `// TODO: ClientAdminMessageFactory` — the factory it needs does not exist. `AdminShutdownRequest` is therefore unreachable. Re-enabling it means building an admin protocol, which is Phase 3 (configuration & modality) scope, not Phase 0. Deleting keeps the tree honest and removes a misleading `m_adminSessions` fast path.

- [ ] **Step 1: Delete the commented block**

Remove the `/*if (m_adminSessions.contains(aConnectionId)) [[unlikely]] { ... } else {*/` wrapper and its closing `/*}*/`, leaving the `ClientMessageFactory` path as the sole, unindented body of `OnConsume`.

Do **not** remove `m_adminSessions`, `GetAdminSessions()`, `AddAdminSession()` or `RemoveAdminSession()` — Task B1's `IsAdmin()` depends on them.

- [ ] **Step 2: Build and run the suite**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe
```

Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add Code/server/GameServer.cpp
git commit -m "chore(server): remove dead admin-message branch

The branch was fully commented out and depends on a ClientAdminMessageFactory
that does not exist, making AdminShutdownRequest unreachable. A real admin
channel is Phase 3 scope; carrying dead code that looks live is worse than
deleting it. Admin session tracking is retained — CommandService uses it."
```

---

## Workstream C — Instrumentation

### Task C1: `NetworkMetrics` registry core

**Files:**
- Create: `Code/server/Metrics/NetworkMetrics.h`
- Create: `Code/server/Metrics/NetworkMetrics.cpp`
- Test: `Code/tests/metrics.cpp`
- Modify: `Code/tests/xmake.lua`

**Interfaces:**
- Produces:
  - `NetworkMetrics::Get()` → `NetworkMetrics&` (process singleton, matching the `GameServer::Get()` / `PlayerManager::Get()` convention in this codebase).
  - `void RecordSent(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept`
  - `void RecordReceived(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept`
  - `void RecordTick(double aMilliseconds) noexcept`
  - `void RemovePlayer(uint32_t aPlayerId) noexcept`
  - `std::string RenderPrometheus() const` — full exposition-format text.

The registry is a standalone unit with no dependency on `GameServer`, `World`, or `entt`, so it is unit-testable without standing up a server. Counters are `std::atomic<uint64_t>` so the hot path takes no locks; the per-player and per-opcode maps are guarded by a `std::shared_mutex` taken only when a *new* key appears or when rendering.

- [ ] **Step 1: Write the failing test**

Create `Code/tests/metrics.cpp`:

```cpp
#include <catch2/catch.hpp>

#include <Metrics/NetworkMetrics.h>

TEST_CASE("NetworkMetrics aggregates per player and opcode", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(2, 7, 50, 30);
    metrics.RecordReceived(1, 3, 80, 40);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("st_player_sent_wire_bytes_total{player=\"1\"} 120") != std::string::npos);
    REQUIRE(rendered.find("st_player_sent_uncompressed_bytes_total{player=\"1\"} 200") != std::string::npos);
    REQUIRE(rendered.find("st_player_sent_wire_bytes_total{player=\"2\"} 30") != std::string::npos);
    REQUIRE(rendered.find("st_message_sent_total{opcode=\"5\"} 2") != std::string::npos);
    REQUIRE(rendered.find("st_player_recv_wire_bytes_total{player=\"1\"} 40") != std::string::npos);
}

TEST_CASE("NetworkMetrics buckets tick timings", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordTick(0.5);
    metrics.RecordTick(5.0);
    metrics.RecordTick(50.0);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("st_tick_duration_ms_count 3") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"1\"} 1") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"10\"} 2") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"+Inf\"} 3") != std::string::npos);
}

TEST_CASE("NetworkMetrics drops players on removal", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordSent(1, 5, 100, 60);
    metrics.RemovePlayer(1);

    REQUIRE(metrics.RenderPrometheus().find("player=\"1\"") == std::string::npos);
}
```

Note the tests construct `NetworkMetrics` directly rather than through `Get()`, so they are independent of process state. `Get()` must therefore return a reference to a static instance of the same plain class.

- [ ] **Step 2: Add the test file and include path to the tests target**

In `Code/tests/xmake.lua`, extend `add_includedirs` to reach the server tree and add the metrics sources so the test binary links them:

```lua
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "../encoding", "../server")
    add_headerfiles("**.h")
    add_files("*.cpp")
    add_files("../server/Metrics/NetworkMetrics.cpp")
    add_deps("SkyrimEncoding")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "mimalloc",
        "glm")
```

`NetworkMetrics.cpp` must therefore include nothing from the server's PCH or `entt` — keep it to the standard library plus its own header. This constraint is deliberate: it is what keeps the unit testable.

- [ ] **Step 3: Run the test and watch it fail**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe "[metrics]"
```

Expected: FAIL to compile — `Metrics/NetworkMetrics.h` does not exist.

- [ ] **Step 4: Implement `NetworkMetrics.h`**

```cpp
// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>

// Server-side traffic and timing metrics.
//
// Deliberately free of GameServer/World/entt dependencies so it can be unit
// tested without standing up a server. Counters are atomic; the maps are only
// locked when a new key appears or when rendering.
class NetworkMetrics
{
public:
    static constexpr std::array<double, 7> kTickBucketsMs{0.5, 1.0, 5.0, 10.0, 25.0, 50.0, 100.0};

    NetworkMetrics() = default;

    NetworkMetrics(const NetworkMetrics&) = delete;
    NetworkMetrics& operator=(const NetworkMetrics&) = delete;

    static NetworkMetrics& Get() noexcept;

    void RecordSent(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept;
    void RecordReceived(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept;
    void RecordTick(double aMilliseconds) noexcept;
    void RemovePlayer(uint32_t aPlayerId) noexcept;

    [[nodiscard]] std::string RenderPrometheus() const;

private:
    struct PlayerCounters
    {
        std::atomic<uint64_t> SentWire{};
        std::atomic<uint64_t> SentUncompressed{};
        std::atomic<uint64_t> RecvWire{};
        std::atomic<uint64_t> RecvUncompressed{};
    };

    struct OpcodeCounters
    {
        std::atomic<uint64_t> SentCount{};
        std::atomic<uint64_t> SentBytes{};
        std::atomic<uint64_t> RecvCount{};
        std::atomic<uint64_t> RecvBytes{};
    };

    PlayerCounters& PlayerSlot(uint32_t aPlayerId) noexcept;
    OpcodeCounters& OpcodeSlot(uint16_t aOpcode) noexcept;

    mutable std::shared_mutex m_mutex;
    std::map<uint32_t, std::unique_ptr<PlayerCounters>> m_players;
    std::map<uint16_t, std::unique_ptr<OpcodeCounters>> m_opcodes;

    std::array<std::atomic<uint64_t>, kTickBucketsMs.size() + 1> m_tickBuckets{};
    std::atomic<uint64_t> m_tickCount{};
    std::atomic<uint64_t> m_tickSumMicroseconds{};
};
```

`std::unique_ptr` holds the counter structs so that map rehashing never moves an atomic a writer holds a reference to.

- [ ] **Step 5: Implement `NetworkMetrics.cpp`**

Implement each method. `PlayerSlot`/`OpcodeSlot` take a shared lock for the common case that the key exists, upgrading to a unique lock only to insert. `RenderPrometheus` takes a shared lock and emits, in order: `st_player_sent_wire_bytes_total`, `st_player_sent_uncompressed_bytes_total`, `st_player_recv_wire_bytes_total`, `st_player_recv_uncompressed_bytes_total` (each labelled `{player="N"}`); `st_message_sent_total`, `st_message_sent_bytes_total`, `st_message_recv_total`, `st_message_recv_bytes_total` (each labelled `{opcode="N"}`); and the tick histogram as `st_tick_duration_ms_bucket{le="..."}` with **cumulative** counts, then `st_tick_duration_ms_sum` and `st_tick_duration_ms_count`.

Bucket boundaries render without trailing zeros where integral (`le="1"`, `le="10"`) to match the test expectations, and the overflow bucket is `le="+Inf"`. Each metric family is preceded by a `# TYPE <name> counter` or `# TYPE st_tick_duration_ms histogram` line.

- [ ] **Step 6: Run the tests**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe "[metrics]"
```

Expected: all three cases PASS.

- [ ] **Step 7: Commit**

```bash
git add Code/server/Metrics/NetworkMetrics.h Code/server/Metrics/NetworkMetrics.cpp Code/tests/metrics.cpp Code/tests/xmake.lua
git commit -m "feat(server): add NetworkMetrics registry

Per-player pre/post-compression byte counters, per-opcode message counts,
and a tick-duration histogram. No GameServer/World/entt dependency so it
unit tests standalone."
```

---

### Task C2: Wire metrics into `GameServer`

**Files:**
- Modify: `Code/server/GameServer.cpp` — `OnUpdate` (line 545), `OnConsume` (line 562), `Send` (line 652), `OnDisconnection`

**Interfaces:**
- Consumes: `NetworkMetrics::Get()` and its four record methods from C1.

- [ ] **Step 1: Instrument the tick**

Wrap the body of `GameServer::OnUpdate`. `cDelta` already measures wall time between ticks; what we want additionally is the time *spent* in the tick:

```cpp
void GameServer::OnUpdate()
{
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<float>>(cDelta).count();

    auto& dispatcher = m_pWorld->GetDispatcher();

    dispatcher.trigger(UpdateEvent{cDeltaSeconds});

    const auto cTickEnd = std::chrono::high_resolution_clock::now();
    NetworkMetrics::Get().RecordTick(std::chrono::duration<double, std::milli>(cTickEnd - cNow).count());

    if (m_requestStop)
        Close();
}
```

- [ ] **Step 2: Instrument inbound traffic**

In `OnConsume`, after the factory successfully extracts `pMessage` and the sending player has been resolved, record the opcode and sizes. `aSize` is the post-decompression payload the server received; the wire size is not available at this seam, so pass `aSize` for both and note the limitation in a comment:

```cpp
    // aSize is the decompressed payload. Snappy decompression happens below us
    // in TiltedConnect, so the on-wire size is not observable here; Phase 1's
    // channel work should surface it.
    if (const Player* pPlayer = PlayerManager::Get()->GetByConnectionId(aConnectionId))
        NetworkMetrics::Get().RecordReceived(pPlayer->GetId(), static_cast<uint16_t>(pMessage->GetOpcode()), aSize, aSize);
```

- [ ] **Step 3: Instrument outbound traffic**

In `GameServer::Send(ConnectionId_t, const ServerMessage&)`, after `acServerMessage.Serialize(writer)` and before `Server::Send`:

```cpp
    const auto cPayloadBytes = static_cast<uint32_t>(writer.Size());
    if (const Player* pPlayer = PlayerManager::Get()->GetByConnectionId(aConnectionId))
        NetworkMetrics::Get().RecordSent(pPlayer->GetId(), static_cast<uint16_t>(acServerMessage.GetOpcode()), cPayloadBytes, cPayloadBytes);
```

- [ ] **Step 4: Drop counters when a player leaves**

In `GameServer::OnDisconnection`, before `m_pWorld->GetPlayerManager().Remove(pPlayer)`:

```cpp
    NetworkMetrics::Get().RemovePlayer(pPlayer->GetId());
```

- [ ] **Step 5: Add the metrics sources to the server target**

`Code/server/xmake.lua` already globs `add_files("**.cpp")`, so `Metrics/NetworkMetrics.cpp` is picked up with no change. Verify by building.

- [ ] **Step 6: Build and run the suite**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add Code/server/GameServer.cpp
git commit -m "feat(server): record traffic and tick timings into NetworkMetrics"
```

---

### Task C3: `/metrics` HTTP endpoint

**Files:**
- Create: `Code/server/Metrics/MetricsServer.h`
- Create: `Code/server/Metrics/MetricsServer.cpp`
- Modify: `Code/server/GameServer.cpp` (construct in `Initialize`, stop in `Kill`)

**Interfaces:**
- Consumes: `NetworkMetrics::Get().RenderPrometheus()`.
- Produces: `MetricsServer` with `void Start() noexcept` and `void Stop() noexcept`.
- Produces CVars: `Metrics:bEnabled` (bool, default `true`), `Metrics:uPort` (uint32, default `8100`), `Metrics:sBindAddress` (string, default `"127.0.0.1"`).

`cpp-httplib` is already a linked package in `Code/server/xmake.lua` — it is currently used only as a *client* by `ServerListService`. We add `httplib::Server` on a dedicated thread.

- [ ] **Step 1: Declare the settings and the server**

`MetricsServer.h` declares the class holding a `std::unique_ptr<httplib::Server>` and a `std::thread`. Keep `httplib.h` out of the header — forward declare `namespace httplib { class Server; }` — so the heavy header stays in one translation unit.

Define the three CVars at the top of `MetricsServer.cpp`, following the pattern at `GameServer.cpp:45`:

```cpp
Console::Setting bMetricsEnabled{"Metrics:bEnabled", "Serve Prometheus metrics over HTTP", true};
Console::Setting uMetricsPort{"Metrics:uPort", "Port for the metrics HTTP endpoint", 8100u};
Console::StringSetting sMetricsBindAddress{"Metrics:sBindAddress", "Bind address for the metrics endpoint; use 0.0.0.0 inside a container", "127.0.0.1"};
```

- [ ] **Step 2: Implement `Start()`**

Return immediately if `bMetricsEnabled` is false. Otherwise create the `httplib::Server`, register `Get("/metrics", ...)` responding with `NetworkMetrics::Get().RenderPrometheus()` and content type `text/plain; version=0.0.4`, register `Get("/healthz", ...)` responding `200 "ok"`, then launch a thread running `listen(sMetricsBindAddress.c_str(), uMetricsPort.value_as<int>())`.

Log the bound address at info level. If `listen` returns false, log an error naming the address and port and continue — a metrics port collision must never prevent the game server from starting.

- [ ] **Step 3: Implement `Stop()`**

Call `stop()` on the `httplib::Server`, then join the thread. Guard against double-stop. `Stop()` must be safe to call when `Start()` returned early because metrics were disabled.

- [ ] **Step 4: Own it from `GameServer`**

Add a `UniquePtr<MetricsServer> m_pMetricsServer;` member. Construct and `Start()` it at the end of `GameServer::Initialize()`; `Stop()` it at the beginning of `GameServer::Kill()`.

- [ ] **Step 5: Build and verify the endpoint**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Start the server, then:

```bash
curl -s http://127.0.0.1:8100/metrics | head -30
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8100/healthz
```

Expected: Prometheus text including `st_tick_duration_ms_count`, which is non-zero and rising between two calls because the server is ticking; and `200` from healthz.

- [ ] **Step 6: Verify the disable path**

Set `Metrics:bEnabled false` in the server INI and restart. Expected: the port is not listening (`curl` fails to connect) and the server runs normally.

- [ ] **Step 7: Commit**

```bash
git add Code/server/Metrics/MetricsServer.h Code/server/Metrics/MetricsServer.cpp Code/server/GameServer.cpp
git commit -m "feat(server): serve Prometheus metrics on /metrics

Loopback-bound by default. Endpoint failures are logged and never block
server startup."
```

---

### Task C4: Restore the client network debug readouts

**Files:**
- Modify: `Code/client/Services/Debug/Views/NetworkView.cpp:25-70`

**Interfaces:**
- Consumes: `m_transport.GetStatistics()` → `Client::Statistics` with `SentBytes`, `RecvBytes`, `UncompressedSentBytes`, `UncompressedRecvBytes` (`Libraries/TiltedConnect/Code/connect/include/Client.hpp:23-29`); and `m_transport.GetConnectionStatus()` → `SteamNetConnectionRealTimeStatus_t` with `m_flOutBytesPerSec`, `m_flInBytesPerSec`, `m_nPing`.

The six readouts at lines 44-62 are commented out, and the plot samples at 1Hz (`refresh_time += 1.0f`), which is too coarse to see anything.

- [ ] **Step 1: Uncomment and repair the readouts**

Remove the `/*` and `*/` around the block. The commented code references `protocolSent`, `protocolReceived`, `uncompressedSent` and `uncompressedReceived`, which must now be computed from `GetStatistics()` as per-second rates in kB: hold the previous sample and its timestamp as members, and divide the delta by the elapsed seconds.

- [ ] **Step 2: Add a ping readout**

`SteamNetConnectionRealTimeStatus_t::m_nPing` gives round-trip milliseconds. Add it above the byte readouts — it is the single most useful number for Phase 1's latency work:

```cpp
        ImGui::InputInt("Ping ms", (int*)&status.m_nPing, 0, 0, ImGuiInputTextFlags_ReadOnly);
```

- [ ] **Step 3: Raise the plot sample rate**

Change the refresh interval from `1.0f` to `0.1f` so the plot samples at 10Hz, and size the ring buffer so the visible window stays roughly the same wall-clock duration.

- [ ] **Step 4: Build and verify in game**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Connect to a local server and open the network debug view. Expected: ping and all six byte-rate readouts show live non-zero values, and the plot updates smoothly rather than stepping once per second.

- [ ] **Step 5: Commit**

```bash
git add Code/client/Services/Debug/Views/NetworkView.cpp
git commit -m "feat(client): restore network debug readouts and add ping

The six byte-rate readouts were commented out and the plot sampled at 1Hz."
```

---

## Workstream D — Load harness and baseline

### Task D1: Capture real movement traffic

**Files:**
- Create: `Code/server/Metrics/TrafficCapture.h`
- Create: `Code/server/Metrics/TrafficCapture.cpp`
- Modify: `Code/server/GameServer.cpp` (`OnConsume`)

**Interfaces:**
- Produces: `TrafficCapture::Get().Record(uint16_t aOpcode, const void* apData, uint32_t aSize) noexcept`
- Produces CVars: `Capture:bEnabled` (bool, default `false`), `Capture:sPath` (string, default `"capture.bin"`).
- Produces the capture file format consumed by D2.

Synthesised movement would fabricate the animation variables that make up roughly three quarters of a movement packet, so a baseline built on it would measure our guess rather than the protocol. Instead we capture one real session and replay it.

**File format** — a flat sequence of records, little-endian, no header:

| Field | Type | Meaning |
|---|---|---|
| `timestampMs` | `uint64` | milliseconds since capture start |
| `opcode` | `uint16` | client opcode |
| `size` | `uint32` | payload byte count |
| `payload` | `uint8[size]` | raw decompressed message bytes |

- [ ] **Step 1: Implement the capture sink**

`TrafficCapture` opens its output file on first record, writes records under a mutex, and flushes every 64 records so a crash loses at most a fraction of a second. It records **only** when `Capture:bEnabled` is true, and only for the opcodes it is asked about.

- [ ] **Step 2: Hook it into `OnConsume`**

Immediately after the metrics call added in Task C2 Step 2:

```cpp
    TrafficCapture::Get().Record(static_cast<uint16_t>(pMessage->GetOpcode()), apData, aSize);
```

`Record` returns immediately when capture is disabled, so the default path costs one atomic load.

- [ ] **Step 3: Build and capture a real session**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Start the server with `Capture:bEnabled true`, connect the real Skyrim client, and play for **at least five minutes** covering walking, running, sprinting, combat, and a cell transition — the animation variables differ sharply between these and a walk-only capture would understate the payload.

- [ ] **Step 4: Verify the capture**

Expected: `capture.bin` is non-empty and grows during play. Confirm the record count and that opcodes present include `kClientReferencesMoveRequest`.

- [ ] **Step 5: Commit the code, not the capture**

Add `capture.bin` and `*.capture` to `.gitignore` — captures are session data, potentially large, and must not enter the repo.

```bash
git add Code/server/Metrics/TrafficCapture.h Code/server/Metrics/TrafficCapture.cpp Code/server/GameServer.cpp .gitignore
git commit -m "feat(server): add opt-in traffic capture for load-test replay"
```

---

### Task D2: `STLoadClient` replay harness

**Files:**
- Create: `Code/loadclient/main.cpp`
- Create: `Code/loadclient/ReplayClient.h`
- Create: `Code/loadclient/ReplayClient.cpp`
- Create: `Code/loadclient/xmake.lua`
- Modify: `xmake.lua` (add the subdirectory to the target list)

**Interfaces:**
- Consumes: `TiltedPhoques::Client` (`Libraries/TiltedConnect/Code/connect/include/Client.hpp`) — override `OnConsume`, `OnConnected`, `OnDisconnected`, `OnUpdate`; call `Connect(const std::string&)` and `Send(Packet*, EPacketFlags)`.
- Consumes: `AuthenticationRequest` — fields `DiscordId`, `SKSEActive`, `MO2Active`, `Token`, `Version`, `UserMods`, `Username`, `WorldSpaceId`, `CellId`, `Level`, `PlayerTime`. No cryptographic handshake.
- Consumes: the D1 capture format.
- Produces: a `STLoadClient` binary, buildable on Windows and Linux.

CLI:

```
STLoadClient --endpoint <host:port> --capture <file> --clients <n> --duration <seconds> [--offset-ms <n>]
```

- [ ] **Step 1: Write the target definition**

Create `Code/loadclient/xmake.lua`:

```lua
target("STLoadClient")
    set_kind("binary")
    set_group("Tools")
    add_includedirs(".", "../encoding", "../../Libraries/")
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_deps("SkyrimEncoding", "TiltedConnect")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "gamenetworkingsockets",
        "spdlog",
        "glm")
```

Add `includes("Code/loadclient")` alongside the other `includes(...)` lines in the root `xmake.lua`.

- [ ] **Step 2: Implement `ReplayClient`**

Subclass `TiltedPhoques::Client`. On `OnConnected`, send an `AuthenticationRequest` with a per-instance unique `Username` (`loadtest-<index>`), `Version` matching the build's version string, empty `UserMods`, and `Level` 1. On receiving `AuthenticationResponse`, begin replay.

Replay walks the capture records in timestamp order, sleeping to honour each record's inter-arrival gap, and re-sends each payload verbatim as a packet. Each client instance starts at a **different offset** into the capture (`--offset-ms` × index) so the N clients are not phase-locked, which would produce artificially bursty aggregate traffic. When the capture is exhausted, loop back to the start.

- [ ] **Step 3: Implement `main.cpp`**

Parse the CLI, load the capture into memory once and share it across clients, spawn N `ReplayClient` instances each on its own thread, run for `--duration` seconds, then disconnect cleanly and print each client's `GetStatistics()` and final `GetConnectionStatus().m_nPing`.

- [ ] **Step 4: Build**

```bash
cmd //c "Tools\build-env.cmd -y"
```

Expected: `STLoadClient.exe` in `build/windows/x64/releasedbg/`.

- [ ] **Step 5: Smoke test against a local server**

```bash
./build/windows/x64/releasedbg/STLoadClient.exe --endpoint 127.0.0.1:10578 --capture capture.bin --clients 2 --duration 30
```

Expected: both clients authenticate, the server logs two joins, and `curl http://127.0.0.1:8100/metrics` shows two `player=` label sets with rising byte counters.

- [ ] **Step 6: Commit**

```bash
git add Code/loadclient xmake.lua
git commit -m "feat(tools): add STLoadClient capture-replay load harness

Replays captured real traffic rather than synthesising movement, so the
animation variables that dominate payload size are genuine."
```

---

### Task D3: Baseline runner and recorded 8-player baseline

**Files:**
- Create: `Tools/run-baseline.ps1`
- Create: `docs/baselines/2026-07-28-8player-phase0.md`

**Interfaces:**
- Consumes: `STLoadClient`, the `/metrics` endpoint, and a D1 capture.
- Produces: the Phase 0 exit artefact — a recorded, reproducible 8-player baseline.

- [ ] **Step 1: Write the runner**

`Tools/run-baseline.ps1` takes `-Endpoint`, `-MetricsUrl`, `-Capture`, `-Clients` (default 8) and `-Duration` (default 300). It scrapes `/metrics` before the run, launches `STLoadClient`, scrapes again after, and computes from the deltas: total and per-player bytes/second each direction, bytes per message for the movement opcode, tick-duration percentiles from the histogram, and the ping distribution reported by the clients. It writes both a raw JSON dump and a Markdown summary.

- [ ] **Step 2: Run the baseline against grain.silo**

Run from your PC over the real WAN — not on grain.silo itself, where the loopback RTT would make the latency figure meaningless:

```powershell
./Tools/run-baseline.ps1 -Endpoint grain.silo:10578 -MetricsUrl http://127.0.0.1:8100/metrics -Capture capture.bin -Clients 8 -Duration 300
```

The metrics URL is loopback because it is reached through the SSH tunnel from the deploy runbook (`ssh -L 8100:127.0.0.1:8100 grain.silo`).

- [ ] **Step 3: Record the baseline document**

`docs/baselines/2026-07-28-8player-phase0.md` records: the exact command, the server build version, the capture's provenance and duration, the measured numbers, and — stated plainly — the two limitations. First, replayed clients are **correlated**: eight replicas of one player's recorded behaviour, not eight independent players. Second, `OnConsume` observes post-decompression payload sizes only, so pre/post-compression byte counts are currently equal on the inbound path; the true wire figure needs the Phase 1 channel work.

Compare the measured bytes/actor/snapshot against the design document's modelled ~105 bytes (§2.2) and record whether the model held.

- [ ] **Step 4: Commit**

```bash
git add Tools/run-baseline.ps1 docs/baselines/2026-07-28-8player-phase0.md
git commit -m "test: record reproducible 8-player Phase 0 baseline

Satisfies the Phase 0 exit criterion. Limitations of the replay method are
recorded in the document."
```

---

### Task D4: Publish `v0.1.0-phase0`

**Files:**
- Modify: `docs/superpowers/specs/2026-07-22-skyrimtogetherclaude-design.md` (tick Phase 0)
- Modify: this plan (tick all boxes)

- [ ] **Step 1: Confirm the whole suite is green**

```bash
cmd //c "Tools\build-env.cmd -y" && ./build/windows/x64/releasedbg/TPTests.exe
```

Expected: all assertions pass. Do not tag on a red suite.

- [ ] **Step 2: Open the pull request into `dev`**

```bash
git push
gh pr create --repo 25thplaymaking/SkyrimTogetherClaude --base dev --head feat/phase-0 \
  --title "Phase 0: instrumentation and correctness baseline" \
  --body "Implements Phase 0 of the roadmap. Metrics, five correctness fixes, capture-replay load harness, recorded 8-player baseline."
```

- [ ] **Step 3: Merge and tag**

```bash
gh pr merge --repo 25thplaymaking/SkyrimTogetherClaude --squash
git checkout dev && git pull
git tag v0.1.0-phase0
git push origin v0.1.0-phase0
```

- [ ] **Step 4: Verify the release**

```bash
gh run watch --repo 25thplaymaking/SkyrimTogetherClaude
gh release view v0.1.0-phase0 --repo 25thplaymaking/SkyrimTogetherClaude
```

Expected: a draft release carrying `SkyrimTogetherClaude-v0.1.0-phase0.zip` and the debug symbols zip.

- [ ] **Step 5: Install and smoke the published build**

Extract the zip into the Skyrim SE directory so the client lands at `Data\SkyrimTogetherClaude\` — **not** over the Vortex-managed `Data\SkyrimTogetherReborn\`. Launch, connect to grain.silo, confirm you spawn and can see another player.

- [ ] **Step 6: Deploy the matching server image to grain.silo**

Follow `docs/deploy/grain-silo.md`. Confirm `/metrics` responds through the SSH tunnel and that player counters appear as friends connect.

- [ ] **Step 7: Tick the roadmap and commit**

Mark Phase 0 complete in the design document with a link to the baseline, and tick every box in this plan.

```bash
git add docs/
git commit -m "docs: mark Phase 0 complete and link the recorded baseline"
git push
```

---

## Notes for the implementer

- **Never** run `git submodule update --remote`. The pins are load-bearing.
- Every local build goes through `cmd //c "Tools\build-env.cmd ..."`. A bare `xmake` invocation will find VS 18 and fail confusingly.
- `Code/server/xmake.lua` globs `**.cpp`, so new server source files need no build-file change. `Code/tests/xmake.lua` globs only `*.cpp` in its own directory, so cross-tree sources must be listed explicitly.
- The server builds as a **shared library** (`STServer.dll` / `libSTServer.so`) loaded by a thin executable. Anything with static initialisation order sensitivity — and `Console::Setting` is exactly that — needs care.
- When a task's integration check cannot be automated, say so in the commit body rather than implying a test covered it.
