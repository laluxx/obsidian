# Engine is the default; optional developer tools never shadow it.
.DEFAULT_GOAL := all

# Parallelize the whole dependency graph by default, not only the Jolt nested
# CMake build. Keep explicit make -j/-jN and an inherited jobserver authoritative.
# getconf is POSIX; nproc is a fallback. Both return ONLINE logical CPUs.
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || printf 1)
ifeq ($(filter -j% --jobs% j%,$(MAKEFLAGS)),)
  MAKEFLAGS += -j$(JOBS)
endif

# CMake's nested build does not universally participate in GNU make's
# jobserver. Keep its memory-heavy C++ compile bounded by default.
JOLT_JOBS ?= 4
JOLT_BUILD_TYPE ?= Distribution
JOLT_LTO ?= OFF
JOLT_PHYSICS_ROOT ?=
JOLT_NATIVE ?= 0
JOLT_EXTRA_CMAKE ?=
# Explicitly setting JOLT_LTO means reconfigure even when the archives exist.
.PHONY: force-jolt-reconfigure
ifneq ($(filter command line environment,$(origin JOLT_LTO)),)
  JOLT_CONFIG_FORCE = force-jolt-reconfigure
endif
ifeq ($(JOLT_NATIVE),1)
  JOLT_CPU_TUNING = -DCMAKE_CXX_FLAGS=-march=native
else
  JOLT_CPU_TUNING =
endif

CC = gcc
MONADC_ROOT ?= ../monadc
MONADC_INCLUDE = $(MONADC_ROOT)/src/embed/include
MONADC_LIB = $(MONADC_ROOT)/build/lib
MONADC_DEP_OBJECT ?= $(MONADC_ROOT)/build/obj/debug/compiler/src/dep.o
MONADC_INTERNAL_FLAGS = -iquote $(MONADC_ROOT)/src -iquote $(MONADC_ROOT)/src/tooling
MONADC_LLVM_FLAGS = $(shell llvm-config --ldflags --libs core orcjit native passes --system-libs 2>/dev/null)
MONADC_LIVE_LINK = $(MONADC_DEP_OBJECT) \
	$(MONADC_LIB)/libmonad-live.a $(MONADC_LIB)/libmonad.a \
	-pthread -lreadline -lgmp -lclang $(MONADC_LLVM_FLAGS)
# Optimized by default: previous builds accidentally compiled all hot paths at -O0.
# Keep the ISA portable; CPU_FLAGS=-march=native is an opt-in local choice.
OPT_LEVEL ?= -O2
CPU_FLAGS ?=
CFLAGS = -std=c23 $(OPT_LEVEL) $(CPU_FLAGS) -Wall -Wextra -g -fPIC $(shell pkg-config --cflags freetype2) $(shell llvm-config --cflags 2>/dev/null) -I$(MONADC_INCLUDE) $(MONADC_INTERNAL_FLAGS)
JOLT_STATIC = vendor/joltc/build/lib/libJolt.a
JOLTC_STATIC = vendor/joltc/build/lib/libjoltc.a
LDFLAGS = -fuse-ld=mold -rdynamic -lvulkan -lglfw -lX11 -lcglm -lm -lminiz -lmeshoptimizer $(LIBRARIES) $(JOLTC_STATIC) $(JOLT_STATIC) -lstdc++ $(shell pkg-config --libs freetype2 fontconfig) -L$(abspath $(MONADC_LIB)) -Wl,-rpath,$(abspath $(MONADC_LIB)) -lmonad-embed -lmonad-compiler $(MONADC_LIVE_LINK)

# Opt-in C++ MSDF generator: this is deliberately independent of the
# production C/Vulkan binary until the glyph atlas/pipeline switch is ready.
MSDFGEN_SOURCE_DIR ?=
MSDF_BUILD_DIR ?= build/msdf

.PHONY: all internal_build msdf-tool test-headless-middle-orbit
.PHONY: clean distclean rebuild
msdf-tool:
	cmake -S tools/msdf -B $(MSDF_BUILD_DIR) $(if $(MSDFGEN_SOURCE_DIR),-DMSDFGEN_SOURCE_DIR=$(abspath $(MSDFGEN_SOURCE_DIR)))
	cmake --build $(MSDF_BUILD_DIR) --parallel $(JOBS)

GLSLANG = glslangValidator
XXD = xxd

# Project layout: one src/<subsystem>/ directory per architectural owner.
# Include search paths preserve the existing legacy header API in this cycle.
SUBSYSTEMS = app assets audio core editor graphics monad physics platform scene
SRC_INCLUDES = $(addprefix -Isrc/,$(SUBSYSTEMS))
INCLUDES = -I. $(SRC_INCLUDES) -I$(SHADER_DIR) -Ivendor -Ivendor/joltc/include
LIBRARIES = -L/usr/lib/x86_64-linux-gnu
INSTALL_DIR ?= /usr

LIB_NAME = libobsidian
EXECUTABLE = obsidian
MAIN_SOURCE = src/app/main.c
# TinyEXR's C implementation backs the EXR decoder used by the renderer.
VENDOR_C_SOURCES = vendor/tinyexr_c_impl.c
LIB_SOURCES = $(filter-out $(MAIN_SOURCE),$(foreach sub,$(SUBSYSTEMS),$(wildcard src/$(sub)/*.c))) $(VENDOR_C_SOURCES)
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
MAIN_OBJECT = $(MAIN_SOURCE:.c=.o)
ALL_OBJECTS = $(LIB_OBJECTS) $(MAIN_OBJECT)
HEADERS = obsidian.h obsidian_agent.h $(foreach sub,$(SUBSYSTEMS),$(wildcard src/$(sub)/*.h))
DEPFILES = $(ALL_OBJECTS:.o=.d)

# Shaders
SHADER_DIR = shaders
SHADER_VERTS = $(wildcard $(SHADER_DIR)/*.vert)
SHADER_FRAGS = $(wildcard $(SHADER_DIR)/*.frag)
SHADER_COMPS = $(wildcard $(SHADER_DIR)/*.comp)
SHADER_TASKS = $(wildcard $(SHADER_DIR)/*.task)
SHADER_MESHS = $(wildcard $(SHADER_DIR)/*.mesh)
IBL_FP16_SPVS = $(SHADER_DIR)/equirect2cube.fp16.comp.spv $(SHADER_DIR)/irradiance.fp16.comp.spv $(SHADER_DIR)/prefilter.fp16.comp.spv
SHADER_SPVS = $(SHADER_VERTS:.vert=.vert.spv) $(SHADER_FRAGS:.frag=.frag.spv) $(SHADER_COMPS:.comp=.comp.spv) $(SHADER_TASKS:.task=.task.spv) $(SHADER_MESHS:.mesh=.mesh.spv) $(IBL_FP16_SPVS)
SPV_HEADERS = $(SHADER_SPVS:.spv=.spv.h)

# Fonts
FONT_DIR = assets/fonts
FONT_TTFS = $(wildcard $(FONT_DIR)/*.ttf)
FONT_HEADERS = $(FONT_TTFS:.ttf=.h)

# Default target - build executable directly
all: internal_build

internal_build: $(SPV_HEADERS) $(FONT_HEADERS) $(JOLTC_STATIC) $(EXECUTABLE)

# A grouped target is essential: under make -j, two independent recipes
# would otherwise configure/build Jolt and JoltC concurrently in ONE directory.
# This is supported by GNU make 4.3+. Keep configuration out of the hot path
# when the two resulting libraries are already up-to-date.
$(JOLTC_STATIC) $(JOLT_STATIC) &: Makefile vendor/joltc/CMakeLists.txt $(JOLT_CONFIG_FORCE)
	cmake -S vendor/joltc -B vendor/joltc/build \
		-DCMAKE_BUILD_TYPE=$(JOLT_BUILD_TYPE) \
		-DINTERPROCEDURAL_OPTIMIZATION=$(JOLT_LTO) \
		-DJPH_BUILD_SHARED=OFF -DJPH_SAMPLES=OFF -DJPH_TESTS=OFF \
		-DDEBUG_RENDERER_IN_DISTRIBUTION=OFF \
		-DPROFILER_IN_DISTRIBUTION=OFF \
		$(if $(JOLT_PHYSICS_ROOT),-DJOLT_PHYSICS_ROOT=$(abspath $(JOLT_PHYSICS_ROOT))) \
		$(JOLT_CPU_TUNING) $(JOLT_EXTRA_CMAKE)
	cmake --build vendor/joltc/build --config $(JOLT_BUILD_TYPE) --parallel $(JOLT_JOBS)
	@test -s $(JOLTC_STATIC) && test -s $(JOLT_STATIC) || { \
	  echo 'ERROR: missing Jolt/JoltC static libraries after CMake build' >&2; exit 1; }

# Compile shaders to SPIR-V
$(SHADER_DIR)/%.vert.spv: $(SHADER_DIR)/%.vert
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.frag.spv: $(SHADER_DIR)/%.frag
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.comp.spv: $(SHADER_DIR)/%.comp
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.fp16.comp.spv: $(SHADER_DIR)/%.comp
	$(GLSLANG) -V --target-env vulkan1.3 --define-macro OBS_IBL_STORAGE_16F=1 $< -o $@
$(SHADER_DIR)/%.task.spv: $(SHADER_DIR)/%.task
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.mesh.spv: $(SHADER_DIR)/%.mesh
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
# Convert SPIR-V to C header
$(FONT_DIR)/%.h: $(FONT_DIR)/%.ttf
	$(XXD) -i -n $(subst /,_,$(subst .,_,$(subst -,_,$(notdir $<)))) $< > $@

$(SHADER_DIR)/%.spv.h: $(SHADER_DIR)/%.spv
	$(XXD) -i -n $(subst /,_,$(subst .,_,$(notdir $<))) $< > $@

# Compile C sources to object files

$(LIB_OBJECTS) $(MAIN_OBJECT): %.o: %.c $(SPV_HEADERS) $(FONT_HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(DEPFILES)

# Build executable by linking all objects directly
$(EXECUTABLE): Makefile $(ALL_OBJECTS) $(JOLTC_STATIC)
	$(CC) $(ALL_OBJECTS) -o $@ $(LDFLAGS)

# Static library (optional, for distribution)
$(LIB_NAME).a: $(LIB_OBJECTS)
	ar rcs $@ $(LIB_OBJECTS)

# Shared library (optional, for distribution)
$(LIB_NAME).so: $(LIB_OBJECTS) $(JOLTC_STATIC) $(JOLT_STATIC)
	$(CC) -shared -o $@ $(LIB_OBJECTS) $(LDFLAGS)

# Build both libraries
libs: $(FONT_HEADERS) $(LIB_NAME).a $(LIB_NAME).so

test-keychords-monad:
	$(CC) $(CFLAGS) $(INCLUDES) tests/keychords-monad.c src/platform/keychords.c \
		-L$(abspath $(MONADC_LIB)) -Wl,-rpath,$(abspath $(MONADC_LIB)) \
		-lmonad-embed -lglfw -o /tmp/obsidian-keychords-monad
	/tmp/obsidian-keychords-monad

test-keychords-performance:
	$(CC) $(CFLAGS) $(INCLUDES) tests/keychords-performance.c src/platform/keychords.c \
		-L$(abspath $(MONADC_LIB)) -Wl,-rpath,$(abspath $(MONADC_LIB)) \
		-lmonad-embed -lglfw -o /tmp/obsidian-keychords-performance
	/tmp/obsidian-keychords-performance

test-renderer2d-capacity:
	$(CC) $(TEST_NATIVE_CFLAGS) $(INCLUDES) src/graphics/renderer2d_capacity.c \
		tests/renderer2d-capacity.c -o /tmp/obsidian-renderer2d-capacity
	/tmp/obsidian-renderer2d-capacity


# Compile an external consumer with ONLY the versioned public header. The
# linked/live variant is opt-in because it requires actual Vulkan and MonadC.
.PHONY: test-agent-api-compile test-agent-runtime
test-agent-api-compile:
	$(CC) $(TEST_NATIVE_CFLAGS) -I. -fsyntax-only tests/integration/agent_client.c
	$(CC) $(TEST_NATIVE_CFLAGS) -I. -fsyntax-only tests/integration/agent_sculpture.c
	$(CC) $(TEST_NATIVE_CFLAGS) -I. -Isrc/editor -Isrc/scene -fsyntax-only tests/integration/sdf_viewport_runtime.c

$(TEST_BUILD)/agent-client: tests/integration/agent_client.c obsidian_agent.h $(LIB_NAME).so
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -Wall -Wextra -Werror -I. tests/integration/agent_client.c \
		-L. -Wl,-rpath,$(abspath .) -lobsidian -o $@

test-agent-runtime: $(TEST_BUILD)/agent-client
	$(TEST_BUILD)/agent-client tests/monad/native-camera-policy.mon \
		/tmp/obsidian-agent-integration.rgba /tmp/obsidian-agent-integration.json

# QTT-aware publication gate is self-contained, no MonadC SDK required.
# Test native host leases and compiler-authored contract validation directly.
# Headless, deterministic native-boundary checks. The test *behavior* of the
# integrated engine is specified in tests/monad/ and needs a real MonadC host.
# Unlike the Vulkan tests, these compile without third-party dev packages.
TEST_BUILD = build/tests
TEST_NATIVE_CFLAGS ?= -std=c23 -O2 -g -Wall -Wextra -Werror -pedantic

# Cycle 48: compile the real public outer-embedding façade against a deterministic
# backend seam. Production calls still bind directly to Vulkan/MonadC owners.
$(TEST_BUILD)/agent-engine: tests/native/agent_engine.c tests/fixtures/agent_engine_contract.h src/app/agent_engine.c obsidian_agent.h src/graphics/sdf.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/scene/sdf_inspector.c src/app/headless_job.c src/core/atomic_file.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -pthread -DOBS_AGENT_ENGINE_TEST -DOBS_SDF_TEST -I. -Isrc/app -Isrc/core -Isrc/scene -Itests/fixtures \
		tests/native/agent_engine.c src/app/agent_engine.c src/graphics/sdf.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/scene/sdf_inspector.c src/app/headless_job.c src/core/atomic_file.c -lm -o $@


# Cycle 50: actual SDF append, ticket lifetime and per-slot CPU->GPU publication.
$(TEST_BUILD)/sdf-sculpt: tests/native/sdf_sculpt.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/scene/sdf_inspector.c src/graphics/sdf.c src/scene/sdf_sculpt.h src/scene/sdf_tokens.h src/scene/sdf_inspector.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_sculpt.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/scene/sdf_inspector.c src/graphics/sdf.c -lm -o $@

# Cycle 53: executable Emacs-style undo trace against the real SDF journal.
$(TEST_BUILD)/emacs-scene-undo: tests/native/emacs_scene_undo.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c src/scene/sdf_inspector.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/emacs_scene_undo.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@

# Optional authoritative Emacs oracle, intentionally strict if Emacs is absent.
.PHONY: test-emacs-reference
test-emacs-reference:
	emacs -Q --batch -l tests/emacs/undo-reference.el -f ert-run-tests-batch-and-exit

.PHONY: test-emacs-scene-undo
test-emacs-scene-undo: $(TEST_BUILD)/emacs-scene-undo
	@$(TEST_BUILD)/emacs-scene-undo

.PHONY: test-sculpture-runtime
test-sculpture-runtime: $(LIB_NAME).so
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -Wall -Wextra -Werror -I. tests/integration/agent_sculpture.c \
		-L. -Wl,-rpath,$(abspath .) -lobsidian -lm -o $(TEST_BUILD)/agent-sculpture
	$(TEST_BUILD)/agent-sculpture monad/obsidian/sculpture.mon \
		$(TEST_BUILD)/sculpture.rgba $(TEST_BUILD)/sculpture.json
	python3 tools/rgba_to_png.py $(TEST_BUILD)/sculpture.rgba \
		$(TEST_BUILD)/sculpture.png 640 360

# Cycle 50: compile and invoke the REAL sculpt Monad host callbacks against
# a tiny embed declaration seam and actual production scene implementation.
$(TEST_BUILD)/sculpt-api: tests/native/sculpt_api.c tests/fixtures/sculpt_api_contract.h src/monad/sculpt_api.c src/editor/sdf_appearance_clipboard.c src/editor/sdf_appearance_clipboard.h src/core/atomic_file.c src/core/atomic_file.h src/editor/sdf_property_bridge.c src/scene/sdf_sculpt.c src/scene/sdf_sculpt.h src/scene/sdf_tokens.c src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -DOBS_MONAD_SCULPT_TEST -Isrc/scene -Isrc/editor -Isrc/core -Itests/fixtures \
		tests/native/sculpt_api.c src/monad/sculpt_api.c src/editor/sdf_appearance_clipboard.c \
		src/core/atomic_file.c src/editor/sdf_property_bridge.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c \
		src/scene/sdf_inspector.c src/graphics/sdf.c -lm -o $@

# Monad-generated PCM playback and typed native synthesis against an embed seam
# and a fake audio device (no ALSA/miniaudio needed). The host only validates
# and adapts samples; device traffic is recorded, not mixed.
$(TEST_BUILD)/audio-synth: tests/native/audio_synth.c tests/fixtures/audio_api_contract.h src/monad/audio_api.c src/monad/audio_api.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_MONAD_AUDIO_TEST -D_POSIX_C_SOURCE=200809L -Isrc/monad -Isrc/audio -Itests/fixtures \
		tests/native/audio_synth.c src/monad/audio_api.c -lm -o $@

# Cycle 47: exercise the production bounded agent driver, JSON receipt, and
# atomic byte publication using actual C sources (no Vulkan dependency).
$(TEST_BUILD)/headless-job: tests/native/headless_job.c src/app/headless_job.c src/app/headless_job.h src/core/atomic_file.c src/core/atomic_file.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/app -Isrc/core \
		tests/native/headless_job.c src/app/headless_job.c src/core/atomic_file.c -o $@

.PHONY: test test-native test-monad test-frame test-all bench-font test-tooling test-editor-keymaps test-headless-editor-keymaps

$(TEST_BUILD)/repl-model: tests/native/repl_model.c src/monad/repl_model.c src/monad/repl_model.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/repl_model.c src/monad/repl_model.c -o $@

$(TEST_BUILD)/publication-queue-repl: tests/native/publication_queue_repl.c src/monad/publication_queue.c src/monad/publication_queue.h src/monad/repl_model.c src/monad/repl_model.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/publication_queue_repl.c src/monad/publication_queue.c src/monad/repl_model.c -o $@

# Cycle 40: compile the actual async production controller body with a fake
# device/scene seam. The controller itself is not reimplemented in the test.
# The full Vulkan integration still requires a real engine build and GPU.
$(TEST_BUILD)/async-gltf-controller: tests/native/async_gltf_controller.c tests/fixtures/async_gltf_contract.h src/assets/gltf_async.c src/scene/pending_load.c src/scene/cache_textures.c
	@mkdir -p $(TEST_BUILD)
	@{ echo '#define _GNU_SOURCE'; echo '#define _POSIX_C_SOURCE 200809L'; echo '#include "async_gltf_contract.h"'; sed '/^#include /d; /^#define _GNU_SOURCE/d; /^#define _POSIX_C_SOURCE/d' src/assets/gltf_async.c; } > $(TEST_BUILD)/async-gltf-controller-body.c
	$(CC) $(TEST_NATIVE_CFLAGS) -pthread -Itests/fixtures -Isrc/scene -Isrc/graphics -Isrc/assets \
		$(TEST_BUILD)/async-gltf-controller-body.c tests/native/async_gltf_controller.c \
		src/scene/pending_load.c src/scene/cache_textures.c \
        -Wl,--wrap=pthread_create -o $@

# Cycle 41: real cgltf source parsing/validation without renderer dependencies.
# Cycle 43: production render graph with a deterministic Vulkan command seam.
# The real graph is compiled, only the command recorder is replaced.
$(TEST_BUILD)/render-graph-contract: tests/native/render_graph_contract.c tests/fixtures/render_graph_contract.h src/graphics/render_graph.c src/graphics/render_graph.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_RG_CONTRACT_TEST -Itests/fixtures -Isrc/graphics \
		tests/native/render_graph_contract.c src/graphics/render_graph.c -o $@

$(TEST_BUILD)/gltf-source-plan: tests/native/gltf_source_plan.c src/assets/gltf_source_plan.c src/assets/gltf_source_plan.h tests/fixtures/async-triangle.gltf tests/fixtures/async-bad-index.gltf tests/fixtures/async-lines.gltf tests/fixtures/async-material.gltf tests/fixtures/async-static-unsupported.gltf
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -pthread -D_POSIX_C_SOURCE=200809L -Ivendor -Isrc/assets \
		tests/native/gltf_source_plan.c src/assets/gltf_source_plan.c -o $@

# Skin-joint aggregation is the exact production helper used by the Monad API.
$(TEST_BUILD)/gltf-joint-count: tests/native/gltf_joint_count.c src/monad/gltf_joint_count.c src/monad/asset_api.h src/scene/scene.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) $(INCLUDES) -I$(MONADC_INCLUDE) \
		tests/native/gltf_joint_count.c src/monad/gltf_joint_count.c -o $@

# Cycle 44: real cgltf cold static CPU cook with a fake engine ABI seam.
$(TEST_BUILD)/gltf-cook-static: tests/native/gltf_cook_static.c tests/fixtures/static_cook_contract.h tests/fixtures/ao-strength.gltf tests/fixtures/async-static-meshlets.gltf tests/fixtures/async-static-unsupported.gltf tests/fixtures/async-static-hierarchy.gltf tests/fixtures/async-static-orphan.gltf tests/fixtures/async-static-cycle.gltf tests/fixtures/async-static-textured.gltf tests/fixtures/async-static-instancing.gltf tests/fixtures/async-static-variant.gltf tests/fixtures/async-static-required-extension.gltf tests/fixtures/async-static-meshopt.gltf assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf assets/gltf/ABeautifulGame/glTF/ABeautifulGame.bin src/assets/gltf_cook_static.c src/assets/gltf_material.c src/assets/gltf_material.h src/scene/cache_textures.c src/scene/node_topology.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_GLTF_STATIC_COOK_TEST -D_POSIX_C_SOURCE=200809L \
		-Ivendor -Isrc/assets -Isrc/graphics -Isrc/scene -Itests/fixtures \
		tests/native/gltf_cook_static.c src/assets/gltf_cook_static.c src/assets/gltf_material.c src/scene/cache_textures.c src/scene/node_topology.c \
		-lm -pthread -o $@

# The production audio mixer and buffer API with a fake miniaudio device.
# Exercises callback safety without requiring ALSA/PulseAudio or a GPU.
$(TEST_BUILD)/audio-lifetime: tests/native/audio_lifetime.c tests/fixtures/audio_contract.h src/audio/audio.c src/audio/audio.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_AUDIO_CONTRACT_TEST -Itests/fixtures -Isrc/audio \
		tests/native/audio_lifetime.c src/audio/audio.c -pthread -o $@

# Pure host publication journal: fake callbacks, no SDK/Vulkan dependency.
$(TEST_BUILD)/live-binding-txn: tests/native/live_binding_txn.c src/monad/live_binding_txn.c src/monad/live_binding_txn.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/live_binding_txn.c src/monad/live_binding_txn.c -o $@

# Host-side effect/lifetime admission, not a substitute for QTT proofs.
$(TEST_BUILD)/effect-policy: tests/native/effect_policy.c src/monad/effect_policy.c src/monad/publication.c src/monad/effect_policy.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/effect_policy.c src/monad/effect_policy.c src/monad/publication.c -o $@

# Worker performs filesystem I/O only. No Vulkan or MonadC library required.
$(TEST_BUILD)/source-stage: tests/native/source_stage.c src/monad/source_stage.c src/monad/source_stage.h src/monad/publication_queue.c src/monad/publication_queue.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/source_stage.c src/monad/source_stage.c src/monad/publication_queue.c -pthread -o $@

# Worker lifecycle tests do not depend on a Monad runtime or SDK.
$(TEST_BUILD)/source-preflight: tests/native/source_preflight.c src/monad/source_stage.c src/monad/source_stage.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/source_preflight.c src/monad/source_stage.c -pthread -o $@

# Test doubles follow the actual MonadC public compiler.h signature.
# These tests do not imply real inference, codegen or JIT execution.
$(TEST_BUILD)/semantic-preflight: tests/native/semantic_preflight.c src/monad/semantic_preflight.c src/monad/semantic_preflight.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad -I$(MONADC_INCLUDE) \
		tests/native/semantic_preflight.c src/monad/semantic_preflight.c -pthread -o $@

.PHONY: test-semantic-preflight
test-semantic-preflight: $(TEST_BUILD)/semantic-preflight
	@$(TEST_BUILD)/semantic-preflight

$(TEST_BUILD)/publication-queue: tests/native/publication_queue.c src/monad/publication_queue.c src/monad/publication_queue.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/publication_queue.c src/monad/publication_queue.c -o $@

$(TEST_BUILD)/publication-metrics: tests/native/publication_metrics.c src/monad/publication_metrics.c src/monad/publication_metrics.h src/monad/publication_queue.c src/monad/publication_queue.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/publication_metrics.c src/monad/publication_metrics.c src/monad/publication_queue.c -o $@

$(TEST_BUILD)/publication: tests/native/publication.c src/monad/publication.c src/monad/publication.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/publication.c src/monad/publication.c -o $@

$(TEST_BUILD)/frame-state: tests/native/frame_state.c src/core/frame_state.c src/core/frame_state.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/core tests/native/frame_state.c src/core/frame_state.c -lm -o $@

$(TEST_BUILD)/font-utf8: tests/native/font_utf8.c src/graphics/font_utf8.c src/graphics/font_utf8.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/font_utf8.c src/graphics/font_utf8.c -o $@

$(TEST_BUILD)/hotreload-stats: tests/native/hotreload_stats.c src/monad/hotreload_stats.c src/monad/hotreload_stats.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/hotreload_stats.c src/monad/hotreload_stats.c -o $@

$(TEST_BUILD)/hotreload-watcher: tests/native/hotreload_watcher.c src/monad/hotreload.c src/monad/hotreload.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/hotreload_watcher.c src/monad/hotreload.c -o $@

# Host-level O(1) typed foreign-slot validation; requires no SDK or Vulkan.
# Generation checked IDs shared by dense mesh and node foreign capabilities.
# Pure C, no Vulkan or Monad SDK required; stress cases live in tests/native/.
$(TEST_BUILD)/material-assets: tests/native/material_assets.c src/scene/material_assets.c src/scene/object_identity.c src/scene/mesh_properties.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/material_assets.c src/scene/material_assets.c src/scene/object_identity.c -lm -o $@

# OMDL cache layout/short-read/malformed-size tests, without Vulkan or SDK.
$(TEST_BUILD)/omdl-digest-bench: tools/bench/omdl_digest_bench.c src/assets/omdl_cache.c src/assets/omdl_cache.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -Wall -Wextra -Werror -Isrc/assets tools/bench/omdl_digest_bench.c src/assets/omdl_cache.c -o $@

.PHONY: bench-omdl-digest
bench-omdl-digest: $(TEST_BUILD)/omdl-digest-bench
	@$(TEST_BUILD)/omdl-digest-bench

# CPU-side in-flight owner and generation/ticket contracts, no Vulkan needed.
$(TEST_BUILD)/geometry-upload-flight: tests/native/geometry_upload_flight.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_flight.h src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_upload_flight.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

$(TEST_BUILD)/geometry-upload-memory: tests/native/geometry_upload_memory.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c src/graphics/geometry_upload_batch.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_upload_memory.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

$(TEST_BUILD)/geometry-upload-batch: tests/native/geometry_upload_batch.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c src/graphics/geometry_upload_batch.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_upload_batch.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

$(TEST_BUILD)/geometry-budget: tests/native/geometry_budget.c src/graphics/geometry_budget.c src/graphics/geometry_budget.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_budget.c src/graphics/geometry_budget.c -o $@

$(TEST_BUILD)/omdl-cache: tests/native/omdl_cache.c src/assets/omdl_cache.c src/assets/omdl_cache.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/assets tests/native/omdl_cache.c src/assets/omdl_cache.c -o $@

$(TEST_BUILD)/node-topology: tests/native/node_topology.c src/scene/node_topology.c src/scene/node_topology.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/node_topology.c src/scene/node_topology.c -o $@

$(TEST_BUILD)/node-topology-bench: tools/bench/node_topology_bench.c src/scene/node_topology.c src/scene/node_topology.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -Wall -Wextra -Werror -Isrc/scene tools/bench/node_topology_bench.c src/scene/node_topology.c -o $@

.PHONY: bench-node-topology
bench-node-topology: $(TEST_BUILD)/node-topology-bench
	@$(TEST_BUILD)/node-topology-bench $(or $(NODES),4096) $(or $(FRAMES),3000)

$(TEST_BUILD)/skin-override-index: tests/native/skin_override_index.c src/scene/skin_override_index.c src/scene/skin_override_index.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/skin_override_index.c src/scene/skin_override_index.c -o $@

.PHONY: test-skin-override-index
test-skin-override-index: $(TEST_BUILD)/skin-override-index
	@$(TEST_BUILD)/skin-override-index

test-native: test-skin-override-index

$(TEST_BUILD)/gltf-skin-override-runtime: \
	tests/native/gltf_skin_override_runtime.c src/assets/gltf_loader.c \
	src/assets/gltf_loader.h src/scene/scene.h \
	src/scene/skin_override_index.c src/scene/skin_override_index.h \
	src/scene/animation_channel_index.c src/scene/animation_channel_index.h \
	src/graphics/mesh_bone_overrides.c src/scene/node_overrides.c \
	src/scene/node_overrides.h \
	src/core/context.h src/graphics/vulkan_setup.h \
	src/editor/animation_editor.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections \
		tests/native/gltf_skin_override_runtime.c src/assets/gltf_loader.c \
		src/scene/skin_override_index.c src/scene/animation_channel_index.c \
		src/graphics/mesh_bone_overrides.c \
		src/scene/node_overrides.c -Wl,--gc-sections -lm -o $@

.PHONY: test-gltf-skin-override-runtime
test-gltf-skin-override-runtime: $(TEST_BUILD)/gltf-skin-override-runtime
	@$(TEST_BUILD)/gltf-skin-override-runtime

test-native: test-gltf-skin-override-runtime

$(TEST_BUILD)/skin-override-index-bench: tools/bench/skin_override_index_bench.c src/scene/skin_override_index.c src/scene/skin_override_index.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -Isrc/scene tools/bench/skin_override_index_bench.c src/scene/skin_override_index.c -o $@

.PHONY: bench-skin-overrides
bench-skin-overrides: $(TEST_BUILD)/skin-override-index-bench
	@$(TEST_BUILD)/skin-override-index-bench $(or $(BENCH_FRAMES),4)

# Exercise production animate_scene() with a synthetic channel/node matrix.
# Timings include channel scanning, sampling and hierarchy composition, but no
# renderer, GPU upload or Monad dispatch.
$(TEST_BUILD)/gltf-animation-channels-bench: \
	tools/bench/gltf_animation_channels_bench.c src/assets/gltf_loader.c \
	src/assets/gltf_loader.h src/scene/scene.h \
	src/scene/skin_override_index.c src/scene/skin_override_index.h \
	src/scene/animation_channel_index.c src/scene/animation_channel_index.h \
	src/graphics/mesh_bone_overrides.c src/graphics/renderer.h \
	src/scene/node_overrides.c src/scene/node_overrides.h \
	src/core/context.h src/graphics/vulkan_setup.h \
	src/editor/animation_editor.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections \
		tools/bench/gltf_animation_channels_bench.c src/assets/gltf_loader.c \
		src/scene/skin_override_index.c src/scene/animation_channel_index.c \
		src/graphics/mesh_bone_overrides.c \
		src/scene/node_overrides.c -Wl,--gc-sections -lm -o $@

.PHONY: bench-animation-channels
bench-animation-channels: $(TEST_BUILD)/gltf-animation-channels-bench
	@$(TEST_BUILD)/gltf-animation-channels-bench $(or $(BENCH_ITERATIONS),16)

$(TEST_BUILD)/transparent-sort-bench: tools/bench/transparent_sort_bench.c src/graphics/renderer.c src/graphics/renderer.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -D_POSIX_C_SOURCE=200809L \
		-ffunction-sections -fdata-sections tools/bench/transparent_sort_bench.c \
		src/graphics/renderer.c -Wl,--gc-sections -lvulkan -lglfw -lcglm -lm \
		-o $@

.PHONY: test-transparent-sort
$(TEST_BUILD)/transparent-sort: tests/native/transparent_sort.c src/graphics/renderer.c src/graphics/renderer.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -D_POSIX_C_SOURCE=200809L \
		-ffunction-sections -fdata-sections tests/native/transparent_sort.c \
		src/graphics/renderer.c -Wl,--gc-sections -Wl,--wrap=realloc \
		-lvulkan -lglfw -lcglm -lm -o $@

test-transparent-sort: $(TEST_BUILD)/transparent-sort
	@$(TEST_BUILD)/transparent-sort

test-native: test-transparent-sort

.PHONY: bench-transparent-sort
bench-transparent-sort: $(TEST_BUILD)/transparent-sort-bench
	@$(TEST_BUILD)/transparent-sort-bench $(or $(BENCH_ITERATIONS),16)

$(TEST_BUILD)/gltf-topology-fixture: tests/native/gltf_topology_fixture.c src/scene/node_topology.c src/scene/node_topology.h src/scene/node_overrides.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Ivendor -Isrc/scene tests/native/gltf_topology_fixture.c src/scene/node_topology.c src/scene/node_overrides.c -lm -o $@

$(TEST_BUILD)/node-overrides: tests/native/node_overrides.c src/scene/node_overrides.c src/scene/node_overrides.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/node_overrides.c src/scene/node_overrides.c -lm -o $@

$(TEST_BUILD)/mesh-properties: tests/native/mesh_properties.c src/scene/mesh_properties.c src/scene/mesh_properties.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/mesh_properties.c src/scene/mesh_properties.c -lm -o $@

$(TEST_BUILD)/object-identity: tests/native/object_identity.c src/scene/object_identity.c src/scene/object_identity.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/object_identity.c src/scene/object_identity.c -o $@

$(TEST_BUILD)/slot-index: tests/native/slot_index.c src/monad/slot_index.c src/monad/slot_index.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/slot_index.c src/monad/slot_index.c -o $@

$(TEST_BUILD)/identity-bench: tools/bench/identity_bench.c src/scene/object_identity.c src/scene/object_identity.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -Isrc/scene \
		tools/bench/identity_bench.c src/scene/object_identity.c -o $@

.PHONY: bench-object-identity
bench-object-identity: $(TEST_BUILD)/identity-bench
	@$(TEST_BUILD)/identity-bench $(or $(BENCH_SLOTS),8192) $(or $(BENCH_ITERATIONS),1000000)

$(TEST_BUILD)/foreign-slots-bench: tools/bench/foreign_slots_bench.c src/monad/slot_index.c src/monad/slot_index.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -Isrc/monad tools/bench/foreign_slots_bench.c src/monad/slot_index.c -o $@

.PHONY: bench-foreign-slots
bench-foreign-slots: $(TEST_BUILD)/foreign-slots-bench
	@$(TEST_BUILD)/foreign-slots-bench $(or $(BENCH_ITERATIONS),100000)

# Constant-space hot foreign-dispatch profiler; no SDK/Vulkan requirement.
$(TEST_BUILD)/dispatch-metrics: tests/native/dispatch_metrics.c src/monad/dispatch_metrics.c src/monad/dispatch_metrics.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad tests/native/dispatch_metrics.c src/monad/dispatch_metrics.c -o $@

$(TEST_BUILD)/dispatch-metrics-bench: tools/bench/dispatch_metrics_bench.c src/monad/dispatch_metrics.c src/monad/dispatch_metrics.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -Isrc/monad tools/bench/dispatch_metrics_bench.c src/monad/dispatch_metrics.c -o $@

.PHONY: bench-dispatch
bench-dispatch: $(TEST_BUILD)/dispatch-metrics-bench
	@$(TEST_BUILD)/dispatch-metrics-bench $(or $(BENCH_ITERATIONS),2000000)

# Compiler-owned frozen native generation: test uses the actual MonadC ABI
# header, with a deterministic test double for runtime behavior. This is an
# SDK signature/host-policy integration test, NOT a live JIT execution test.
$(TEST_BUILD)/native-read: tests/native/native_read.c src/monad/native_read.c src/monad/native_read.h src/monad/publication.c src/monad/publication.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad -I$(MONADC_INCLUDE) \
		tests/native/native_read.c src/monad/native_read.c src/monad/publication.c -o $@

# No runtime dependency: post-publication ABI diagnostic comparisons.
$(TEST_BUILD)/catalog-transition: tests/native/catalog_transition.c src/monad/catalog_transition.c src/monad/catalog_transition.h src/monad/export_catalog.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad -I$(MONADC_INCLUDE) \
		tests/native/catalog_transition.c src/monad/catalog_transition.c -o $@

# SDK-authoritative certified exports/QTT view; test doubles implement only
# published public declarations, and never pretend to run MonadC/LLVM.
$(TEST_BUILD)/export-catalog: tests/native/export_catalog.c src/monad/export_catalog.c src/monad/export_catalog.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/monad -I$(MONADC_INCLUDE) \
		tests/native/export_catalog.c src/monad/export_catalog.c -o $@

.PHONY: test-catalog-transition
test-catalog-transition:
	@test -f $(MONADC_INCLUDE)/monad/monad.h || { echo 'Missing Monad SDK header: $(MONADC_INCLUDE)/monad/monad.h'; exit 1; }
	@$(MAKE) --no-print-directory $(TEST_BUILD)/catalog-transition
	@$(TEST_BUILD)/catalog-transition

.PHONY: test-export-catalog
test-export-catalog:
	@test -f $(MONADC_INCLUDE)/monad/monad.h || { echo 'Missing Monad SDK header: $(MONADC_INCLUDE)/monad/monad.h'; exit 1; }
	@$(MAKE) --no-print-directory $(TEST_BUILD)/export-catalog
	@$(TEST_BUILD)/export-catalog

.PHONY: test-native-read
# Available only with an installed (or explicitly specified) real SDK header.
test-native-read:
	@test -f $(MONADC_INCLUDE)/monad/monad.h || { echo 'Missing Monad SDK header: $(MONADC_INCLUDE)/monad/monad.h'; exit 1; }
	@$(MAKE) --no-print-directory $(TEST_BUILD)/native-read
	@$(TEST_BUILD)/native-read

# Includes the existing renderer capacity regression, which also has no GPU dependency.
# Real keymap source compiled with isolated SDK/Vulkan/GLFW test declarations.
# These are interface-shaped stubs for host ownership tests, NOT a renderer.
$(TEST_BUILD)/keymap-image: tests/native/keymap_image.c src/platform/keychords.c src/platform/keychords.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -D_POSIX_C_SOURCE=200809L -Itests/fixtures -Isrc/platform -Isrc/core \
		tests/native/keymap_image.c src/platform/keychords.c -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup -o $@

$(TEST_BUILD)/keymap-image-profile: tests/native/keymap_image.c src/platform/keychords.c src/platform/keychords.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -D_POSIX_C_SOURCE=200809L -DOBS_KEYCHORD_PROFILE -Itests/fixtures -Isrc/platform -Isrc/core \
		tests/native/keymap_image.c src/platform/keychords.c -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup -o $@

.PHONY: bench-keymap
bench-keymap: $(TEST_BUILD)/keymap-image $(TEST_BUILD)/keymap-image-profile
	@OBSIDIAN_BENCH_KEYMAP=1 $(TEST_BUILD)/keymap-image
	@OBSIDIAN_BENCH_KEYMAP=1 $(TEST_BUILD)/keymap-image-profile

# Non-destructive AGENTS.md sync and deterministic patch tooling checks.
test-tooling:
	python3 tests/tooling/test_incremental_workflow.py
	python3 tests/tooling/test_engine_bench_schema.py
	python3 tests/tooling/test_bench_cli.py
	python3 tests/tooling/test_make_cli.py

# Pure owner-thread disjoint GPU reservations + pending-scene publication.
$(TEST_BUILD)/geometry-pending: tests/native/geometry_pending.c src/graphics/geometry_pending.c src/graphics/geometry_pending.h src/graphics/geometry_budget.c src/graphics/geometry_budget.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_pending.c src/graphics/geometry_pending.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

# Integration of the disjoint reservation journal with the real owned CPU
# upload-flight lifecycle. GPU fence polling remains a separate backend gate.
$(TEST_BUILD)/geometry-scene-upload: tests/native/geometry_scene_upload.c src/graphics/geometry_scene_upload.c src/graphics/geometry_scene_upload.h src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_scene_upload.c src/graphics/geometry_scene_upload.c src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

# Adapter logic is compiled from the production source with only the
# Vulkan-heavy include replaced by equivalent interface-shaped declarations.
# This is NOT a Vulkan SDK/driver test.
$(TEST_BUILD)/scene-load-vk-contract: tests/native/scene_load_vk_contract.c tests/fixtures/scene_load_vk_contract.h src/graphics/scene_load_vk.c src/graphics/scene_load_vk.h
	@mkdir -p $(TEST_BUILD)
	{ printf '#define OBS_SCENE_LOAD_VK_CONTRACT_TEST\n'; cat src/graphics/scene_load_vk.c; } > $(TEST_BUILD)/scene-load-vk-adapter-body.c
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene -Isrc/graphics -Itests/fixtures $(TEST_BUILD)/scene-load-vk-adapter-body.c tests/native/scene_load_vk_contract.c -o $@

# Pending asset-bundle ownership + prepared GPU scene activation.
# Uses the real geometry flight/journal through a simulated Vulkan backend.
$(TEST_BUILD)/pending-scene-load: tests/native/pending_load.c src/scene/pending_load.c src/scene/pending_load.h src/graphics/geometry_async.c src/graphics/geometry_scene_upload.c src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene -Isrc/graphics tests/native/pending_load.c src/scene/pending_load.c src/graphics/geometry_async.c src/graphics/geometry_scene_upload.c src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

# One-flight GPU transfer adapter with fake nonblocking fence backend. These
# tests exercise owned bytes and publication; not Vulkan driver execution.
$(TEST_BUILD)/geometry-async: tests/native/geometry_async.c src/graphics/geometry_async.c src/graphics/geometry_async.h src/graphics/geometry_scene_upload.c src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/geometry_async.c src/graphics/geometry_async.c src/graphics/geometry_scene_upload.c src/graphics/geometry_pending.c src/graphics/geometry_upload_flight.c src/graphics/geometry_upload_batch.c src/graphics/geometry_budget.c -o $@

# Private OMDL texture descriptor plan; no Vulkan SDK or texture-pool required.
$(TEST_BUILD)/cache-textures: tests/native/cache_textures.c src/scene/cache_textures.c src/scene/cache_textures.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/cache_textures.c src/scene/cache_textures.c -o $@

# Releasable texture references + pending SceneLoad ownership. Real C source,
# mock renderer; requires no Vulkan SDK. No GPU residency claim.
$(TEST_BUILD)/cache-texture-lease: tests/native/cache_texture_lease.c src/scene/cache_textures.c src/scene/pending_load.c src/scene/cache_textures.h src/scene/pending_load.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene -Isrc/graphics tests/native/cache_texture_lease.c src/scene/cache_textures.c src/scene/pending_load.c -o $@

# Renderer-owned retained texture references, including Cycle 38 lease callbacks.
$(TEST_BUILD)/texture-refs: tests/native/texture_refs.c src/graphics/texture_refs.c src/scene/cache_textures.c src/graphics/texture_refs.h src/scene/cache_textures.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics -Isrc/scene tests/native/texture_refs.c src/graphics/texture_refs.c src/scene/cache_textures.c -o $@

# Allocation-failure-atomic scene storage and allocation-free mesh publication.
$(TEST_BUILD)/mesh-storage: tests/native/mesh_storage.c src/scene/mesh_storage.c src/scene/mesh_storage.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene tests/native/mesh_storage.c src/scene/mesh_storage.c -o $@

$(TEST_BUILD)/gpu-profile: tests/native/gpu_profile.c src/core/gpu_profile.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/core tests/native/gpu_profile.c -lm -o $@

$(TEST_BUILD)/engine-bench: tests/native/engine_bench.c src/core/engine_bench.c src/core/engine_bench.h src/core/gpu_profile.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/core tests/native/engine_bench.c src/core/engine_bench.c -o $@

.PHONY: test-native-profile test-antialiasing-policy
test-native-profile: $(TEST_BUILD)/gpu-profile $(TEST_BUILD)/engine-bench
	@$(TEST_BUILD)/gpu-profile
	@$(TEST_BUILD)/engine-bench

$(TEST_BUILD)/antialiasing-policy: tests/native/antialiasing_policy.c src/graphics/antialiasing_policy.c src/graphics/antialiasing_policy.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/graphics tests/native/antialiasing_policy.c src/graphics/antialiasing_policy.c -o $@

test-antialiasing-policy: $(TEST_BUILD)/antialiasing-policy
	@$(TEST_BUILD)/antialiasing-policy

$(TEST_BUILD)/shadow-cache-key: tests/native/shadow_cache_key.c src/graphics/shadow_cache.c src/graphics/shadow_cache.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) $(INCLUDES) -Isrc/graphics -Isrc/scene \
		tests/native/shadow_cache_key.c src/graphics/shadow_cache.c -lm -o $@

test-native: test-native-profile test-agent-api-compile test-antialiasing-policy

test-native: $(TEST_BUILD)/shadow-cache-key

$(TEST_BUILD)/selection-visibility: tests/native/selection_visibility.c src/scene/selection.c src/scene/scene.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) $(INCLUDES) tests/native/selection_visibility.c src/scene/selection.c -o $@

test-native: $(TEST_BUILD)/selection-visibility

test-native: $(TEST_BUILD)/sdf-viewport

test-native: $(TEST_BUILD)/sdf-typed-viewport

test-native: $(TEST_BUILD)/sdf-property-bridge

test-native: $(TEST_BUILD)/sdf-property-gesture

test-native: $(TEST_BUILD)/viewport-coordinates

test-native: $(TEST_BUILD)/sdf-appearance-clipboard

test-native: $(TEST_BUILD)/scene-gesture $(TEST_BUILD)/emacs-scene-undo $(TEST_BUILD)/sculpt-api $(TEST_BUILD)/audio-synth $(TEST_BUILD)/sdf-sculpt $(TEST_BUILD)/agent-engine $(TEST_BUILD)/headless-job $(TEST_BUILD)/gltf-cook-static $(TEST_BUILD)/render-graph-contract $(TEST_BUILD)/audio-lifetime $(TEST_BUILD)/gltf-source-plan $(TEST_BUILD)/gltf-joint-count $(TEST_BUILD)/async-gltf-controller $(TEST_BUILD)/texture-refs $(TEST_BUILD)/cache-textures $(TEST_BUILD)/cache-texture-lease $(TEST_BUILD)/mesh-storage $(TEST_BUILD)/scene-load-vk-contract $(TEST_BUILD)/pending-scene-load $(TEST_BUILD)/geometry-async $(TEST_BUILD)/geometry-pending $(TEST_BUILD)/geometry-scene-upload $(TEST_BUILD)/geometry-upload-flight $(TEST_BUILD)/geometry-upload-memory $(TEST_BUILD)/geometry-upload-batch $(TEST_BUILD)/geometry-budget $(TEST_BUILD)/omdl-cache $(TEST_BUILD)/gltf-topology-fixture $(TEST_BUILD)/node-topology $(TEST_BUILD)/material-assets $(TEST_BUILD)/node-overrides $(TEST_BUILD)/mesh-properties $(TEST_BUILD)/object-identity $(TEST_BUILD)/slot-index $(TEST_BUILD)/dispatch-metrics $(TEST_BUILD)/keymap-image $(TEST_BUILD)/effect-policy $(TEST_BUILD)/live-binding-txn $(TEST_BUILD)/source-stage $(TEST_BUILD)/source-preflight $(TEST_BUILD)/repl-model $(TEST_BUILD)/font-utf8 $(TEST_BUILD)/hotreload-stats $(TEST_BUILD)/hotreload-watcher $(TEST_BUILD)/frame-state $(TEST_BUILD)/publication $(TEST_BUILD)/publication-queue $(TEST_BUILD)/publication-queue-repl $(TEST_BUILD)/publication-metrics $(TEST_BUILD)/sdf-appearance-clipboard
	@$(TEST_BUILD)/viewport-coordinates
	@$(TEST_BUILD)/sdf-viewport
	@$(TEST_BUILD)/sdf-typed-viewport
	@$(TEST_BUILD)/sdf-appearance-clipboard
	@$(TEST_BUILD)/sdf-property-bridge
	@$(TEST_BUILD)/sdf-property-gesture
	@$(TEST_BUILD)/scene-gesture
	@$(TEST_BUILD)/emacs-scene-undo
	@$(TEST_BUILD)/sculpt-api
	@$(TEST_BUILD)/audio-synth
	@$(TEST_BUILD)/sdf-sculpt
	@$(TEST_BUILD)/agent-engine
	@$(TEST_BUILD)/headless-job
	@$(TEST_BUILD)/audio-lifetime
	@$(TEST_BUILD)/texture-refs
	@$(TEST_BUILD)/render-graph-contract
	@$(TEST_BUILD)/shadow-cache-key
	@$(TEST_BUILD)/selection-visibility
	@$(TEST_BUILD)/gltf-cook-static
	@$(TEST_BUILD)/gltf-source-plan
	@$(TEST_BUILD)/gltf-joint-count
	@$(TEST_BUILD)/async-gltf-controller
	@$(TEST_BUILD)/cache-textures
	@$(TEST_BUILD)/cache-texture-lease
	@$(TEST_BUILD)/mesh-storage
	@$(TEST_BUILD)/scene-load-vk-contract
	@$(TEST_BUILD)/pending-scene-load
	@$(TEST_BUILD)/geometry-async
	@$(TEST_BUILD)/geometry-scene-upload
	@$(TEST_BUILD)/geometry-pending
	@$(TEST_BUILD)/geometry-upload-flight
	@$(TEST_BUILD)/geometry-upload-memory
	@$(TEST_BUILD)/geometry-upload-batch
	@$(TEST_BUILD)/geometry-budget
	@$(TEST_BUILD)/omdl-cache
	@$(TEST_BUILD)/gltf-topology-fixture
	@$(TEST_BUILD)/node-topology
	@$(TEST_BUILD)/material-assets
	@$(TEST_BUILD)/node-overrides
	@$(TEST_BUILD)/mesh-properties
	@$(TEST_BUILD)/object-identity
	@$(TEST_BUILD)/slot-index
	@$(TEST_BUILD)/dispatch-metrics
	@$(TEST_BUILD)/keymap-image
	@$(TEST_BUILD)/effect-policy
	@$(TEST_BUILD)/live-binding-txn
	@$(TEST_BUILD)/source-stage
	@$(TEST_BUILD)/source-preflight
	@$(TEST_BUILD)/repl-model
	@$(TEST_BUILD)/publication
	@$(TEST_BUILD)/publication-queue
	@$(TEST_BUILD)/publication-metrics
	@$(TEST_BUILD)/publication-queue-repl
	@$(TEST_BUILD)/font-utf8
	@$(TEST_BUILD)/hotreload-stats
	@$(TEST_BUILD)/hotreload-watcher
	@$(TEST_BUILD)/frame-state
	@if printf '#include <vulkan/vulkan_core.h>\n' | $(CC) -x c -E - >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory test-renderer2d-capacity; \
	else \
	  printf '%s\n' 'SKIP: renderer2d capacity integration (Vulkan SDK headers unavailable)'; \
	fi

# Headless Vulkan profile target. The timestamped directory preserves baselines;
# an explicit existing BENCH_OUTPUT is rejected rather than overwritten.
BENCH_RUNS ?= 5
BENCH_WARMUP_FRAMES ?= 120
BENCH_FRAMES ?= 600
BENCH_COLOR ?= auto
BENCH_GPU_TIMESTAMPS ?= on
BENCH_DEBUG_LABELS ?= off
BENCH_VALIDATION ?= off
BENCH_WORK_COUNTERS ?= on
BENCH_COMPARE_TO ?=
BENCH_COMPARISON_METRIC ?= total_ns
BENCH_MAX_REGRESSION_PCT ?=
BENCH_ASSET_ROOT ?= assets
BENCH_MONAD_MANIFEST ?= tests/monad/bench-scene.mon
BENCH_OUTPUT ?= build/bench/monad-$(shell date -u +%Y%m%dT%H%M%SZ)
BENCH_SELECTION ?= all

# Canonical aggregate CLI. Individual recipes below remain Make-owned build/run
# implementations; ./make provides selection, requirements and status output.
.PHONY: bench
bench:
	@./make bench "$(BENCH_SELECTION)" --runs "$(BENCH_RUNS)" \
	  --asset-root "$(BENCH_ASSET_ROOT)"

.PHONY: bench-monad
bench-monad: $(EXECUTABLE)
	@test ! -e "$(BENCH_OUTPUT)" || { \
	  echo "Refusing to overwrite benchmark artifacts: $(BENCH_OUTPUT)" >&2; exit 1; }
	OBSIDIAN_BENCH_ASSET_ROOT="$(BENCH_ASSET_ROOT)" \
	  python3 tools/bench/engine_bench.py --executable ./$(EXECUTABLE) \
	  --build-compiler "$(CC)" --build-flags "$(CFLAGS)" \
	  --runs "$(BENCH_RUNS)" --warmup-frames "$(BENCH_WARMUP_FRAMES)" \
	  --frames "$(BENCH_FRAMES)" --headless \
	  --gpu-timestamps "$(BENCH_GPU_TIMESTAMPS)" \
	  --debug-labels "$(BENCH_DEBUG_LABELS)" \
	  $(if $(filter on,$(BENCH_VALIDATION)),--validation) \
	  --work-counters "$(BENCH_WORK_COUNTERS)" \
	  $(if $(BENCH_COMPARE_TO),--compare-to "$(BENCH_COMPARE_TO)") \
	  $(if $(BENCH_COMPARE_TO),--comparison-metric "$(BENCH_COMPARISON_METRIC)") \
	  $(if $(BENCH_MAX_REGRESSION_PCT),--max-regression-pct "$(BENCH_MAX_REGRESSION_PCT)") \
	  --monad-manifest "$(BENCH_MONAD_MANIFEST)" \
	  $(if $(filter on,$(BENCH_GPU_TIMESTAMPS)),\
	  --require-gpu-stage shadow.cascade.0 \
	  --require-gpu-stage shadow.cascade.1 \
	  --require-gpu-stage shadow.cascade.2 \
	  --require-gpu-stage shadow.cascade.3 \
	  --require-gpu-stage hzb.mip.0) \
	  --output "$(BENCH_OUTPUT)" --color "$(BENCH_COLOR)"

.PHONY: bench-monad-validation
bench-monad-validation:
	@$(MAKE) --no-print-directory bench-monad \
	  BENCH_VALIDATION=on BENCH_RUNS=1 BENCH_WARMUP_FRAMES=0 \
	  BENCH_OUTPUT="build/bench/validation-$(shell date -u +%Y%m%dT%H%M%SZ)"

$(TEST_BUILD)/headless-startup-render: tests/native/headless_startup_render.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) tests/native/headless_startup_render.c -o $@

.PHONY: test-headless-startup-render
test-headless-startup-render: $(EXECUTABLE) $(TEST_BUILD)/headless-startup-render
	@$(TEST_BUILD)/headless-startup-render ./$(EXECUTABLE) --test-antialiasing

$(TEST_BUILD)/matcap-render: tests/native/matcap_render.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) $< -o $@

.PHONY: test-headless-matcap
test-headless-matcap: $(EXECUTABLE) $(TEST_BUILD)/matcap-render
	@$(TEST_BUILD)/matcap-render ./$(EXECUTABLE)

.PHONY: test-headless-async-obj
test-headless-async-obj: $(EXECUTABLE)
	@mkdir -p $(TEST_BUILD)
	@./$(EXECUTABLE) --headless-raw $(TEST_BUILD)/async-obj.rgba \
		--test-obj-async tests/fixtures/async_triangle.obj \
		--width 256 --height 256

.PHONY: test-vulkan-validation-headless
test-vulkan-validation-headless: $(EXECUTABLE) $(TEST_BUILD)/headless-startup-render
	@OBSIDIAN_TEST_VULKAN_VALIDATION=1 $(TEST_BUILD)/headless-startup-render

$(TEST_BUILD)/dynamic-vertex-vulkan: tests/native/dynamic_vertex_vulkan.c $(LIB_OBJECTS) $(JOLTC_STATIC)
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJECTS) -o $@ $(LDFLAGS) \
		-Wl,--wrap=vkAllocateMemory -Wl,--wrap=vkMapMemory

.PHONY: test-vulkan-dynamic-headless test-vulkan-dynamic-window
test-vulkan-dynamic-headless: $(TEST_BUILD)/dynamic-vertex-vulkan
	@OBSIDIAN_TEST_VULKAN_VALIDATION=1 $(TEST_BUILD)/dynamic-vertex-vulkan

test-vulkan-dynamic-window: $(TEST_BUILD)/dynamic-vertex-vulkan
	@OBSIDIAN_TEST_VULKAN_VALIDATION=1 $(TEST_BUILD)/dynamic-vertex-vulkan --window

.PHONY: test-headless-sponza
test-headless-sponza: $(EXECUTABLE)
	@mkdir -p $(TEST_BUILD)
	@OBSIDIAN_BENCHMARK_FRAMES=1 OBSIDIAN_BENCHMARK_WARMUP=0 \
		python3 tools/bench/engine_bench.py --executable ./$(EXECUTABLE) \
		--scene assets/gltf/Sponza/glTF/Sponza.gltf --headless \
		--width 256 --height 256 --runs 1 --warmup-frames 0 --frames 1 \
		--scene-cache cold --output $(TEST_BUILD)/headless-sponza --color never
	@python3 tests/tooling/test_headless_sponza_report.py \
		$(TEST_BUILD)/headless-sponza

PBR_AO_BENCH_RUNS ?= 5
PBR_AO_WARMUP_FRAMES ?= 30
PBR_AO_BENCH_FRAMES ?= 60
.PHONY: test-headless-pbr-ao
test-headless-pbr-ao: $(EXECUTABLE)
	@mkdir -p $(TEST_BUILD)
	@python3 tools/bench/engine_bench.py --executable ./$(EXECUTABLE) \
		--scene assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf --headless \
		--width 256 --height 256 --runs $(PBR_AO_BENCH_RUNS) \
		--warmup-frames $(PBR_AO_WARMUP_FRAMES) \
		--frames $(PBR_AO_BENCH_FRAMES) --scene-cache warm \
		--require-gpu-stage main_forward \
		--output $(TEST_BUILD)/pbr-ao-bench --color never
	@python3 tests/tooling/test_pbr_ao_report.py \
		$(TEST_BUILD)/pbr-ao-bench

PBR_LIGHTING_BENCH_RUNS ?= 5
PBR_LIGHTING_BENCH_FRAMES ?= 60
.PHONY: test-headless-pbr-lighting
test-headless-pbr-lighting: $(EXECUTABLE)
	@python3 tools/bench/engine_bench.py --executable ./$(EXECUTABLE) \
		--scene tests/fixtures/async-material.gltf --headless \
		--width 256 --height 256 --runs $(PBR_LIGHTING_BENCH_RUNS) \
		--warmup-frames 0 --frames $(PBR_LIGHTING_BENCH_FRAMES) \
		--scene-cache warm --sun-intensity 3 \
		--require-gpu-stage main_forward \
		--output $(TEST_BUILD)/pbr-lighting-sun --color never
	@python3 tools/bench/engine_bench.py --executable ./$(EXECUTABLE) \
		--scene tests/fixtures/async-material.gltf --headless \
		--width 256 --height 256 --runs $(PBR_LIGHTING_BENCH_RUNS) \
		--warmup-frames 0 --frames $(PBR_LIGHTING_BENCH_FRAMES) \
		--scene-cache warm --sun-intensity 0 \
		--require-gpu-stage main_forward \
		--output $(TEST_BUILD)/pbr-lighting-no-sun --color never
	@python3 tests/tooling/test_pbr_lighting_report.py \
		$(TEST_BUILD)/pbr-lighting-sun $(TEST_BUILD)/pbr-lighting-no-sun

test-headless-middle-orbit: $(EXECUTABLE)
	@mkdir -p $(TEST_BUILD)
	monad test tests/monad/middle-orbit-contract.mon
	env -u OBSIDIAN_MONAD_BOOT \
	  ./$(EXECUTABLE) --headless-raw $(TEST_BUILD)/middle-orbit.rgba \
	  --middle-orbit-test --width 256 --height 256
	@test "$$(wc -c < $(TEST_BUILD)/middle-orbit.rgba)" -eq 262144

test-editor-keymaps:
	monad test tests/monad/editor-keymaps-contract.mon

test-headless-editor-keymaps: $(EXECUTABLE)
	@mkdir -p $(TEST_BUILD)
	env -u OBSIDIAN_MONAD_BOOT \
	  ./$(EXECUTABLE) --headless-raw $(TEST_BUILD)/editor-keymaps.rgba \
	  --editor-panels-test --width 640 --height 480
	@test "$$(wc -c < $(TEST_BUILD)/editor-keymaps.rgba)" -eq 1228800

$(TEST_BUILD)/editor-material-surface: tests/native/editor_material_surface.c $(LIB_OBJECTS) $(JOLTC_STATIC)
	@mkdir -p $(TEST_BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB_OBJECTS) -o $@ $(LDFLAGS)

.PHONY: test-headless-editor-materials
test-headless-editor-materials: $(TEST_BUILD)/editor-material-surface
	env -u OBSIDIAN_MONAD_BOOT $(TEST_BUILD)/editor-material-surface

.PHONY: test-editor-tool-windows
test-editor-tool-windows: $(TEST_BUILD)/editor-material-surface
	env -u OBSIDIAN_MONAD_BOOT $(TEST_BUILD)/editor-material-surface --windows

.PHONY: profile-check
profile-check: $(EXECUTABLE) $(TEST_BUILD)/gpu-profile $(TEST_BUILD)/engine-bench $(TEST_BUILD)/headless-startup-render
	@$(TEST_BUILD)/gpu-profile
	@$(TEST_BUILD)/engine-bench
	@python3 tests/tooling/test_engine_bench_schema.py
	@$(TEST_BUILD)/headless-startup-render
	@$(MAKE) test-vulkan-validation-headless
	@$(MAKE) --no-print-directory bench-monad
	@$(MAKE) --no-print-directory bench-monad-validation

# Keep SDK-backed native-call tests in the default suite whenever possible.
# Skip explicitly when the SDK has not been installed on this machine.
# Compile the new REPL embedding client against the real public SDK headers.
# This proves declarations/type-checking, NOT linking or executing MonadC.
.PHONY: test-repl-abi
test-repl-abi:
	@test -f $(MONADC_INCLUDE)/monad/monad.h || { echo 'Missing Monad SDK headers: $(MONADC_INCLUDE)'; exit 1; }
	$(CC) -std=c23 -Wall -Wextra -Werror -pedantic -fsyntax-only \
		-I$(MONADC_INCLUDE) -Isrc/monad -Isrc/scene \
		src/monad/repl_service.c src/monad/native_read.c src/monad/hotreload_api.c src/monad/export_catalog.c src/monad/catalog_transition.c src/monad/publication_queue.c src/monad/publication_metrics.c src/monad/source_stage.c src/monad/semantic_preflight.c src/monad/dispatch_metrics.c src/monad/slot_index.c
	$(CC) -std=c23 -Wall -Wextra -Werror -pedantic -fsyntax-only \
		-I$(MONADC_INCLUDE) -Itests/fixtures -Isrc/monad -Isrc/scene -Isrc/core \
		src/monad/frame_api.c src/monad/hotreload_api.c

# Integration fixtures are Monad, not pseudo-C tests; require real object space.
test-monad:
	@./tests/run_monad.sh

# Frame tests REQUIRE a renderer + the actual embedded Monad runtime. Loading
# a .mon fixture and exiting is NOT sufficient; see tests/run_frame_monad.sh.
test-frame:
	@./tests/run_frame_monad.sh

# Default always runs what the host can test and reports—not hides—gated work.
test: test-native test-tooling test-editor-keymaps
	@python3 tests/tooling/test_rgba_png.py
	@if [ "$(OBSIDIAN_GPU_TESTS)" = 1 ]; then \
	  $(MAKE) --no-print-directory test-headless-editor-keymaps test-headless-editor-materials || exit $$?; \
	else \
	  printf '%s\n' 'SKIP: headless editor Vulkan surfaces; run make test OBSIDIAN_GPU_TESTS=1'; \
	fi
	@if [ -f "$(MONADC_INCLUDE)/monad/monad.h" ]; then \
	  $(MAKE) --no-print-directory test-native-read || exit $$?; \
	  $(MAKE) --no-print-directory test-export-catalog || exit $$?; \
	  $(MAKE) --no-print-directory test-catalog-transition || exit $$?; \
	  $(MAKE) --no-print-directory test-semantic-preflight || exit $$?; \
	  $(MAKE) --no-print-directory test-repl-abi || exit $$?; \
	else \
	  printf '%s\n' 'SKIP: REPL public Monad SDK declaration check (headers unavailable)'; \
	fi
	@if [ -n "$(OBSIDIAN_MONAD_TEST_RUNNER)" ]; then \
\
	  $(MAKE) --no-print-directory test-monad; \
	else \
	  printf '%s\n' 'SKIP: Monad integration suite; set OBSIDIAN_MONAD_TEST_RUNNER (see docs/TESTING.org)'; \
	fi
	@if [ -n "$(OBSIDIAN_MONAD_FRAME_RUNNER)" ]; then \
	  $(MAKE) --no-print-directory test-frame; \
	else \
	  printf '%s\n' 'SKIP: live frame/Monad integration; set OBSIDIAN_MONAD_FRAME_RUNNER'; \
	fi

# CI/SDK-provisioned machines should run this instead of silently skipping.
test-all: test-native test-native-read test-export-catalog test-catalog-transition test-semantic-preflight test-repl-abi test-monad test-frame test-headless-editor-keymaps test-headless-editor-materials

# Rebuild the native regression suite in an isolated directory with ASan,
# UBSan and LeakSanitizer. This is host/native coverage, not a Vulkan driver
# or full-engine GPU lifecycle sanitizer run.
.PHONY: test-asan
test-asan:
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) --no-print-directory -B TEST_BUILD=build/asan/native \
		TEST_NATIVE_CFLAGS='$(TEST_NATIVE_CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' \
		CFLAGS='$(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' \
		test-native

# One headless quality pass: normal tests, instrumented native tests, CPU
# benchmarks, and optional real Vulkan headless pixel/validation smokes.
.PHONY: test-headless
test-headless:
	$(MAKE) --no-print-directory test
	$(MAKE) --no-print-directory test-asan
	./make bench cpu --color never
	@if [ "$(OBSIDIAN_GPU_TESTS)" = 1 ]; then \
		$(MAKE) --no-print-directory test-headless-startup-render || exit $$?; \
		$(MAKE) --no-print-directory test-vulkan-validation-headless || exit $$?; \
		$(MAKE) --no-print-directory test-headless-sponza || exit $$?; \
		$(MAKE) --no-print-directory test-headless-pbr-lighting || exit $$?; \
		$(MAKE) --no-print-directory test-headless-pbr-ao || exit $$?; \
	else \
		printf '%s\n' 'SKIP: real Vulkan headless pixel/validation/PBR benchmark smokes; set OBSIDIAN_GPU_TESTS=1'; \
	fi

$(TEST_BUILD)/font-utf8-bench: tools/bench/font_utf8_bench.c src/graphics/font_utf8.c src/graphics/font_utf8.h
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -DNDEBUG -Wall -Wextra -Isrc/graphics tools/bench/font_utf8_bench.c src/graphics/font_utf8.c -o $@

bench-font: $(TEST_BUILD)/font-utf8-bench
	@$(TEST_BUILD)/font-utf8-bench $(or $(BENCH_ITERATIONS),10000)

# Installation (installs libraries and headers)
install: libs
	install -d $(INSTALL_DIR)/lib
	install -m 644 $(LIB_NAME).a $(INSTALL_DIR)/lib
	install -m 755 $(LIB_NAME).so $(INSTALL_DIR)/lib
	install -d $(INSTALL_DIR)/include/obsidian
	install -m 644 $(filter-out $(addprefix src/monad/,bindings.h scene_api.h camera_api.h input_api.h),$(HEADERS)) $(INSTALL_DIR)/include/obsidian
	if [ -n "$(SPV_HEADERS)" ]; then \
		install -m 644 $(SPV_HEADERS) $(INSTALL_DIR)/include/obsidian; \
	fi
	@if [ "$(INSTALL_DIR)" = "/usr" ]; then ldconfig; fi

# Uninstall
uninstall:
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).a
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).so
	rm -rf $(INSTALL_DIR)/include/obsidian
	@if [ "$(INSTALL_DIR)" = "/usr" ]; then ldconfig; fi

clean:
	rm -rf $(BUILD_DIR) vendor/joltc/build
	rm -f $(ALL_OBJECTS) $(DEPFILES) $(SHADER_SPVS) $(SPV_HEADERS) $(FONT_HEADERS)
	rm -f $(SHADER_DIR)/*.spv $(SHADER_DIR)/*.spv.h $(FONT_DIR)/*.h
	rm -f $(LIB_NAME).a $(LIB_NAME).so $(EXECUTABLE)
	find . -path ./.git -prune -o -type f \( -name '*.o' -o -name '*.d' \) -exec rm -f {} +
	find . -path ./.git -prune -o -type d \( -name __pycache__ -o -name .pytest_cache \) -prune -exec rm -rf {} +
# Clean everything including shaders
distclean: clean
	rm -f $(SHADER_DIR)/*.spv $(SHADER_DIR)/*.spv.h

# Rebuild everything from scratch
rebuild: clean all

# Run the executable
run: $(EXECUTABLE)
	./$(EXECUTABLE)

# Debug build (rebuild with debug symbols and no optimization)
debug: CFLAGS = -std=c23 -Wall -Wextra -g3 -O0 -fPIC $(shell pkg-config --cflags freetype2) -I$(MONADC_INCLUDE)
debug: rebuild

# Help target
help:
	@echo "Obsidian Engine Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the executable (default; parallel across online CPUs)"
	@echo "  JOBS=N    - Limit default parallelism (explicit make -jN also supported)"
	@echo "  JOLT_JOBS=N  - Limit nested CMake parallelism for Jolt (default 4)"
	@echo "  JOLT_LTO=ON  - Opt into Jolt link-time optimization (default OFF)"
	@echo "  JOLT_NATIVE=1 - Tune Jolt for THIS CPU (not redistributable)"
	@echo "  libs      - Build static and shared libraries"
	@echo "  install   - Install libraries and headers"
	@echo "  uninstall - Remove installed files"
	@echo "  clean     - Remove build artifacts"
	@echo "  distclean - Remove all generated files"
	@echo "  rebuild   - Clean and rebuild everything"
	@echo "  run       - Build and run the executable"
	@echo "  debug     - Build with debug symbols and no optimization"
	@echo "  test      - Run headless tests plus Monad tests when a runner is provided"
	@echo "  test-all  - Require both real embedded Monad and frame integration runners"
	@echo "  test-frame - Run frame-scoped foreign-object integration harness"
	@echo "  bench     - Run the unified benchmark CLI (all registered workloads)"
	@echo "  ./make bench --list - List benchmark names and requirements"
	@echo "  help      - Show this help message"

.PHONY: all libs test-keychords-monad install uninstall clean distclean rebuild run debug help

.PHONY: test-font-unicode
test-font-unicode: libobsidian.so
	$(CC) $(CFLAGS) $(INCLUDES) tests/font-unicode.c -L. -Wl,-rpath,$(CURDIR) -lobsidian $(shell pkg-config --libs freetype2 fontconfig) -o tests/font-unicode
	./tests/font-unicode

# Cycle 39 CPU-only path-dedup benchmark; not renderer frame time.
.PHONY: bench-texture-refs
bench-texture-refs:
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -Wall -Wextra -Isrc/graphics tools/bench/texture_refs.c src/graphics/texture_refs.c -o $(TEST_BUILD)/bench-texture-refs
	@$(TEST_BUILD)/bench-texture-refs

# Cycle 54: real renderer-backed speculative gesture, commit/cancel/conflict.
$(TEST_BUILD)/scene-gesture: tests/native/scene_gesture.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c src/scene/sdf_inspector.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/scene_gesture.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@

.PHONY: test-scene-gesture
test-scene-gesture: $(TEST_BUILD)/scene-gesture
	@$(TEST_BUILD)/scene-gesture

# Cycle 55: real viewport tool against production SDF journal and SSBO writer.
$(TEST_BUILD)/sdf-viewport: tests/native/sdf_viewport.c src/editor/sdf_viewport.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c src/editor/sdf_viewport.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Itests/fixtures -Isrc/editor -Isrc/scene -Isrc/graphics \
		tests/native/sdf_viewport.c src/editor/sdf_viewport.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-viewport
test-sdf-viewport: $(TEST_BUILD)/sdf-viewport
	./$(TEST_BUILD)/sdf-viewport

# Cycle 60: typed primitive picking through the production viewport journal.
$(TEST_BUILD)/sdf-typed-viewport: tests/native/sdf_typed_viewport.c src/editor/sdf_property_bridge.c src/editor/sdf_viewport.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c src/editor/sdf_viewport.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Itests/fixtures -Isrc/editor -Isrc/scene -Isrc/graphics \
		tests/native/sdf_typed_viewport.c src/editor/sdf_property_bridge.c src/editor/sdf_viewport.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-typed-viewport
test-sdf-typed-viewport: $(TEST_BUILD)/sdf-typed-viewport
	@$(TEST_BUILD)/sdf-typed-viewport

# Cycle 61: copy SDF material/emission by value and undo a paste across types.
$(TEST_BUILD)/sdf-appearance-clipboard: tests/native/sdf_appearance_clipboard.c src/core/atomic_file.c src/core/atomic_file.h src/editor/sdf_appearance_clipboard.c src/editor/sdf_appearance_clipboard.h src/editor/sdf_property_bridge.c src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/scene/sdf_sculpt.c src/scene/sdf_sculpt.h src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Itests/fixtures -Isrc/editor -Isrc/scene -Isrc/core -Isrc/graphics \
		tests/native/sdf_appearance_clipboard.c src/editor/sdf_appearance_clipboard.c \
		src/core/atomic_file.c src/editor/sdf_property_bridge.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c \
		src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-appearance-clipboard
test-sdf-appearance-clipboard: $(TEST_BUILD)/sdf-appearance-clipboard
	@$(TEST_BUILD)/sdf-appearance-clipboard

# Cycle 62: independent Python authoring/inspection against the production C
# codec, loader, scene journal, and atomic writer.
.PHONY: test-sdf-appearance-preset
test-sdf-appearance-preset: $(TEST_BUILD)/sdf-appearance-clipboard
	OBSIDIAN_TEST_BUILD='$(TEST_BUILD)' python3 tests/tooling/test_obslook.py

test-native: test-sdf-appearance-preset

# Cycle 63: selected-object undo through the production scene/journal/SSBO.
$(TEST_BUILD)/sdf-selective-undo: tests/native/sdf_selective_undo.c src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_selective_undo.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c \
		src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-selective-undo
test-sdf-selective-undo: $(TEST_BUILD)/sdf-selective-undo
	@$(TEST_BUILD)/sdf-selective-undo

test-native: test-sdf-selective-undo

# Cycle 64: editor input command boundaries share the production undo journal.
$(TEST_BUILD)/scene-undo-input: tests/native/scene_undo_input.c src/platform/scene_undo_input.c src/platform/scene_undo_input.h src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/platform -Isrc/scene -Itests/fixtures \
		tests/native/scene_undo_input.c src/platform/scene_undo_input.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-scene-undo-input
test-scene-undo-input: $(TEST_BUILD)/scene-undo-input
	@$(TEST_BUILD)/scene-undo-input

test-native: test-scene-undo-input

# Cycle 65: adversarial transaction integrity and net-change undo retention.
$(TEST_BUILD)/sdf-undo-transactions: tests/native/sdf_undo_transactions.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_undo_transactions.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-undo-transactions
test-sdf-undo-transactions: $(TEST_BUILD)/sdf-undo-transactions
	@$(TEST_BUILD)/sdf-undo-transactions

test-native: test-sdf-undo-transactions

# Cycle 66: preserve undoability after the original group is evicted by undo.
$(TEST_BUILD)/sdf-undo-adversarial: tests/native/sdf_undo_adversarial.c src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_undo_adversarial.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-undo-adversarial
test-sdf-undo-adversarial: $(TEST_BUILD)/sdf-undo-adversarial
	@$(TEST_BUILD)/sdf-undo-adversarial

test-native: test-sdf-undo-adversarial

# Cycle 67: selective undo dependency safety and deep inverse-chain invariants.
# Both test binaries link actual scene, bounded journal, token and SDF writer.
$(TEST_BUILD)/sdf-undo-dependencies: tests/native/sdf_undo_dependencies.c src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_undo_dependencies.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-undo-dependencies
test-sdf-undo-dependencies: $(TEST_BUILD)/sdf-undo-dependencies
	@$(TEST_BUILD)/sdf-undo-dependencies

$(TEST_BUILD)/sdf-undo-deep: tests/native/sdf_undo_deep.c src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Isrc/scene -Itests/fixtures \
		tests/native/sdf_undo_deep.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-undo-deep
test-sdf-undo-deep: $(TEST_BUILD)/sdf-undo-deep
	@$(TEST_BUILD)/sdf-undo-deep

test-native: test-sdf-undo-dependencies test-sdf-undo-deep

# Fixed-capacity ticket reverse index: no allocation or Vulkan requirement.
$(TEST_BUILD)/sdf-tokens: tests/native/sdf_tokens.c src/scene/sdf_tokens.c src/scene/sdf_tokens.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/scene \
		tests/native/sdf_tokens.c src/scene/sdf_tokens.c -o $@
.PHONY: test-sdf-tokens
test-sdf-tokens: $(TEST_BUILD)/sdf-tokens
	@$(TEST_BUILD)/sdf-tokens

test-native: test-sdf-tokens

# CPU-only benchmark of the production bounded undo journal. Reports per-op
# median/p95/max at 1-, 16-, and 64-object group sizes; no GPU timing claim.
$(TEST_BUILD)/sdf-undo-bench: tools/bench/sdf_undo_bench.c tests/fixtures/agent_engine_contract.h src/scene/sdf_inspector.c src/scene/sdf_inspector.h src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -O3 -Wall -Wextra -Werror -pedantic -DOBS_SDF_TEST \
		-Isrc/scene -Itests/fixtures tools/bench/sdf_undo_bench.c \
		src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c \
		src/graphics/sdf.c -lm -o $@
.PHONY: bench-sdf-undo
bench-sdf-undo: $(TEST_BUILD)/sdf-undo-bench
	@$(TEST_BUILD)/sdf-undo-bench $(or $(BENCH_ITERATIONS),1500)

# Cycle 55: STRICT executable Vulkan/Monad integration of the exact viewport
# drag core (world rays), not a substitute for physical GLFW pointer testing.
.PHONY: test-sdf-viewport-runtime
test-sdf-viewport-runtime: $(LIB_NAME).so
	@mkdir -p $(TEST_BUILD)
	$(CC) -std=c23 -Wall -Wextra -Werror -I. -Isrc/editor -Isrc/scene \
		tests/integration/sdf_viewport_runtime.c -L. -Wl,-rpath,$(abspath .) \
		-lobsidian -lm -o $(TEST_BUILD)/sdf-viewport-runtime
	$(TEST_BUILD)/sdf-viewport-runtime monad/obsidian/sculpture.mon \
		$(TEST_BUILD)/viewport.rgba $(TEST_BUILD)/viewport.json
	python3 tools/rgba_to_png.py $(TEST_BUILD)/viewport.rgba \
		$(TEST_BUILD)/viewport.png 640 360

# Cycle 56: executable production GLFW-coordinate mapper, no Vulkan SDK needed.
$(TEST_BUILD)/viewport-coordinates: tests/native/viewport_coordinates.c src/editor/viewport_coordinates.c src/editor/viewport_coordinates.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -Isrc/editor tests/native/viewport_coordinates.c \
		src/editor/viewport_coordinates.c -lm -o $@
.PHONY: test-viewport-coordinates
test-viewport-coordinates: $(TEST_BUILD)/viewport-coordinates
	./$(TEST_BUILD)/viewport-coordinates

# Cycle 57 — production SDF panel writes use the same Emacs-like undo journal.
$(TEST_BUILD)/sdf-property-bridge: tests/native/sdf_property_bridge.c src/editor/sdf_property_bridge.c src/editor/sdf_property_bridge.h src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Itests/fixtures -Isrc/editor -Isrc/scene -Isrc/graphics \
		tests/native/sdf_property_bridge.c src/editor/sdf_property_bridge.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-property-bridge
test-sdf-property-bridge: $(TEST_BUILD)/sdf-property-bridge
	./$(TEST_BUILD)/sdf-property-bridge

# Cycle 58 — continuous SDF inspector gestures coalesce in the same journal.
$(TEST_BUILD)/sdf-property-gesture: tests/native/sdf_property_gesture.c src/editor/sdf_property_bridge.c src/editor/sdf_property_bridge.h src/scene/sdf_inspector.c src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_NATIVE_CFLAGS) -DOBS_SDF_TEST -Itests/fixtures -Isrc/editor -Isrc/scene -Isrc/graphics \
		tests/native/sdf_property_gesture.c src/editor/sdf_property_bridge.c src/scene/sdf_inspector.c \
		src/scene/sdf_sculpt.c src/scene/sdf_tokens.c src/graphics/sdf.c -lm -o $@
.PHONY: test-sdf-property-gesture
test-sdf-property-gesture: $(TEST_BUILD)/sdf-property-gesture
	./$(TEST_BUILD)/sdf-property-gesture
