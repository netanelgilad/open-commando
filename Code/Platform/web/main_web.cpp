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

int PASCAL WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show);
extern "C" void Rebel_Set_Command_Line(const char *cmd);

int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
  // The OPFS backend spawns its own proxy worker, which may not be started
  // from the browser main thread: mount from a short-lived pthread.
  int mount_result = -1;
  std::thread([&] {
    backend_t opfs = wasmfs_create_opfs_backend();
    mount_result = wasmfs_create_directory("/opfs", 0777, opfs);
  }).join();
  if (mount_result != 0) {
    fprintf(stderr, "[rebel] could not mount OPFS; is this a secure context with cross-origin isolation?\n");
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
    cmd += ' ';
    cmd += argv[i];
  }
  Rebel_Set_Command_Line(cmd.c_str());
  std::string args = cmd.size() > 8 ? cmd.substr(9) : std::string();
  fprintf(stderr, "[rebel] starting Renegade in %s: %s\n", game_dir, cmd.c_str());
  return WinMain((HINSTANCE)GetModuleHandleA(nullptr), nullptr, (LPSTR)args.c_str(), SW_SHOW);
}
