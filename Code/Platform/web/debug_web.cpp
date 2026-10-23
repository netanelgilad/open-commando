// Debug hooks callable from the page (Module._Rebel_Debug_Dump()).
#include "gamemode.h"
#include "dialogmgr.h"
#include "dialogbase.h"

#include <cstdio>
#include <emscripten.h>
#include "wwprofile.h"

static bool PendingDump = false;
extern "C" void Rebel_Debug_Dump(void) { PendingDump = true; } // queued, see Rebel_Run_Pending_Actions

// Console commands (game_info, player_info, amsg, kick...), as typed at the
// Free Dedicated Server's console: the Node server (tools/server) passes its
// stdin lines here. Queued like the other hooks.
#include "consolefunction.h"
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
static std::vector<std::string> PendingConsole;
extern "C" void Rebel_Console_Command(const char *line) { PendingConsole.push_back(line); }

// Lines a host queued in JavaScript (Module.rebelConsoleQueue, an array):
// taken from here, inside the game's own frame. Calling into the game from
// JavaScript instead (ccall) can land while it's suspended in a sleep
// (Asyncify) and corrupt it: the dedicated server hung in "Server Sleep".
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS_DEPS(rebel_console_queue, "$stringToNewUTF8");
EM_JS(char *, Rebel_Take_Console_Queue, (void), {
  var q = Module.rebelConsoleQueue;
  if (!q || !q.length) return 0;
  var text = q.join('\n');
  q.length = 0;
  return stringToNewUTF8(text);
});
#endif
static void Take_Console_Queue() {
#ifdef __EMSCRIPTEN__
  char *text = Rebel_Take_Console_Queue();
  if (!text)
    return;
  for (char *line = strtok(text, "\n"); line; line = strtok(nullptr, "\n"))
    if (*line)
      PendingConsole.push_back(line);
  free(text);
#endif
}
static void Dump_Modes(void) {
  static const char *names[] = {"Combat", "Console", "LAN", "Menu", "Movie", "Overlay", "ScoreScreen", "TextDisplay", "WOL"};
  fprintf(stderr, "[debug] game modes:");
  for (const char *n : names) {
    GameModeClass *m = GameModeManager::Find(n);
    fprintf(stderr, " %s=%s", n, !m ? "missing" : m->Is_Active() ? "ACTIVE" : "off");
  }
  fprintf(stderr, "\n");
  DialogBaseClass *dlg = DialogMgrClass::Get_Active_Dialog();
  fprintf(stderr, "[debug] active dialog %p (id %d)\n", (void *)dlg, dlg ? dlg->Get_Dlg_ID() : -1);
}

#define private public
#include "directinput.h"
#undef private
#include "vector2.h"

// Absolute cursor for the page's non-pointer-locked mode (menus): the engine
// otherwise integrates DirectInput deltas (x2) and would drift from the
// real pointer position.
extern "C" void Rebel_Input_Set_Cursor(int x, int y) { DirectInput::Reset_Cursor_Pos(Vector2((float)x, (float)y)); }

#include "campaign.h"
#include "gameinitmgr.h"

// Test hook: what the Difficulty menu's buttons do (0 = Recruit ... 2 = Commando).
// Exports called from JavaScript run while the game's main loop is paused
// (ASYNCIFY); running game code that can itself pause from there would nest
// pauses. So the hook only records the request and the frame loop runs it
// (Rebel_Run_Pending_Actions, called after each frame yield).
static int PendingCampaign = -1;
static bool PendingBringTarget = false;

extern "C" void Rebel_Debug_Start_Campaign(int difficulty) { PendingCampaign = difficulty; }

// Test hook: moves the nearest other soldier 6 m in front of the player, so a
// test can shoot at a character.
extern "C" void Rebel_Debug_Bring_Target(void) { PendingBringTarget = true; }
static void Bring_Target(void);
// Test hook: damages the soldier last brought, as a hit from the player would.
static float PendingDamage = 0;
extern "C" void Rebel_Debug_Damage_Target(float amount) { PendingDamage = amount; }
static void Damage_Target(float amount);
static void Dump_Input(void);
static bool PendingInputDump = false;
// Test hooks: save to / load from save\\webtest.sav (what the Save and Load
// menus do), and end the mission as won (what a mission script does).
static int PendingSave = 0, PendingLoad = 0, PendingWin = 0;
extern "C" void Rebel_Debug_Save(void) { PendingSave = 1; }
extern "C" void Rebel_Debug_Load(void) { PendingLoad = 1; }
extern "C" void Rebel_Debug_Win_Mission(void) { PendingWin = 1; }
static int PendingEva = -1;
// Test hook for the hang watchdog: busy-waits the main thread (seconds).
static int PendingStall = 0;
// Test hooks for vehicles: spawn a player tank in front of the player, then
// put the player in it (as walking up and pressing the action key does).
static int PendingVehicle = 0; // 1 spawn, 2 enter
extern "C" void Rebel_Debug_Spawn_Vehicle(void) { PendingVehicle = 1; }
extern "C" void Rebel_Debug_Enter_Vehicle(void) { PendingVehicle = 2; }
static void Vehicle_Action(int action);
extern "C" void Rebel_Debug_Stall(int seconds) { PendingStall = seconds; }
// Test hooks for walking the menus: list the active dialog's controls, and
// press a control (as a click on it would, through On_Command).
static int PendingCommand = 0;
extern "C" void Rebel_Debug_Command(int ctrl_id) { PendingCommand = ctrl_id; }
static void List_Controls(void);
static bool PendingList = false;
extern "C" void Rebel_Debug_List_Controls(void) { PendingList = true; }
extern "C" void Rebel_Debug_Open_Eva(int tab) { PendingEva = tab; }
#include "combat.h"
#include "dlgevaencyclopedia.h"
static void Save_Test_Game(void);
static void Load_Test_Game(void);

#include "showroom_web.h"
#include "bots_web.h"

extern bool g_is_loading; // combatgmode.cpp: a level is loading

// Frames run so far (the page and run-server.mjs turn it into frames a second).
static unsigned FrameCount = 0;
extern "C" unsigned Rebel_Frame_Count(void) { return FrameCount; }

extern "C" void Rebel_Run_Pending_Actions(void) {
  FrameCount++;
  // Published for hosts to read without calling in (see Take_Console_Queue).
  if (FrameCount % 30 == 0)
    EM_ASM({ Module.rebelFrames = $0; }, FrameCount);
  Rebel_Showroom_Run_Pending();
  // A level loading pumps messages too (Load_Level -> Windows_Message_Handler),
  // with the old level half released: the bots (their state reads every
  // building and player) and console commands (bot_do...) wait until it's
  // done. Reading a base mid-load crashed the server at each map change.
  if (g_is_loading)
    return;
  Take_Console_Queue();
  Rebel_Bots_Think();
  if (!PendingConsole.empty()) {
    std::vector<std::string> lines;
    lines.swap(PendingConsole);
    for (const std::string &line : lines)
      ConsoleFunctionManager::Parse_Input(line.c_str());
  }
  if (PendingCampaign >= 0) {
    int difficulty = PendingCampaign;
    PendingCampaign = -1;
    fprintf(stderr, "[debug] starting campaign, difficulty %d\n", difficulty);
    GameInitMgrClass::Initialize_SP(); // as DifficultyMenuClass::On_Command does
    CampaignManager::Start_Campaign(difficulty);
  }
  if (PendingBringTarget) {
    PendingBringTarget = false;
    Bring_Target();
  }
  if (PendingDamage > 0) {
    float amount = PendingDamage;
    PendingDamage = 0;
    Damage_Target(amount);
  }
  if (PendingSave) {
    PendingSave = 0;
    Save_Test_Game();
  }
  if (PendingLoad) {
    PendingLoad = 0;
    Load_Test_Game();
  }
  if (PendingEva >= 0) {
    int tab = PendingEva;
    PendingEva = -1;
    fprintf(stderr, "[debug] opening EVA tab %d\n", tab);
    EVAEncyclopediaMenuClass::Display((EVAEncyclopediaMenuClass::TAB_ID)tab);
  }
  if (PendingVehicle) {
    int action = PendingVehicle;
    PendingVehicle = 0;
    Vehicle_Action(action);
  }
  if (PendingStall > 0) {
    int secs = PendingStall;
    PendingStall = 0;
    WWPROFILE("Rebel_Debug_Stall");
    fprintf(stderr, "[debug] stalling the main thread for %d s\n", secs);
    double until = emscripten_get_now() + secs * 1000.0;
    while (emscripten_get_now() < until) {
    }
  }
  if (PendingList) {
    PendingList = false;
    List_Controls();
  }
  if (PendingCommand) {
    int id = PendingCommand;
    PendingCommand = 0;
    if (DialogBaseClass *dlg = DialogMgrClass::Get_Active_Dialog()) {
      fprintf(stderr, "[debug] dialog %d: command %d\n", dlg->Get_Dlg_ID(), id);
      dlg->On_Command(id, 0, 0);
    }
  }
  if (PendingDump) {
    PendingDump = false;
    Dump_Modes();
  }
  if (PendingInputDump) {
    PendingInputDump = false;
    Dump_Input();
  }
  if (PendingWin) {
    PendingWin = 0;
    fprintf(stderr, "[debug] mission complete (success)\n");
    CombatManager::Mission_Complete(true);
  }
}

// Input pipeline state, for diagnosing "keys do nothing in game".
#define private public
#include "input.h"
#undef private
#include "combat.h"
#include "soldier.h"
#include "vehicle.h"
#include "ccamera.h"
extern bool GameInFocus;
extern int FunctionKeyStates[];

// Queued like the other hooks: reading game state straight from a page call
// can catch a level half torn down (it crashed once during a load).
extern "C" void Rebel_Debug_Input(void) { PendingInputDump = true; }
static void Dump_Input(void) {
  fprintf(stderr, "[debug] input: menu=%d console=%d captured=%d W=%d mouseL=%d axisX=%ld fwd=%.2f turnL=%.2f fn_fwd_key=%d\n",
          (int)Input::MenuMode, (int)Input::ConsoleMode, (int)DirectInput::Captured,
          DirectInput::Get_Keyboard_Button(0x11), DirectInput::Get_Mouse_Button(0),
          DirectInput::Get_Mouse_Axis(DirectInput::MOUSE_X_AXIS), Input::FunctionValue[INPUT_FUNCTION_MOVE_FORWARD],
          Input::FunctionValue[INPUT_FUNCTION_TURN_LEFT], (int)Input::FunctionPrimaryKeys[INPUT_FUNCTION_MOVE_FORWARD]);
  fprintf(stderr, "[debug] input: cinematic=%d keystate=%x f9=%x btnval=%x focus=%d sync=%u\n",
          COMBAT_CAMERA ? (int)COMBAT_CAMERA->Is_In_Cinematic() : -1, FunctionKeyStates[INPUT_FUNCTION_MOVE_FORWARD], (int)DirectInput::Get_Button_Value(0x43),
          (int)DirectInput::Get_Button_Value(0x11), (int)GameInFocus, CombatManager::Get_Sync_Time());
  if (SoldierGameObj *star = COMBAT_STAR) {
    if (VehicleGameObj *v = star->Get_Vehicle()) {
      Vector3 vp, vel;
      v->Get_Position(&vp);
      v->Get_Velocity(vel);
      fprintf(stderr, "[debug] vehicle at %.2f %.2f %.2f speed %.2f, driver=%d\n", vp.X, vp.Y, vp.Z, vel.Length(),
              v->Get_Driver() == star);
    }
    Vector3 p;
    star->Get_Position(&p);
    fprintf(stderr, "[debug] star at %.2f %.2f %.2f facing %.2f, controlled=%d\n", p.X, p.Y, p.Z, star->Get_Facing(),
            (int)star->Is_Human_Controlled());
  } else
    fprintf(stderr, "[debug] no star\n");
}

#include "gameobjmanager.h"
#include "phys.h"
#include <cmath>

#include "damage.h"
static int BroughtId = 0;

static void Bring_Target(void) {
  SoldierGameObj *star = COMBAT_STAR;
  if (!star)
    return;
  Vector3 me;
  star->Get_Position(&me);
  SoldierGameObj *best = nullptr;
  float best_d = 1e30f;
  for (SLNode<SmartGameObj> *n = GameObjManager::Get_Smart_Game_Obj_List()->Head(); n; n = n->Next()) {
    SoldierGameObj *s = n->Data()->As_SoldierGameObj();
    if (!s || s == star)
      continue;
    Vector3 p;
    s->Get_Position(&p);
    float d = (p - me).Length2();
    if (d < best_d) {
      best_d = d;
      best = s;
    }
  }
  if (!best) {
    fprintf(stderr, "[debug] no soldier to bring\n");
    return;
  }
  float f = star->Get_Facing();
  Vector3 to = me + Vector3(cosf(f) * 2.5f, sinf(f) * 2.5f, 0.0f);
  BroughtId = best->Get_ID();
  best->Peek_Physical_Object()->Set_Position(to);
  fprintf(stderr, "[debug] brought soldier %d (%s) from %.0f m to %.1f %.1f %.1f\n", best->Get_ID(),
          best->Get_Definition().Get_Name(), sqrtf(best_d), to.X, to.Y, to.Z);
}

static void Damage_Target(float amount) {
  SmartGameObj *obj = GameObjManager::Find_SmartGameObj(BroughtId);
  SoldierGameObj *s = obj ? obj->As_SoldierGameObj() : nullptr;
  if (!s) {
    fprintf(stderr, "[debug] no target to damage\n");
    return;
  }
  fprintf(stderr, "[debug] damaging soldier %d by %.0f\n", BroughtId, amount);
  OffenseObjectClass offense(amount, 0, COMBAT_STAR);
  s->Apply_Damage_Extended(offense);
  fprintf(stderr, "[debug] damage applied\n");
}

#include "savegame.h"
#include "commandosaveload.h"
#include "gamemode.h"
#include "god.h"

static void Save_Test_Game(void) {
  fprintf(stderr, "[debug] saving save\\webtest.sav\n");
  SaveGameManager::Set_Description(L"web test");
  SaveGameManager::Save_Game("save\\webtest.sav", &_CommandoSaveLoad, NULL);
  WideStringClass desc, mission;
  bool ok = SaveGameManager::Peek_Description("save\\webtest.sav", desc, mission);
  StringClass map;
  SaveGameManager::Peek_Map_Name("save\\webtest.sav", map);
  fprintf(stderr, "[debug] saved: peek=%d desc='%ls' mission='%ls' map='%s'\n", (int)ok, desc.Peek_Buffer(),
          mission.Peek_Buffer(), map.Peek_Buffer());
}

// As LoadSPGameMenuClass::Load_Game (non-replay path).
static void Load_Test_Game(void) {
  fprintf(stderr, "[debug] loading save\\webtest.sav\n");
  if (GameModeManager::Find("Combat")->Is_Active() || GameModeManager::Find("Combat")->Is_Suspended()) {
    GameInitMgrClass::End_Game();
    GameModeManager::Safely_Deactivate();
  }
  GameInitMgrClass::Initialize_SP();
  GameInitMgrClass::Start_Game("save\\webtest.sav", -1, 0);
  cGod::Reset_Inventory();
}

#include "dialogcontrol.h"
#include <typeinfo>

static void List_Controls(void) {
  DialogBaseClass *dlg = DialogMgrClass::Get_Active_Dialog();
  if (!dlg) {
    fprintf(stderr, "[debug] controls: no active dialog\n");
    return;
  }
  fprintf(stderr, "[debug] controls of dialog %d (%d):\n", dlg->Get_Dlg_ID(), dlg->Get_Control_Count());
  for (int i = 0; i < dlg->Get_Control_Count(); i++) {
    DialogControlClass *c = dlg->Get_Control(i);
    const RectClass &r = c->Get_Window_Rect();
    StringClass text;
    WideStringClass(c->Get_Text()).Convert_To(text);
    fprintf(stderr, "[debug]   ctl %d %s at %.0f,%.0f-%.0f,%.0f %s%s'%s'\n", c->Get_ID(), typeid(*c).name(), r.Left, r.Top,
            r.Right, r.Bottom, c->Is_Enabled() ? "" : "disabled ", c->Is_Visible() ? "" : "hidden ", text.Peek_Buffer());
  }
}

#include "vehicle.h"
#include "objlibrary.h"
#include "pscene.h"
#include "obbox.h"
#include "aabox.h"

static int SpawnedVehicleId = 0;

static void Vehicle_Action(int action) {
  SoldierGameObj *star = COMBAT_STAR;
  if (!star)
    return;
  if (action == 1) {
    Vector3 me;
    star->Get_Position(&me);
    float f = star->Get_Facing();
    Matrix3D tm(true);
    tm.Set_Translation(me + Vector3(cosf(f) * 8.0f, sinf(f) * 8.0f, 4.0f)); // dropped from 4 m: starts clear of the ground
    tm.Rotate_Z(f);
    BaseGameObj *base = ObjectLibraryManager::Create_Object("GDI_Medium_Tank_Player");
    VehicleGameObj *v = base && base->As_PhysicalGameObj() ? base->As_PhysicalGameObj()->As_VehicleGameObj() : nullptr;
    if (!v) {
      fprintf(stderr, "[debug] could not create GDI_Medium_Tank_Player\n");
      return;
    }
    v->Set_Transform(tm);
    v->Start_Observers();
    SpawnedVehicleId = v->Get_ID();

    fprintf(stderr, "[debug] spawned vehicle %d\n", SpawnedVehicleId);
  } else {
    SmartGameObj *o = GameObjManager::Find_SmartGameObj(SpawnedVehicleId);
    VehicleGameObj *v = o ? o->As_VehicleGameObj() : nullptr;
    if (!v) {
      fprintf(stderr, "[debug] no vehicle to enter\n");
      return;
    }
    v->Add_Occupant(star);
    fprintf(stderr, "[debug] entered vehicle %d (driver %p)\n", SpawnedVehicleId, (void *)v->Get_Driver());
  }
}

// The local player's ping as the server measures it (ms; -1 when not in a
// network game). Read by the page's network overlay (web/rebel.js).
#include "playermanager.h"
#include "player.h"
#include "cnetwork.h"
extern "C" int Rebel_My_Ping(void) {
  if (!cNetwork::I_Am_Client())
    return -1;
  cPlayer *me = cPlayerManager::Find_Player(cNetwork::Get_My_Id());
  return me ? me->Get_Ping() : -1;
}

// Other soldiers' position corrections since the last call (soldier.cpp,
// Phys3Class::_NetStats): how often and how far the client was off when a
// server update came. JSON; read by the page's network overlay.
#include "phys3.h"
extern bool Rebel_Net_Smoothing;
extern "C" const char *Rebel_Net_Corrections(void) {
  static char buf[160];
  Phys3Class::NetCorrectionStats &st = Phys3Class::_NetStats;
  snprintf(buf, sizeof(buf), "{\"updates\":%d,\"avg\":%.2f,\"max\":%.2f,\"over1m\":%d,\"pops\":%d,\"jumps\":%d,\"smoothing\":%s}",
           st.count, st.count ? st.sum / st.count : 0.0f, st.max, st.over1, st.overPop, st.jumps, Rebel_Net_Smoothing ? "true" : "false");
  st = {0, 0, 0, 0, 0, 0};
  return buf;
}

// net_smoothing on|off: blend other soldiers' corrections in (on) or jump to
// the server's position as the original game did (off), for comparing.
class NetSmoothingConsoleFunctionClass : public ConsoleFunctionClass {
public:
  const char *Get_Name(void) override { return "net_smoothing"; }
  const char *Get_Help(void) override { return "NET_SMOOTHING on|off - blend other soldiers' network corrections in."; }
  void Activate(const char *input) override {
    Rebel_Net_Smoothing = !(input && (strstr(input, "off") || strstr(input, "0")));
    Print("network smoothing %s\n", Rebel_Net_Smoothing ? "on" : "off");
  }
};

void Rebel_Web_Add_Commands(DynamicVectorClass<ConsoleFunctionClass *> &list) { list.Add(new NetSmoothingConsoleFunctionClass()); }
