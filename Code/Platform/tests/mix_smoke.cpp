// Smoke test for the portable platform layer: opens the retail archives
// through the engine's own MixFileFactoryClass using Windows-style paths.
// Run under node with the game directory mounted at /game (NODEFS).
#include "ffactory.h"
#include "wwfile.h"
#include "mixfile.h"
#include "wwstring.h"
#include "vector.h"

#include <cstdio>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (typeof process === 'undefined') return;
    var dir = process.env.REBEL_GAME_DIR;
    if (!dir) throw new Error("set REBEL_GAME_DIR to the Renegade install");
    FS.mkdir('/game');
    FS.mount(NODEFS, {root : dir}, '/game');
  });
#endif
  chdir("/game");
  const char *archives[] = {"data\\ALWAYS.DAT", "DATA\\always2.dat", "Data\\M01.MIX"};
  int failures = 0;
  {
    // Layer check: libc path resolution, then the engine's file factory.
    FILE *raw = fopen("data\\ALWAYS.DAT", "rb");
    char magic[5] = {0};
    if (raw) {
      fread(magic, 1, 4, raw);
      fclose(raw);
    }
    printf("%s fopen(data\\ALWAYS.DAT) magic=%s\n", raw ? "OK  " : "FAIL", magic);
    FileClass *f = _TheFileFactory->Get_File("data\\ALWAYS.DAT");
    bool opened = f && f->Open();
    printf("%s factory open, size %d\n", opened ? "OK  " : "FAIL", opened ? f->Size() : -1);
    if (opened)
      f->Close();
    _TheFileFactory->Return_File(f);
    if (!raw || !opened)
      failures++;
  }
  for (const char *name : archives) {
    MixFileFactoryClass mix(name, _TheFileFactory);
    if (!mix.Is_Valid()) {
      printf("FAIL %s: not a valid MIX archive\n", name);
      failures++;
      continue;
    }
    DynamicVectorClass<StringClass> list;
    mix.Build_Filename_List(list);
    printf("OK   %-20s %5d files", name, list.Count());
    for (int i = 0; i < list.Count() && i < 3; i++)
      printf("  %s", list[i].Peek_Buffer());
    printf("\n");
    if (list.Count() == 0)
      failures++;
    // Read one member back through the factory interface.
    if (list.Count() > 0) {
      FileClass *f = mix.Get_File(list[0].Peek_Buffer());
      if (f && f->Open()) {
        printf("     first member '%s' size %d\n", list[0].Peek_Buffer(), f->Size());
        f->Close();
      } else {
        printf("FAIL could not open member %s\n", list[0].Peek_Buffer());
        failures++;
      }
      mix.Return_File(f);
    }
  }
  printf(failures ? "SMOKE FAILED (%d)\n" : "SMOKE PASSED\n", failures);
  return failures ? 1 : 0;
}
