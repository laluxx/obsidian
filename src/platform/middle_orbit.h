#ifndef OBSIDIAN_MIDDLE_ORBIT_H
#define OBSIDIAN_MIDDLE_ORBIT_H

#include "camera.h"
#include <stdbool.h>

typedef struct ObsMiddleOrbitDrag {
    double last_x;
    double last_y;
    bool active;
} ObsMiddleOrbitDrag;

void obs_middle_orbit_begin(ObsMiddleOrbitDrag *drag, double x, double y);
void obs_middle_orbit_end(ObsMiddleOrbitDrag *drag);
bool obs_middle_orbit_motion(ObsMiddleOrbitDrag *drag, double x, double y,
                             Camera *cam, vec3 pivot);

#endif
