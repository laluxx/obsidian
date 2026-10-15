/// renderer2d_capacity — graphics subsystem
#pragma once

#include <stddef.h>

/// Public interface

/* Choose a bounded geometric capacity for the streaming 2D vertex buffer. */
size_t renderer2d_capacity_for(size_t current, size_t needed, size_t maximum);
