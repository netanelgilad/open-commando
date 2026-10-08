/*
** Rebel audio backend: the Miles-style API from compat/mss.h on OpenAL.
**
** - WAV images (PCM 8/16-bit, IMA ADPCM) and MP3 (minimp3) are decoded to
**   16-bit PCM once and cached as AL buffers keyed by image contents.
** - 2D samples are AL sources relative to the listener; pan is done by
**   placing mono sources on a unit half-circle in front of the listener.
** - 3D samples are world-space sources with a clamped linear falloff
**   between the min and max distances WWAudio supplies.
** - Streams (music, long dialogue) are decoded incrementally into a small
**   queue of AL buffers, refilled by Rebel_Audio_Update() every frame.
**
** Miles' 3D space is left-handed (z forward); OpenAL is right-handed
** (-z forward), so z is negated on the way in.
**
** OpenAL calls must happen on the main runtime thread under Emscripten.
** Releases requested from other threads are deferred to the next update.
*/
#include "mss.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

/* ---------------------------------------------------------- decoded audio */

namespace {

struct Pcm {
  std::vector<int16_t> samples; // interleaved
  int channels = 1;
  int rate = 22050;
  int Frames() const { return channels ? (int)(samples.size() / channels) : 0; }
};

struct WavInfo {
  int format = 0, channels = 0, rate = 0, bits = 0, block_align = 0;
  const uint8_t *data = nullptr;
  uint32_t data_len = 0;
  uint32_t file_len = 0;
  uint32_t samples_per_block = 0;
};

uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

bool Parse_Wav(const void *image, uint32_t size_hint, WavInfo &w) {
  const uint8_t *p = (const uint8_t *)image;
  if (!p || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
    return false;
  uint32_t riff_len = Rd32(p + 4) + 8;
  w.file_len = size_hint ? std::min(size_hint, riff_len) : riff_len;
  uint32_t off = 12;
  bool have_fmt = false;
  while (off + 8 <= w.file_len) {
    const uint8_t *c = p + off;
    uint32_t len = Rd32(c + 4);
    if (memcmp(c, "fmt ", 4) == 0 && len >= 16) {
      w.format = Rd16(c + 8);
      w.channels = Rd16(c + 10);
      w.rate = (int)Rd32(c + 12);
      w.block_align = Rd16(c + 20);
      w.bits = Rd16(c + 22);
      if (w.format == WAVE_FORMAT_IMA_ADPCM && len >= 20)
        w.samples_per_block = Rd16(c + 26);
      have_fmt = true;
    } else if (memcmp(c, "data", 4) == 0) {
      w.data = c + 8;
      w.data_len = std::min(len, w.file_len - (off + 8));
      break;
    }
    off += 8 + len + (len & 1);
  }
  if (have_fmt && w.format == WAVE_FORMAT_IMA_ADPCM && !w.samples_per_block && w.channels)
    w.samples_per_block = (uint32_t)((w.block_align - 4 * w.channels) * 8 / (4 * w.channels) + 1);
  return have_fmt && w.data && w.channels > 0;
}

// IMA ADPCM, as written by the Windows ACM codec (format tag 0x11).
const int ImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int ImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct ImaState {
  int predictor = 0;
  int index = 0;
  int16_t Step(int nibble) {
    int step = ImaStepTable[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    predictor = std::clamp(predictor + diff, -32768, 32767);
    index = std::clamp(index + ImaIndexTable[nibble & 15], 0, 88);
    return (int16_t)predictor;
  }
};

void Decode_Ima(const WavInfo &w, Pcm &out) {
  const int ch = w.channels;
  const uint32_t block = (uint32_t)w.block_align;
  const uint32_t spb = w.samples_per_block;
  if (!block || !spb || ch > 2)
    return;
  out.samples.reserve((size_t)(w.data_len / block + 1) * spb * ch);
  std::vector<int16_t> frame((size_t)spb * ch);
  for (uint32_t off = 0; off + 4u * ch <= w.data_len; off += block) {
    const uint8_t *b = w.data + off;
    uint32_t avail = std::min(block, w.data_len - off);
    ImaState st[2];
    for (int c = 0; c < ch; c++) {
      st[c].predictor = (int16_t)Rd16(b + 4 * c);
      st[c].index = std::clamp((int)b[4 * c + 2], 0, 88);
      frame[c] = (int16_t)st[c].predictor;
    }
    // After the headers, data comes in 4-byte groups per channel (8 samples).
    uint32_t pos = 4u * ch;
    uint32_t produced = 1;
    while (produced < spb && pos + 4u * ch <= avail) {
      for (int c = 0; c < ch; c++) {
        for (int i = 0; i < 4; i++) {
          uint8_t byte = b[pos + c * 4 + i];
          uint32_t s0 = produced + i * 2, s1 = s0 + 1;
          int16_t v0 = st[c].Step(byte & 0x0F);
          int16_t v1 = st[c].Step(byte >> 4);
          if (s0 < spb) frame[s0 * ch + c] = v0;
          if (s1 < spb) frame[s1 * ch + c] = v1;
        }
      }
      pos += 4u * ch;
      produced += 8;
    }
    uint32_t frames = std::min(produced, spb);
    out.samples.insert(out.samples.end(), frame.begin(), frame.begin() + (size_t)frames * ch);
  }
}

bool Decode_Wav(const void *image, uint32_t size, Pcm &out) {
  WavInfo w;
  if (!Parse_Wav(image, size, w))
    return false;
  out.channels = w.channels;
  out.rate = w.rate;
  if (w.data_len == 0) {
    // Some retail files (e.g. wind4r.wav) carry an empty data chunk: play silence.
    out.samples.assign((size_t)w.channels, 0);
    return true;
  }
  if (w.format == WAVE_FORMAT_PCM && w.bits == 16) {
    out.samples.resize(w.data_len / 2);
    memcpy(out.samples.data(), w.data, out.samples.size() * 2);
  } else if (w.format == WAVE_FORMAT_PCM && w.bits == 8) {
    out.samples.resize(w.data_len);
    for (uint32_t i = 0; i < w.data_len; i++)
      out.samples[i] = (int16_t)(((int)w.data[i] - 128) << 8);
  } else if (w.format == WAVE_FORMAT_IMA_ADPCM) {
    Decode_Ima(w, out);
  } else {
    fprintf(stderr, "[audio] unsupported WAV format tag %d\n", w.format);
    return false;
  }
  return !out.samples.empty();
}

bool Looks_Like_Mp3(const void *image, uint32_t size) {
  const uint8_t *p = (const uint8_t *)image;
  if (size < 3)
    return false;
  if (memcmp(p, "ID3", 3) == 0)
    return true;
  return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0;
}

bool Decode_Mp3(const void *image, uint32_t size, Pcm &out) {
  mp3dec_t dec;
  mp3dec_file_info_t info;
  memset(&info, 0, sizeof(info));
  if (mp3dec_load_buf(&dec, (const uint8_t *)image, size, &info, nullptr, nullptr) != 0 || !info.samples) {
    free(info.buffer);
    return false;
  }
  out.channels = info.channels;
  out.rate = info.hz;
  out.samples.assign(info.buffer, info.buffer + info.samples);
  free(info.buffer);
  return true;
}

ALenum Al_Format(int channels) { return channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16; }

/* -------------------------------------------------------- buffer cache */

struct CachedBuffer {
  ALuint buffer = 0;
  int rate = 22050;
  int channels = 1;
  int frames = 0;
  size_t bytes = 0;
  unsigned last_use = 0;
  int users = 0;
};

struct CacheKey {
  const void *ptr;
  uint32_t size;
  uint32_t hash;
  bool operator<(const CacheKey &o) const {
    if (ptr != o.ptr) return ptr < o.ptr;
    if (size != o.size) return size < o.size;
    return hash < o.hash;
  }
};

uint32_t Hash_Prefix(const void *image, uint32_t size) {
  // FNV-1a over the first 256 bytes: distinguishes reused allocations.
  const uint8_t *p = (const uint8_t *)image;
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < std::min<uint32_t>(size, 256); i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

/* ------------------------------------------------------------- objects */

struct Sample {
  ALuint source = 0;
  bool is_3d = false;
  bool is_listener = false;
  CacheKey key{};
  CachedBuffer *buf = nullptr;
  S32 volume = 127;
  S32 pan = 64;
  S32 rate = 0; // 0 = native
  S32 loop_count = 1;
  S32 user_data[8] = {};
  F32 effects_level = 0;
  bool released = false;
};

struct Stream {
  ALuint source = 0;
  std::vector<uint8_t> file;
  bool is_mp3 = false;
  mp3dec_ex_t mp3;
  bool mp3_open = false;
  Pcm wav; // WAV streams are decoded fully
  size_t wav_pos = 0;
  int channels = 2, rate = 44100;
  uint64_t total_frames = 0;
  uint64_t frames_queued = 0; // position (frames) of the end of the queue
  uint64_t frames_played_base = 0;
  ALuint buffers[4] = {};
  S32 volume = 127, pan = 64, rate_override = 0, loop_count = 1;
  bool playing = false, paused = false, finished = false;
  bool released = false;
};

// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::recursive_mutex &Lock_Instance() {
  static std::recursive_mutex *instance = new std::recursive_mutex();
  return *instance;
}
#define Lock Lock_Instance()
ALCdevice *Device = nullptr;
ALCcontext *Context = nullptr;
RebelAudioDriver Driver2D = {0, nullptr};
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::set<Sample *> &Samples_Instance() {
  static std::set<Sample *> *instance = new std::set<Sample *>();
  return *instance;
}
#define Samples Samples_Instance()
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::set<Stream *> &Streams_Instance() {
  static std::set<Stream *> *instance = new std::set<Stream *>();
  return *instance;
}
#define Streams Streams_Instance()
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::map<CacheKey, CachedBuffer> &Cache_Instance() {
  static std::map<CacheKey, CachedBuffer> *instance = new std::map<CacheKey, CachedBuffer>();
  return *instance;
}
#define Cache Cache_Instance()
size_t CacheBytes = 0;
unsigned UseClock = 0;
const size_t CacheLimit = 192u << 20;
Sample Listener;
char LastError[256] = "";
AIL_file_open_callback FileOpen = nullptr;
AIL_file_close_callback FileClose = nullptr;
AIL_file_seek_callback FileSeek = nullptr;
AIL_file_read_callback FileRead = nullptr;

bool On_Main_Thread() {
#ifdef __EMSCRIPTEN__
  return emscripten_is_main_runtime_thread();
#else
  return true;
#endif
}

void Set_Error(const char *msg) { snprintf(LastError, sizeof(LastError), "%s", msg); }

bool Ensure_Device() {
  if (Context)
    return true;
  Device = alcOpenDevice(nullptr);
  if (!Device) {
    Set_Error("alcOpenDevice failed");
    return false;
  }
  Context = alcCreateContext(Device, nullptr);
  if (!Context || !alcMakeContextCurrent(Context)) {
    Set_Error("alcCreateContext failed");
    return false;
  }
  alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
  alListener3f(AL_POSITION, 0, 0, 0);
  ALfloat orient[6] = {0, 0, -1, 0, 1, 0};
  alListenerfv(AL_ORIENTATION, orient);
  return true;
}

void Evict_Cache() {
  while (CacheBytes > CacheLimit) {
    auto victim = Cache.end();
    for (auto it = Cache.begin(); it != Cache.end(); ++it)
      if (it->second.users == 0 && (victim == Cache.end() || it->second.last_use < victim->second.last_use))
        victim = it;
    if (victim == Cache.end())
      return;
    alDeleteBuffers(1, &victim->second.buffer);
    CacheBytes -= victim->second.bytes;
    Cache.erase(victim);
  }
}

CachedBuffer *Load_Buffer(const void *image, uint32_t size, bool mp3_hint, CacheKey &key_out) {
  if (!image)
    return nullptr;
  WavInfo w;
  bool is_wav = Parse_Wav(image, size, w);
  if (is_wav && !size)
    size = w.file_len;
  if (!size)
    return nullptr;
  CacheKey key{image, size, Hash_Prefix(image, size)};
  key_out = key;
  auto it = Cache.find(key);
  if (it != Cache.end()) {
    it->second.last_use = ++UseClock;
    return &it->second;
  }
  Pcm pcm;
  bool ok = is_wav ? Decode_Wav(image, size, pcm)
                   : ((mp3_hint || Looks_Like_Mp3(image, size)) && Decode_Mp3(image, size, pcm));
  if (!ok) {
    Set_Error("could not decode sample");
    return nullptr;
  }
  CachedBuffer cb;
  alGenBuffers(1, &cb.buffer);
  alBufferData(cb.buffer, Al_Format(pcm.channels), pcm.samples.data(), (ALsizei)(pcm.samples.size() * 2), pcm.rate);
  cb.rate = pcm.rate;
  cb.channels = pcm.channels;
  cb.frames = pcm.Frames();
  cb.bytes = pcm.samples.size() * 2;
  cb.last_use = ++UseClock;
  CacheBytes += cb.bytes;
  auto &slot = Cache[key];
  slot = cb;
  Evict_Cache();
  return &Cache[key];
}

float Gain(S32 volume) { return std::clamp(volume, 0, 127) / 127.0f; }

void Apply_Pan(ALuint src, S32 pan) {
  // Mono sources only; stereo sources ignore position in OpenAL.
  float a = (std::clamp(pan, 0, 127) - 64) / 64.0f * 1.5707963f;
  alSource3f(src, AL_POSITION, std::sin(a), 0.0f, -std::cos(a));
}

void Apply_Pitch(ALuint src, S32 rate, int native) {
  float pitch = (rate > 0 && native > 0) ? (float)rate / (float)native : 1.0f;
  alSourcef(src, AL_PITCH, std::clamp(pitch, 0.25f, 4.0f));
}

void Detach(Sample *s) {
  if (s->source) {
    alSourceStop(s->source);
    alSourcei(s->source, AL_BUFFER, 0);
  }
  if (s->buf)
    s->buf->users--;
  s->buf = nullptr;
}

void Destroy_Sample(Sample *s) {
  Detach(s);
  if (s->source)
    alDeleteSources(1, &s->source);
  Samples.erase(s);
  delete s;
}

void Destroy_Stream(Stream *st);

void Flush_Releases() {
  std::vector<Sample *> dead;
  for (Sample *s : Samples)
    if (s->released)
      dead.push_back(s);
  for (Sample *s : dead)
    Destroy_Sample(s);
  std::vector<Stream *> dead_streams;
  for (Stream *st : Streams)
    if (st->released)
      dead_streams.push_back(st);
  for (Stream *st : dead_streams)
    Destroy_Stream(st);
}

Sample *New_Sample(bool is_3d) {
  if (!Ensure_Device())
    return nullptr;
  Sample *s = new Sample();
  s->is_3d = is_3d;
  alGenSources(1, &s->source);
  alSourcei(s->source, AL_SOURCE_RELATIVE, is_3d ? AL_FALSE : AL_TRUE);
  if (!is_3d)
    alSourcef(s->source, AL_ROLLOFF_FACTOR, 0.0f);
  Samples.insert(s);
  return s;
}

bool Attach(Sample *s, const void *image, uint32_t size, bool mp3_hint) {
  Detach(s);
  CacheKey key;
  CachedBuffer *b = Load_Buffer(image, size, mp3_hint, key);
  if (!b)
    return false;
  s->buf = b;
  s->key = key;
  b->users++;
  alSourcei(s->source, AL_BUFFER, (ALint)b->buffer);
  alSourcef(s->source, AL_GAIN, Gain(s->volume));
  Apply_Pitch(s->source, s->rate, b->rate);
  if (!s->is_3d)
    Apply_Pan(s->source, b->channels == 1 ? s->pan : 64);
  return true;
}

/* ------------------------------------------------------------- streams */

const int StreamChunkFrames = 8192;

size_t Stream_Decode(Stream *st, int16_t *out, size_t frames) {
  if (st->is_mp3) {
    size_t got = mp3dec_ex_read(&st->mp3, out, frames * st->channels);
    return got / st->channels;
  }
  size_t avail = (st->wav.samples.size() - st->wav_pos) / st->channels;
  size_t n = std::min(avail, frames);
  memcpy(out, st->wav.samples.data() + st->wav_pos, n * st->channels * 2);
  st->wav_pos += n * st->channels;
  return n;
}

void Stream_Rewind(Stream *st, uint64_t frame) {
  if (st->is_mp3)
    mp3dec_ex_seek(&st->mp3, frame * st->channels);
  else
    st->wav_pos = std::min<size_t>(frame * st->channels, st->wav.samples.size());
}

// Decodes and queues one chunk; handles looping. Returns false at the end.
bool Stream_Queue_One(Stream *st, ALuint buf) {
  std::vector<int16_t> pcm((size_t)StreamChunkFrames * st->channels);
  size_t got = Stream_Decode(st, pcm.data(), StreamChunkFrames);
  if (got == 0 && st->loop_count != 1) {
    if (st->loop_count > 1)
      st->loop_count--;
    Stream_Rewind(st, 0);
    got = Stream_Decode(st, pcm.data(), StreamChunkFrames);
  }
  if (got == 0)
    return false;
  alBufferData(buf, Al_Format(st->channels), pcm.data(), (ALsizei)(got * st->channels * 2), st->rate);
  alSourceQueueBuffers(st->source, 1, &buf);
  st->frames_queued += got;
  return true;
}

void Stream_Service(Stream *st) {
  if (!st->playing || st->paused || st->finished)
    return;
  ALint processed = 0;
  alGetSourcei(st->source, AL_BUFFERS_PROCESSED, &processed);
  while (processed-- > 0) {
    ALuint buf;
    alSourceUnqueueBuffers(st->source, 1, &buf);
    ALint size = 0, bits = 16, chans = 1;
    alGetBufferi(buf, AL_SIZE, &size);
    alGetBufferi(buf, AL_CHANNELS, &chans);
    st->frames_played_base += (uint64_t)size / (bits / 8) / std::max(chans, 1);
    if (!Stream_Queue_One(st, buf)) {
      ALint queued = 0;
      alGetSourcei(st->source, AL_BUFFERS_QUEUED, &queued);
      if (queued == 0)
        st->finished = true;
    }
  }
  ALint state = 0;
  alGetSourcei(st->source, AL_SOURCE_STATE, &state);
  ALint queued = 0;
  alGetSourcei(st->source, AL_BUFFERS_QUEUED, &queued);
  if (state != AL_PLAYING && queued > 0)
    alSourcePlay(st->source); // underrun: restart
}

void Destroy_Stream(Stream *st) {
  if (st->source) {
    alSourceStop(st->source);
    alSourcei(st->source, AL_BUFFER, 0);
    alDeleteSources(1, &st->source);
  }
  alDeleteBuffers(4, st->buffers);
  if (st->mp3_open)
    mp3dec_ex_close(&st->mp3);
  Streams.erase(st);
  delete st;
}

bool Read_Via_Callbacks(const char *filename, std::vector<uint8_t> &out) {
  if (FileOpen && FileRead && FileClose && FileSeek) {
    U32 h = 0;
    if (!FileOpen(filename, &h))
      return false;
    S32 size = FileSeek(h, 0, AIL_FILE_SEEK_END);
    FileSeek(h, 0, AIL_FILE_SEEK_BEGIN);
    if (size <= 0) {
      FileClose(h);
      return false;
    }
    out.resize((size_t)size);
    U32 got = FileRead(h, out.data(), (U32)size);
    FileClose(h);
    out.resize(got);
    return got > 0;
  }
  FILE *f = fopen(filename, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(size > 0 ? (size_t)size : 0);
  size_t got = fread(out.data(), 1, out.size(), f);
  fclose(f);
  out.resize(got);
  return got > 0;
}

Sample *As_Sample(void *h) {
  if (h == &Listener)
    return &Listener;
  Sample *s = static_cast<Sample *>(h);
  return Samples.count(s) ? s : nullptr;
}

Stream *As_Stream(HSTREAM h) {
  Stream *s = reinterpret_cast<Stream *>(h);
  return Streams.count(s) ? s : nullptr;
}

} // namespace

/* =================================================================== API */

#define LOCK std::lock_guard<std::recursive_mutex> guard(Lock)

extern "C" void Rebel_Audio_Update(void) {
  LOCK;
  if (!Context)
    return;
  Flush_Releases();
  for (Stream *st : Streams)
    Stream_Service(st);
}

extern "C" S32 AIL_startup(void) { return 1; }

extern "C" void AIL_shutdown(void) {
  LOCK;
  std::vector<Sample *> all(Samples.begin(), Samples.end());
  for (Sample *s : all)
    Destroy_Sample(s);
  std::vector<Stream *> streams(Streams.begin(), Streams.end());
  for (Stream *st : streams)
    Destroy_Stream(st);
  for (auto &c : Cache)
    alDeleteBuffers(1, &c.second.buffer);
  Cache.clear();
  CacheBytes = 0;
  if (Context) {
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(Context);
    Context = nullptr;
  }
  if (Device) {
    alcCloseDevice(Device);
    Device = nullptr;
  }
}

extern "C" S32 AIL_set_preference(U32, S32) { return 0; }
extern "C" char *AIL_last_error(void) { return LastError; }
extern "C" void AIL_lock(void) { Lock.lock(); }
extern "C" void AIL_unlock(void) { Lock.unlock(); }
extern "C" void AIL_stop_timer(HTIMER) {}
extern "C" void AIL_release_timer_handle(HTIMER) {}

extern "C" void AIL_set_file_callbacks(AIL_file_open_callback open, AIL_file_close_callback close,
                                       AIL_file_seek_callback seek, AIL_file_read_callback read) {
  FileOpen = open;
  FileClose = close;
  FileSeek = seek;
  FileRead = read;
}

extern "C" S32 AIL_WAV_info(void const *data, AILSOUNDINFO *info) {
  WavInfo w;
  if (!Parse_Wav(data, 0, w))
    return 0;
  info->format = w.format;
  info->data_ptr = w.data;
  info->data_len = w.data_len;
  info->rate = (U32)w.rate;
  info->bits = w.bits;
  info->channels = w.channels;
  info->block_size = (U32)w.block_align;
  info->initial_ptr = data;
  if (w.format == WAVE_FORMAT_IMA_ADPCM && w.block_align)
    info->samples = (w.data_len / w.block_align) * w.samples_per_block;
  else if (w.bits && w.channels)
    info->samples = w.data_len / (w.bits / 8) / w.channels;
  return 1;
}

/* ------------------------------------------------------------ 2D driver */

extern "C" S32 AIL_waveOutOpen(HDIGDRIVER *drvr, LPHWAVEOUT *waveout, S32, LPWAVEFORMAT) {
  LOCK;
  if (waveout)
    *waveout = nullptr;
  if (!Ensure_Device()) {
    if (drvr)
      *drvr = nullptr;
    return -1;
  }
  Driver2D.emulated_ds = 0;
  if (drvr)
    *drvr = &Driver2D;
  return AIL_NO_ERROR;
}

extern "C" void AIL_waveOutClose(HDIGDRIVER) {}

/* ------------------------------------------------------------- samples */

extern "C" HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER) {
  LOCK;
  return (HSAMPLE)New_Sample(false);
}

extern "C" void AIL_release_sample_handle(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s || s == &Listener)
    return;
  if (On_Main_Thread())
    Destroy_Sample(s);
  else
    s->released = true;
}

extern "C" void AIL_init_sample(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return;
  Detach(s);
  s->volume = 127;
  s->pan = 64;
  s->rate = 0;
  s->loop_count = 1;
  alSourcei(s->source, AL_LOOPING, AL_FALSE);
  alSourcef(s->source, AL_GAIN, 1.0f);
  alSourcef(s->source, AL_PITCH, 1.0f);
}

extern "C" S32 AIL_set_named_sample_file(HSAMPLE h, char const *suffix, void const *image, U32 size, S32) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return 0;
  bool mp3 = suffix && strlen(suffix) >= 4 && strcasecmp(suffix + strlen(suffix) - 4, ".mp3") == 0;
  return Attach(s, image, size, mp3) ? 1 : 0;
}

static void Start(Sample *s) {
  if (!s || !s->buf)
    return;
  alSourcei(s->source, AL_LOOPING, s->loop_count == 1 ? AL_FALSE : AL_TRUE);
  alSourceRewind(s->source);
  alSourcePlay(s->source);
}

extern "C" void AIL_start_sample(HSAMPLE h) {
  LOCK;
  Start(As_Sample(h));
}

extern "C" void AIL_stop_sample(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s && s->source)
    alSourcePause(s->source);
}

extern "C" void AIL_resume_sample(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s || !s->buf)
    return;
  ALint state = 0;
  alGetSourcei(s->source, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING)
    alSourcePlay(s->source);
}

extern "C" void AIL_end_sample(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s && s->source)
    alSourceStop(s->source);
}

extern "C" void AIL_set_sample_volume(HSAMPLE h, S32 volume) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return;
  s->volume = volume;
  alSourcef(s->source, AL_GAIN, Gain(volume));
}

extern "C" S32 AIL_sample_volume(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  return s ? s->volume : 0;
}

extern "C" void AIL_set_sample_pan(HSAMPLE h, S32 pan) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return;
  s->pan = pan;
  if (!s->is_3d && (!s->buf || s->buf->channels == 1))
    Apply_Pan(s->source, pan);
}

extern "C" S32 AIL_sample_pan(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  return s ? s->pan : 64;
}

extern "C" void AIL_set_sample_playback_rate(HSAMPLE h, S32 rate) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return;
  s->rate = rate;
  Apply_Pitch(s->source, rate, s->buf ? s->buf->rate : rate);
}

extern "C" S32 AIL_sample_playback_rate(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return 0;
  return s->rate ? s->rate : (s->buf ? s->buf->rate : 22050);
}

extern "C" void AIL_set_sample_loop_count(HSAMPLE h, S32 count) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s)
    return;
  s->loop_count = count;
  alSourcei(s->source, AL_LOOPING, count == 1 ? AL_FALSE : AL_TRUE);
}

extern "C" S32 AIL_sample_loop_count(HSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  return s ? s->loop_count : 0;
}

extern "C" void AIL_set_sample_ms_position(HSAMPLE h, S32 ms) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s && s->buf)
    alSourcef(s->source, AL_SEC_OFFSET, ms / 1000.0f);
}

extern "C" void AIL_sample_ms_position(HSAMPLE h, S32 *total_ms, S32 *current_ms) {
  LOCK;
  Sample *s = As_Sample(h);
  S32 total = 0, cur = 0;
  if (s && s->buf && s->buf->rate) {
    total = (S32)((long long)s->buf->frames * 1000 / s->buf->rate);
    ALfloat sec = 0;
    alGetSourcef(s->source, AL_SEC_OFFSET, &sec);
    cur = (S32)(sec * 1000.0f);
  }
  if (total_ms)
    *total_ms = total;
  if (current_ms)
    *current_ms = cur;
}

extern "C" void AIL_set_sample_user_data(HSAMPLE h, U32 index, S32 value) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s && index < 8)
    s->user_data[index] = value;
}

extern "C" S32 AIL_sample_user_data(HSAMPLE h, U32 index) {
  LOCK;
  Sample *s = As_Sample(h);
  return (s && index < 8) ? s->user_data[index] : 0;
}

extern "C" HPROVIDER AIL_set_sample_processor(HSAMPLE, U32, HPROVIDER) { return 0; }

// No reverb filter provider yet: WWAudio disables reverb when none is found.
extern "C" S32 AIL_enumerate_filters(HPROENUM *, HPROVIDER *, char **) { return 0; }
extern "C" void AIL_set_filter_sample_preference(HSAMPLE, char const *, void const *) {}

/* -------------------------------------------------------------- streams */

static HSTREAM Open_Stream(char const *filename) {
  if (!Ensure_Device())
    return nullptr;
  Stream *st = new Stream();
  if (!filename || !Read_Via_Callbacks(filename, st->file)) {
    Set_Error("stream file not found");
    delete st;
    return nullptr;
  }
  WavInfo w;
  if (Parse_Wav(st->file.data(), (uint32_t)st->file.size(), w)) {
    if (!Decode_Wav(st->file.data(), (uint32_t)st->file.size(), st->wav)) {
      delete st;
      return nullptr;
    }
    st->channels = st->wav.channels;
    st->rate = st->wav.rate;
    st->total_frames = (uint64_t)st->wav.Frames();
    st->file.clear();
    st->file.shrink_to_fit();
  } else {
    if (mp3dec_ex_open_buf(&st->mp3, st->file.data(), st->file.size(), MP3D_SEEK_TO_SAMPLE) != 0) {
      Set_Error("could not open mp3 stream");
      delete st;
      return nullptr;
    }
    st->is_mp3 = true;
    st->mp3_open = true;
    st->channels = st->mp3.info.channels;
    st->rate = st->mp3.info.hz;
    st->total_frames = st->mp3.samples / std::max(st->channels, 1);
  }
  alGenSources(1, &st->source);
  alSourcei(st->source, AL_SOURCE_RELATIVE, AL_TRUE);
  alSourcef(st->source, AL_ROLLOFF_FACTOR, 0.0f);
  alGenBuffers(4, st->buffers);
  Streams.insert(st);
  return reinterpret_cast<HSTREAM>(st);
}

extern "C" HSTREAM AIL_open_stream(HDIGDRIVER, char const *filename, S32) {
  LOCK;
  return Open_Stream(filename);
}

extern "C" HSTREAM AIL_open_stream_by_sample(HDIGDRIVER, HSAMPLE, char const *filename, S32) {
  LOCK;
  return Open_Stream(filename);
}

extern "C" void AIL_close_stream(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  if (On_Main_Thread())
    Destroy_Stream(st);
  else
    st->released = true;
}

static void Stream_Start_From(Stream *st, uint64_t frame) {
  alSourceStop(st->source);
  alSourcei(st->source, AL_BUFFER, 0);
  Stream_Rewind(st, frame);
  st->frames_played_base = frame;
  st->frames_queued = frame;
  st->finished = false;
  for (ALuint b : st->buffers)
    if (!Stream_Queue_One(st, b))
      break;
  alSourcef(st->source, AL_GAIN, Gain(st->volume));
  Apply_Pitch(st->source, st->rate_override, st->rate);
  if (st->channels == 1)
    Apply_Pan(st->source, st->pan);
  alSourcePlay(st->source);
  st->playing = true;
  st->paused = false;
}

extern "C" void AIL_start_stream(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  if (st)
    Stream_Start_From(st, 0);
}

extern "C" void AIL_pause_stream(HSTREAM h, S32 onoff) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  st->paused = onoff != 0;
  if (st->paused)
    alSourcePause(st->source);
  else if (st->playing)
    alSourcePlay(st->source);
  else
    Stream_Start_From(st, 0); // Miles starts a never-started stream on unpause
}

extern "C" void AIL_set_stream_volume(HSTREAM h, S32 volume) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  st->volume = volume;
  alSourcef(st->source, AL_GAIN, Gain(volume));
}

extern "C" S32 AIL_stream_volume(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  return st ? st->volume : 0;
}

extern "C" void AIL_set_stream_pan(HSTREAM h, S32 pan) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  st->pan = pan;
  if (st->channels == 1)
    Apply_Pan(st->source, pan);
}

extern "C" S32 AIL_stream_pan(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  return st ? st->pan : 64;
}

extern "C" void AIL_set_stream_playback_rate(HSTREAM h, S32 rate) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  st->rate_override = rate;
  Apply_Pitch(st->source, rate, st->rate);
}

extern "C" S32 AIL_stream_playback_rate(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  return st ? (st->rate_override ? st->rate_override : st->rate) : 0;
}

extern "C" void AIL_set_stream_loop_count(HSTREAM h, S32 count) {
  LOCK;
  Stream *st = As_Stream(h);
  if (st)
    st->loop_count = count;
}

extern "C" S32 AIL_stream_loop_count(HSTREAM h) {
  LOCK;
  Stream *st = As_Stream(h);
  return st ? st->loop_count : 0;
}

extern "C" void AIL_set_stream_loop_block(HSTREAM, S32, S32) {} // whole-file loops only

extern "C" void AIL_set_stream_ms_position(HSTREAM h, S32 ms) {
  LOCK;
  Stream *st = As_Stream(h);
  if (!st)
    return;
  uint64_t frame = (uint64_t)std::max(ms, 0) * (uint64_t)st->rate / 1000u;
  if (frame >= st->total_frames)
    frame = 0;
  bool was_playing = st->playing && !st->paused;
  Stream_Start_From(st, frame);
  if (!was_playing) {
    alSourcePause(st->source);
    st->paused = true;
  }
}

extern "C" void AIL_stream_ms_position(HSTREAM h, S32 *total_ms, S32 *current_ms) {
  LOCK;
  Stream *st = As_Stream(h);
  S32 total = 0, cur = 0;
  if (st && st->rate) {
    total = (S32)(st->total_frames * 1000 / st->rate);
    ALint offset = 0;
    alGetSourcei(st->source, AL_SAMPLE_OFFSET, &offset);
    uint64_t frame = st->frames_played_base + (uint64_t)std::max(offset, 0);
    if (st->total_frames)
      frame %= st->total_frames;
    cur = (S32)(frame * 1000 / st->rate);
  }
  if (total_ms)
    *total_ms = total;
  if (current_ms)
    *current_ms = cur;
}

/* --------------------------------------------------------- 3D providers */

static const HPROVIDER Provider3D = 1;

extern "C" S32 AIL_enumerate_3D_providers(HPROENUM *next, HPROVIDER *dest, char **name) {
  if (*next != 0)
    return 0;
  *next = 1;
  *dest = Provider3D;
  static char provider_name[] = "Rebel OpenAL 3D";
  *name = provider_name;
  return 1;
}

extern "C" S32 AIL_open_3D_provider(HPROVIDER lib) {
  LOCK;
  return (lib == Provider3D && Ensure_Device()) ? M3D_NOERR : 1;
}

extern "C" void AIL_close_3D_provider(HPROVIDER) {}
extern "C" void AIL_set_3D_speaker_type(HPROVIDER, S32) {} // browser handles output layout

extern "C" H3DPOBJECT AIL_3D_open_listener(HPROVIDER) {
  Listener.is_listener = true;
  return &Listener;
}

/* ---------------------------------------------------------- 3D samples */

extern "C" H3DSAMPLE AIL_allocate_3D_sample_handle(HPROVIDER) {
  LOCK;
  Sample *s = New_Sample(true);
  if (s)
    alSourcef(s->source, AL_ROLLOFF_FACTOR, 1.0f);
  return (H3DSAMPLE)s;
}

extern "C" void AIL_release_3D_sample_handle(H3DSAMPLE h) { AIL_release_sample_handle(h); }

extern "C" U32 AIL_set_3D_sample_file(H3DSAMPLE h, void const *image) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s || s->is_listener)
    return 0;
  // 3D sources must be mono for positional playback; stereo files still play.
  return Attach(s, image, 0, false) ? 1 : 0;
}

extern "C" void AIL_start_3D_sample(H3DSAMPLE h) { AIL_start_sample(h); }
extern "C" void AIL_stop_3D_sample(H3DSAMPLE h) { AIL_stop_sample(h); }
extern "C" void AIL_resume_3D_sample(H3DSAMPLE h) { AIL_resume_sample(h); }
extern "C" void AIL_end_3D_sample(H3DSAMPLE h) { AIL_end_sample(h); }
extern "C" void AIL_set_3D_sample_volume(H3DSAMPLE h, S32 v) { AIL_set_sample_volume(h, v); }
extern "C" S32 AIL_3D_sample_volume(H3DSAMPLE h) { return AIL_sample_volume(h); }
extern "C" void AIL_set_3D_sample_playback_rate(H3DSAMPLE h, S32 r) { AIL_set_sample_playback_rate(h, r); }
extern "C" S32 AIL_3D_sample_playback_rate(H3DSAMPLE h) { return AIL_sample_playback_rate(h); }
extern "C" void AIL_set_3D_sample_loop_count(H3DSAMPLE h, U32 loops) { AIL_set_sample_loop_count(h, (S32)loops); }
extern "C" U32 AIL_3D_sample_loop_count(H3DSAMPLE h) { return (U32)AIL_sample_loop_count(h); }

// Offsets/lengths are in bytes of the source format; report 16-bit PCM bytes.
extern "C" void AIL_set_3D_sample_offset(H3DSAMPLE h, U32 offset) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s && s->buf)
    alSourcei(s->source, AL_BYTE_OFFSET, (ALint)offset);
}

extern "C" U32 AIL_3D_sample_offset(H3DSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s || !s->buf)
    return 0;
  ALint off = 0;
  alGetSourcei(s->source, AL_BYTE_OFFSET, &off);
  return (U32)std::max(off, 0);
}

extern "C" U32 AIL_3D_sample_length(H3DSAMPLE h) {
  LOCK;
  Sample *s = As_Sample(h);
  return (s && s->buf) ? (U32)s->buf->bytes : 0;
}

extern "C" void AIL_set_3D_sample_distances(H3DSAMPLE h, F32 max_dist, F32 min_dist) {
  LOCK;
  Sample *s = As_Sample(h);
  if (!s || s->is_listener)
    return;
  alSourcef(s->source, AL_REFERENCE_DISTANCE, std::max(min_dist, 0.01f));
  alSourcef(s->source, AL_MAX_DISTANCE, std::max(max_dist, min_dist + 0.01f));
}

extern "C" void AIL_set_3D_sample_effects_level(H3DSAMPLE h, F32 level) {
  LOCK;
  Sample *s = As_Sample(h);
  if (s)
    s->effects_level = level; // reverb send: no effect without EFX
}

extern "C" void AIL_set_3D_object_user_data(H3DPOBJECT obj, U32 index, S32 value) {
  AIL_set_sample_user_data((HSAMPLE)obj, index, value);
}

extern "C" S32 AIL_3D_object_user_data(H3DPOBJECT obj, U32 index) { return AIL_sample_user_data((HSAMPLE)obj, index); }

extern "C" void AIL_set_3D_position(H3DPOBJECT obj, F32 x, F32 y, F32 z) {
  LOCK;
  Sample *s = As_Sample(obj);
  if (!s)
    return;
  if (s->is_listener)
    alListener3f(AL_POSITION, x, y, -z);
  else
    alSource3f(s->source, AL_POSITION, x, y, -z);
}

extern "C" void AIL_set_3D_orientation(H3DPOBJECT obj, F32 xf, F32 yf, F32 zf, F32 xu, F32 yu, F32 zu) {
  LOCK;
  Sample *s = As_Sample(obj);
  if (!s)
    return;
  if (s->is_listener) {
    ALfloat o[6] = {xf, yf, -zf, xu, yu, -zu};
    alListenerfv(AL_ORIENTATION, o);
  } else {
    alSource3f(s->source, AL_DIRECTION, xf, yf, -zf);
  }
}

extern "C" void AIL_set_3D_velocity_vector(H3DPOBJECT obj, F32 dx, F32 dy, F32 dz) {
  LOCK;
  Sample *s = As_Sample(obj);
  if (!s)
    return;
  // Miles velocities are per millisecond; OpenAL wants units per second.
  if (s->is_listener)
    alListener3f(AL_VELOCITY, dx * 1000.0f, dy * 1000.0f, -dz * 1000.0f);
  else
    alSource3f(s->source, AL_VELOCITY, dx * 1000.0f, dy * 1000.0f, -dz * 1000.0f);
}

/* ------------------------------------------------------------ test hook */

// Decodes an in-memory WAV/MP3 image without touching OpenAL. Used by the
// node smoke test to validate the decoders against the retail data.
extern "C" int Rebel_Audio_Decode_Test(const void *image, U32 size, int is_mp3, int *frames, int *rate,
                                       int *channels, int *peak) {
  Pcm pcm;
  bool ok = is_mp3 ? Decode_Mp3(image, size, pcm) : Decode_Wav(image, size, pcm);
  if (!ok)
    return 0;
  *frames = pcm.Frames();
  *rate = pcm.rate;
  *channels = pcm.channels;
  int p = 0;
  for (int16_t v : pcm.samples)
    p = std::max(p, std::abs((int)v));
  *peak = p;
  return 1;
}
