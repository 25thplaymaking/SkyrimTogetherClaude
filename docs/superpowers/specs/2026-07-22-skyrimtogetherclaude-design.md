# SkyrimTogetherClaude — Design & Roadmap

**Date:** 2026-07-22
**Base:** TiltedEvolution `v1.8.0` (tiltedphoques/TiltedEvolution)
**Fork:** [25thplaymaking/SkyrimTogetherClaude](https://github.com/25thplaymaking/SkyrimTogetherClaude)
**License:** GPL-3.0 (inherited — any distributed binary obliges source release)

---

## 1. Context

### 1.1 Target

A **private co-operative server for a trusted group** (~8–16 players). Cheat resistance is
explicitly *not* a driving requirement; **consistency, latency, and NPC quality are**.

### 1.2 Compatibility stance

**Break freely.** Our clients talk only to our servers. Wire-format compatibility with upstream
1.8.0 is abandoned deliberately — nearly every improvement below changes the protocol. We keep
`upstream` as a remote to harvest bug fixes, but we do not constrain design to stay mergeable.

### 1.3 Goals, in the user's words

1. Improve the netcode as much as possible.
2. Add modality — mod configuration and client configuration.
3. Go server-authoritative if possible.
4. Improve how interactions with NPCs are handled.

Goal 3 is answered in §3. Goals 1, 2, 4 are the substance of the roadmap in §5.

---

## 2. What we are starting from

Findings below are ground truth from the v1.8.0 tree, cited `file:line`.

### 2.1 Architecture

The server is **not a game server**. It is an `entt` entity broker with a network poll loop.

- **Zero simulation.** `GameServer::OnUpdate` (`Code/server/GameServer.cpp:545-559`) fires one
  `UpdateEvent`. The only state the server evolves on its own is the in-game clock
  (`CalendarService::OnUpdate`).
- **Form-id-blind.** The ESM/ESP loader exists but is switched off —
  `Code/components/es_loader/ESLoader.cpp:44-51` returns an empty `RecordCollection` with
  `LoadFiles()` commented out. Record parsers for `NPC_`, `REFR`, `CONT`, `CLMT`, `GMST`, `WRLD`
  and **`NAVM` (navmesh)** are dead code.
- **All gameplay runs client-side**, gated by one hook — `Code/client/Games/Skyrim/Actor.cpp:1233-1241`:
  ```cpp
  char TP_MAKE_THISCALL(HookActorProcess, Actor, float a2)
  {
      // Don't process AI if we own the actor
      if (apThis->GetExtension()->IsRemote())
          return 0;
      return TiltedPhoques::ThisCall(RealActorProcess, apThis, a2);
  }
  ```
- **Server-side world partitioning was planned and abandoned.** `Code/server/Game/Cell.h`,
  `Region.h`, `Map.h` are empty stubs; `Region.h` carries a commented-out
  `Map<glm::ivec2, Cell> m_cells;`.

Transport is Valve **GameNetworkingSockets**; payload serialization is custom bit-packing via
`TiltedPhoques::Buffer`; **Snappy** compresses each packet; server scripting is **Lua 5.4.7 via sol2**.

### 2.2 Netcode defects

| Defect | Evidence |
|---|---|
| **100% of traffic is reliable+Nagle.** `kUnreliable` appears **zero** times in `Code/`. A single dropped movement snapshot head-of-line blocks every subsequent packet for that client. | `Libraries/TiltedConnect/.../SteamInterface.hpp:5-9`; grep count 0 |
| **Delta compression is neutered.** `Movement::Serialize` diffs against a default-constructed baseline, so every int and float is flagged changed every tick. | `Code/encoding/Structs/Movement.cpp:22` — `Variables.GenerateDiff(AnimationVariables{}, aWriter)` |
| **~430ms end-to-end latency.** 100ms client send + ~33ms rebroadcast + 300ms hard-coded interpolation buffer. | `Code/client/Services/Generic/CharacterService.cpp:1466`, `:1494` |
| **No extrapolation.** When the interpolation buffer starves, remote actors freeze. | `Code/client/Systems/InterpolationSystem.cpp:41-71` |
| **Position quantised to whole game units, still costs 64 bits.** `static_cast<int32_t>` discards sub-unit precision. `Direction` is a raw unquantised 32-bit float. | `Code/encoding/Structs/Vector3_NetQuantize.cpp:50-70` |
| **`bPremiumMode` is theatre.** Flips tick 30→60Hz, but movement broadcast is separately gated at 20ms sampled on tick boundaries — premium buys ~1Hz. `TogglePremium` never re-calls `Host()`, so it does nothing live. | `Code/server/GameServer.cpp:126-129`, `:53-59`; `Code/server/Services/CharacterService.cpp:790-797` |
| **1 MiB heap buffer allocated per outgoing message per player.** ~240 MiB/s of churn at 30Hz × 8 players, with an unused `ScratchAllocator` sitting adjacent. | `Code/server/GameServer.cpp:653-655` |
| **No server-side traffic metrics at all.** Client `Statistics` exists but the useful readouts are commented out. | `Code/client/Services/Debug/Views/NetworkView.cpp:44-62` |

**Wire cost, one humanoid actor, one snapshot** (60 booleans / 14 ints / 13 floats):
~830 bits ≈ **105 bytes**, of which ~75% is animation variables being resent in full. With real
delta encoding this should fall to **~35–40 bytes**.

### 2.3 Authority and validation

- **Ownership is first-come.** `AssignCharacterRequest` grants ownership to the first client
  claiming a form id, with no proximity or legitimacy check
  (`Code/server/Services/CharacterService.cpp:177-255`).
- **Ownership can be stolen without validation.** `RequestOwnershipClaim` →
  `TransferOwnership` performs no sender check (`:373-376`, `:651-678`).
- **Zero ownership checks** in `ActorValueService` (0/4 handlers), `MagicService` (0/4),
  `CombatService` (0/1), `ObjectService`.
- **Health is delta-only and never reconverges.** `RequestHealthChangeBroadcast` carries an
  unbounded float applied to any entity id, and health is explicitly excluded from the periodic
  actor-value resync. Source carries the open question:
  `// TODO(cosideci): should server side health not be updated?`
  (`Code/server/Services/ActorValueService.cpp:79`)
- **Death is decided independently by every client** that sees health cross zero.
- **Damage stacks additively across clients** with no hit ids, dedup, or ordering.
- **Privilege escalation bug.** `CommandService` compares the admin session against a `PlayerId`
  taken *from the packet* rather than the authenticated sender — any client can run `/settime`
  by supplying an admin's id, and it dereferences without a null check.
  (`Code/server/Services/CommandService.cpp:20-45`)
- **The admin channel is dead code.** The admin-message branch in `GameServer::OnConsume` is
  entirely commented out (`Code/server/GameServer.cpp:566-580`), making `AdminShutdownRequest`
  unreachable.
- `GameServer::ValidateAuthParams` is a stub that returns `false` and is never called (`:803-806`).
- `CancelAssignmentRequest` is sent by clients and has **no server handler**.

### 2.4 NPC handling

- **AI sync is compiled out** — `#define AI_SYNC 0` (`Code/client/Games/References.cpp:81`),
  with the comment *"Disable AI sync for now, experiment didn't work."* Note the disabled block
  references `s_execInitPackage`, which lives in a different translation unit — **it would not
  compile if simply re-enabled.**
- **Unowned NPCs are destroyed.** If no in-range player can take ownership, the entity is
  deleted outright (`Code/server/Services/CharacterService.cpp:346-347`).
- **Ownership handoff loses all AI state** — package stack, combat target, and current procedure
  are not transferred. The new owner's engine restarts AI from scratch.
- **Dialogue is cosmetic only.** Only the `.wav` filename and subtitle string cross the wire.
  No dialogue lock, no shared topic state, no arbitration — two players can talk to the same NPC
  simultaneously and each will cut the other's line via `StopCurrentDialogue(true)`.
- **Temporary actors (`0xFF……`) are never deduped**, so each client creates a separate server
  entity for the same random-encounter NPC.
- **Combat targeting sync was built and disabled** — `#if 0` around
  `Code/client/Services/Generic/CombatService.cpp:163-250`.
- Animation sync is an acknowledged stopgap: *"the _best_ solution … is to implement animation
  graphs serialization … But since that is a pretty hard reverse engineering task, let's do
  simple action replays until better days."* (`Code/server/Game/Animation/ActionReplayCache.h:7-11`)

### 2.5 Configuration and extensibility

- **Server config** is a linked list of statically-constructed CVars; INI sections are just `:`
  name prefixes, not a schema. No hot reload, no ranges, no validation metadata. `kLocked` is
  enforced for `/set` but **not** for INI or argv loading. `Gameplay:bAutoPartyJoin` is
  **declared twice** in different translation units — `/set` mutates one object while
  `PartyService` reads the other.
- **Only 7 settings replicate to clients** (`ServerSettings`), and its `operator==` omits
  `SyncPlayerCalendar`.
- **There is no native client config at all.** Client settings live in browser `localStorage`
  inside the CEF cache. There is no client→server preferences channel.
- **Lua is a skeleton.** It can gate joins, filter chat, set time, and poke a
  `MovementComponent`. It **cannot** send messages to clients (no message type is constructible
  from Lua), register opcodes, spawn actors, touch inventory/quests/magic, read config, or read
  the mod list. `ESLoader_Bindings.cpp` is an empty stub, never invoked. There is no
  `removeEventHandler`.
- **Dependency resolution is exact-match only.** The documented `*` wildcard is unimplemented;
  omitting a version yields `0.0.0`, which is falsy and *always* rejects the resource. No
  topological ordering — dependencies are not guaranteed to load before dependents.
- **ModPolicy is symmetric set-equality of plugin filenames.** A client missing a server mod is
  rejected as hard as one with an extra mod. There is no required-vs-forbidden distinction, and
  no version/hash checking. Animation mods are frequently plugin-less, so ModPolicy **cannot
  detect them at all** — despite them being the main cause of animation desync.
- `skyrimtogether.reconnect()` is called by the UI but never registered natively —
  it throws `TypeError`.

---

## 3. Server authority: the decision

**Full server-authoritative simulation is out of scope, permanently.**

This was researched rather than assumed. Four findings decided it:

1. **OpenMW** — 18 years, 500+ contributors, ~45k LOC of gameplay simulation, still no 1.0,
   for a 2002 game with no behavior graphs, navmesh, ragdolls, or perks. They did not match
   Morrowind's AI; they replaced it with Recast/Detour because reproduction was both hard and bad.
2. **TES3MP** — multiplayer built *on top of OpenMW*, i.e. with **complete source access to
   every gameplay system**. They still chose ping-based client authority handoff, and their own
   guide states *"The Server is dumb."* Full source access does not yield server authority; the
   simulation is too coupled to the client to relocate.
3. **skymp** — six years, 44 contributors, a working server-side Papyrus VM, and its
   authoritative damage formula is still `baseWeaponDamage × armorPenalty` with hardcoded
   2×/0.1×/1.3× multipliers and five open TODOs. Its input is still a client-reported `hitData`.
   NPCs remain on the roadmap; its flagship 650-player deployment **deletes every base-game NPC**.
4. **`maximegmd/OpenSkyrim`** — attempted by TiltedEvolution's own lead in 2017.
   Two commits, eight minutes, abandoned.

Realistic floor for partial simulation good enough to adjudicate combat and AI: **15–30
engineer-years**, with no upper bound and a serious chance the AI never reaches acceptable fidelity.

**Headless Skyrim as authority is also rejected.** The `TES` singleton holds exactly one
`gridCells`, one `interiorCell`, one `worldSpace` — confirmed in CommonLibSSE and independently
`static_assert`ed in this repo at `Code/client/Games/TES.h:31-47`. One instance can be
authoritative over exactly one 5×5-cell region: the same coverage an ordinary player client
already provides. N players in N places needs N instances that cannot see each other,
reintroducing the cross-instance sync problem with twice the processes. Havok's framerate-clamped
timestep additionally means a software-rendered instance simulates in slow motion relative to
its clients.

### 3.1 What we build instead

**The server can be made to check whether what a client claims is *possible*. It cannot be made
to decide what *happens*.** These are different projects separated by ~two orders of magnitude.

We adopt the first, in the specific form that matters for trusted co-op:

- **Arbitration, not adjudication.** Clients keep computing damage numbers in-engine. The server
  owns the *accumulator* and the *death decision*, keyed by hit id, broadcasting **absolute**
  health rather than deltas. This eliminates the desync class without any reverse engineering.
- **Plausibility bounds** (Phase 5) — cheap once ESM data exists, and valuable for robustness
  against bugs even with no adversary.

### 3.2 Deferred variant, recorded for later

A **fixed "anchor" instance** — a headless client parked permanently in one city, holding that
grid loaded and permanently owning its NPCs — is viable and would give persistent living hubs.
N instances buy N fixed regions. This is a persistence feature, not an architecture. Recorded,
not scheduled.

---

## 4. Architecture

Four tracks plus two foundation phases. Tracks are independent by design; each ships alone.

```
Phase 0  Instrumentation ──┐
                           ├──> Phase 1  Netcode spine
                           ├──> Phase 2  NPC consistency
                           ├──> Phase 3  Config & modding
                           │
Phase 4  ESM foundation ───┴──> Phase 5  Plausibility checks
                                Phase 6  NPC behaviour (frontier)
```

### 4.1 Design principles

- **Measure before optimising.** Phase 0 exists because every estimate in §2.2 is a model, not a
  measurement.
- **Each phase ships playable.** No phase is pure foundation with no felt benefit, except
  Phase 4 — which is explicitly late for that reason.
- **Protocol changes land together.** Bit-packing is touched once, in Phase 1, not twice.
- **Small, bounded units.** New subsystems (ack protocol, health ledger, config schema) get their
  own files with defined interfaces rather than accreting into the existing god-services.
  `CharacterService.cpp` is already ~860 lines doing eight jobs; we do not add a ninth.

### 4.2 How this document is used

This is a **roadmap spec**, not an implementation plan. It is deliberately larger than one
implementation cycle.

Each phase gets its **own implementation plan** written immediately before that phase begins, not
now — because Phase 0's measurements should inform Phase 1's design, Phase 1's outcome should
inform Phase 2's, and so on. Writing detailed plans for Phase 4+ today would be guessing.

The commitment implied by approving this document is to **Phase 0**, and to the sequencing and
the out-of-scope decisions in §3 and §7. Later phases are direction, not schedule, and may be
re-cut or dropped as earlier phases produce evidence.

---

## 5. Roadmap

### Phase 0 — Instrumentation & correctness baseline

*Goal: be able to see what is happening, and fix the free wins.*

- Server-side traffic metrics: per-player sent/recv bytes pre- and post-compression, per-message-type
  counts, tick timing histogram. Mirror the client's existing `Statistics` shape.
- Restore the commented-out client debug readouts (`NetworkView.cpp:44-62`); fix the 1Hz plot refresh.
- Repeatable measurement harness: scripted N-client load with recorded bandwidth/latency output.
- **Fix `CommandService` privilege escalation** — authenticate against `acMessage.pPlayer->GetId()`,
  not the packet-supplied `PlayerId`. Add the missing null check.
- Re-enable or delete the dead admin-message branch (`GameServer.cpp:566-580`).
- Fix the duplicate `Gameplay:bAutoPartyJoin` declaration.
- Register `skyrimtogether.reconnect()` natively, or remove it from the UI.
- Add the missing `CancelAssignmentRequest` server handler.

**Exit criteria:** a bandwidth/latency baseline for 8 players is recorded and reproducible.

### Phase 1 — Netcode spine

*Goal: ~430ms → ~150ms end-to-end; ~2.5–3× bandwidth reduction on the hot path.*

This is one coherent piece of work. The ack protocol is the foundation that makes unreliable
delivery safe *and* real delta compression possible; splitting it yields neither cleanly.

- **1a — Snapshot/ack protocol.** Sequence-numbered snapshots; receiver acks the last applied
  baseline; sender deltas against the acked baseline (Quake3/Source model). Per-player baseline
  ring buffer on the server, per-server baseline on the client.
- **1b — Real delta compression.** Replace `GenerateDiff(AnimationVariables{}, ...)` with a diff
  against the acked baseline. The machinery already exists and is correct — only the baseline
  is wrong.
- **1c — Channel split.** Movement snapshots → unreliable. `ActionEvent`s (order-sensitive,
  drive animation replay) → reliable, separate message. This is the change that removes
  head-of-line blocking.
- **1d — Latency budget.** Client send rate 10Hz → 20–30Hz (roughly bandwidth-neutral given 1b).
  Interpolation buffer 300ms hard-coded → adaptive, sized to measured jitter, with dead-reckoning
  extrapolation on starve instead of freezing.
- **1e — Encoding and allocation.** Position quantised relative to cell origin (~32 bits, *and*
  recovers the sub-unit precision currently discarded). `Direction` quantised to 8–12 bits.
  Use the existing `ScratchAllocator` instead of a 1 MiB heap allocation per message per player.
- Make `bPremiumMode` either meaningful or delete it; make `TogglePremium` re-apply live.

**Exit criteria:** measured end-to-end latency ≤200ms and hot-path bytes/actor/snapshot ≤50 at
8 players, against the Phase 0 baseline.

**Risks:** the channel split is the highest-risk item — anything implicitly relying on ordered
movement delivery must be found first. Extrapolation can overshoot; needs a clamp and a
snap-back rule.

### Phase 2 — NPC consistency

*Goal: NPCs stop dying twice, vanishing, and rubber-banding.*

- **2a — Server-owned health ledger.** Clients submit *hit claims* carrying a hit id, target,
  and client-computed damage. The server dedupes by hit id, applies to its own value, and
  broadcasts **absolute** health. The server alone decides death. Clients reconcile to the
  server value rather than accumulating deltas.
  Removes: additive cross-client stacking, independent death decisions, permanent health drift.
- **2b — `orphanMode`.** Adopt FiveM's model: when no owner is in range, the entity enters a
  server-held *unowned* state instead of being destroyed (`CharacterService.cpp:346-347`).
  AI pauses; state persists; the entity re-materialises when a player comes into scope.
  Per-entity policy: `DeleteWhenNotRelevant` / `DeleteOnOwnerDisconnect` / `KeepEntity`.
- **2c — Ownership rework.** Deterministic server-elected owner replacing first-come claims.
  Validate that the sender owns what it mutates, consistently, across *all* handlers — currently
  0/4 in `ActorValueService`, 0/4 in `MagicService`, 0/1 in `CombatService`.
  Predictive handoff to remove the round-trip stall.
  Transfer AI context (package, combat target) with ownership rather than restarting cold.
- **2d — Dedupe temporaries.** Give `0xFF……` actors a stable cross-client identity so a random
  encounter is one entity, not one per observer.

**Exit criteria:** in a scripted two-player fight against one NPC, health converges on all
clients and exactly one death event is observed.

### Phase 3 — Configuration & modding modality

*Goal: the explicit ask. Entirely greenfield, no ESM dependency.*

- **3a — Server config.** Declarative schema with types, ranges, defaults, and descriptions.
  Validation at load with actionable errors. Runtime reload. Enforce `kLocked` on INI/argv paths,
  not just `/set`. Expand `ServerSettings` replication well beyond the current 7 fields, with
  change notification, and fix the `operator==` omission.
- **3b — Client config.** A real native client configuration file (not CEF `localStorage`), with
  a client→server preferences channel so servers can read and, where appropriate, constrain
  client settings. Expose it in the existing Angular settings UI.
- **3c — Lua API.** Make it a real API: constructible message types so scripts can talk to
  clients; entity/world query and mutation bindings; config read/write; mod-list access
  (currently `#if 0`); `removeEventHandler`; custom event definition and triggering. Wire up the
  empty `ESLoader_Bindings.cpp` once Phase 4 lands.
- **3d — Resource system.** Real semver ranges (implement the documented `*`), topological load
  ordering, transitive dependency handling. Per-resource configuration files.
- **3e — Mod policy.** Replace symmetric set-equality with explicit **required** / **forbidden** /
  **optional** rules, plus version or hash checking. Address the animation-mod blind spot —
  plugin-less behavior mods are the main desync cause and are currently invisible to ModPolicy.
  Consider hashing the animation graph descriptor set as a compatibility token.

**Exit criteria:** a server operator can configure gameplay without recompiling; a Lua resource
can implement a non-trivial gamemode feature end-to-end including client communication.

### Phase 4 — ESM foundation

*Goal: the server learns what form ids mean. Explicitly deferred — this is the one phase with
no immediate felt benefit.*

- Re-enable `ESLoader::LoadFiles()` (`ESLoader.cpp:44-51`).
- Extend the record set past the current eight. Needed: `WEAP`, `ARMO`, `MGEF`, `SPEL`, `PERK`,
  `RACE`, `CSTY`, `LVLI`, `ENCH`, `AVIF`, `CELL`.
- Build the server world model — fill in the `Cell.h` / `Region.h` / `Map.h` stubs with a real
  spatial index.
- Replace grid-only interest management with distance/priority-based relevancy using real cell
  geometry.

**Exit criteria:** the server can answer "what is this form id, and what are its base stats?"

### Phase 5 — Plausibility checks

*Goal: robustness. Cheap once Phase 4 exists. Valuable against bugs even with no adversary.*

- Movement rate limiting from `RACE` base speed × max plausible multiplier, with a grace window
  and violation budget. **Legitimate teleports (script `MoveTo`, door transitions, fast travel)
  must be whitelisted or false positives will be intolerable.**
- Cell/worldspace adjacency validation via the door REFR graph.
- Damage upper bounds from `WEAP`/`ARMO`/`ENCH` — bound, not reproduce.
- Per-message-type token buckets. Currently there is no rate limiting anywhere.
- Navmesh reachability as a **soft** signal only. The `NVNM` parser already exists
  (`Chunks.h:333-381`), but navmesh covers NPC-walkable ground — players legitimately stand on
  rocks, tables, ledges and in water. Log/flag, do not reject.

### Phase 6 — NPC behaviour (frontier)

*Goal: NPCs feel alive to everyone, not just their owner. Highest risk; scope controlled.*

- **Revive AI package sync.** Note the disabled block will not compile as-is —
  `References.cpp:104` references `s_execInitPackage`, which is a `static thread_local` in
  `Actor.cpp`'s translation unit. Fix the seam before evaluating whether the sync works.
- **Dialogue arbitration.** A server-held conversation lock per NPC, plus shared topic/branch
  state, replacing the current cosmetic-only relay.
- **Server-side idle simulation** for unowned NPCs using Phase 4's navmesh — schedules,
  wandering, container respawn. This is the skymp path and should be treated as research: strictly
  time-boxed, with a defined fallback to "unowned NPCs simply pause" (Phase 2b) if it does not pan out.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| **Phase 1 channel split breaks ordering assumptions** | Audit every consumer of `ServerReferencesMoveRequest` before splitting; keep `ActionEvent`s reliable. |
| **Phase 6 is open-ended by nature** | Time-boxed, with Phase 2b as the defined fallback. It is last precisely so nothing depends on it. |
| **Upstream divergence makes fixes unharvestable** | Accepted, deliberately (§1.2). Keep `upstream` remote; cherry-pick selectively. |
| **Scope is large for a private server** | Phases 0–3 deliver every felt improvement. 4–6 are optional continuations, not commitments. |
| **Animation-mod desync is invisible to ModPolicy** | Phase 3e. Flagged as the highest-value modding fix. |
| **Estimates in §2.2 are models, not measurements** | Phase 0 exists to replace them with data before Phase 1 commits to a design. |

---

## 7. Out of scope

- Full server-authoritative simulation (§3) — permanently.
- Headless Skyrim as a general authority (§3) — the fixed-anchor variant is recorded but unscheduled.
- Wire compatibility with upstream 1.8.0 (§1.2).
- Fallout 4 support. Upstream's own port stalled; we do not inherit it.
- Anti-cheat as a *driving* requirement. Phase 5 delivers robustness; hardening against a
  determined adversary is not a goal for a trusted-group server.

---

## 8. Open questions

1. Phase 3b — should the server be able to *enforce* client settings (e.g. forbid a FOV or
   timescale), or only read them?
2. Phase 2a — on health reconciliation, do we snap or interpolate to the server value? Snapping
   is correct but visible.
3. Phase 3e — is hashing the animation descriptor set an acceptable compatibility token, or do
   we need real behavior-file hashing?
4. Target player count. The design assumes 8–16. Beyond ~30 the deferred Phase 5 interest-management
   work becomes mandatory rather than optional.
