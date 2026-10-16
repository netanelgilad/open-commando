// Walking grid for bots (see navgrid_web.h).
//
// Each 1 m cell is probed with downward rays from the top of the level; every
// upward-facing surface with a soldier's headroom above it is a node (several
// per cell: ground, building floors, tunnels). Neighboring nodes are linked
// when a soldier-sized box can be swept from one to the other (static
// geometry only: doors open, vehicles move). Paths are A* over the links,
// then shortened to the farthest waypoints walkable in a straight line.
#include "navgrid_web.h"

#include "combat.h"
#include "gamedata.h"
#include "physcoltest.h"
#include "pscene.h"
#include "aabox.h"
#include "lineseg.h"
#include "castres.h"
#include "doors.h"
#include "obbox.h"

#include <emscripten.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <string>

namespace NavGrid {
namespace {

const float CELL = 1.0f;
const float HEADROOM = 1.9f;    // free height a soldier needs above a floor
const float MAX_RISE = 0.7f;    // height change between neighboring nodes
const float STEP = 0.55f;       // obstacles lower than this are stepped over
const int MAX_LAYERS = 6;
const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

struct Node {
  Vector3 pos;
  int links[10]; // 8 neighbors (DX/DY order), then door links
};

std::string Level;          // map the grid was built for
Vector3 Min, Max;           // level extents
int NX = 0, NY = 0;
std::vector<int> CellStart; // nodes of cell c: CellStart[c] .. CellStart[c + 1]
std::vector<Node> Nodes;
std::vector<float> Penalty; // extra cost of a node, where bots got stuck

int Cell_Of(float x, float y, int *ix = nullptr, int *iy = nullptr) {
  int i = (int)floorf((x - Min.X) / CELL), j = (int)floorf((y - Min.Y) / CELL);
  if (ix)
    *ix = i;
  if (iy)
    *iy = j;
  if (i < 0 || j < 0 || i >= NX || j >= NY)
    return -1;
  return j * NX + i;
}

// Can a soldier walk straight from a to b (both on the ground)?
bool Sweep(const Vector3 &a, const Vector3 &b) {
  const float half = (HEADROOM - STEP) * 0.5f;
  AABoxClass box(Vector3(a.X, a.Y, a.Z + STEP + half), Vector3(0.3f, 0.3f, half - 0.05f));
  CastResultStruct res;
  PhysAABoxCollisionTestClass test(box, b - a, &res, DEFAULT_COLLISION_GROUP, COLLISION_TYPE_PHYSICAL);
  test.CheckDynamicObjs = false;
  COMBAT_SCENE->Cast_AABox(test);
  return !res.StartBad && res.Fraction >= 1.0f;
}

void Probe_Cell(float x, float y, std::vector<Node> &out) {
  float ceiling = 1e30f, z = Max.Z + 1.0f;
  for (int layer = 0; layer < MAX_LAYERS * 2 && z > Min.Z; layer++) {
    LineSegClass ray(Vector3(x, y, z), Vector3(x, y, Min.Z - 1.0f));
    CastResultStruct res;
    PhysRayCollisionTestClass test(ray, &res, DEFAULT_COLLISION_GROUP, COLLISION_TYPE_PHYSICAL);
    test.CheckDynamicObjs = false;
    COMBAT_SCENE->Cast_Ray(test);
    if (res.StartBad) {
      z -= 0.25f;
      continue;
    }
    if (res.Fraction >= 1.0f)
      break;
    float hit = z + (Min.Z - 1.0f - z) * res.Fraction;
    if (res.Normal.Z > 0.7f && ceiling - hit >= HEADROOM && (int)out.size() < MAX_LAYERS) {
      Node n;
      n.pos.Set(x, y, hit);
      for (int &l : n.links)
        l = -1;
      out.push_back(n);
    }
    ceiling = hit;
    z = hit - 0.05f;
  }
}

int Nearest(const Vector3 &p);

// Doors are static (animated) objects: the floor under them is probed with
// them out of the way, links don't pass them, and each door gets a node of
// its own at its center, linked to the floor in front of and behind it (its
// two trigger zones), so paths go through doors in the middle.
void Ignore_Doors(bool ignore) {
  RefPhysListIterator it = COMBAT_SCENE->Get_Static_Object_Iterator();
  for (it.First(); !it.Is_Done(); it.Next())
    if (it.Peek_Obj()->As_DoorPhysClass()) {
      if (ignore)
        it.Peek_Obj()->Inc_Ignore_Counter();
      else
        it.Peek_Obj()->Dec_Ignore_Counter();
    }
}

void Link(int a, int b) {
  for (int k = 8; k < 10; k++)
    if (Nodes[a].links[k] < 0 || Nodes[a].links[k] == b) {
      Nodes[a].links[k] = b;
      return;
    }
}

int Add_Door_Links(void) {
  int doors = 0;
  RefPhysListIterator it = COMBAT_SCENE->Get_Static_Object_Iterator();
  for (it.First(); !it.Is_Done(); it.Next()) {
    DoorPhysClass *door = it.Peek_Obj()->As_DoorPhysClass();
    if (!door)
      continue;
    OBBoxClass z1, z2;
    OBBoxClass::Transform(door->Get_Transform(), door->Get_DoorPhysDef()->Get_Trigger_Zone1(), &z1);
    OBBoxClass::Transform(door->Get_Transform(), door->Get_DoorPhysDef()->Get_Trigger_Zone2(), &z2);
    int a = Nearest(z1.Center), b = Nearest(z2.Center);
    if (a < 0 || b < 0 || a == b || (Nodes[a].pos - z1.Center).Length() > 4.0f || (Nodes[b].pos - z2.Center).Length() > 4.0f)
      continue;
    Node mid;
    mid.pos = (Nodes[a].pos + Nodes[b].pos) * 0.5f;
    for (int &l : mid.links)
      l = -1;
    int m = (int)Nodes.size();
    Nodes.push_back(mid);
    Link(m, a);
    Link(m, b);
    Link(a, m);
    Link(b, m);
    doors++;
  }
  return doors;
}

void Build(void) {
  double t0 = emscripten_get_now();
  Ignore_Doors(true);
  COMBAT_SCENE->Get_Level_Extents(Min, Max);
  NX = (int)ceilf((Max.X - Min.X) / CELL);
  NY = (int)ceilf((Max.Y - Min.Y) / CELL);
  Nodes.clear();
  CellStart.assign(NX * NY + 1, 0);
  std::vector<Node> cell;
  for (int j = 0; j < NY; j++)
    for (int i = 0; i < NX; i++) {
      CellStart[j * NX + i] = (int)Nodes.size();
      cell.clear();
      Probe_Cell(Min.X + (i + 0.5f) * CELL, Min.Y + (j + 0.5f) * CELL, cell);
      Nodes.insert(Nodes.end(), cell.begin(), cell.end());
    }
  CellStart[NX * NY] = (int)Nodes.size();
  Ignore_Doors(false);
  double t1 = emscripten_get_now();
  int links = 0;
  for (int j = 0; j < NY; j++)
    for (int i = 0; i < NX; i++)
      for (int n = CellStart[j * NX + i]; n < CellStart[j * NX + i + 1]; n++)
        for (int d = 0; d < 8; d++) {
          int ni = i + DX[d], nj = j + DY[d];
          if (ni < 0 || nj < 0 || ni >= NX || nj >= NY)
            continue;
          int c = nj * NX + ni, best = -1;
          float best_dz = MAX_RISE;
          for (int m = CellStart[c]; m < CellStart[c + 1]; m++) {
            float dz = fabsf(Nodes[m].pos.Z - Nodes[n].pos.Z);
            if (dz <= best_dz) {
              best_dz = dz;
              best = m;
            }
          }
          if (best >= 0 && Sweep(Nodes[n].pos, Nodes[best].pos)) {
            Nodes[n].links[d] = best;
            links++;
          }
        }
  int doors = Add_Door_Links();
  Penalty.assign(Nodes.size(), 0.0f);
  fprintf(stderr, "[nav] %s: %dx%d cells from %.1f %.1f, %d nodes, %d links, %d doors (probe %.0f ms, links %.0f ms)\n", Level.c_str(), NX, NY, Min.X, Min.Y,
          (int)Nodes.size(), links, doors, t1 - t0, emscripten_get_now() - t1);
}

// The node a soldier at `p` stands on (or the nearest one around). Nodes
// far above or below count as farther (a roof over the soldier's cell).
int Nearest(const Vector3 &p) {
  int ci, cj;
  Cell_Of(p.X, p.Y, &ci, &cj);
  int best = -1;
  float best_d = 1e30f;
  for (int r = 0; r <= 10; r++) {
    float ring = (r - 1) * CELL;
    if (best >= 0 && ring > 0 && ring * ring > best_d)
      break; // nothing in this ring or beyond can be nearer
    for (int j = cj - r; j <= cj + r; j++)
      for (int i = ci - r; i <= ci + r; i++) {
        if (i < 0 || j < 0 || i >= NX || j >= NY || (abs(i - ci) != r && abs(j - cj) != r))
          continue;
        for (int n = CellStart[j * NX + i]; n < CellStart[j * NX + i + 1]; n++) {
          Vector3 d = Nodes[n].pos - p;
          float score = d.X * d.X + d.Y * d.Y + 16.0f * d.Z * d.Z;
          if (score < best_d) {
            best_d = score;
            best = n;
          }
        }
      }
  }
  return best;
}

// Straight walkable: a clear sweep, and the ground between follows the line
// (no hole or hump the sweep can't see).
bool Straight(int a, int b, const std::vector<int> &route, size_t ia, size_t ib) {
  const Vector3 &pa = Nodes[a].pos, &pb = Nodes[b].pos;
  for (size_t k = ia + 1; k < ib; k++) {
    const Vector3 &p = Nodes[route[k]].pos;
    float t = (float)(k - ia) / (float)(ib - ia);
    if (fabsf(p.Z - (pa.Z + (pb.Z - pa.Z) * t)) > 0.6f)
      return false;
  }
  return Sweep(pa, pb);
}

} // namespace

bool Ensure(void) {
  if (!COMBAT_SCENE || !PTheGameData)
    return false;
  std::string level = The_Game()->Get_Map_Name().Peek_Buffer();
  if (level != Level || Nodes.empty()) {
    Level = level;
    Build();
  }
  return !Nodes.empty();
}

int Node_Count(void) { return (int)Nodes.size(); }

namespace {
// A* from start toward goal; the route ends at the goal or, if it can't be
// reached, at the reachable node nearest to it. Returns that node's distance.
float Search(int start, int goal, std::vector<int> &route) {
  const Vector3 &gp = Nodes[goal].pos;
  std::vector<float> cost(Nodes.size(), 1e30f);
  std::vector<int> came(Nodes.size(), -1);
  typedef std::pair<float, int> Entry;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
  cost[start] = 0;
  open.push(Entry((Nodes[start].pos - gp).Length(), start));
  int closest = start;
  float closest_h = (Nodes[start].pos - gp).Length();
  while (!open.empty()) {
    Entry e = open.top();
    open.pop();
    int n = e.second;
    float h = (Nodes[n].pos - gp).Length();
    if (e.first > cost[n] + h + 0.01f)
      continue; // stale entry
    if (h < closest_h) {
      closest_h = h;
      closest = n;
    }
    if (n == goal)
      break;
    for (int l : Nodes[n].links) {
      if (l < 0)
        continue;
      float c = cost[n] + (Nodes[l].pos - Nodes[n].pos).Length() + Penalty[l];
      if (c < cost[l]) {
        cost[l] = c;
        came[l] = n;
        open.push(Entry(c + (Nodes[l].pos - gp).Length(), l));
      }
    }
  }
  route.clear();
  for (int n = closest; n >= 0; n = came[n])
    route.push_back(n);
  std::reverse(route.begin(), route.end());
  return closest_h;
}
} // namespace

bool Find_Path(const Vector3 &from, const Vector3 &to, std::vector<Vector3> &path) {
  path.clear();
  if (!Ensure())
    return false;
  int start = Nearest(from), goal = Nearest(to);
  if (start < 0 || goal < 0)
    return false;
  std::vector<int> route;
  Search(start, goal, route);
  int first = -1;
  if (route.size() < 2 && (Nodes[start].pos - Nodes[goal].pos).Length() > 30.0f) {
    // Standing on a scrap of floor that leads nowhere (against a wall, on a
    // ledge): start from a node around instead.
    int ci, cj;
    Cell_Of(from.X, from.Y, &ci, &cj);
    for (int r = 1; r <= 3 && route.size() < 2; r++)
      for (int j = cj - r; j <= cj + r && route.size() < 2; j++)
        for (int i = ci - r; i <= ci + r && route.size() < 2; i++) {
          if (i < 0 || j < 0 || i >= NX || j >= NY || (abs(i - ci) != r && abs(j - cj) != r))
            continue;
          for (int n = CellStart[j * NX + i]; n < CellStart[j * NX + i + 1] && route.size() < 2; n++)
            if (fabsf(Nodes[n].pos.Z - from.Z) < 1.5f) {
              Search(n, goal, route);
              first = n; // walk onto it first
            }
        }
  }
  if (first >= 0 && route.size() >= 2)
    path.push_back(Nodes[first].pos);
  // Keep the farthest waypoint reachable in a straight line, up to 24 nodes on.
  size_t i = 0;
  while (i + 1 < route.size()) {
    size_t next = i + 1;
    for (size_t k = std::min(route.size() - 1, i + 24); k > i + 1; k--)
      if (Straight(route[i], route[k], route, i, k)) {
        next = k;
        break;
      }
    path.push_back(Nodes[route[next]].pos);
    i = next;
  }
  return !path.empty();
}

void Block(const Vector3 &p, float radius) {
  if (Nodes.empty())
    return;
  int ci, cj, r = (int)ceilf(radius / CELL);
  Cell_Of(p.X, p.Y, &ci, &cj);
  for (int j = cj - r; j <= cj + r; j++)
    for (int i = ci - r; i <= ci + r; i++) {
      if (i < 0 || j < 0 || i >= NX || j >= NY)
        continue;
      for (int n = CellStart[j * NX + i]; n < CellStart[j * NX + i + 1]; n++)
        if ((Nodes[n].pos - p).Length() <= radius + 1.0f)
          Penalty[n] = std::min(Penalty[n] + 20.0f, 100.0f);
    }
}

bool Dump(const char *file) {
  if (!Ensure())
    return false;
  FILE *f = fopen(file, "wb");
  if (!f)
    return false;
  fprintf(f, "P5\n%d %d\n255\n", NX, NY);
  for (int j = NY - 1; j >= 0; j--)
    for (int i = 0; i < NX; i++) {
      int best = 0;
      for (int n = CellStart[j * NX + i]; n < CellStart[j * NX + i + 1]; n++) {
        int links = 0;
        for (int l : Nodes[n].links)
          links += l >= 0;
        best = std::max(best, Penalty[n] > 0 ? 20 : 40 + links * 26);
      }
      fputc(best, f);
    }
  fclose(f);
  return true;
}

} // namespace NavGrid
