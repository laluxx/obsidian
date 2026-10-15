/// Cycle 56 — test the production coordinate mapper, not a duplicate formula
#include "viewport_coordinates.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static ObsViewportPixel p;
static void mapping(double x, double y, int w, int h, unsigned rw, unsigned rh,
                    bool outside, double expected_x, double expected_y) {
    assert(obs_viewport_cursor_pixel(x,y,w,h,rw,rh,outside,&p));
    assert(fabs(p.x-expected_x) < 1e-9);
    assert(fabs(p.y-expected_y) < 1e-9);
}
int main(void) {
    /* 1x, 2x, fractional and nonuniform scale (including resize-in-flight). */
    mapping(320,180,640,360,640,360,false,320,180);
    assert(p.inside);
    mapping(320,180,640,360,1280,720,false,640,360);
    mapping(320,180,640,360,960,540,false,480,270);
    mapping(320,180,640,360,1280,1080,false,640,540);
    mapping(0,0,640,360,1280,720,false,0,0);
    mapping(639.5,359.5,640,360,1280,720,false,1279,719);
    /* Capture must not clamp cursor at viewport edge: visible jump on drag. */
    assert(!obs_viewport_cursor_pixel(-10,180,640,360,1280,720,false,&p));
    mapping(-10,180,640,360,1280,720,true,-20,360);
    assert(!p.inside);
    mapping(650,370,640,360,1280,720,true,1300,740);
    mapping(320,180,640,360,1280,720,true,640,360);
    assert(p.inside);
    assert(!obs_viewport_cursor_pixel(640,0,640,360,1280,720,false,&p));
    assert(!obs_viewport_cursor_pixel(0,360,640,360,1280,720,false,&p));
    assert(!obs_viewport_cursor_pixel(NAN,0,640,360,1280,720,true,&p));
    assert(!obs_viewport_cursor_pixel(INFINITY,0,640,360,1280,720,true,&p));
    assert(!obs_viewport_cursor_pixel(0,0,0,360,1280,720,true,&p));
    assert(!obs_viewport_cursor_pixel(0,0,640,-1,1280,720,true,&p));
    assert(!obs_viewport_cursor_pixel(0,0,640,360,0,720,true,&p));
    assert(!obs_viewport_cursor_pixel(0,0,640,360,1280,0,true,&p));
    assert(!obs_viewport_cursor_pixel(0,0,640,360,1280,720,false,NULL));
    assert(!obs_viewport_cursor_pixel(1e100,0,640,360,1280,720,true,&p));
    /* Dynamic input across a resize: mapping is recomputed, not cached. */
    for (unsigned i=1; i<=4000; ++i) {
        int w = 500 + (int)(i % 51), h = 200 + (int)(i % 31);
        unsigned rw = 1000 + i % 127, rh = 400 + i % 79;
        mapping(w*0.25,h*0.75,w,h,rw,rh,false,rw*0.25,rh*0.75);
    }
    puts("Cycle 56: 4000 dynamic HiDPI/resize coordinate cases PASS");
    return 0;
}
