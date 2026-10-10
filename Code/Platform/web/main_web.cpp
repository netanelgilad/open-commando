/*
** Browser entry point for the Renegade build.
**
** Filesystem (WasmFS):
**   /opfs                 Origin Private File System (persistent, per origin)
**   /opfs/renegade        the user's game files, imported once by the page
**   /opfs/persist         registry (settings) and other writable state
**   /rebel/chat.res       dialog resources compiled from Commando/chat.rc
** The page (web/index.html) fills /opfs/renegade from the user's own install
** before calling main. Everything else is the unmodified WinMain.
**
** The same build also runs under Node as a headless dedicated server
** (tools/server/run-server.mjs): there /opfs is a host directory given with
** --root=<dir> (holding renegade/ and persist/) instead of the OPFS.
*/
#include "windows.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/wasmfs.h>
#endif

#include "registry.h"
#include "_globals.h"
#include "dx8wrapper.h"

int PASCAL WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show);

// The Windows game takes its resolution from the registry, written by the
// separate WWConfig tool. Here the page chooses it (--res=WxH, from the
// window size or ?res=) and it is stored the same way before WinMain runs.
static void Store_Resolution(int w, int h) {
  RegistryClass reg(APPLICATION_SUB_KEY_NAME_RENDER);
  if (!reg.Is_Valid())
    return;
  reg.Set_String(VALUE_NAME_RENDER_DEVICE_NAME, "WebGL 2.0 (Rebel D3D9 fixed-function)");
  reg.Set_Int(VALUE_NAME_RENDER_DEVICE_WIDTH, w);
  reg.Set_Int(VALUE_NAME_RENDER_DEVICE_HEIGHT, h);
  reg.Set_Int(VALUE_NAME_RENDER_DEVICE_DEPTH, 32);
  reg.Set_Int(VALUE_NAME_RENDER_DEVICE_WINDOWED, 1);
  reg.Set_Int(VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, 32);
  fprintf(stderr, "[rebel] resolution %dx%d\n", w, h);
}
extern "C" void Rebel_Set_Command_Line(const char *cmd);

#include "wwprofile.h"
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
#endif

extern "C" volatile unsigned Rebel_Heartbeat;
extern "C" const char *volatile Rebel_Phase;

// Hang watchdog: if the main thread neither yields a frame nor pumps messages
// for 10 s, report where it is (the engine's WWPROFILE scope chain, read
// racily) from this thread, which keeps running while the main thread is
// stuck. Reports again every 30 s while the stall lasts.
static void Hang_Watchdog() {
  unsigned last = Rebel_Heartbeat;
  double since = emscripten_get_now(); // when the heartbeat last changed
  double next_report = 10000.0;        // ms of stall before the (next) report
  for (;;) {
    sleep(1); // may oversleep on throttled machines: time is measured below
    unsigned now = Rebel_Heartbeat;
    double stalled = emscripten_get_now() - since;
    if (now != last) {
      if (next_report > 10000.0)
        emscripten_console_errorf("[hang] main thread resumed after %.0f s", stalled / 1000.0);
      last = now;
      since = emscripten_get_now();
      next_report = 10000.0;
      continue;
    }
    if (stalled >= next_report) {
      next_report = stalled + 30000.0;
      char chain[1024] = "";
      size_t len = 0;
      const WWProfileHierachyNodeClass *n = WWProfileManager::Peek_Current_Node();
      for (int depth = 0; n && depth < 40; depth++, n = n->Get_Parent()) {
        const char *name = n->Get_Name() ? n->Get_Name() : "?";
        int w = snprintf(chain + len, sizeof(chain) - len, "%s%s", depth ? " <- " : "", name);
        if (w < 0 || (size_t)w >= sizeof(chain) - len)
          break;
        len += (size_t)w;
      }
      // Printed from this thread directly: stderr from threads is relayed
      // through the main thread, which is the one that's stuck.
      char line[1300];
      snprintf(line, sizeof(line), "[hang] main thread stuck for %.0f s; last step: %s; scopes: %s", stalled / 1000.0,
               Rebel_Phase, chain);
      emscripten_console_error(line);
    }
  }
}

int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
  const char *host_root = nullptr; // Node: a host directory instead of the OPFS
  for (int i = 1; i < argc; i++)
    if (strncmp(argv[i], "--root=", 7) == 0)
      host_root = argv[i] + 7;
  int mount_result = -1;
  if (host_root && EM_ASM_INT({ return typeof process === 'object' && !!process.versions && !!process.versions.node; })) {
    mount_result = wasmfs_create_directory("/opfs", 0777, wasmfs_create_node_backend(host_root));
  } else {
    // The OPFS backend spawns its own proxy worker, which may not be started
    // from the browser main thread: mount from a short-lived pthread.
    std::thread([&] {
      backend_t opfs = wasmfs_create_opfs_backend();
      mount_result = wasmfs_create_directory("/opfs", 0777, opfs);
    }).join();
  }
  if (mount_result != 0) {
    fprintf(stderr, host_root ? "[rebel] could not mount %s\n"
                              : "[rebel] could not mount OPFS; is this a secure context with cross-origin isolation?\n",
            host_root);
    return 1;
  }
#endif
  const char *game_dir = getenv("REBEL_GAME_DIR");
  if (!game_dir)
    setenv("REBEL_GAME_DIR", game_dir = "/opfs/renegade", 0);
  mkdir("/opfs/persist", 0777);
  setenv("REBEL_REGISTRY_PATH", "/opfs/persist/registry.txt", 0);
  setenv("REBEL_RESOURCE_FILE", "/rebel/chat.res", 0);

  struct stat st;
  if (stat(game_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[rebel] game data not found at %s\n", game_dir);
    return 2;
  }
  if (chdir(game_dir) != 0) {
    fprintf(stderr, "[rebel] chdir(%s) failed\n", game_dir);
    return 2;
  }

  // Command line: Game.exe plus whatever the page passed (e.g. -nointro).
  std::string cmd = "Game.exe";
  for (int i = 1; i < argc; i++) {
    int w = 0, h = 0;
    if (strncmp(argv[i], "--root=", 7) == 0)
      continue;
    if (sscanf(argv[i], "--res=%dx%d", &w, &h) == 2) {
      if (w >= 640 && h >= 480 && w <= 7680 && h <= 4320)
        Store_Resolution(w, h);
      continue;
    }
    cmd += ' ';
    cmd += argv[i];
  }
  Rebel_Set_Command_Line(cmd.c_str());
#ifdef __EMSCRIPTEN__
  WWProfileManager::Track_Scopes_Only(); // main thread scope chain for the watchdog
  std::thread(Hang_Watchdog).detach();
#endif
  std::string args = cmd.size() > 8 ? cmd.substr(9) : std::string();
  fprintf(stderr, "[rebel] starting Renegade in %s: %s\n", game_dir, cmd.c_str());
  return WinMain((HINSTANCE)GetModuleHandleA(nullptr), nullptr, (LPSTR)args.c_str(), SW_SHOW);
}
