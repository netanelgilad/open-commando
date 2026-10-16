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
#include "navgrid_web.h"
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
  bool accepted = false; // the last order was taken by the soldier's action
  Vector3 dest;      // where the current path leads
  std::vector<Vector3> path; // waypoints (navgrid_web.cpp)
  size_t wp = 0;     // the one being walked to
  double lastGoto = 0, nextStep = 0;
  Vector3 progressPos; // last place it moved on from, and when
  double progressAt = 0;
  int stuck = 0;     // replans for being blocked
  int stuckRow = 0;  // ... in a row, without moving on
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

void Plan(Bot &b, const Vector3 &me, const Vector3 &dest) {
  b.dest = dest;
  NavGrid::Find_Path(me, dest, b.path);
  b.wp = 0;
  b.lastGoto = 0;
}

// Goals, every 4-6 s: where to go next.
void Think_Bot(Bot &b, SoldierGameObj *s, const Vector3 &me, double now) {
  if (now < b.nextThink)
    return;
  b.nextThink = now + 4000 + (rand() % 2000); // not in lockstep
  Vector3 dest = b.point;
  BuildingGameObj *bld = nullptr;
  if (b.goal != GOAL_GOTO) {
    bld = Building_For(b, me, b.goal == GOAL_DEFEND);
    if (!bld)
      return;
    bld->Get_Position(&dest);
    if (b.goal == GOAL_DEFEND) { // patrol around our building
      float a = (float)(rand() % 628) / 100.0f;
      dest += Vector3(14.0f * cosf(a), 14.0f * sinf(a), 0);
    }
  }
  bool arrived = b.wp >= b.path.size();
  if (arrived && b.goal == GOAL_ATTACK && (me - dest).Length() < 30.0f) {
    // There: shoot the building (the soldier's own AI turns on anyone who
    // shows up).
    ActionParamsStruct params;
    params.Set_Basic((long)0, 60, 7000 + (-b.id));
    params.Set_Attack(bld, 40.0f, 1.0f, true);
    b.accepted = s->Get_Action()->Attack(params);
    return;
  }
  if (arrived || (dest - b.dest).Length() > 8.0f || b.goal == GOAL_DEFEND)
    Plan(b, me, dest);
}

// Walking, every 1/4 s: the next waypoint of the path, straight at it (the
// engine's planner can't plan on these maps); replan when stuck.
void Step_Bot(Bot &b, SoldierGameObj *s, const Vector3 &me, double now) {
  if (b.wp >= b.path.size())
    return;
  size_t before = b.wp;
  while (b.wp < b.path.size()) {
    Vector3 d = b.path[b.wp] - me;
    if (d.X * d.X + d.Y * d.Y > 1.5f * 1.5f || fabsf(d.Z) > 2.5f)
      break;
    b.wp++;
  }
  if (b.wp >= b.path.size())
    return;
  if ((me - b.progressPos).Length() > 1.5f) {
    b.progressPos = me;
    b.progressAt = now;
    b.stuckRow = 0;
  } else if (now - b.progressAt > 4000) {
    // Blocked (another soldier, a vehicle, something the grid missed): jump,
    // avoid the spot, plan again; still stuck after that, wander off a few
    // meters first (the goal loop picks the goal up again).
    b.progressAt = now;
    b.stuck++;
    b.stuckRow++;
    s->Set_Boolean_Control(ControlClass::BOOLEAN_JUMP);
    Vector3 ahead = b.path[b.wp] - me;
    ahead.Z = 0;
    if (ahead.Length() > 0.1f)
      NavGrid::Block(me + ahead * (1.0f / ahead.Length()), 1.0f);
    if (b.stuckRow >= 3) {
      float a = (float)(rand() % 628) / 100.0f;
      Plan(b, me, me + Vector3(6.0f * cosf(a), 6.0f * sinf(a), 0));
    } else {
      Plan(b, me, b.dest);
    }
    if (b.path.empty())
      return;
  }
  if (b.wp == before && b.lastGoto && now - b.lastGoto < 3000)
    return;
  ActionParamsStruct params;
  params.Set_Basic((long)0, 60, 7000 + (-b.id));
  params.Set_Movement(b.path[b.wp], 1.0f, 1.0f);
  params.MovePathfind = false;
  b.accepted = s->Get_Action()->Goto(params);
  b.lastGoto = now;
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
      Vector3 p(0, 0, 0), vel(0, 0, 0);
      if (s) {
        s->Get_Position(&p);
        s->Get_Velocity(vel);
      }
      Say("bot %s %s %s %s at %.1f %.1f %.1f, speed %.1f, waypoint %d/%d to %.1f %.1f, replans %d\n", b.name.c_str(),
          b.team == PLAYERTYPE_GDI ? "GDI" : "Nod", GOALS[b.goal], s ? "alive" : "dead", p.X, p.Y, p.Z, vel.Length(),
          (int)b.wp, (int)b.path.size(), b.dest.X, b.dest.Y, b.stuck);
    }
    Say("%d bots, walking grid %d nodes\n", (int)Bots.size(), NavGrid::Node_Count());
  }
};

class BotNavConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_nav"; }
  const char *Get_Help(void) override { return "BOT_NAV [file] - write the bots' walking grid as an image (PGM)."; }
  void Activate(const char *input) override {
    const char *file = input && *input ? input : "navgrid.pgm";
    Say(NavGrid::Dump(file) ? "walking grid written to %s\n" : "no walking grid (%s)\n", file);
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
  list.Add(new BotNavConsoleFunctionClass());
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
  for (Bot &b : Bots) {
    if (now < b.nextStep)
      continue;
    b.nextStep = now + 250;
    SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
    if (!s || !s->Get_Action()) { // dead: cGod respawns it
      b.path.clear();
      b.wp = 0;
      continue;
    }
    Vector3 me;
    s->Get_Position(&me);
    Think_Bot(b, s, me, now);
    Step_Bot(b, s, me, now);
  }
}
