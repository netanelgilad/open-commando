// Walking grids for bots (see navgrid_web.h).
//
// Each cell is probed with downward rays from the top of the level; every
// upward-facing surface with enough headroom above it is a node (several per
// cell: ground, building floors, tunnels). Neighboring nodes are linked when
// a box the size of the walker can be swept from one to the other (static
// geometry only: doors open, vehicles move). Paths are A* over the links,
// then shortened to the farthest waypoints passable in a straight line.
// Two grids: soldiers (1 m cells, doors linked through their centers) and
// vehicles (2 m cells, a tank-sized box, no doors).
#include "navgrid_web.h"
#include "wwprofile.h"

#include "combat.h"
#include "gamedata.h"
#include "physcoltest.h"
#include "pscene.h"
#include "aabox.h"
#include "lineseg.h"
#include "castres.h"
#include "doors.h"
#include "obbox.h"
#include "w3d_file.h"

#include <emscripten.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <string>

namespace NavGrid {
namespace {

const int MAX_LAYERS = 6;
const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

struct Node {
  Vector3 pos;
  int links[10];       // 8 neighbors (DX/DY order), then door links
  bool hazard = false; // tiberium: hurts soldiers
};

bool Is_Tiberium(uint32 surface) {
  return surface == SURFACE_TYPE_TIBERIUM_FIELD || surface == SURFACE_TYPE_TIBERIUM_WATER ||
         surface == SURFACE_TYPE_TIBERIUM_WATER_PERMEABLE || surface == SURFACE_TYPE_UNDERWATER_TIBERIUM_DIRT;
}

// Doors are static (animated) objects: the floor under them is probed with
// them out of the way; for soldiers, links don't pass them and each door
// gets a node of its own at its center, linked to the floor in front of and
// behind it (its two trigger zones), so paths go through doors in the middle.
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

struct Grid {
  const char *name;
  float CELL;
  float HEADROOM; // free height needed above a floor
  float MAX_RISE; // height change between neighboring nodes
  float STEP;     // obstacles lower than this are stepped (driven) over
  float HALF;     // half the width of the walker's box
  bool doors;     // door nodes (soldiers)
  float HAZARD;   // extra cost of a tiberium node (soldiers; vehicles don't mind)

  std::string Level; // what the grid was built for (map, scene)
  Vector3 Min, Max;  // level extents
  int NX = 0, NY = 0;
  std::vector<int> CellStart; // nodes of cell c: CellStart[c] .. CellStart[c + 1]
  std::vector<Node> Nodes;
  std::vector<float> Penalty; // extra cost of a node, where bots got stuck

  // Built a slice at a time (a big map takes seconds; the server keeps
  // running meanwhile): probe row by row, then link row by row.
  enum Phase { PHASE_NONE, PHASE_PROBE, PHASE_LINK, PHASE_READY };
  Phase Building = PHASE_NONE;
  int Row = 0, Links = 0;
  double BuildStarted = 0, BuildMs = 0;

  Grid(const char *n, float cell, float headroom, float max_rise, float step, float half, bool with_doors, float hazard)
      : name(n), CELL(cell), HEADROOM(headroom), MAX_RISE(max_rise), STEP(step), HALF(half), doors(with_doors), HAZARD(hazard) {}

  bool Ready(void) const { return Building == PHASE_READY && !Nodes.empty(); }
  int Cell_Of(float x, float y, int *ix = nullptr, int *iy = nullptr) const {
    int i = (int)floorf((x - Min.X) / CELL), j = (int)floorf((y - Min.Y) / CELL);
    if (ix)
      *ix = i;
    if (iy)
      *iy = j;
    if (i < 0 || j < 0 || i >= NX || j >= NY)
      return -1;
    return j * NX + i;
  }

  // Can the walker go straight from a to b (both on the ground)?
  bool Sweep(const Vector3 &a, const Vector3 &b) {
    const float half = (HEADROOM - STEP) * 0.5f;
    AABoxClass box(Vector3(a.X, a.Y, a.Z + STEP + half), Vector3(HALF, HALF, half - 0.05f));
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
        n.hazard = Is_Tiberium(res.SurfaceType);
        for (int &l : n.links)
          l = -1;
        out.push_back(n);
      }
      ceiling = hit;
      z = hit - 0.05f;
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

  void Start_Build(void) {
    COMBAT_SCENE->Get_Level_Extents(Min, Max);
    NX = (int)ceilf((Max.X - Min.X) / CELL);
    NY = (int)ceilf((Max.Y - Min.Y) / CELL);
    Nodes.clear();
    Penalty.clear();
    CellStart.assign(NX * NY + 1, 0);
    Building = PHASE_PROBE;
    Row = Links = 0;
    BuildStarted = emscripten_get_now();
    BuildMs = 0;
  }

  void Probe_Row(int j) {
    std::vector<Node> cell;
    for (int i = 0; i < NX; i++) {
      CellStart[j * NX + i] = (int)Nodes.size();
      cell.clear();
      Probe_Cell(Min.X + (i + 0.5f) * CELL, Min.Y + (j + 0.5f) * CELL, cell);
      Nodes.insert(Nodes.end(), cell.begin(), cell.end());
    }
  }

  void Link_Row(int j) {
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
            Links++;
          }
        }
  }

  void Build_Step(double budget_ms) {
    double t0 = emscripten_get_now();
    if (Building == PHASE_PROBE) {
      Ignore_Doors(true);
      while (Row < NY && emscripten_get_now() - t0 < budget_ms)
        Probe_Row(Row++);
      Ignore_Doors(false);
      if (Row == NY) {
        CellStart[NX * NY] = (int)Nodes.size();
        Building = PHASE_LINK;
        Row = 0;
      }
    } else if (Building == PHASE_LINK) {
      while (Row < NY && emscripten_get_now() - t0 < budget_ms)
        Link_Row(Row++);
      if (Row == NY) {
        int door_nodes = doors ? Add_Door_Links() : 0;
        Penalty.assign(Nodes.size(), 0.0f);
        Building = PHASE_READY;
        int tiberium = 0;
      for (const Node &n : Nodes)
        tiberium += n.hazard;
      fprintf(stderr, "[nav] %s, %s grid (%s): %dx%d cells, %d nodes (%d on tiberium), %d links, %d doors (%.0f ms of work over %.1f s)\n",
                The_Game()->Get_Map_Name().Peek_Buffer(), name, Level.c_str(), NX, NY, (int)Nodes.size(), tiberium, Links, door_nodes,
                BuildMs + emscripten_get_now() - t0,
                (emscripten_get_now() - BuildStarted) / 1000.0);
      }
    }
    BuildMs += emscripten_get_now() - t0;
  }

  // The node a walker at `p` stands on (or the nearest one around). Nodes
  // far above or below count as farther (a roof over the soldier's cell).
  int Nearest(const Vector3 &p) {
    // A point off the map starts from the nearest edge cell; one that isn't a
    // number has no nearest node. (An infinite coordinate made the cell index
    // INT_MAX and `ci + r` overflow: the server hung here, in a bot's "go".)
    if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z) || NX <= 0 || NY <= 0)
      return -1;
    float fi = floorf((p.X - Min.X) / CELL), fj = floorf((p.Y - Min.Y) / CELL);
    int ci = (int)std::min(std::max(fi, 0.0f), (float)(NX - 1));
    int cj = (int)std::min(std::max(fj, 0.0f), (float)(NY - 1));
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
      if ((HAZARD > 0 && Nodes[route[k]].hazard) || fabsf(p.Z - (pa.Z + (pb.Z - pa.Z) * t)) > STEP + 0.05f)
        return false;
    }
    // Not across tiberium: the ground under the whole line, a cell at a time.
    if (HAZARD > 0) {
      Vector3 d = pb - pa;
      int steps = (int)(d.Length() / CELL);
      for (int k = 1; k < steps; k++) {
        Vector3 q = pa + d * ((float)k / steps);
        int c = Cell_Of(q.X, q.Y);
        if (c < 0)
          continue;
        for (int n = CellStart[c]; n < CellStart[c + 1]; n++)
          if (Nodes[n].hazard && fabsf(Nodes[n].pos.Z - q.Z) < 1.5f)
            return false;
      }
    }
    return Sweep(pa, pb);
  }

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
        float c = cost[n] + (Nodes[l].pos - Nodes[n].pos).Length() + Penalty[l] + (Nodes[l].hazard ? HAZARD : 0.0f);
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
  bool Find_Path(const Vector3 &from, const Vector3 &to, std::vector<Vector3> &path) {
    path.clear();
    if (!Ready())
      return false; // see Ensure
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

  // The grid as JSON for game records (a map under paths and heat maps):
  // origin, cell size and rows of cells as runs, "<char><count>", where '#'
  // has no floor, '.' walkable, '~' tiberium, '!' where bots got stuck.
  bool Map_Json(std::string &j) {
    if (Building != PHASE_READY)
      return false;
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"minX\":%.1f,\"minY\":%.1f,\"cell\":%.2f,\"nx\":%d,\"ny\":%d,\"rows\":[", Min.X, Min.Y, CELL, NX, NY);
    j += buf;
    for (int y = 0; y < NY; y++) {
      j += y ? ",\"" : "\"";
      char run = 0;
      int count = 0;
      for (int x = 0; x <= NX; x++) {
        char c = 0;
        if (x < NX) {
          c = '#';
          for (int n = CellStart[y * NX + x]; n < CellStart[y * NX + x + 1]; n++)
            c = Penalty[n] > 0 ? '!' : Nodes[n].hazard ? (c == '.' ? '.' : '~') : '.';
        }
        if (c == run) {
          count++;
          continue;
        }
        if (count) {
          j += run;
          j += std::to_string(count);
        }
        run = c;
        count = 1;
      }
      j += '"';
    }
    j += "]}";
    return true;
  }

  bool Dump(const char *file) {
    if (Building != PHASE_READY)
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


  // Keep up with the loaded level (`key`: what is loaded); one slice of work.
  void Step(const std::string &key, double budget_ms) {
    if (Level != key) {
      Level = key;
      Start_Build();
    }
    if (Building != PHASE_READY)
      Build_Step(budget_ms);
  }
};

// Soldiers walk around tiberium: a meter of field costs like 9 m of path.
Grid Soldiers("soldier", 1.0f, 1.9f, 0.7f, 0.55f, 0.3f, true, 8.0f);
Grid Vehicles("vehicle", 2.0f, 3.0f, 1.2f, 0.8f, 1.5f, false, 0.0f);
Grid &For(Kind k) { return k == VEHICLE ? Vehicles : Soldiers; }

} // namespace

bool Ensure(void) {
  if (!COMBAT_SCENE || !PTheGameData || CombatManager::Is_Loading_Level())
    return false;
  // What is actually loaded (the map name changes before the level does: the
  // score screen still runs on the old one).
  Vector3 lo, hi;
  COMBAT_SCENE->Get_Level_Extents(lo, hi);
  int statics = 0;
  RefPhysListIterator it = COMBAT_SCENE->Get_Static_Object_Iterator();
  for (it.First(); !it.Is_Done(); it.Next())
    statics++;
  char key[256];
  snprintf(key, sizeof(key), "%d statics, %.1f %.1f to %.1f %.1f", statics, lo.X, lo.Y, hi.X, hi.Y);
  // 3 ms a frame (the server keeps its frame rate on a small CPU): soldiers
  // first, then vehicles.
  Soldiers.Step(key, 3.0);
  if (Soldiers.Ready())
    Vehicles.Step(key, 3.0);
  return Soldiers.Ready();
}

bool On_Tiberium(const Vector3 &p) {
  if (!Soldiers.Ready())
    return false;
  int n = Soldiers.Nearest(p);
  return n >= 0 && Soldiers.Nodes[n].hazard && (Soldiers.Nodes[n].pos - p).Length() < 2.0f;
}

bool Off_Tiberium(const Vector3 &p, Vector3 &out) {
  Grid &g = Soldiers;
  if (!g.Ready())
    return false;
  int ci, cj;
  g.Cell_Of(p.X, p.Y, &ci, &cj);
  for (int r = 1; r <= 25; r++)
    for (int j = cj - r; j <= cj + r; j++)
      for (int i = ci - r; i <= ci + r; i++) {
        if (i < 0 || j < 0 || i >= g.NX || j >= g.NY || (abs(i - ci) != r && abs(j - cj) != r))
          continue;
        for (int n = g.CellStart[j * g.NX + i]; n < g.CellStart[j * g.NX + i + 1]; n++)
          if (!g.Nodes[n].hazard && fabsf(g.Nodes[n].pos.Z - p.Z) < 2.0f) {
            out = g.Nodes[n].pos;
            return true;
          }
      }
  return false;
}

int Node_Count(Kind k) { return For(k).Ready() ? (int)For(k).Nodes.size() : 0; }
bool Find_Path(Kind k, const Vector3 &from, const Vector3 &to, std::vector<Vector3> &path) {
  WWPROFILE("Find Path");
  if (k == VEHICLE && !Vehicles.Ready())
    k = SOLDIER; // not built yet: the soldiers' grid will do
  return For(k).Find_Path(from, to, path);
}
void Block(Kind k, const Vector3 &p, float radius) { For(k).Block(p, radius); }
bool Dump(Kind k, const char *file) { return For(k).Dump(file); }
bool Map_Json(Kind k, std::string &out) { return For(k).Map_Json(out); }

} // namespace NavGrid
