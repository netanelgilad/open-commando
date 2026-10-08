// Compares in-memory sizes of W3D emitter structs with the chunk sizes in
// the retail data, to catch layout differences between MSVC and wasm32.
#include "ffactory.h"
#include "mixfile.h"
#include "wwfile.h"
#include "chunkio.h"
#include "wwstring.h"
#include "vector.h"
#include "w3d_file.h"
#include "part_ldr.h"
#include "proto.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int main() {
#ifdef __EMSCRIPTEN__
  EM_ASM({
    FS.mkdir('/game');
    FS.mount(NODEFS, {root : process.env.REBEL_GAME_DIR}, '/game');
  });
#endif
  chdir("/game");
  std::map<unsigned, std::map<unsigned, int>> seen; // 0 = ok, 0xFFFF = failed by error code
  int emitters = 0;
  printf("sizeof: header %zu user %zu info %zu infoV2 %zu props %zu\n", sizeof(W3dEmitterHeaderStruct),
         sizeof(W3dEmitterUserInfoStruct), sizeof(W3dEmitterInfoStruct), sizeof(W3dEmitterInfoStructV2),
         sizeof(W3dEmitterPropertyStruct));
  const char *archives[] = {"data\\always.dat", "data\\M13.mix", "data\\M01.mix", "data\\M02.mix", "data\\always2.dat"};
  for (const char *archive : archives) {
  MixFileFactoryClass mix(archive, _TheFileFactory);
  DynamicVectorClass<StringClass> list;
  mix.Build_Filename_List(list);
  printf("archive %s: %d files\n", archive, list.Count());

  for (int i = 0; i < list.Count(); i++) {
    const char *n = list[i].Peek_Buffer();
    size_t len = strlen(n);
    if (len < 4 || strcasecmp(n + len - 4, ".w3d"))
      continue;
    FileClass *f = mix.Get_File(n);
    if (!f || !f->Open()) {
      mix.Return_File(f);
      continue;
    }
    ChunkLoadClass c(f);
    while (c.Open_Chunk()) {
      if (c.Cur_Chunk_ID() == W3D_CHUNK_EMITTER) {
        emitters++;
        ParticleEmitterDefClass def;
        WW3DErrorType err = def.Load_W3D(c);
        if (err != WW3D_ERROR_OK) {
          static int shown = 0;
          if (shown++ < 5)
            printf("  emitter in %s failed: error %d\n", n, (int)err);
          seen[0xFFFF][err]++;
        } else {
          seen[0][0]++;
        }
      }
      c.Close_Chunk();
    }
    f->Close();
    mix.Return_File(f);
  }
  }
  printf("emitters scanned: %d\n", emitters);
  for (auto &k : seen) {
    printf("  chunk 0x%04x sizes:", k.first);
    int shown = 0;
    for (auto &s : k.second)
      if (shown++ < 6)
        printf(" %u(x%d)", s.first, s.second);
    printf("\n");
  }
}
