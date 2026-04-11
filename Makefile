CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -g -fPIC $(shell pkg-config --cflags freetype2 guile-3.0)
LDFLAGS = -fuse-ld=mold -lvulkan -lglfw -lX11 -lcglm -lm -lminiz $(shell pkg-config --libs freetype2 guile-3.0)

GLSLANG = glslangValidator
XXD = xxd

# Paths
INCLUDES = -I/usr/include -I. -I$(SHADER_DIR)
LIBRARIES = -L/usr/lib/x86_64-linux-gnu
INSTALL_DIR = /usr

# Project settings
LIB_NAME = libobsidian
EXECUTABLE = obsidian

# Source files
LIB_SOURCES = $(filter-out main.c, $(wildcard *.c))
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
MAIN_OBJECT = main.o
ALL_OBJECTS = $(LIB_OBJECTS) $(MAIN_OBJECT)

HEADERS = $(wildcard *.h)

# Shaders
SHADER_DIR = shaders
SHADER_VERTS = $(wildcard $(SHADER_DIR)/*.vert)
SHADER_FRAGS = $(wildcard $(SHADER_DIR)/*.frag)
SHADER_COMPS = $(wildcard $(SHADER_DIR)/*.comp)
SHADER_SPVS = $(SHADER_VERTS:.vert=.vert.spv) $(SHADER_FRAGS:.frag=.frag.spv) $(SHADER_COMPS:.comp=.comp.spv)
SPV_HEADERS = $(SHADER_SPVS:.spv=.spv.h)

# Default target - build executable directly
all:
	@$(MAKE) -j$$(nproc) internal_build

internal_build: $(SPV_HEADERS) $(EXECUTABLE)

# Compile shaders to SPIR-V
$(SHADER_DIR)/%.vert.spv: $(SHADER_DIR)/%.vert
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.frag.spv: $(SHADER_DIR)/%.frag
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
$(SHADER_DIR)/%.comp.spv: $(SHADER_DIR)/%.comp
	$(GLSLANG) -V --target-env vulkan1.3 $< -o $@
# Convert SPIR-V to C header
$(SHADER_DIR)/%.spv.h: $(SHADER_DIR)/%.spv
	$(XXD) -i -n $(subst /,_,$(subst .,_,$(notdir $<))) $< > $@

# Compile C sources to object files
%.o: %.c $(HEADERS) $(SPV_HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build executable by linking all objects directly
$(EXECUTABLE): $(ALL_OBJECTS)
	$(CC) $(ALL_OBJECTS) -o $@ $(LDFLAGS)

# Static library (optional, for distribution)
$(LIB_NAME).a: $(LIB_OBJECTS)
	ar rcs $@ $(LIB_OBJECTS)

# Shared library (optional, for distribution)
$(LIB_NAME).so: $(LIB_OBJECTS)
	$(CC) -shared -o $@ $(LIB_OBJECTS) $(LDFLAGS)

# Build both libraries
libs: $(LIB_NAME).a $(LIB_NAME).so

# Installation (installs libraries and headers)
install: libs
	install -d $(INSTALL_DIR)/lib
	install -m 644 $(LIB_NAME).a $(INSTALL_DIR)/lib
	install -m 755 $(LIB_NAME).so $(INSTALL_DIR)/lib
	install -d $(INSTALL_DIR)/include/obsidian
	install -m 644 $(HEADERS) $(INSTALL_DIR)/include/obsidian
	if [ -n "$(SPV_HEADERS)" ]; then \
		install -m 644 $(SPV_HEADERS) $(INSTALL_DIR)/include/obsidian; \
	fi
	ldconfig

# Uninstall
uninstall:
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).a
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).so
	rm -rf $(INSTALL_DIR)/include/obsidian
	ldconfig

clean:
	rm -f $(ALL_OBJECTS) $(SHADER_SPVS) $(SPV_HEADERS)
	rm -f $(LIB_NAME).a $(LIB_NAME).so $(EXECUTABLE)
# Clean everything including shaders
distclean: clean
	rm -f $(SHADER_DIR)/*.spv $(SHADER_DIR)/*.spv.h

# Rebuild everything from scratch
rebuild: clean all

# Run the executable
run: $(EXECUTABLE)
	./$(EXECUTABLE)

# Debug build (rebuild with debug symbols and no optimization)
debug: CFLAGS = -std=c23 -Wall -Wextra -g3 -O0 -fPIC $(shell pkg-config --cflags freetype2 guile-3.0)
debug: rebuild

# Help target
help:
	@echo "Obsidian Engine Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the executable (default)"
	@echo "  libs      - Build static and shared libraries"
	@echo "  install   - Install libraries and headers"
	@echo "  uninstall - Remove installed files"
	@echo "  clean     - Remove build artifacts"
	@echo "  distclean - Remove all generated files"
	@echo "  rebuild   - Clean and rebuild everything"
	@echo "  run       - Build and run the executable"
	@echo "  debug     - Build with debug symbols and no optimization"
	@echo "  help      - Show this help message"

.PHONY: all libs install uninstall clean distclean rebuild run debug help
