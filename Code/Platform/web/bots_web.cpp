// Bots for online matches (server side; tools/server, services/match).
//
// A bot is an engine player with a negative id (cPlayer, like an AI player:
// the netcode already skips sends to ids < 0): cGod gives it a soldier and
// respawns it like anyone, the soldier runs the engine's own combat AI
// (SoldierObserverClass: sees, takes cover, shoots), it shows on the
// scoreboard and counts for its team. On top of that a small goal loop
// keeps each bot busy: push to an enemy building, or to a point given with
// bot_order (the hook for a smarter commander later).
//
// Console (server): bot_add [gdi|nod|auto] [count], bot_remove [count|all],
// bot_list, bot_order <name|all> <attack|defend|goto x y z>.
#include "bots_web.h"

#include "consolefunction.h"
#include "god.h"
#include "player.h"
#include "playermanager.h"
#include "gameobjmanager.h"
#include "soldier.h"
#include "building.h"
#include "action.h"
#include "actionparams.h"
#include "cnetwork.h"
#include "gamedata.h"
#include "playertype.h"
#include "wwmath.h"
#include "pathfind.h"
#include "pathfindsector.h"
#include "control.h"
#include "consolemode.h"

#include <emscripten.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Console output, also on stderr (a browser host has no server console).
void Say(const char *format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  ConsoleBox.Print("%s", line);
  if (!ConsoleBox.Is_Exclusive())
    fprintf(stderr, "[bots] %s", line);
}

enum Goal { GOAL_ATTACK, GOAL_DEFEND, GOAL_GOTO };

struct Bot {
  int id;            // player id (< 0)
  std::string name;
  int team;          // PLAYERTYPE_GDI / PLAYERTYPE_NOD
  Goal goal = GOAL_ATTACK;
  Vector3 point;     // GOAL_GOTO
  double nextThink = 0;
  int target = 0;    // building being pushed (object id), 0 = pick one
  bool accepted = false; // the last order was taken by the soldier's action
  Vector3 lastPos;   // where it was at the last think (progress check)
  int stuck = 0;     // thinks in a row without progress
};

std::vector<Bot> Bots;
int NextId = -100;
const char *NAMES[] = {"Sgt. Hammer", "Cpl. Vance", "Pvt. Ortega", "Lt. Kova", "Sgt. Riggs", "Pvt. Nash",
                       "Cpl. Mercer", "Lt. Shaw", "Pvt. Duke", "Sgt. Kane", "Cpl. Ivy", "Pvt. Brock"};
int NameIndex = 0;

int Team_Size(int team) { return cPlayerManager::Tally_Team_Size(team); }

int Pick_Team(const char *arg) {
  if (arg && strstr(arg, "gdi"))
    return PLAYERTYPE_GDI;
  if (arg && strstr(arg, "nod"))
    return PLAYERTYPE_NOD;
  return Team_Size(PLAYERTYPE_GDI) <= Team_Size(PLAYERTYPE_NOD) ? PLAYERTYPE_GDI : PLAYERTYPE_NOD;
}

bool Add(int team) {
  if (!cNetwork::I_Am_Server() || !PTheGameData)
    return false;
  if (cPlayerManager::Count() >= The_Game()->Get_Max_Players())
    return false;
  Bot b;
  b.id = NextId--;
  b.name = std::string("[BOT] ") + NAMES[NameIndex++ % (sizeof(NAMES) / sizeof(NAMES[0]))];
  b.team = team;
  WideStringClass wide;
  wide.Convert_From(b.name.c_str());
  cPlayer *p = cGod::Create_Player(b.id, wide, team, 0, false);
  if (!p)
    return false;
  p->Set_Player_Type(team); // Create_Player may balance teams on its own
  b.team = p->Get_Player_Type();
  Bots.push_back(b);
  Say("%s joined %s\n", b.name.c_str(), b.team == PLAYERTYPE_GDI ? "GDI" : "Nod");
  return true;
}

void Remove(size_t index) {
  Bot b = Bots[index];
  Bots.erase(Bots.begin() + index);
  cNetwork::Delete_Player_Objects(b.id); // the soldier
  if (cPlayer *p = cPlayerManager::Find_Player(b.id)) {
    p->Set_Is_Active(false);
    p->Set_Delete_Pending();
  }
  Say("%s left\n", b.name.c_str());
}

// A point on the pathfinding grid near `around` (ring search at a few
// heights): building origins and points derived from them are often under
// the terrain or inside geometry, where no path can end. Falls back to `around`.
Vector3 Walkable_Near(const Vector3 &around, const Vector3 &toward) {
  PathfindClass *pf = PathfindClass::Get_Instance();
  if (!pf || !pf->Does_Pathfind_Data_Exist())
    return around;
  Vector3 dir = toward - around;
  dir.Z = 0;
  float base = dir.Length() > 0.1f ? atan2f(dir.Y, dir.X) : 0.0f;
  for (float radius = 12.0f; radius <= 30.0f; radius += 6.0f) {
    for (int k = 0; k < 12; k++) {
      // Try the side facing `toward` first, then fan out.
      float a = base + ((k & 1) ? 1 : -1) * ((k + 1) / 2) * (6.2832f / 12.0f);
      for (float dz = -4.0f; dz <= 10.0f; dz += 2.0f) {
        Vector3 p(around.X + radius * cosf(a), around.Y + radius * sinf(a), around.Z + dz);
        if (PathfindSectorClass *sector = pf->Find_Sector(p, 1.0f)) {
          const AABoxClass &box = sector->Get_Bounding_Box();
          return Vector3(p.X, p.Y, box.Center.Z - box.Extent.Z + 0.5f);
        }
      }
    }
  }
  return around;
}

// Nearest standing enemy building to `from` (or our own, to defend).
BuildingGameObj *Building_For(const Bot &b, const Vector3 &from, bool own) {
  BuildingGameObj *best = nullptr;
  float best_d = 1e30f;
  for (SLNode<BuildingGameObj> *n = GameObjManager::Get_Building_Game_Obj_List()->Head(); n; n = n->Next()) {
    BuildingGameObj *bld = n->Data();
    if (bld->Is_Destroyed())
      continue;
    int t = bld->Get_Player_Type();
    if (t != PLAYERTYPE_GDI && t != PLAYERTYPE_NOD)
      continue;
    if (own != (t == b.team))
      continue;
    Vector3 pos;
    bld->Get_Position(&pos);
    float d = (pos - from).Length();
    if (d < best_d) {
      best_d = d;
      best = bld;
    }
  }
  return best;
}

void Think_Bot(Bot &b, double now) {
  if (now < b.nextThink)
    return;
  b.nextThink = now + 4000 + (rand() % 2000); // every 4-6 s, not in lockstep
  SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
  if (!s || !s->Get_Action())
    return; // dead: cGod respawns it
  Vector3 me;
  s->Get_Position(&me);
  ActionParamsStruct params;
  params.Set_Basic((long)0, 60, 7000 + (-b.id));
  if (b.goal == GOAL_GOTO) {
    params.Set_Movement(b.point, 1.0f, 3.0f);
    b.accepted = s->Get_Action()->Goto(params);
    return;
  }
  BuildingGameObj *bld = Building_For(b, me, b.goal == GOAL_DEFEND);
  if (!bld)
    return;
  Vector3 pos;
  bld->Get_Position(&pos);
  if (b.goal == GOAL_DEFEND) {
    // Patrol near our building: a point a little off it, changing each time.
    float a = (float)(rand() % 628) / 100.0f;
    pos += Vector3(8.0f * cosf(a), 8.0f * sinf(a), 0);
    params.Set_Movement(pos, 0.8f, 3.0f);
    b.accepted = s->Get_Action()->Goto(params);
    return;
  }
  // Attack: run to walkable ground by the building (the soldier AI engages
  // anyone it sees on the way). If the planner still can't get there, walk
  // straight at it in hops and sidestep when blocked.
  pos = Walkable_Near(pos, me);
  b.point = pos;
  float moved = (me - b.lastPos).Length();
  b.lastPos = me;
  b.stuck = moved < 2.0f ? b.stuck + 1 : 0;
  Vector3 hop = pos;
  if (b.stuck >= 2) {
    Vector3 d = pos - me;
    d.Z = 0;
    float dl = d.Length();
    if (dl > 40.0f)
      hop = me + d * (40.0f / dl);
    if (b.stuck >= 4) { // blocked: go around
      Vector3 sidestep(-d.Y, d.X, 0);
      sidestep.Normalize();
      hop = me + sidestep * ((b.stuck & 1) ? 15.0f : -15.0f);
    }
    params.MovePathfind = false;
  }
  params.Set_Movement(hop, 1.0f, 5.0f);
  b.accepted = s->Get_Action()->Goto(params);
}

Bot *Find(const char *name) {
  for (Bot &b : Bots)
    if (strstr(b.name.c_str(), name))
      return &b;
  return nullptr;
}

class BotAddConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_add"; }
  const char *Get_Help(void) override { return "BOT_ADD [gdi|nod|auto] [count] - add AI players (server only)."; }
  void Activate(const char *input) override {
    int count = 1;
    if (input) {
      const char *digits = input;
      while (*digits && (*digits < '0' || *digits > '9'))
        digits++;
      if (*digits)
        count = atoi(digits);
    }
    for (int i = 0; i < count && i < 32; i++)
      if (!Add(Pick_Team(input)))
        break;
  }
};

class BotRemoveConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_remove"; }
  const char *Get_Help(void) override { return "BOT_REMOVE [count|all|gdi|nod] - remove AI players (server only)."; }
  void Activate(const char *input) override {
    bool all = input && strstr(input, "all");
    int team = input && strstr(input, "gdi") ? PLAYERTYPE_GDI : input && strstr(input, "nod") ? PLAYERTYPE_NOD : -1;
    int count = input && atoi(input) > 0 ? atoi(input) : 1;
    for (int i = (int)Bots.size() - 1; i >= 0 && (all || count > 0); i--) {
      if (team >= 0 && Bots[i].team != team)
        continue;
      Remove(i);
      count--;
    }
  }
};

class BotListConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_list"; }
  const char *Get_Help(void) override { return "BOT_LIST - list AI players, their goals and positions."; }
  void Activate(const char *) override {
    static const char *GOALS[] = {"attack", "defend", "goto"};
    for (Bot &b : Bots) {
      SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
      Vector3 p(0, 0, 0);
      if (s)
        s->Get_Position(&p);
      ActionClass *a = s ? s->Get_Action() : nullptr;
      Vector3 vel(0, 0, 0);
      float fwd = 0, turn = 0;
      if (s) {
        s->Get_Velocity(vel);
        fwd = s->Get_Control().Get_Analog(ControlClass::ANALOG_MOVE_FORWARD);
        turn = s->Get_Control().Get_Analog(ControlClass::ANALOG_TURN_LEFT);
      }
      PathfindClass *pf = PathfindClass::Get_Instance();
      bool here = pf && s && pf->Find_Sector(p, 1.0f) != nullptr, there = pf && pf->Find_Sector(b.point, 1.0f) != nullptr;
      Say("  %s: control fwd %.2f turn %.2f, speed %.2f, pathfind sector here %d at target %d\n", b.name.c_str(), fwd, turn,
          vel.Length(), (int)here, (int)there);
      Say("bot %s %s %s %s at %.1f %.1f %.1f -> %.1f %.1f, order %s, acting %d active %d busy %d\n", b.name.c_str(),
                       b.team == PLAYERTYPE_GDI ? "GDI" : "Nod", GOALS[b.goal], s ? "alive" : "dead", p.X, p.Y, p.Z, b.point.X,
                       b.point.Y, b.accepted ? "taken" : "refused", a ? (int)a->Is_Acting() : -1, a ? (int)a->Is_Active() : -1,
                       a ? (int)a->Is_Busy() : -1);
    }
    PathfindClass *pf = PathfindClass::Get_Instance();
    Say("%d bots, pathfind data %s\n", (int)Bots.size(), pf && pf->Does_Pathfind_Data_Exist() ? "yes" : "no");
  }
};

class BotOrderConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_order"; }
  const char *Get_Help(void) override { return "BOT_ORDER <name|all> <attack|defend|goto x y z> - give AI players a goal."; }
  void Activate(const char *input) override {
    if (!input)
      return;
    char who[64] = {0}, what[16] = {0};
    float x = 0, y = 0, z = 0;
    int n = sscanf(input, "%63s %15s %f %f %f", who, what, &x, &y, &z);
    if (n < 2)
      return;
    Goal g = !strcmp(what, "defend") ? GOAL_DEFEND : !strcmp(what, "goto") ? GOAL_GOTO : GOAL_ATTACK;
    for (Bot &b : Bots) {
      if (strcmp(who, "all") && !strstr(b.name.c_str(), who))
        continue;
      b.goal = g;
      b.point = Vector3(x, y, z);
      b.nextThink = 0;
    }
  }
};

} // namespace

void Rebel_Bots_Add_Commands(DynamicVectorClass<ConsoleFunctionClass *> &list) {
  list.Add(new BotAddConsoleFunctionClass());
  list.Add(new BotRemoveConsoleFunctionClass());
  list.Add(new BotListConsoleFunctionClass());
  list.Add(new BotOrderConsoleFunctionClass());
}

int Rebel_Bots_Count(void) { return (int)Bots.size(); }

void Rebel_Bots_Think(void) {
  if (Bots.empty() || !cNetwork::I_Am_Server())
    return;
  // Forget bots whose player is gone (a map change resets players).
  for (int i = (int)Bots.size() - 1; i >= 0; i--)
    if (!cPlayerManager::Find_Player(Bots[i].id))
      Bots.erase(Bots.begin() + i);
  double now = emscripten_get_now();
  for (Bot &b : Bots)
    Think_Bot(b, now);
}
