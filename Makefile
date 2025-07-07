CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g3 -o3
LDFLAGS = -lvulkan -lglfw -lcglm -lm
GLSLANG = glslangValidator
XXD = xxd

# For Linux
INCLUDES = -I/usr/include
LIBRARIES = -L/usr/lib/x86_64-linux-gnu

# Project settings
TARGET = revox
SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)

# Shaders
SHADERS = vert.vert frag.frag
SPV = $(SHADERS:.vert=.spv)
SPV := $(SPV:.frag=.spv)

SPV_HEADERS = $(SPV:.spv=.spv.h)

all: $(TARGET)

# Compile shaders if they are newer
%.spv: %.vert
	$(GLSLANG) -V $< -o $@

%.spv: %.frag
	$(GLSLANG) -V $< -o $@

# Convert to C header (xxd -i)
%.spv.h: %.spv
	$(XXD) -i $< > $@

# Build executable
$(TARGET): $(SPV_HEADERS) $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LIBRARIES) $(LDFLAGS)

# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(TARGET) *.o *.spv *.spv.h

.PHONY: all clean
