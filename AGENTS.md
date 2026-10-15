# Repository Guidelines

## Project Overview

Obsidian is a C23 Vulkan engine/editor with scene, asset, physics, audio, and input subsystems. A single embedded MonadC runtime exposes selected engine capabilities to compiled Monad code; the engine and renderer remain C-owned. Treat `README.org`, `docs/ARCHITECTURE.org`, and `TODO.org` as the current project map and task queue. Cycle reports describe historical snapshots, not current verification.

## Architecture & Data Flow

`src/app/main.c` starts the sample application. The GLFW/event loop drives editor, scene, physics, and rendering. Monad work is admitted through the owner-thread event-loop checkpoint (`obs_monad_engine_process_pending`); compilation/publication still runs synchronously there and can stall a frame. `src/monad/runtime.c` owns runtime lifecycle and publication, `bindings.c` registers host calls, and subsystem `*_api.c` files expose narrow capabilities. Foreign values are typed and generation/lifetime checked. Keep Monad evaluation out of audio, GPU-driver, and input callbacks; keep raw Vulkan handles and render internals out of Monad bindings.

## Key Directories

- `src/app/`: executable entry point and sample scene.
- `src/{assets,audio,core,editor,graphics,physics,platform,scene}/`: native subsystem implementations.
- `src/monad/`: embedding lifecycle, host capabilities, REPL, publication queue and diagnostics.
- `monad/obsidian/`: Monad modules; `tests/monad/`: live-host integration fixtures.
- `tests/native/`: deterministic headless C tests; `tests/`: runner scripts and build checks.
- `shaders/`, `assets/`, `vendor/`, `tools/`: GPU programs, runtime data, dependencies, and optional utilities.
- `docs/`: subsystem contracts, tests/performance guidance, and cycle reports.

## Development Commands

Run from `obsidian/` so relative assets and modules resolve:

```sh
make -j2                         # Build engine; Jolt's nested build has its own JOBS
make run                         # Build and launch the graphical sample
make libs                        # Build libobsidian.a and libobsidian.so
make test-native                 # Headless native regressions
make test                        # Native tests plus SDK checks when available; integration may SKIP
make test-repl-abi               # Type-check bridge code against MonadC public headers
make test-monad                  # Requires OBSIDIAN_MONAD_TEST_RUNNER
make test-frame                  # Requires OBSIDIAN_MONAD_FRAME_RUNNER and a live GPU host
make bench-font BENCH_ITERATIONS=20000  # CPU UTF-8 scan only
```

`make test-all` is strict; set both `OBSIDIAN_MONAD_TEST_RUNNER` and `OBSIDIAN_MONAD_FRAME_RUNNER`. The runners must execute assertions in a real embedded host; parsing fixtures or passing native seam tests does not prove JIT, QTT, or renderer integration. `make msdf-tool MSDFGEN_SOURCE_DIR=/path/to/msdfgen` builds the optional MSDF utility. No lint target or formatter configuration is present; compiler warnings use the Makefile's `-Wall -Wextra` flags. `make clean` also removes the Jolt build directory. `make` does not track flag changes; after archive overlays or header/SDK changes, ensure dependent objects are rebuilt.

## Code Conventions & Common Patterns

Match nearby C: four-space indentation, braces on declaration lines, `snake_case` functions, `Obs`-prefixed public types, uppercase constants/include guards, and `/// SectionName` navigation comments. No generated headers are hand-edited.

Use the owning subsystem's API rather than reaching across internals. Monad callbacks return `monad_status_t`, report failures through `monad_error_t`, enforce owner-thread access, and validate foreign type plus runtime/structural generation before dereferencing. Register capabilities in `src/monad/bindings.c` or the owning `*_api.c`; do not scatter `monad_value_t` through rendering code. Make ownership and cleanup explicit for heap values, foreign references, and GPU resources. Keep frame/audio/render hot paths allocation- and logging-free unless a measured design requires otherwise.

Cross-thread work uses a bounded mailbox/request identity, not calls into the mutable Monad runtime. `source_stage.[ch]` reads BOOT files on one worker into immutable, owned snapshots (16 MiB maximum); the owner thread rechecks revision before publication. Optional `semantic_preflight.[ch]` runs only on that worker when `OBSIDIAN_MONAD_PREFLIGHT=1`; its copied diagnostics are advisory, not QTT/native-code proof, and never veto authoritative publication. The publication queue is owner-thread-only; BOOT requests coalesce, REPL source remains in its bounded model, and cancellation affects queued work only. File staging/preflight is asynchronous, but `monad_load_source` compilation/publication remains synchronous. Callback registrations made during source loading are journaled by `live_binding_txn.[ch]` and replayed only after native publication succeeds; this is not general host-effect rollback, and replay can still fail after publication. `publication_metrics.[ch]` separates queue wait from synchronous checkpoint CPU duration; it is not GPU/frame-time telemetry.

## Important Files

- `Makefile`: engine, libraries, tests, generated shader/font headers, and optional tools.
- `README.md` / `README.org`: setup, build, demo, and capability overview.
- `src/app/main.c`, `src/platform/window.c`: sample entry and event/frame checkpoints.
- `src/monad/runtime.c`, `bindings.c`, `publication_queue.[ch]`, `publication_metrics.[ch]`, `semantic_preflight.[ch]`, `live_binding_txn.[ch]`: runtime lifecycle, host boundary, queued requests, timing, advisory checks, and publication-scoped callback effects.
- `tests/build-contract.py`: build-plan and link-input checks; `tests/run_monad.sh` and `tests/run_frame_monad.sh`: real-host runner contracts.
- `docs/ARCHITECTURE.org`, `docs/EMBEDDING.org`, `docs/HOTRELOAD.org`, `docs/TESTING.org`, `BUILD-PROBLEMS.org`: ownership, runtime boundaries, validation requirements, and known issues.

## Runtime/Tooling Preferences

The supported build is Linux-oriented: GCC with C23 support, GNU Make, CMake/C++, `pkg-config`, Vulkan 1.3 tooling/driver, GLFW/X11, CGLM, FreeType/fontconfig, `glslangValidator`, `xxd`, `mold`, miniz, meshoptimizer, and Jolt. MonadC is expected at `../monadc`; override with `MONADC_ROOT=/path/to/monadc`. Its compatible `build/lib/libmonad-compiler.so` and `libmonad-embed.so` are required. There is no Node/Bun runtime or JavaScript package manager in this project; Python 3 is used by build checks.

## Testing & QA

Keep permanent tests deterministic and consumer-facing. Put isolated C invariants in `tests/native/`; use `tests/monad/` plus a real configured host for compiler/embedding behavior. Test failure, lifetime, bounds, and transition cases—not just registration or not-throw behavior. For graphical changes, run the actual app on a compatible display/GPU and report startup separately from interactive correctness or frame-time measurements.

Always distinguish `PASS`, `SKIP`, `FAIL`, and `NOT VERIFIED`. The current `make test` native suite is not a substitute for either integration runner. The standalone `python3 tests/build-contract.py` checks build-plan/link inputs, not runtime font loading. The generated `MapleMono-NF-Regular.h` is not included by current C sources, so its source TTF is not required for the engine build; separately, `src/app/main.c` still tries to load `JetBrainsMono-Regular.ttf`, while the editor's `load_editor_font()` path uses `OBSIDIAN_EDITOR_FONT` or Fontconfig fallback. `make test-build` is referenced by older guidance but is not a Makefile target. Check `BUILD-PROBLEMS.org` before relying on earlier build/JIT claims.
