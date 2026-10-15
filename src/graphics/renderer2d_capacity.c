/// renderer2d_capacity — graphics subsystem
#include "renderer2d_capacity.h"

/// Implementation

size_t renderer2d_capacity_for(size_t current, size_t needed, size_t maximum) {
    if (maximum == 0 || needed > maximum) return 0;

    size_t capacity = current;
    if (capacity == 0) {
        capacity = maximum < 4096 ? maximum : 4096;
    } else if (capacity > maximum) {
        capacity = maximum;
    }

    while (capacity < needed) {
        if (capacity > maximum / 2) {
            capacity = maximum;
            break;
        }
        capacity *= 2;
    }
    return capacity;
}
