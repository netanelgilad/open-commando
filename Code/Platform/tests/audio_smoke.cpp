// Decodes every .wav and .mp3 inside the retail archives with the Rebel
// audio decoders (IMA ADPCM, PCM, minimp3) and reports failures.
#include "ffactory.h"
#include "mixfile.h"
#include "wwfile.h"
#include "wwstring.h"
#include "vector.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern "C" int Rebel_Audio_Decode_Test(const void *image, unsigned size, int is_mp3, int *frames, int *rate,
                                       int *channels, int *peak);

static bool Ends_With(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

int main() {
#ifdef __EMSCRIPTEN__
  EM_ASM({
    FS.mkdir('/game');
    FS.mount(NODEFS, {root : process.env.REBEL_GAME_DIR}, '/game');
  });
#endif
  chdir("/game");
  MixFileFactoryClass mix("data\\always.dat", _TheFileFactory);
  DynamicVectorClass<StringClass> list;
  mix.Build_Filename_List(list);
  int wav_ok = 0, wav_bad = 0, mp3_ok = 0, mp3_bad = 0, silent = 0;
  long long total_frames = 0;
  std::vector<char> data;
  for (int i = 0; i < list.Count(); i++) {
    const char *name = list[i].Peek_Buffer();
    bool is_wav = Ends_With(name, ".wav"), is_mp3 = Ends_With(name, ".mp3");
    if (!is_wav && !is_mp3)
      continue;
    FileClass *f = mix.Get_File(name);
    if (!f || !f->Open()) {
      mix.Return_File(f);
      continue;
    }
    data.resize(f->Size());
    f->Read(data.data(), (int)data.size());
    f->Close();
    mix.Return_File(f);
    int frames = 0, rate = 0, ch = 0, peak = 0;
    int ok = Rebel_Audio_Decode_Test(data.data(), (unsigned)data.size(), is_mp3, &frames, &rate, &ch, &peak);
    if (ok && frames > 0) {
      (is_mp3 ? mp3_ok : wav_ok)++;
      total_frames += frames;
      if (peak < 64)
        silent++;
      if (is_mp3 && mp3_ok <= 3)
        printf("  mp3 %-30s %d Hz %d ch %.1f s peak %d\n", name, rate, ch, frames / (double)rate, peak);
    } else {
      (is_mp3 ? mp3_bad : wav_bad)++;
      if (wav_bad + mp3_bad <= 10)
        printf("  FAIL %s (%zu bytes)\n", name, data.size());
    }
  }
  printf("wav: %d ok, %d failed | mp3: %d ok, %d failed | near-silent: %d | %.1f hours decoded\n", wav_ok, wav_bad,
         mp3_ok, mp3_bad, silent, total_frames / 22050.0 / 3600.0);
  bool pass = wav_bad == 0 && mp3_bad == 0 && wav_ok > 0;
  printf(pass ? "AUDIO SMOKE PASSED\n" : "AUDIO SMOKE FAILED\n");
  return pass ? 0 : 1;
}
