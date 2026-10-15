# Monad engine how-tos

These are engine modules, not standalone Monad programs. They use live,
scene-owned engine objects and require Obsidian's embedded Monad runtime.
All Monad code lives under `monad/`; these examples live in `monad/howto/`.

Style rule: untyped Scheme throughout — `(define (name args) body)`,
`(define name value)`, `set!`, `if`, `begin`, `and`/`or`/`not`. No `::`
signatures, no guards, no `->` arrows, except numeric `[x : Int] -> Float`
pins where inference needs them (see `snake.mon` cell helpers). Top-level
host calls are avoided: capability borrows belong inside function bodies,
which standalone `check` defers to link time.

**Verification tiers (honest labels, see each file header):**
- `PUBLISHES` — boots headlessly today (`camera-policy`, `orbit-policy`).
- `DOCUMENTS INTENT` — standalone `monad check` passes every body and fails
  only at `on-frame!` (host macro, link-time); live boot stops at the
  compiler gate, observed as one of: `MONAD-C0004` (Frame callbacks),
  `MONAD-C0005` (resource queries), or environment-snapshot failure.
- `PROPOSES API` — no host binding exists yet; the guide is the API request
  (`text-print`, `scene-load`). `audio-play` uses `Audio.Synth` from Monad core;
  the native host accepts PCM and is covered by `build/tests/audio-synth`.
  Default editor typing playback awaits the cross-module QTT certificate
  boundary; boot currently rejects its scalar core-function call.
- `LIVE PLUGIN` — loads through `OBSIDIAN_MONAD_EDITOR_BOOT`
  (`editor-plugin`); forms mirror `monad/obsidian/editor.mon`.
- `SNAKE` — the playable-game spike (`snake.mon`): parses, host calls check,
  but module `set!` inside a frame callback is a compiler type error
  (`Frame -> Bool` vs `? -io-> Bool`). Filed in
  `docs/MONADC-API-REQUESTS.org`; awaits effectful callbacks or a host
  state store.

`monad check` alone does not exercise host bindings; standalone linking
reports host callbacks as unresolved. A successful C build is not proof
that Monad can call a binding — only the tier label plus its evidence is.

## Run a module

```sh
OBSIDIAN_MONAD_BOOT=monad/howto/animate-mesh.mon ./obsidian
OBSIDIAN_MONAD_EDITOR_BOOT=monad/howto/editor-plugin.mon ./obsidian
./obsidian --eval-file monad/howto/camera-policy.mon --frames 2 \
  --output /tmp/camera.rgba --agent-report /tmp/camera.json \
  --width 320 --height 720
```

A custom boot module replaces the default editor orbit policy; boot examples
include the required middle-mouse orbit functions. Drop/load a glTF scene in
the editor after startup where the example needs meshes. The headless
`--eval-file` path does not load an arbitrary glTF scene or return arbitrary
Monad values.

## Examples

Camera: `camera-policy`, `orbit-policy`, `camera-fly`, `camera-state`,
`gltf-orbit-360`.
Mesh/material: `animate-mesh`, `material-on-mesh`, `material-paint`,
`material-preset-cycle`, `material-reads`, `mesh-visibility`, `mesh-reads`,
`asset-inspect`, `transform-place`.
Scene/glTF: `inspect-gltf-skeleton`, `scene-stats`, `node-tree`, `gltf-nodes`.
SDF sculpt: `sdf-sculpt`, `sdf-pick-edit`, `sdf-gesture`, `sdf-appearance`,
`sdf-reads`.
Render: `render-quality`, `render-toggles`.
Input: `input-observe`, `input-keys`, `key-command`.
Host/telemetry: `frame-telemetry`, `frame-scope`, `dispatch-watch`,
`publish-telemetry`, `native-catalog`, `effect-gate`, `reload-self`.
Editor: `editor-plugin`, `editor-frames`.
New/proposed bindings: `audio-play` (core PCM, host playback), `text-print`,
`scene-load`.
Games: `snake` (awaiting effectful callbacks).

`gltf-joint-count` counts joint entries per skin; it does not deduplicate
repeated node IDs across skins. A live instance without skin metadata returns
zero. A stale foreign instance is rejected.

No binding exposes custom shader uniforms, shader compilation, or pipeline
mutation. The transform example animates a mesh; it does not animate shader
code. Audio.Synth currently provides bounded sine notes and a two-octave
typing instrument; waveform selection, envelopes, and polyphony are future
core API extensions.
