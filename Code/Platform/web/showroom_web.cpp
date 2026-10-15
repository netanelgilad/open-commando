// The online front screen ("showroom"): the game boots straight to it
// (--showroom) and shows a soldier from the game's own assets, idling under
// a slowly orbiting camera, behind the page's Play / Play online buttons
// (web/rebel.js). Its own small scene (model, lights, camera), drawn instead
// of the main menu while it is up.
//
//   Rebel_Showroom(on, model)            show / leave (back to the main menu)
//   Rebel_Online_Connect(server, name)   join a match server ("ip:port") the
//                                        way +connect does (GameSpy launch)
// Both are queued and run between frames (Rebel_Run_Pending_Actions).
#include "showroom_web.h"

#include "camera.h"
#include "ww3d.h"
#include "scene.h"
#include "light.h"
#include "rendobj.h"
#include "assetmgr.h"
#include "hanim.h"
#include "render2d.h"
#include "wwmath.h"
#include "timemgr.h"
#include "gamespyadmin.h"
#include "useroptions.h"
#include "dialogtests.h"

#include <arpa/inet.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <string>

namespace {
bool Active = false;
bool PendingSet = false, PendingOn = false;
std::string PendingModel;
bool PendingConnect = false;
std::string ConnectServer, ConnectName;
SimpleSceneClass *Scene = nullptr;
CameraClass *Camera = nullptr;
RenderObjClass *Model = nullptr;
HAnimClass *Anim = nullptr;
double Started = 0;

const char *DEFAULT_MODEL = "C_AG_HAVOC";
const char *IDLE_ANIM = "S_A_HUMAN.H_A_A0A0";

void Add_Light(const Vector3 &pos, const Vector3 &color) {
  LightClass *light = new LightClass;
  light->Set_Position(pos);
  light->Set_Intensity(1.0f);
  light->Set_Force_Visible(true);
  light->Set_Flag(LightClass::NEAR_ATTENUATION, false);
  light->Set_Far_Attenuation_Range(1000, 1000);
  light->Set_Ambient(Vector3(0, 0, 0));
  light->Set_Diffuse(color);
  light->Set_Specular(color * 0.5f);
  Scene->Add_Render_Object(light);
  REF_PTR_RELEASE(light);
}

void Free_Scene(void) {
  if (Model)
    Model->Remove();
  REF_PTR_RELEASE(Model);
  REF_PTR_RELEASE(Anim);
  REF_PTR_RELEASE(Camera);
  REF_PTR_RELEASE(Scene);
}

void Enter(const std::string &model_name) {
  const char *name = model_name.empty() ? DEFAULT_MODEL : model_name.c_str();
  Free_Scene();
  Scene = new SimpleSceneClass;
  Scene->Set_Ambient_Light(Vector3(0.32f, 0.33f, 0.38f));
  Add_Light(Vector3(3.0f, -3.5f, 3.5f), Vector3(1.0f, 0.93f, 0.82f)); // key: warm, front-left
  Add_Light(Vector3(-3.0f, 3.0f, 2.8f), Vector3(0.40f, 0.55f, 1.0f)); // rim: cool, behind
  Model = WW3DAssetManager::Get_Instance()->Create_Render_Obj(name);
  if (!Model) {
    fprintf(stderr, "[showroom] model %s not found\n", name);
    Free_Scene();
    return;
  }
  Anim = WW3DAssetManager::Get_Instance()->Get_HAnim(IDLE_ANIM);
  if (Anim)
    Model->Set_Animation(Anim, 0, RenderObjClass::ANIM_MODE_LOOP);
  Model->Set_Transform(Matrix3D(true));
  Scene->Add_Render_Object(Model);
  Camera = new CameraClass();
  const RectClass &screen = Render2DClass::Get_Screen_Resolution();
  float hfov = DEG_TO_RAD(40.0f);
  Camera->Set_View_Plane(hfov, (screen.Height() / screen.Width()) * hfov);
  Camera->Set_Clip_Planes(0.05f, 200.0f);
  Started = emscripten_get_now();
  Active = true;
  fprintf(stderr, "[showroom] on (%s)\n", name);
}

void Leave(void) {
  if (!Active)
    return;
  Active = false;
  Free_Scene();
  fprintf(stderr, "[showroom] off\n");
}
} // namespace

bool Rebel_Showroom_Startup = false;

bool Rebel_Showroom_Active(void) { return Active; }

void Rebel_Showroom_Enter_Now(void) { Enter(DEFAULT_MODEL); }

// The camera sways slowly around a front-left view of the whole soldier,
// who stands left of center (the page's buttons are on the right).
void Rebel_Showroom_Render(void) {
  if (!Scene || !Camera)
    return;
  double t = (emscripten_get_now() - Started) / 1000.0;
  float a = (float)(-0.45 + 0.30 * sin(t * 0.18));
  const float dist = 5.4f;
  Vector3 focus(0.0f, 0.0f, 0.92f);
  Vector3 eye(focus.X + dist * cosf(a), focus.Y + dist * sinf(a), 1.45f);
  // Aim a little to the soldier's right, so he sits left of center on screen.
  Vector3 side(-sinf(a), cosf(a), 0.0f);
  Matrix3D tm;
  tm.Look_At(eye, focus + side * 0.38f, 0.0f);
  Camera->Set_Transform(tm);
  WW3D::Render(Scene, Camera, true, true, Vector3(0.05f, 0.06f, 0.08f));
}

void Rebel_Showroom_Run_Pending(void) {
  if (PendingSet) {
    PendingSet = false;
    if (PendingOn)
      Enter(PendingModel);
    else
      Leave();
  }
  if (PendingConnect) {
    PendingConnect = false;
    // What +connect <ip:port> +netplayername <name> does on the command line
    // (cUserOptions::Parse_Command_Line): the GameSpy launch path joins the
    // server directly, with no lobby screens.
    std::string host = ConnectServer, port = "4848";
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
      port = host.substr(colon + 1);
      host = host.substr(0, colon);
    }
    Leave();
    cUserOptions::GameSpyNickname.Set(ConnectName.c_str());
    cGameSpyAdmin::Set_Game_Host_Ip(inet_addr(host.c_str()));
    cGameSpyAdmin::Set_Game_Host_Port((unsigned short)atoi(port.c_str()));
    SplashIntroMenuDialogClass::Set_Complete(true);
    cGameSpyAdmin::Set_Is_Launch_From_Gamespy_Requested(true);
    fprintf(stderr, "[showroom] connecting to %s:%s as %s\n", host.c_str(), port.c_str(), ConnectName.c_str());
  }
}

extern "C" void Rebel_Showroom(int on, const char *model) {
  PendingSet = true;
  PendingOn = on != 0;
  PendingModel = model ? model : "";
}

extern "C" void Rebel_Online_Connect(const char *server, const char *name) {
  PendingConnect = true;
  ConnectServer = server ? server : "";
  ConnectName = name ? name : "Player";
}
