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
// bot_fill <players>, bot_list, bot_order <name|all> <attack|defend|goto x y z>,
// bot_nav [file].
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
#include "vendor.h"
#include "purchasesettings.h"
#include "basecontroller.h"
#include "sctextobj.h"
#include "vehicle.h"
#include "damage.h"
#include "physcoltest.h"
#include "pscene.h"
#include "lineseg.h"
#include "castres.h"
#include "widestring.h"
#include "combat.h"
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
  // Brain mode (bot_do): what to shoot while walking or standing.
  int targetId = 0;      // a soldier or vehicle (object id), 0 = none
  bool shootAt = false;  // ... or a point (a building)
  Vector3 shootPoint;
  bool dirty = false;    // a new order: act on it now
  bool driving = false;  // in a vehicle (its grid, its orders)
  // A short maneuver (bot_do maneuver): a point to get to first, by when,
  // reversing (vehicles), then back on the path.
  bool detour = false, detourBackup = false;
  Vector3 detourPoint;
  double detourUntil = 0, waitUntil = 0;
};

std::vector<Bot> Bots;
bool Brain = false;      // bot_brain on: an outside brain decides (bot_do), see below
double NextState = 0;
int Fill = 0;            // bot_fill: keep this many players in the game
double NextFill = 0;
int NextId = -100;
// Enough for a full 32-player game of bots, each name unique (the brain and
// players tell bots apart by name).
const char *NAMES[] = {"Sgt. Hammer", "Cpl. Vance", "Pvt. Ortega", "Lt. Kova", "Sgt. Riggs", "Pvt. Nash",
                       "Cpl. Mercer", "Lt. Shaw", "Pvt. Duke", "Sgt. Kane", "Cpl. Ivy", "Pvt. Brock",
                       "Sgt. Reyes", "Cpl. Dalton", "Pvt. Kim", "Lt. Moreau", "Sgt. Okafor", "Pvt. Lindqvist",
                       "Cpl. Ferro", "Lt. Haddad", "Pvt. Novak", "Sgt. Brandt", "Cpl. Tanaka", "Pvt. Silva",
                       "Lt. Quinn", "Sgt. Petrov", "Cpl. Adeyemi", "Pvt. Walsh", "Lt. Costa", "Sgt. Varga",
                       "Cpl. Nolan", "Pvt. Rourke", "Lt. Sato", "Sgt. Diaz"};
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

// The grid for how the bot gets around right now: on foot or driving.
NavGrid::Kind Grid_For(const Bot &b) {
  SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
  return s && s->Get_Vehicle() ? NavGrid::VEHICLE : NavGrid::SOLDIER;
}

void Plan(Bot &b, const Vector3 &me, const Vector3 &dest) {
  b.dest = dest;
  // On foot, never to a spot in a tiberium field (the middle of some maps
  // is one): the nearest safe ground instead.
  NavGrid::Kind kind = Grid_For(b);
  Vector3 safe;
  if (kind == NavGrid::SOLDIER && NavGrid::On_Tiberium(dest) && NavGrid::Off_Tiberium(dest, safe))
    b.dest = safe;
  NavGrid::Find_Path(kind, me, b.dest, b.path);
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
    // shows up). At a point on it, not at the object: the attack action
    // takes its target for a physical object, which a building is not.
    ActionParamsStruct params;
    params.Set_Basic((long)0, 60, 7000 + (-b.id));
    params.Set_Attack(dest + Vector3(0, 0, 2.0f), 40.0f, 1.0f, true);
    b.accepted = s->Get_Action()->Attack(params);
    return;
  }
  if (arrived || (dest - b.dest).Length() > 8.0f || b.goal == GOAL_DEFEND)
    Plan(b, me, dest);
}

// One order to the soldier: walk to `move` (or stand), shooting at the
// target if there is one.
void Order(Bot &b, SoldierGameObj *s, const Vector3 *move) {
  // In a vehicle, the vehicle takes the orders (it has its own action: the
  // engine's vehicle AI drives it there).
  SmartGameObj *actor = s->Get_Vehicle() ? (SmartGameObj *)s->Get_Vehicle() : (SmartGameObj *)s;
  ActionParamsStruct params;
  params.Set_Basic((long)0, 60, 7000 + (-b.id));
  if (move) {
    params.Set_Movement(*move, 1.0f, actor == s ? 1.0f : 3.0f);
    params.MovePathfind = false;
    params.MoveBackup = b.detour && b.detourBackup; // vehicles: in reverse
  }
  PhysicalGameObj *target = b.targetId ? GameObjManager::Find_PhysicalGameObj(b.targetId) : nullptr;
  if (target && target->Get_Defense_Object()->Get_Health() <= 0)
    target = nullptr;
  if (target) {
    params.Set_Attack(target, 80.0f, 0.5f, true);
    b.accepted = actor->Get_Action()->Attack(params);
  } else if (b.shootAt) {
    params.Set_Attack(b.shootPoint, 80.0f, 1.0f, true);
    b.accepted = actor->Get_Action()->Attack(params);
  } else if (move) {
    b.accepted = actor->Get_Action()->Goto(params);
  }
}

// Walking, every 1/4 s: the next waypoint of the path, straight at it (the
// engine's planner can't plan on these maps); replan when stuck.
void Step_Bot(Bot &b, SoldierGameObj *s, const Vector3 &me, double now) {
  if (now < b.waitUntil)
    return; // holding still (maneuver "wait")
  if (b.detour) {
    Vector3 d = b.detourPoint - me;
    d.Z = 0;
    if (d.Length() > 2.5f && now < b.detourUntil) {
      if (now - b.lastGoto > 500) {
        Order(b, s, &b.detourPoint);
        b.lastGoto = now;
      }
      return;
    }
    // Done (or given up): back on a path from here.
    b.detour = false;
    b.progressPos = me;
    b.progressAt = now;
    Plan(b, me, b.dest);
  }
  size_t before = b.wp;
  const float reach = s->Get_Vehicle() ? 4.0f : 1.5f; // a vehicle can't stop on a spot
  while (b.wp < b.path.size()) {
    Vector3 d = b.path[b.wp] - me;
    if (d.X * d.X + d.Y * d.Y > reach * reach || fabsf(d.Z) > 2.5f)
      break;
    b.wp++;
  }
  bool walking = b.wp < b.path.size();
  if (walking) {
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
        NavGrid::Block(Grid_For(b), me + ahead * (1.0f / ahead.Length()), s->Get_Vehicle() ? 3.0f : 1.0f);
      if (s->Get_Vehicle() && b.stuckRow >= 4) {
        // A vehicle that can't get on (the walking grid is made for soldiers):
        // out, and on foot.
        s->Set_Boolean_Control(ControlClass::BOOLEAN_ACTION);
        b.stuckRow = 0;
        Say("%s leaves a stuck vehicle\n", b.name.c_str());
      }
      if (b.stuckRow >= 3 && !Brain) {
        float a = (float)(rand() % 628) / 100.0f;
        Plan(b, me, me + Vector3(6.0f * cosf(a), 6.0f * sinf(a), 0));
      } else {
        Plan(b, me, b.dest);
      }
      walking = b.wp < b.path.size();
    }
  }
  bool fighting = b.targetId || b.shootAt;
  if (!walking && !fighting)
    return;
  if (!b.dirty && b.wp == before && b.lastGoto && now - b.lastGoto < (fighting ? 1500 : 3000))
    return;
  Order(b, s, walking ? &b.path[b.wp] : nullptr);
  b.lastGoto = now;
  b.dirty = false;
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
    if (!all && team < 0) // from the bigger team
      team = Team_Size(PLAYERTYPE_GDI) >= Team_Size(PLAYERTYPE_NOD) ? PLAYERTYPE_GDI : PLAYERTYPE_NOD;
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
    Say("%d bots, walking grids: soldiers %d nodes, vehicles %d\n", (int)Bots.size(), NavGrid::Node_Count(NavGrid::SOLDIER), NavGrid::Node_Count(NavGrid::VEHICLE));
  }
};

class BotFillConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_fill"; }
  const char *Get_Help(void) override {
    return "BOT_FILL <players> - keep the game at this many players with bots, teams even (0: off).";
  }
  void Activate(const char *input) override {
    Fill = input ? std::max(0, std::min(32, atoi(input))) : 0;
    NextFill = 0;
    Say("bots fill the game up to %d players\n", Fill);
  }
};

class BotNavConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_nav"; }
  const char *Get_Help(void) override { return "BOT_NAV [file|vehicles] - write a walking grid as an image (PGM)."; }
  void Activate(const char *input) override {
    bool vehicles = input && strstr(input, "vehicle");
    const char *file = vehicles ? "navgrid-vehicles.pgm" : input && *input ? input : "navgrid.pgm";
    Say(NavGrid::Dump(vehicles ? NavGrid::VEHICLE : NavGrid::SOLDIER, file) ? "walking grid written to %s\n" : "no walking grid (%s)\n", file);
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

// ---------------------------------------------------------------------------
// Brain mode: the bots' decisions come from outside (tools/server/bot-brain.mjs:
// scripted, decision models, language models). The server streams the world
// as each bot may know it, 4 times a second, plus every chat line; the brain
// answers with console commands:
//
//   bot_brain on|off
//   bot_do <id> go <x> <y> <z>      walk there (walking grid)
//   bot_do <id> stop
//   bot_do <id> target <object id>  shoot at a soldier/vehicle (0: stop)
//   bot_do <id> shoot <x> <y> <z>   shoot at a point (a building)
//   bot_do <id> buy character|vehicle <n>   (the team's shop, see "shop")
//   bot_do <id> enter [vehicle]     get into the empty vehicle next to it (no id: the use key)
//   bot_do <id> maneuver steer_left|steer_right|reverse_left|reverse_right|strafe_left|strafe_right|replan|wait|get_out
//   bot_do <id> leave_tiberium     walk off the field by the shortest way
//   bot_do <id> jump
//   bot_do <id> say team|all <text>

EM_JS(void, Rebel_JS_Bot_State, (const char *json), {
  if (Module.rebelBotState) Module.rebelBotState(UTF8ToString(json));
});
EM_JS(void, Rebel_JS_Bot_Chat, (int sender, int type, const char *text), {
  if (Module.rebelBotChat) Module.rebelBotChat(sender, type, UTF8ToString(text));
});

std::string Narrow(const WideStringClass &w) {
  std::string out;
  for (const wchar_t *c = w.Peek_Buffer(); c && *c; c++)
    out += (*c >= 32 && *c < 127) ? (char)*c : '?';
  return out;
}

void Json_String(std::string &out, const std::string &v) {
  out += '"';
  for (char c : v) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  out += '"';
}

void Json_Pos(std::string &out, const Vector3 &p) {
  char buf[96];
  snprintf(buf, sizeof(buf), "[%.1f,%.1f,%.1f]", p.X, p.Y, p.Z);
  out += buf;
}

const char *Team_Name(int t) { return t == PLAYERTYPE_GDI ? "GDI" : t == PLAYERTYPE_NOD ? "Nod" : "none"; }

float Health_Of(DamageableGameObj *o) {
  DefenseObjectClass *d = o->Get_Defense_Object();
  float max = d->Get_Health_Max() + d->Get_Shield_Strength_Max();
  return max > 0 ? (d->Get_Health() + d->Get_Shield_Strength()) / max : 0;
}

// Line of sight from a bot's eyes (static geometry only).
bool Can_See(const Vector3 &eye, PhysicalGameObj *o) {
  Vector3 at = o->Get_Bullseye_Position();
  if ((at - eye).Length() > 120.0f)
    return false;
  CastResultStruct res;
  LineSegClass ray(eye, at);
  PhysRayCollisionTestClass test(ray, &res, BULLET_COLLISION_GROUP, COLLISION_TYPE_PROJECTILE);
  test.CheckDynamicObjs = false;
  COMBAT_SCENE->Cast_Ray(test);
  return res.Fraction >= 1.0f;
}

// What's around a bot, by ray casts from about waist (bumper) height:
// distances (m, up to 15) ahead, ahead-left/right (40 deg), left, right and
// behind, relative to where it faces; other vehicles and soldiers count.
void Feel(SoldierGameObj *s, float out[6]) {
  PhysicalGameObj *body = s->Get_Vehicle() ? (PhysicalGameObj *)s->Get_Vehicle() : (PhysicalGameObj *)s;
  Matrix3D tm = body->Get_Transform();
  Vector3 at = tm.Get_Translation() + Vector3(0, 0, 1.0f);
  Vector3 fwd = tm.Get_X_Vector();
  fwd.Z = 0;
  if (fwd.Length() < 0.01f)
    fwd.Set(1, 0, 0);
  fwd.Normalize();
  Vector3 left(-fwd.Y, fwd.X, 0);
  const float c = 0.766f, sn = 0.643f; // cos/sin 40 deg
  Vector3 dirs[6] = {fwd, fwd * c + left * sn, fwd * c - left * sn, left, -left, -fwd};
  body->Peek_Physical_Object()->Inc_Ignore_Counter();
  if (body != s)
    s->Peek_Physical_Object()->Inc_Ignore_Counter();
  for (int i = 0; i < 6; i++) {
    LineSegClass ray(at, at + dirs[i] * 15.0f);
    CastResultStruct res;
    PhysRayCollisionTestClass test(ray, &res, DEFAULT_COLLISION_GROUP, COLLISION_TYPE_PHYSICAL);
    COMBAT_SCENE->Cast_Ray(test);
    out[i] = 15.0f * res.Fraction;
  }
  body->Peek_Physical_Object()->Dec_Ignore_Counter();
  if (body != s)
    s->Peek_Physical_Object()->Dec_Ignore_Counter();
}

void Emit_State(double now) {
  std::string j;
  j.reserve(8192);
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"t\":%.0f,\"map\":", now);
  j += buf;
  Json_String(j, The_Game()->Get_Map_Name().Peek_Buffer());
  j += ",\"buildings\":[";
  bool first = true;
  for (SLNode<BuildingGameObj> *n = GameObjManager::Get_Building_Game_Obj_List()->Head(); n; n = n->Next()) {
    BuildingGameObj *bld = n->Data();
    Vector3 p;
    bld->Get_Position(&p);
    snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"team\":\"%s\",\"hp\":%.2f,\"pos\":", first ? "" : ",", bld->Get_ID(),
             Team_Name(bld->Get_Player_Type()), bld->Is_Destroyed() ? 0.0f : Health_Of(bld));
    j += buf;
    Json_Pos(j, p);
    j += ",\"name\":";
    Json_String(j, bld->Get_Definition().Get_Name());
    j += "}";
    first = false;
  }
  j += "],\"players\":[";
  first = true;
  std::vector<SoldierGameObj *> soldiers;
  for (SLNode<cPlayer> *n = cPlayerManager::Get_Player_Object_List()->Head(); n; n = n->Next()) {
    cPlayer *pl = n->Data();
    if (!pl->Get_Is_Active().Is_True())
      continue;
    SoldierGameObj *so = GameObjManager::Find_Soldier_Of_Client_ID(pl->Get_Id());
    Vector3 p(0, 0, 0);
    if (so) {
      so->Get_Position(&p);
      soldiers.push_back(so);
    }
    VehicleGameObj *veh = so ? so->Get_Vehicle() : nullptr;
    snprintf(buf, sizeof(buf),
             "%s{\"id\":%d,\"obj\":%d,\"team\":\"%s\",\"bot\":%s,\"alive\":%s,\"hp\":%.2f,\"credits\":%d,\"kills\":%d,"
             "\"deaths\":%d,\"vehicle\":%d,\"pos\":",
             first ? "" : ",", pl->Get_Id(), so ? so->Get_ID() : 0, Team_Name(pl->Get_Player_Type()),
             pl->Get_Id() < 0 ? "true" : "false", so ? "true" : "false", so ? Health_Of(so) : 0.0f,
             (int)pl->Get_Money(), pl->Get_Kills(), pl->Get_Deaths(), veh ? veh->Get_ID() : 0);
    j += buf;
    Json_Pos(j, p);
    j += ",\"name\":";
    Json_String(j, Narrow(pl->Get_Name()));
    j += ",\"char\":";
    Json_String(j, so ? so->Get_Definition().Get_Name() : "");
    j += "}";
    first = false;
  }
  j += "],\"vehicles\":[";
  first = true;
  std::vector<VehicleGameObj *> vehicles;
  for (SLNode<SmartGameObj> *n = GameObjManager::Get_Smart_Game_Obj_List()->Head(); n; n = n->Next()) {
    VehicleGameObj *v = n->Data()->As_VehicleGameObj();
    if (!v || v->Get_Defense_Object()->Get_Health() <= 0)
      continue;
    vehicles.push_back(v);
    Vector3 p;
    v->Get_Position(&p);
    SoldierGameObj *driver = v->Get_Driver();
    snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"team\":\"%s\",\"hp\":%.2f,\"driver\":%d,\"pos\":", first ? "" : ",",
             v->Get_ID(), Team_Name(v->Get_Player_Type()), Health_Of(v), driver ? driver->Get_ID() : 0);
    j += buf;
    Json_Pos(j, p);
    j += ",\"name\":";
    Json_String(j, v->Get_Definition().Get_Name());
    j += "}";
    first = false;
  }
  // What each team can buy (the purchase terminal's lists) and whether its
  // base has power (prices double without it).
  j += "],\"shop\":{";
  for (int t = 0; t < 2; t++) {
    int team = t == 0 ? PLAYERTYPE_GDI : PLAYERTYPE_NOD;
    BaseControllerClass *base = BaseControllerClass::Find_Base(team);
    snprintf(buf, sizeof(buf), "%s\"%s\":{\"powered\":%s", t ? "," : "", Team_Name(team), base && base->Is_Base_Powered() ? "true" : "false");
    j += buf;
    for (int kind = 0; kind < 2; kind++) {
      j += kind == 0 ? ",\"characters\":[" : ",\"vehicles\":[";
      PurchaseSettingsDefClass *def = PurchaseSettingsDefClass::Find_Definition(
          kind == 0 ? PurchaseSettingsDefClass::TYPE_CLASSES : PurchaseSettingsDefClass::TYPE_VEHICLES,
          t == 0 ? PurchaseSettingsDefClass::TEAM_GDI : PurchaseSettingsDefClass::TEAM_NOD);
      bool any = false;
      for (int i = 0; def && i < 10; i++) { // PurchaseSettingsDefClass::MAX_ENTRIES (protected)
        if (!def->Get_Definition(i) || def->Get_Cost(i) <= 0)
          continue;
        snprintf(buf, sizeof(buf), "%s{\"index\":%d,\"cost\":%d,\"name\":", any ? "," : "", i, def->Get_Cost(i));
        j += buf;
        WideStringClass name(def->Get_Name(i));
        Json_String(j, Narrow(name));
        j += "}";
        any = true;
      }
      j += "]";
    }
    j += "}";
  }
  j += "},\"bots\":[";
  first = true;
  for (Bot &b : Bots) {
    SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
    snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"waypoint\":%d,\"path\":%d,\"stuck\":%d,\"target\":%d,",
             first ? "" : ",", b.id, (int)b.wp, (int)b.path.size(), b.stuckRow, b.targetId);
    j += buf;
    first = false;
    if (!s) {
      j += "\"sees\":[]}";
      continue;
    }
    // Its surroundings (Feel) and how it's moving.
    float f[6];
    Feel(s, f);
    Vector3 vel(0, 0, 0);
    if (s->Get_Vehicle())
      s->Get_Vehicle()->Get_Velocity(vel);
    else
      s->Get_Velocity(vel);
    Vector3 here;
    s->Get_Position(&here);
    snprintf(buf, sizeof(buf), "\"tiberium\":%s,", !s->Get_Vehicle() && NavGrid::On_Tiberium(here) ? "true" : "false");
    j += buf;
    snprintf(buf, sizeof(buf), "\"feel\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],\"speed\":%.1f,\"driving\":%s,\"maneuver\":%s,\"sees\":[",
             f[0], f[1], f[2], f[3], f[4], f[5], vel.Length(), s->Get_Vehicle() ? "true" : "false",
             b.detour || now < b.waitUntil ? "true" : "false");
    j += buf;
    Vector3 eye;
    s->Get_Position(&eye);
    eye.Z += 1.6f;
    bool any = false;
    // Enemy soldiers on foot and enemy vehicles in line of sight.
    for (SoldierGameObj *o : soldiers)
      if (o != s && o->Get_Player_Type() != b.team && !o->Get_Vehicle() && Can_See(eye, o)) {
        snprintf(buf, sizeof(buf), "%s%d", any ? "," : "", o->Get_ID());
        j += buf;
        any = true;
      }
    for (VehicleGameObj *v : vehicles)
      if (v->Get_Player_Type() != b.team && v->Get_Player_Type() >= 0 && Can_See(eye, v)) {
        snprintf(buf, sizeof(buf), "%s%d", any ? "," : "", v->Get_ID());
        j += buf;
        any = true;
      }
    j += "],\"dest\":";
    Json_Pos(j, b.dest);
    j += "}";
  }
  j += "]}";
  Rebel_JS_Bot_State(j.c_str());
}

class BotBrainConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_brain"; }
  const char *Get_Help(void) override { return "BOT_BRAIN on|off - bots take orders from an outside brain (bot_do)."; }
  void Activate(const char *input) override {
    Brain = input && strstr(input, "on");
    Say("bot brain %s\n", Brain ? "on" : "off");
  }
};

class BotDoConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "bot_do"; }
  const char *Get_Help(void) override {
    return "BOT_DO <id> go x y z|stop|target <obj>|shoot x y z|hold_fire|buy character|vehicle <n>|enter|jump|say team|all <text>";
  }
  void Activate(const char *input) override {
    int id = 0, n = 0;
    char verb[16] = {0};
    if (!input || sscanf(input, "%d %15s %n", &id, verb, &n) < 2)
      return;
    const char *rest = input + n;
    Bot *bot = nullptr;
    for (Bot &b : Bots)
      if (b.id == id)
        bot = &b;
    if (!bot)
      return;
    Bot &b = *bot;
    SoldierGameObj *s = GameObjManager::Find_Soldier_Of_Client_ID(b.id);
    float x, y, z;
    b.dirty = true;
    if (!strcmp(verb, "go") && sscanf(rest, "%f %f %f", &x, &y, &z) == 3) {
      b.point = Vector3(x, y, z);
      if (s) {
        Vector3 me;
        s->Get_Position(&me);
        Plan(b, me, b.point);
      }
    } else if (!strcmp(verb, "stop")) {
      b.path.clear();
      b.wp = 0;
      if (s) {
        Vector3 me;
        s->Get_Position(&me);
        Order(b, s, &me);
      }
    } else if (!strcmp(verb, "target")) {
      b.targetId = atoi(rest);
      b.shootAt = false;
    } else if (!strcmp(verb, "shoot") && sscanf(rest, "%f %f %f", &x, &y, &z) == 3) {
      b.shootAt = true;
      b.shootPoint = Vector3(x, y, z);
      b.targetId = 0;
    } else if (!strcmp(verb, "hold_fire")) {
      b.shootAt = false;
      b.targetId = 0;
    } else if (!strcmp(verb, "buy") && s) {
      // At a purchase terminal's prices, with the game's own checks (credits,
      // the factory or barracks standing).
      char what[16] = {0};
      int index = -1;
      if (sscanf(rest, "%15s %d", what, &index) != 2 || index < 0)
        return;
      bool vehicle = !strcmp(what, "vehicle");
      VendorClass::PURCHASE_ERROR result =
          VendorClass::Purchase_Item(s, vehicle ? VendorClass::TYPE_VEHICLE : VendorClass::TYPE_CHARACTER, index, -1, true);
      static const char *RESULTS[] = {"bought", "pending", "no funds", "no factory", "not in stock"};
      Say("%s buys %s %d: %s\n", b.name.c_str(), what, index, result >= 0 && result <= 4 ? RESULTS[result] : "failed");
    } else if (!strcmp(verb, "enter") && s) {
      // Into an empty vehicle next to it, the way the vehicle's door
      // transition does (a bot can't aim for the door zone); "enter" alone
      // is the use key (out of a vehicle).
      int vid = atoi(rest);
      PhysicalGameObj *o = vid ? GameObjManager::Find_PhysicalGameObj(vid) : nullptr;
      VehicleGameObj *v = o ? o->As_VehicleGameObj() : nullptr;
      Vector3 me, at;
      s->Get_Position(&me);
      if (v && !s->Get_Vehicle() && !v->Get_Driver() && (v->Get_Position(&at), (at - me).Length() < 8.0f)) {
        v->Add_Occupant(s);
        Say("%s gets in a %s\n", b.name.c_str(), v->Get_Definition().Get_Name());
      } else if (!vid) {
        s->Set_Boolean_Control(ControlClass::BOOLEAN_ACTION);
      }
    } else if (!strcmp(verb, "maneuver") && s) {
      // A short move to get unstuck or around something, chosen by the brain
      // from what the bot feels around it, then back on a path.
      PhysicalGameObj *body = s->Get_Vehicle() ? (PhysicalGameObj *)s->Get_Vehicle() : (PhysicalGameObj *)s;
      Matrix3D tm = body->Get_Transform();
      Vector3 me = tm.Get_Translation(), fwd = tm.Get_X_Vector();
      fwd.Z = 0;
      if (fwd.Length() < 0.01f)
        fwd.Set(1, 0, 0);
      fwd.Normalize();
      Vector3 left(-fwd.Y, fwd.X, 0);
      bool vehicle = s->Get_Vehicle() != nullptr;
      float far_ = vehicle ? 10.0f : 5.0f, side = vehicle ? 7.0f : 4.0f;
      double now = emscripten_get_now();
      auto detour = [&](const Vector3 &to, bool backup) {
        b.detour = true;
        b.detourBackup = backup && vehicle;
        b.detourPoint = to;
        b.detourUntil = now + 4000;
        b.lastGoto = 0;
      };
      if (strstr(rest, "steer_left"))
        detour(me + fwd * far_ + left * side, false);
      else if (strstr(rest, "steer_right"))
        detour(me + fwd * far_ - left * side, false);
      else if (strstr(rest, "reverse_left"))
        detour(me - fwd * (far_ * 0.6f) + left * (side * 0.5f), true);
      else if (strstr(rest, "reverse_right"))
        detour(me - fwd * (far_ * 0.6f) - left * (side * 0.5f), true);
      else if (strstr(rest, "strafe_left") || strstr(rest, "strafe_right")) {
        // A quick side-step, still shooting (the order keeps the target).
        detour(me + left * (strstr(rest, "left") ? 3.5f : -3.5f), false);
        b.detourUntil = now + 1500;
      } else if (strstr(rest, "replan")) {
        NavGrid::Block(Grid_For(b), me + fwd * (vehicle ? 4.0f : 1.5f), vehicle ? 3.0f : 1.0f);
        Plan(b, me, b.dest);
      } else if (strstr(rest, "wait")) {
        b.waitUntil = now + 2000;
        Order(b, s, &me);
      } else if (strstr(rest, "get_out") && vehicle) {
        s->Set_Boolean_Control(ControlClass::BOOLEAN_ACTION);
      }
    } else if (!strcmp(verb, "leave_tiberium") && s) {
      // Out of the field by the shortest way, then on with the plan.
      Vector3 me, out;
      s->Get_Position(&me);
      if (NavGrid::Off_Tiberium(me, out)) {
        b.detour = true;
        b.detourBackup = false;
        b.detourPoint = out;
        b.detourUntil = emscripten_get_now() + 5000;
        b.lastGoto = 0;
      }
    } else if (!strcmp(verb, "jump")) {
      if (s)
        s->Set_Boolean_Control(ControlClass::BOOLEAN_JUMP);
    } else if (!strcmp(verb, "say")) {
      bool team = !strncmp(rest, "team ", 5);
      const char *text = strchr(rest, ' ');
      if (!text || !text[1] || !cNetwork::I_Am_Server())
        return;
      WideStringClass wide;
      wide.Convert_From(std::string(text + 1).substr(0, 200).c_str());
      cScTextObj *msg = new cScTextObj;
      msg->Init(wide, team ? TEXT_MESSAGE_TEAM : TEXT_MESSAGE_PUBLIC, false, b.id);
    }
  }
};

} // namespace

void Rebel_Bots_Add_Commands(DynamicVectorClass<ConsoleFunctionClass *> &list) {
  list.Add(new BotAddConsoleFunctionClass());
  list.Add(new BotRemoveConsoleFunctionClass());
  list.Add(new BotListConsoleFunctionClass());
  list.Add(new BotOrderConsoleFunctionClass());
  list.Add(new BotFillConsoleFunctionClass());
  list.Add(new BotBrainConsoleFunctionClass());
  list.Add(new BotDoConsoleFunctionClass());
  list.Add(new BotNavConsoleFunctionClass());
}

int Rebel_Bots_Count(void) { return (int)Bots.size(); }

void Rebel_Bots_On_Chat(int sender, int type, const WideStringClass &text) {
  if (Brain)
    Rebel_JS_Bot_Chat(sender, type, Narrow(text).c_str());
}

// bot_fill: one bot in or out every second until the game has Fill players
// and even teams; humans joining push bots out. Bots only play with humans:
// with none in the game they all leave (no bots-only games).
void Fill_Bots(double now) {
  if (now < NextFill || !PTheGameData || CombatManager::Is_Loading_Level())
    return;
  NextFill = now + 1000;
  int humans = 0;
  for (SLNode<cPlayer> *n = cPlayerManager::Get_Player_Object_List()->Head(); n; n = n->Next())
    if (n->Data()->Get_Is_Active().Is_True() && n->Data()->Get_Id() >= 0)
      humans++;
  if (!humans) {
    if (!Bots.empty())
      Remove(Bots.size() - 1);
    return;
  }
  int gdi = Team_Size(PLAYERTYPE_GDI), nod = Team_Size(PLAYERTYPE_NOD), total = gdi + nod;
  int big = gdi > nod ? PLAYERTYPE_GDI : PLAYERTYPE_NOD;
  bool uneven = abs(gdi - nod) > 1;
  if (total > Fill || (uneven && total >= Fill)) {
    for (int i = (int)Bots.size() - 1; i >= 0; i--)
      if (Bots[i].team == big || !uneven) {
        Remove(i);
        return;
      }
  } else if (total < Fill) {
    Add(Pick_Team(nullptr));
  }
}

void Rebel_Bots_Think(void) {
  if (!cNetwork::I_Am_Server())
    return;
  double now = emscripten_get_now();
  if (Fill > 0)
    Fill_Bots(now);
  // The walking grid builds as soon as a map with bot_fill on is loaded, a
  // small slice a frame, so it's ready before the first human (and bots)
  // arrive instead of slowing the server down when they do.
  if (Fill > 0 || !Bots.empty())
    NavGrid::Ensure();
  if (Bots.empty())
    return;
  if (Brain && now >= NextState && !CombatManager::Is_Loading_Level()) {
    NextState = now + (Bots.size() > 8 ? 500 : 250); // big games: half as often (line of sight is bots x players)
    Emit_State(now);
  }
  // Forget bots whose player is gone (a map change resets players).
  for (int i = (int)Bots.size() - 1; i >= 0; i--)
    if (!cPlayerManager::Find_Player(Bots[i].id))
      Bots.erase(Bots.begin() + i);
  for (Bot &b : Bots) {
    // A bot's team is the player's: the server remixes teams at a map change.
    if (cPlayer *p = cPlayerManager::Find_Player(b.id)) {
      if (p->Get_Player_Type() != b.team) {
        b.team = p->Get_Player_Type();
        b.targetId = 0;
        b.shootAt = false;
        b.path.clear();
        b.wp = 0;
      }
    }
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
    if ((s->Get_Vehicle() != nullptr) != b.driving) { // in or out: the other grid
      b.driving = s->Get_Vehicle() != nullptr;
      b.detour = false;
      if (b.wp < b.path.size())
        Plan(b, me, b.dest);
    }
    if (!Brain)
      Think_Bot(b, s, me, now);
    Step_Bot(b, s, me, now);
  }
}
