// Walking grid for bots (navgrid_web.cpp): multiplayer maps carry the
// engine's pathfinding data in parts only, so the server measures each map
// itself (ray casts on a 1 m grid, box sweeps between neighbors) and plans
// with A* over it.
#pragma once
#include "vector3.h"
#include <vector>

namespace NavGrid {
// Builds the grid for the current level if it isn't (logs how long it took).
bool Ensure(void);
// A path of waypoints from `from` toward `to` (as close as the grid gets).
bool Find_Path(const Vector3 &from, const Vector3 &to, std::vector<Vector3> &path);
// Something the grid doesn't know about is in the way at `p` (a bot got
// stuck there): paths avoid it from now on, if they can.
void Block(const Vector3 &p, float radius);
// Writes the grid as a PGM image (top view, brighter = more links).
bool Dump(const char *file);
int Node_Count(void);
} // namespace NavGrid
