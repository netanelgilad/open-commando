// Loads STRINGS.TDB from the retail always.dat through the engine's own
// chunk/save-load code and prints what the translation DB ended up with.
#include "ffactory.h"
#include "mixfile.h"
#include "wwfile.h"
#include "chunkio.h"
#include "saveload.h"
#include "translatedb.h"
#include "translateobj.h"

#include <cstdio>
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
  {
    // Mirror Commando/init.cpp: base factory with sub directories, then mixes.
    static SimpleFileFactoryClass base;
    base.Append_Sub_Directory("DATA\\");
    base.Append_Sub_Directory("SAVE\\");
    base.Append_Sub_Directory("CONFIG\\");
    FileClass *b = base.Get_File("STRINGS.TDB");
    printf("base factory: name='%s' available=%d\n", b->File_Name(), b->Is_Available());
    base.Return_File(b);
    MixFileFactoryClass *m = new MixFileFactoryClass("Always.dat", &base);
    printf("mix via base: valid=%d\n", m->Is_Valid());
    FileClass *mf = m->Get_File("STRINGS.TDB");
    printf("member before open: %p available=%d\n", (void *)mf, mf ? mf->Is_Available() : -1);
    if (mf) m->Return_File(mf);
  }
  MixFileFactoryClass mix("data\\always.dat", _TheFileFactory);
  FileClass *file = mix.Get_File("STRINGS.TDB");
  if (!file || !file->Open(FileClass::READ)) {
    printf("FAIL: STRINGS.TDB not found in always.dat\n");
    return 1;
  }
  printf("STRINGS.TDB size %d, Is_Available=%d\n", file->Size(), file->Is_Available());
  {
    FileClass *g = _TheFileFactory->Get_File("STRINGS.TDB");
    bool opened = g && g->Open(FileClass::READ);
    printf("via _TheFileFactory: opened=%d available=%d size=%d\n", opened, g ? g->Is_Available() : -1, opened ? g->Size() : -1);
    if (g) { g->Close(); _TheFileFactory->Return_File(g); }
  }
  {
    TDBObjClass probe;
    uint32 id = probe.Get_Factory().Chunk_ID();
    printf("TDBObj factory chunk 0x%08x registered=%d\n", id, SaveLoadSystemClass::Find_Persist_Factory(id) != nullptr);
  }
  {
    // Walk the top-level chunks to see what the file contains.
    FileClass *f2 = mix.Get_File("STRINGS.TDB");
    f2->Open(FileClass::READ);
    ChunkLoadClass c2(f2);
    while (c2.Open_Chunk()) {
      printf("  top chunk 0x%08x len %u\n", c2.Cur_Chunk_ID(), c2.Cur_Chunk_Length());
      while (c2.Open_Chunk()) {
        static int shown = 0;
        if (shown++ < 6)
          printf("    sub chunk 0x%08x len %u\n", c2.Cur_Chunk_ID(), c2.Cur_Chunk_Length());
        c2.Close_Chunk();
      }
      c2.Close_Chunk();
    }
    f2->Close();
    mix.Return_File(f2);
  }
  TranslateDBClass::Initialize();
  ChunkLoadClass cload(file);
  bool ok = SaveLoadSystemClass::Load(cload);
  file->Close();
  mix.Return_File(file);
  printf("SaveLoadSystemClass::Load -> %d, loaded=%d, version=%u, objects=%d\n", ok, TranslateDBClass::Is_Loaded(),
         TranslateDBClass::Get_Version_Number(), TranslateDBClass::Get_Object_Count());
  int shown = 0;
  for (int i = 0; i < TranslateDBClass::Get_Object_Count() && shown < 5; i++) {
    TDBObjClass *obj = TranslateDBClass::Get_Object(i);
    if (!obj)
      continue;
    const WideStringClass &s = obj->Get_String();
    printf("  [%d] %s = \"%ls\"\n", i, obj->Get_ID_Desc().Peek_Buffer(), s.Peek_Buffer());
    shown++;
  }
  return TranslateDBClass::Is_Loaded() ? 0 : 1;
}
