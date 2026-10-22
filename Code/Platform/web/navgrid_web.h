// Walking grids for bots (navgrid_web.cpp): multiplayer maps carry the
// engine's pathfinding data in parts only, so the server measures each map
// itself (ray casts on a grid, box sweeps between neighbors) and plans with
// A* over it: one grid for soldiers (1 m), one for vehicles (2 m, a tank's
// width).
#pragma once
#include "vector3.h"
#include <vector>

#include <string>

namespace NavGrid {
enum Kind { SOLDIER, VEHICLE };
// Builds the grids for the current level, a slice per call (call it every
// frame); true once the soldiers' grid is ready (the vehicles' follows).
bool Ensure(void);
// A path of waypoints from `from` toward `to` (as close as the grid gets).
bool Find_Path(Kind kind, const Vector3 &from, const Vector3 &to, std::vector<Vector3> &path);
// Something the grid doesn't know about is in the way at `p` (a bot got
// stuck there): paths avoid it from now on, if they can.
void Block(Kind kind, const Vector3 &p, float radius);
// Writes a grid as a PGM image (top view, brighter = more links).
bool Dump(Kind kind, const char *file);
bool Map_Json(Kind kind, std::string &out); // the grid for game records (see navgrid_web.cpp)
int Node_Count(Kind kind);
// Standing on tiberium (it hurts soldiers)?
bool On_Tiberium(const Vector3 &p);
// The nearest walkable spot off tiberium (within 25 m).
bool Off_Tiberium(const Vector3 &p, Vector3 &out);
} // namespace NavGrid
