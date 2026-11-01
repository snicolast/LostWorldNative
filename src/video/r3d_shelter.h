#pragma once
// Shelter (not in the original): whether the camera stands under a roof, so
// the weather (r3d_weather.h) keeps out of the indoor scenes (the INGEN
// buildings, the service corridor) the way it keeps out of the dark ones.
// The rain, the lightning, the overcast light and the weather's fog all
// stop there.
//
// The test looks straight up from the camera and from a ring of points
// around it, through the opaque scenery of the world viewport (already in
// view space, the camera at the origin): a ray stopped by a surface not far
// above counts as covered. Cut-out triangles (foliage, fences: the rain
// falls through a canopy) and the actors (a dinosaur's head over the
// camera) are ignored.
#include <cstddef>

#include "r3d_scene.h"

struct R3DShelter {
    static constexpr int RAYS = 9;            // the camera and a ring of eight around it
    static constexpr float RING = 3.0f;       // radius of the ring (world units)
    static constexpr float MIN_UP = 0.3f;     // a surface this close above does not count (the camera's own nose)
    static constexpr float MAX_UP = 60.0f;    // ... nor one this far up (a cliff, a sky dome)
    static constexpr int COVERED = 6;         // of the RAYS: under a roof

    // How many of the vertical rays hit a roof: vertices v[0..count) (whole
    // triangles), axes: the world x, y (up), z axes in view space
    static int hits(const R3DVertex* v, size_t count, const float axes[9]);

    // Once per game frame: tests every few frames, and changes its mind only
    // after two tests that agree (a beam crossing above for a moment)
    void step(const R3DVertex* v, size_t count, const float axes[9]);
    void clear() { sheltered_ = false, pending_ = 0, frame_ = 0; }
    bool sheltered() const { return sheltered_; }
    int last_hits() const { return last_hits_; }

private:
    bool sheltered_ = false;
    int pending_ = 0;   // tests in a row that disagree with sheltered_
    int frame_ = 0;
    int last_hits_ = 0;
};
