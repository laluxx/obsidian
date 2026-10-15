# Obsidian Engine

> **Born of fire, shaped by reflection.**
> A performance-oriented Vulkan engine gradually gaining live native Monad programming.

## Overview

Obsidian is a hand-written C Vulkan engine with GPU-driven rendering, scene,
input, audio, and an integrated editor.  Its original C runtime is preserved.
The new `src/monad/` bridge gradually enables typed engine capabilities from
compiled Monad code, following the embedding approach proven in Glemax.

It is *not* currently a completed live game language or a complete REPL.
Cycle 1 introduced the persistent Monad host. Cycle 2 added structurally
invalidated scene-node foreign objects, guarded camera and read-only input
capabilities, and deferred/coalesced reload requests. Cycle 3 adds shared
foreign-reference validation for scene-owned meshes, SDFs and glTF instances,
plus a read-only renderer-policy capability. Dynamic host-object
bindings still require real MonadC integration tests, and transactional
hot replacement is not yet available.

### Cycle 63 — Object-scoped undo

With the F8 SDF viewport active, **Ctrl+Alt+Z** undoes the latest active
original command involving the selected sphere, box or terrain while keeping
newer unrelated edits. Multi-object commands remain atomic and refuse to
undo on conflict. Normal **Ctrl+Z** remains chronological; **Ctrl+Shift+Z**
remains redo. The same selected-object operation is available through Monad
`sdf-inspect-undo-selected!` and the public
`obs_agent_scene_undo_selected` C API.

```sh
make test-sdf-selective-undo
make -j2 test
```

This is object-scoped scene undo, not Emacs text-region selective undo.
Actual GLFW, Vulkan and linked MonadC acceptance remain unverified; see
`docs/CYCLE63-REPORT.org` and `docs/SDF-OBJECT-UNDO.org`.

### Cycle 62 — Durable, portable OBSLOOK appearance presets

With the F8 SDF tool active, **Ctrl+Shift+C** samples appearance and
**Ctrl+Shift+S** saves it atomically as `obsidian-appearance.obslook`;
**Ctrl+Shift+L** loads that file into the clipboard and **Ctrl+Shift+V**
applies it to a selected sphere, box or terrain as one undoable edit. Set
`OBSIDIAN_SDF_LOOK_PATH` to override the path. The file contains appearance
values only—not geometry, transforms, CSG or scene identity.

`tools/obslook.py` independently authors and inspects the 100-byte format.
`make test-sdf-appearance-preset` exercises the Python/C codec boundary and
production scene journal. Actual GLFW shortcuts, Vulkan output and linked
MonadC execution remain unverified. See `docs/CYCLE62-REPORT.org` and
`docs/OBSLOOK-V1.org`.

### Cycle 61 — Copy/paste SDF appearance across primitive types

With the opt-in F8 SDF tool enabled, Ctrl+Shift+C samples the selected
primitive's material and emission; Ctrl+Shift+V applies those values to the
selected sphere, box or terrain. Geometry, transform, CSG settings and terrain
octaves stay unchanged. Paste is one undoable journal command. Compiled Monad
exposes the same value-only clipboard via `sdf-appearance-copy!` and
`sdf-appearance-paste!`.

```sh
make test-sdf-appearance-clipboard
make -j2 test
```

Native and callback-seam checks cover cross-type copy/paste, undo/redo,
failure preservation, effect rejection and fenced CPU-to-GPU slot copies.
Actual GLFW shortcuts, rendered Vulkan pixels and linked MonadC execution
remain unverified. See `docs/CYCLE61-REPORT.org`.

### Cycle 60 — Typed sphere/box viewport selection

The opt-in SDF viewport can analytically pick and drag translation-only
spheres and axis-aligned boxes. Box position and extents now use the shared
checked inspector journal; undo/redo updates CPU picking geometry. Existing
sphere-only Monad/agent APIs remain sphere-only. This is primitive-surface
picking, not CSG/terrain raymarching or support for rotated/scaled SDFs.

```sh
make test-sdf-typed-viewport
# Full Vulkan/GLFW/MonadC acceptance requires a workstation:
make test-sdf-viewport-runtime
make run
```

See `docs/CYCLE60-REPORT.org`. Native coverage passes; interactive Vulkan
acceptance is not established here.

### Cycle 56 — Correct GLFW/HiDPI viewport coordinates and captured gestures
MonadC/Vulkan integration. Apply the patch over the existing Cycle 4 tree.

### Cycle 56 — Correct GLFW/HiDPI viewport coordinates and captured gestures

The opt-in F8 SDF editor now converts GLFW logical cursor positions to the
current Vulkan render target pixel extent before projecting real camera rays.
A captured drag can continue outside the window without clamping. Lost GLFW
button-release events are reconciled by the input poll; focus loss requests
cancellation even if an external scene change initially prevents rollback.
Swapchain recreation also preserves camera clip planes and the Vulkan Y flip.

```sh
make test-viewport-coordinates test-sdf-viewport
make -j2 test
# Requires the full graphics/Monad SDK and desktop input:
make test-sdf-viewport-runtime
make run
```

See `docs/CYCLE56-REPORT.org` and `docs/VIEWPORT-INPUT-OWNERSHIP.org`.
Actual GLFW/Vulkan/MonadC display acceptance is **not** established by the
native tests in this container.

### Cycle 58 — Continuous inspector edits coalesce into one undo command

The production SDF sphere inspector now previews Position, base RGBA, Metallic,
Roughness, radius, CSG operation/smoothness, and emission through one
scene-epoch-owned transaction. A drag or picker session commits one shared
undo command; Escape, focus loss, selection switch, or closing the Inspector
requests checked rollback. Conflicting external writes keep the transaction
open rather than claiming success. Other SDF types keep their prior direct
edit path.

```sh
make test-sdf-property-gesture
make -j2 test
# Requires complete MonadC and a Vulkan host for interactive acceptance:
make test-sdf-viewport-runtime
make run
```

See `docs/CYCLE58-REPORT.org` and `docs/SDF-PANEL-TRANSACTIONS.org`. Native
tests prove the production journal/bridge path, not visible widget, picker,
focus, or physical-display behavior.

### Cycle 57 — Sidebar SDF edits share scene undo

For spheres, the visible inspector now sends Position, base RGBA, Roughness,
and Metallic through the same epoch-checked scene journal used by viewport
gestures, Monad, and the agent API. Combined position/material changes are
one atomic undo command. Invalid or stale edits and sidebar writes during an
active viewport preview fail closed. Radius, CSG, emission, non-sphere fields,
and continuous sidebar-drag coalescing remain outside this slice.

```sh
make test-sdf-property-bridge
make -j2 test
# Full integration requires the MonadC SDK and Vulkan host:
make test-sdf-viewport-runtime
```

See `docs/CYCLE57-REPORT.org` and `docs/SDF-PANEL-UNDO.org`. Native tests do
not establish interactive widget, modal picker, or physical-display behavior.

### Cycle 54 — Live, cancellable scene gestures

The production SDF inspector now exposes *speculative edits*: begin a group,
preview changes across one or more spheres, then commit them as one Emacs-style
undoable command or cancel them as a whole. The committed journal is left
untouched by cancellation. A conflicting outside mutation fails closed; each
group is limited to 64 property changes (not scene topology changes).
Both the public agent API and compiled Monad use the same gesture state.

```sh
make test-scene-gesture    # production scene + SDF SSBO source, native tests
make -j2 test              # all available native/tooling tests
make test-sculpture-runtime # strict real MonadC/Vulkan acceptance; SDK required
```

For full behavior and verification limits, see `docs/SCENE-GESTURES.org` and
`docs/CYCLE54-REPORT.org`.

See `TODO.org` for the sole active queue and `docs/ARCHITECTURE.org` /
`docs/EMBEDDING.org` for ownership and first-slice API contracts.

### Cycle 18 — Host effects and callback preflight

Engine-side runtime effects now have a shared, tested admission policy for
read/write/GPU/audio/I/O and frame lifetimes. The Monad language can inspect
current mutation readiness through `(engine-mutable?)` and
`(engine-gpu-mutable?)`. The callback publication journal checks an entire
pending key/frame replacement before retiring old callbacks.

This is **not QTT-certified host importing or all-or-nothing JIT+host commit**:
compilation can still stall frames, and a post-publication keymap allocation
failure is still possible. See `docs/HOST-EFFECTS.org` and the single
`TODO.org` for the exact guarantees and remaining tasks.

### Repository layout

- `src/{app,assets,core,platform,graphics,scene,physics,audio,editor,monad}/`: owned C subsystems
- `monad/`: all Monad sources — `obsidian/` engine policy/library modules,
  `Foreign/` per-type method modules, `howto/` runnable engine examples
- `tests/monad/`: Monad integration fixtures; requires a real embedded host test runner
- `tests/native/`: headless checks for native ABI-adjacent utilities
- `shaders/`, `vendor/`, `docs/`: shaders, dependencies and architecture

### Live Monad demo (requires MonadC and engine dependencies)

```sh
OBSIDIAN_MONAD_BOOT=monad/obsidian/live-demo.mon ./obsidian
```

Press `C-M-<right>` to apply camera movement computed by a compiled
Monad `Int -> Int` function; F5 queues compilation of the configured boot file
at the next explicit event-loop checkpoint.
Actual in-process same-symbol reload semantics need verification against MonadC.  This is currently a *development reload*, not an
atomic rollback-safe production hot-swap.

Without `OBSIDIAN_MONAD_BOOT`, the engine loads
`monad/obsidian/editor-orbit.mon` as its default editor policy module.
Middle-button orbit direction and sensitivity are Monad-defined. Any custom
boot module that replaces the default must export both typed
`obsidian_middle_orbit_yaw_urad` and `obsidian_middle_orbit_pitch_urad`
functions to retain middle-drag orbit.

### Headless Monad-driven image

An agent can run one offscreen Vulkan frame, publish a Monad source unit into
the engine-owned host, and capture the resulting RGBA8 image without creating
a GLFW window:

```sh
./obsidian --eval '(module Main)
(define (obsidian_camera_step_mm [current : Int] -> Int)
  (+ current 500))' \
  --output /tmp/frame.rgba --width 1280 --height 720
```

`--eval` compiles/publishes Monad source-unit text that defines the typed
`obsidian_camera_step_mm : Int -> Int` entry and invokes it through the
existing camera-policy bridge; it is not a general interpreter for arbitrary
expressions. The frame uses the current deterministic sample draw callback.
See `docs/HEADLESS-RAW.org` for output format and limits. A stable multi-frame
engine-as-library agent API remains on the TODO queue.

### Cycle 52: procedural scene editing with reversible inspection

The existing =monad/obsidian/sculpture.mon= headless demonstration now
selects a sphere, edits its material and uses Monad undo/redo. An external
client continues the same scene selection/history and captures a Vulkan
frame. This is a bounded in-memory inspector, not yet a visual GUI panel or
persistent scene-history engine. Compile the native boundaries with
`make test`, and run `make test-sculpture-runtime` on a provisioned Vulkan/
MonadC installation. See `docs/CYCLE52-REPORT.org` for exact evidence.

### Cycle 53: Emacs-style reversible scene history

Undo now follows GNU Emacs' command semantics rather than a linear redo
cursor: consecutive undo steps traverse older edits; after a command break,
ordinary undo undoes an undo. `undo-only` deliberately skips such inverses,
and `undo-redo` consumes them. Multiple scene property edits can form one
atomic inspector command group. The same behavior is available through Monad
and the public agent API. Run `make test-emacs-scene-undo` and read
`docs/EMACS-SCENE-UNDO.org` for source comparisons and caveats.

### Quickstart

```C
#include <obsidian/obsidian.h>

int sw = 1920;
int sh = 1080;

int main() {
    /* Optional for hosts with their own editor.  Must precede initWindow. */
    obs_set_editor_enabled(false);
    initWindow(sw, sh, "Program name");

    Font *jetb = load_font("JetBrainsMonoNerdFont-Regular.ttf", 40);

    keychord_bind(&keymap, "M-=", nextTheme,     "Next theme",     PRESS);
    keychord_bind(&keymap, "M--", previousTheme, "Previous theme", PRESS);

    loadThemeByName("modus-vivendi");

    while (!windowShouldClose()) {
        beginFrame();
        clear_background(CT.bg);
        fps(jetb, context.swapChainExtent.width - 300, 100, CT.error);
        endFrame();
    }

    return 0;
}
```

The 2D renderer also exposes a face-text ABI. `character_ex` accepts an
`ObsFaceShaderData` payload, copied to every vertex of a glyph quad. The
payload is available in `shaders/2D.frag` as `fragFaceData0` (shader id,
logical buffer position, line, column), `fragFaceData1` (face-run offset,
codepoint, flags, event kind), `fragFaceEventData` (event offset, count,
serial, anchor), and `fragFaceParams` (four user values). The event kind is
none, insert, or delete; deletion retains a bounded snapshot of removed
glyphs for the event duration. `fragFaceEvent.x`/`.y` provide event time and
duration, while `pc.frame.x` supplies current time in seconds. Hosts should
use logical positions, not pixel coordinates, for effects that must survive
insertion and deletion. `pc.pointer.xy` supplies the current screen-space
pointer position for proximity effects; a negative x means pointer data is
unavailable. The host updates this once per pointer motion, and the renderer
passes the latest position to each 2D draw.

---

### 🚧 Under Construction

**Obsidian** is still cooling.
The first shards of reflection are forming — a renderer, a REPL, a spark of self-awareness.
Contributions, discussions, and ideas are welcome while the core takes shape.

### Build prerequisites and archive completeness

Files matching `*.blend.zip` are versioned with Git LFS. Install Git LFS before
cloning so checkout retrieves the asset contents rather than leaving pointer
files in the working tree; run `git lfs pull` if a checkout still has pointers.

This source snapshot does not contain the original runtime `assets/` tree.
Cycle 6 removes the generated MapleMono header dependency. The editor now
loads `OBSIDIAN_EDITOR_FONT`, an optional local TTF, or a Fontconfig monospace
font. A functional graphics build still requires Vulkan, GLFW, CGLM, FreeType,
fontconfig, shader compilation tools, Jolt and a sibling MonadC checkout.
See `docs/CYCLE1-REPORT.org` and `docs/CYCLE2-REPORT.org` for verification status.

## Incremental Cycle 4 — live Monad editing and MSDF

Source navigation now uses `/// SectionName` throughout Obsidian-owned C
and GLSL code; third-party vendored source is deliberately unchanged.

Optional automatic root-module reload:

```sh
OBSIDIAN_MONAD_BOOT=monad/obsidian/live-demo.mon \
OBSIDIAN_MONAD_WATCH=1 ./obsidian
```

`F5` triggers immediate reload at the next owner-thread safe point. File-watch
recompilation is debounced; it is currently synchronous, **not** atomic
transactional image replacement. See [`docs/HOTRELOAD.org`](docs/HOTRELOAD.org)
for implementation and MonadC staging prerequisites.

For real multi-channel glyph distance textures, install upstream `msdfgen`
and run `make msdf-tool`; or configure
`MSDFGEN_SOURCE_DIR=/path/to/msdfgen`. See
[`docs/MSDF.org`](docs/MSDF.org) for integration guidance and the distinction
from Obsidian's existing 3D ray-marched SDF shader. The current UI font
renderer still uses its original bitmap atlas by default.

## Incremental Cycle 5 — real measurements and safer image lifetimes

The live image remains owned by MonadC. On reload, Obsidian expires old
Monad key closures and disables the optional native camera callback if
publication fails; this is fail-closed behavior, **not transactional rollback**.
Compiler-load CPU latency (last/p95/p99) is exposed to Monad through the
existing object space. See `docs/HOTRELOAD.org` for the proposed QTT-aware
candidate/commit protocol (not currently implemented in MonadC).

The bitmap font path now validates UTF-8 scalar boundaries, preflights glyphs
before determining atlas UVs, and has an opt-in `OBSIDIAN_FONT_PROFILE=1` mode.

```sh
./make help                 # unified Python CLI help
./make clean                # all generated engine/build/test outputs
make help                   # original GNU Make target help
./make test                 # backend suite; explicit SKIP for gated live suites
./make test all             # strict integration gate; configure both runners
./make help test            # unified test suite catalog
./make test native          # headless native regressions
./make bench --list         # unified benchmark catalog and requirements
./make bench cpu            # every registered CPU-only microbenchmark
./make bench engine         # headless Vulkan/Monad workload
OBSIDIAN_FONT_PROFILE=1 ./obsidian
```

`./make` is an additive CLI over the existing Makefile, not a replacement.
GNU Make remains the backend and every existing target/recipe remains callable
directly. `./make test` maps to `make test`; `./make test all` maps to the
strict `make test-all`. The default test gate visibly skips unavailable live
SDK/runner suites; strict `all` requires the configured integration runners.

`./make bench` runs all registered workloads sequentially. `make bench`
delegates to the same CLI; set `BENCH_SELECTION=cpu`, `keymap`, or another
listed name to select a subset. The engine and validation workloads require
their documented GPU, asset, and MonadC dependencies.

See `docs/TESTING.org`, `docs/CYCLE5-REPORT.org`, and `docs/perf/`.

## Incremental Cycle 6 — embedded Monad language surface

`make` and `make -j2` now select the engine (`all`), never the optional
`msdf-tool` by default. The editor no longer includes an unshipped generated
MapleMono header; it uses `OBSIDIAN_EDITOR_FONT`, an optional local TTF, then
Fontconfig. Original runtime icons/assets are still not included in this
source archive, and a complete engine run remains unverified.

The engine registers the compact Monad foreign-object API directly:

```scheme
(let ([world (scene)])
  (if (> (mesh-count world) 0)
      (let ([m (mesh 0)])
        (mesh-visible! m True)
        (mesh-name m))
      "empty scene"))
```

`(mesh index)` is an engine-owned **borrow**, not a new mesh allocation. Read
`docs/LANGUAGE.org` and `monad/obsidian/language.mon` for the complete
short API and native `Syntax -> Syntax` macro examples. The dynamic compiled
MonadC bridge and macro execution still require real in-engine tests.

Per-object Monad method modules live in `monad/Foreign/<Type>.mon`; each
declares the corresponding `◯` receiver family and delegates methods to the
existing checked host operations. These are source interfaces, not yet an
executable typed bridge: Monad's module importer does not currently import
the engine's nominal foreign types, and standalone compilation cannot link
the dynamically registered callbacks. Do not treat the modules as runtime
validated until both compiler/host integration and a live-engine test exist.
In MonadC, `monad_compiler_import_foreign_types` currently populates the
separate QTT foreign-type inventory; source analysis does not consume that
inventory, and native source imports are limited to scalar `i64`. This needs
a MonadC source-type and dynamic-call ABI change, not an Obsidian-side alias.

From your canonical `obsidian/` directory, run `make test` (headless), or
`make test-all` with `OBSIDIAN_MONAD_TEST_RUNNER` for the strict Monad gate.

## Cycle 7: automatic parallel builds and live Monad frames

Ordinary `make` builds the engine across all available online logical CPU
cores by default, unless a job limit is supplied (`make -j4`, or `make JOBS=4`).
Jolt and JoltC are configured/built as one grouped CMake target; its nested
build is capped at four jobs by default (`JOLT_JOBS=...`). `JOLT_NATIVE=1`
is an opt-in local-CPU optimization, not suitable for redistributable binaries.
Jolt link-time optimization is off by default after the GCC 16.2.1/binutils
2.47 LTO archiver failed while creating `libJolt.a` with impossible allocation
sizes. Opt in with `JOLT_LTO=ON` when that toolchain path is known to work.

Monad frame programming is exposed as `(frame)` (a one-frame foreign object),
`(on-frame! callback)`, `frame-delta`, dimensions and engine CPU profiling.
Example boot source: `monad/obsidian/live-frame.mon`. See
`docs/FRAME-API.org` for semantics and the separate GPU-backed integration
harness. `make test` covers headless lifecycle tests but does not claim a
working Monad/Vulkan integration without the required external SDKs.


### Cycle 8: QTT-aware native hot replacement (research and host gate)

See `docs/QTT-HOT-REPLACEMENT.org`, `docs/CYCLE8-REPORT.org`, and the root
`TODO.org`. The native host now tracks callback leases, publication phases,
and incompatible compiler-contract descriptors. Full transactional reload
**requires** MonadC staged images and compiler-authored QTT metadata; the
current in-place loader is explicitly non-transactional. Test with
`make test-native` and use `make test-all` only with a real embed test runner.


## Experimental native Monad REPL (Cycle 9)

With the engine and the actual MonadC SDK linked, toggle Obsidian's REPL
overlay with `C-c r`. It uses the existing GPU-backed 2D font renderer,
not a second scripting VM. Submit source **definitions** with Enter, use
Ctrl-J for a source newline, Ctrl-P/N for history and Ctrl-G to close.
`:check <source>` requests semantic preparation without publication.

Native compile presently runs synchronously at the engine checkpoint, so
this isn't yet a nonblocking hot-reload UI. The compiler extension requests
needed to fix that are in `docs/MONADC-API-REQUESTS.org`.

`make test` runs the headless REPL model and existing native tests.
`make test-repl-abi` additionally requires the actual MonadC public headers.

## Cycle 16 — Optional off-thread Monad compiler preflight

Set `OBSIDIAN_MONAD_PREFLIGHT=1` to run advisory parse/type diagnostics in a
separate worker-owned MonadC compiler session before boot-module publication.
The GPU REPL `:preflight` and `(source-preflight)` in
`monad/obsidian/live-reload.mon` report status and worker time. The live
runtime **still makes every publication decision and compiles native code on
the owner thread**; preflight results never veto a source update. See
`docs/SEMANTIC-PREFLIGHT.org` and `docs/CYCLE16-REPORT.org`.

### Cycle 17: publication-safe live callbacks

`on-frame!` and `bind-key!` registrations made while compiling a Monad
module are now journaled, rather than applied to the running image. Rejected
compilation preserves the previous callbacks. `make test` verifies the
headless transaction semantics; real MonadC+Vulkan integration still requires
a configured engine workstation. See `docs/CYCLE17-REPORT.org`.

## Cycle 21 — Profile embedded Monad dispatch CPU cost

Set `OBSIDIAN_MONAD_PROFILE_FFI=1` before launching the engine to profile the
per-frame Monad callback and generation-pinned compiled native camera policy.
The REPL command `:dispatch` and the `(frame-dispatch-health)` /
`(camera-dispatch-health)` Monad helpers report attempts, errors, and CPU p95
bucket bounds. With profiling disabled, no additional clock reads occur.
Run `make bench-dispatch` for a **recorder-only** benchmark, not an FPS estimate.
See `docs/CYCLE21-REPORT.org` for exact scope, results and test limitations.

## Cycle 22 — Constant-time foreign resource lookup

Mesh, scene-node, SDF, and glTF references retain their Monad type and
generation checks, then use a checked O(1) dense-slot resolver rather than a
linear pointer scan. The resolver assumes ordinary flat-address CPU pointers;
it is not a CHERI/capability-pointer implementation. Use `(sdf-index ref)`,
`(gltf-index ref)`, `with-sdf`, and `with-gltf` for the added resource forms.
`make bench-foreign-slots` measures only C slot membership. See
`docs/FOREIGN-SLOT-BOUNDARY.org` and `docs/CYCLE22-REPORT.org`.

### Cycle 47 — bounded headless agent jobs

Submit a Monad **source file**, run 1–10000 production offscreen frame steps,
then atomically publish the final RGBA8 image and a JSON success receipt:

```sh
./obsidian --eval-file monad/obsidian/live-demo.mon --frames 120 \
  --output /tmp/frame.rgba --agent-report /tmp/frame.json \
  --width 640 --height 480
```

The compiled module must export `obsidian_camera_step_mm : Int -> Int`.
Each frame checks its compiled camera-policy call and runs the existing
headless Vulkan submission. This is **not** arbitrary expression evaluation
or guaranteed fixed-time game simulation. Reports are only written after a
successful image; existing receipts are removed before an attempt. See
`docs/HEADLESS-RAW.org` for ownership, failure and verification boundaries.

### Cycle 48 — external headless C agent API

A standalone `obsidian_agent.h` now exposes a versioned, opaque and
owner-thread-only API for a headless engine session. External C programs can
create a session, publish source files or inline Monad units, submit frames,
capture raw images/JSON receipts and shut down without including internal
Vulkan or compiler headers. The engine remains a single process-global
renderer/runtime; this is *not* multi-instance embedding. See
`docs/AGENT-ABI.org` and `tests/integration/agent_client.c`.

`make test-agent-api-compile` checks the public client without third-party
dependencies. On a machine with all graphics/MonadC dependencies, run the
strict `make test-agent-runtime` to compile and execute the real linked engine.
Source-seam tests passing alone do not establish driver/JIT correctness.

### Cycle 49 — real typed SDF scene-control slice

The Cycle 48 external C agent can now create a sphere in Obsidian's production
scene and SDF renderer buffers, move it, update its material and query its
state using a value-typed `ObsAgentSdfRef` ticket. Scene reset invalidates old
tickets; capacity, failed GPU quiescence and invalid values reject without
publishing partial edits. These operations use the real Monad host effect
admission gate and require a ready, published runtime generation. They do not
expose raw C/GPU pointers.

The upgraded `tests/integration/agent_client.c` exercises this complete
create/move/material/query/step/capture sequence against the linked library
with `make test-agent-runtime`. Device-wide GPU idle is currently required
for each changed SDF edit, pending fence-slot dirty uploads. This is a
conservative correctness contract, **not** a high-throughput mutation API or
a compiler-proven QTT scene import. The native seam tests pass; actual linked
Vulkan/MonadC execution and physics advancement remain unverified. See
`docs/CYCLE49-REPORT.org` and `docs/AGENT-ABI.org`.

### Cycle 50 — compiled Monad builds a living SDF sculpture

A real vertical slice connects **compiled Monad source → host-admitted scene
creation → stable lifetime-checked sphere tickets → deferred per-fence-slot SSBO
uploads → production headless Vulkan frames → captured RGBA8 and viewable PNG**.
It uses the engine's existing GPU raymarcher, not a separate renderer.

`monad/obsidian/sculpture.mon` uses `on-frame!` to construct five colored
spheres during its first frame and move/recolor the central sphere on following
frames. Its host operations are `sdf-sphere!` (seven finite numbers),
`sdf-ticket`, `sdf-move!`, `sdf-tint!`, and `sdf-x`. Host effect checks reject
writes during native image publication; integer tickets are validated against
the current scene lifetime (they are **not** compiler-authored QTT proofs).
The standalone `tests/integration/agent_sculpture.c` client requires only
`obsidian_agent.h`: it loads the module, advances five frames, queries real
SDF state, captures a sixth frame, and checks output dimensions. The `make`
target converts the result to a PNG without Pillow:

```sh
# Full Vulkan, MonadC, Jolt, GLFW and compatible GPU driver required:
make test-sculpture-runtime
# Image: build/tests/sculpture.png
# Machine-readable run: build/tests/sculpture.json
```

The portable portion is always testable via `make test`: real production C
scene/host bindings, per-slot GPU staging semantics with mapped-buffer fixtures,
SDF capacity/lifetime validation, external client C syntax, and PNG round trips.
The linked JIT+Vulkan integration target is **strict**: without a real GPU and
MonadC development toolchain it does not claim to pass. See
`docs/CYCLE50-REPORT.org` and `docs/SDF-SCULPTURE.org` for the precise contract.

### Cycle 51 — pick Monad-created geometry and edit it from an external client

One generation-checked, process-monotone sphere-ticket registry now serves
both the compiled Monad bindings and the public external C API. A ray through
a sphere created *inside Monad* returns an `ObsAgentSdfRef` which the external
client can query and edit; C-created spheres also have the same Monad IDs.
No scene-array pointer, GPU address or opaque compiler allocation escapes.

The new `obs_agent_scene_pick_ray()` returns the nearest translated primitive
sphere hit, including distance, position and normal. `OBS_AGENT_NO_HIT` is
separate from invalid input. The new read-only Monad `sdf-raycast` accepts
origin XYZ, direction XYZ and maximum distance; it returns a positive Int64
ticket or zero for no hit. This is a CPU primitive test, **not** a shader-
exact smooth-union or mesh raycast.

`make test-sculpture-runtime` now exercises the whole sequence: compiled Monad
creates five objects, performs spatial selection and tints one, the C client
picks that same object and edits its material, then a real Vulkan frame and
RGBA/PNG/JSON output are required. Native source tests pass; linked GPU/JIT
acceptance remains unverified until run with the full toolchain. See
`docs/CYCLE51-REPORT.org` for evidence and limitations.

### Cycle 55 — Interactive SDF sphere viewport gestures

The editor now has an **opt-in F8 SDF drag tool**. With editor mode active,
click a ticketed SDF sphere and move the cursor to preview its position in the
actual Vulkan scene. Release to create **one** Emacs-style undoable change;
press **Escape** during the drag to cancel without changing undo history.
While the viewport tool is on, **Ctrl+Z** and **Ctrl+Shift+Z** undo/redo scene
edits (provided an editor panel is not consuming the pointer). Losing focus
also attempts safe cancellation. Other editor workflows remain the default
when F8 is off. Thousands of drag motion samples are coalesced to a single
transaction record, not the 64-record bounded group budget.

Run `make test-sdf-viewport` to test the actual CPU scene, journal and SDF
publication sources, or `make test-sdf-viewport-runtime` with a complete
Vulkan/MonadC toolchain for strict end-to-end execution of a compiled Monad
sculpture and image output. A real GLFW display acceptance test remains
necessary for pointer/pixel correctness. See `docs/SDF-VIEWPORT.org`.
