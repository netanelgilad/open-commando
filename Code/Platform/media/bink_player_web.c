/*
** Bink cutscene decoder for the page (a separate, single-threaded WebAssembly
** module built by tools/build-bink.sh against a minimal FFmpeg: the bink
** demuxer and the bink/binkaudio decoders, LGPL 2.1+).
**
** The page copies a .bik file from the browser's file storage into this
** module's memory and pulls decoded output with bp_next():
**   1 = a video frame is ready (bp_rgba(), bp_pts() in seconds),
**   2 = audio is ready (bp_audio(): interleaved float samples, bp_audio_samples()),
**   0 = end of file, < 0 = error.
** Frames come out as RGBA (BT.601, limited range) for a 2D canvas.
*/
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  int64_t size, pos;
} MemInput;

static MemInput Input;
static AVFormatContext *Format;
static AVIOContext *Io;
static AVCodecContext *Video, *Audio;
static int VideoStream = -1, AudioStream = -1;
static AVPacket *Packet;
static AVFrame *Frame;
static uint8_t *Rgba;
static float *Pcm;
static int PcmCapacity, PcmSamples; // frames (per channel) in Pcm
static double Pts;
static int Pending; // packet sent to a decoder that may still have frames: 1 video, 2 audio

static int Read(void *opaque, uint8_t *buf, int n) {
  MemInput *in = opaque;
  int64_t left = in->size - in->pos;
  if (left <= 0)
    return AVERROR_EOF;
  if (n > left)
    n = (int)left;
  memcpy(buf, in->data + in->pos, (size_t)n);
  in->pos += n;
  return n;
}

static int64_t Seek(void *opaque, int64_t offset, int whence) {
  MemInput *in = opaque;
  if (whence == AVSEEK_SIZE)
    return in->size;
  int64_t base = (whence & ~AVSEEK_FORCE) == SEEK_CUR ? in->pos : (whence & ~AVSEEK_FORCE) == SEEK_END ? in->size : 0;
  int64_t p = base + offset;
  if (p < 0 || p > in->size)
    return -1;
  in->pos = p;
  return p;
}

EMSCRIPTEN_KEEPALIVE void bp_close(void) {
  avcodec_free_context(&Video);
  avcodec_free_context(&Audio);
  if (Format)
    avformat_close_input(&Format);
  if (Io) {
    av_freep(&Io->buffer);
    avio_context_free(&Io);
  }
  av_packet_free(&Packet);
  av_frame_free(&Frame);
  free(Rgba);
  Rgba = NULL;
  free(Pcm);
  Pcm = NULL;
  PcmCapacity = PcmSamples = 0;
  VideoStream = AudioStream = -1;
  Pending = 0;
}

static AVCodecContext *Open_Decoder(int stream) {
  AVCodecParameters *par = Format->streams[stream]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(par->codec_id);
  if (!codec)
    return NULL;
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx || avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, NULL) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  ctx->pkt_timebase = Format->streams[stream]->time_base;
  return ctx;
}

// data must stay valid until bp_close (the page keeps it in this module's heap).
EMSCRIPTEN_KEEPALIVE int bp_open(const uint8_t *data, int size) {
  bp_close();
  Input.data = data;
  Input.size = size;
  Input.pos = 0;
  const int buffer_size = 1 << 16;
  uint8_t *buffer = av_malloc(buffer_size);
  Io = avio_alloc_context(buffer, buffer_size, 0, &Input, Read, NULL, Seek);
  Format = avformat_alloc_context();
  if (!Io || !Format)
    return -1;
  Format->pb = Io;
  if (avformat_open_input(&Format, NULL, NULL, NULL) < 0)
    return -2;
  if (avformat_find_stream_info(Format, NULL) < 0)
    return -3;
  VideoStream = av_find_best_stream(Format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  AudioStream = av_find_best_stream(Format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (VideoStream < 0 || !(Video = Open_Decoder(VideoStream)))
    return -4;
  if (AudioStream >= 0 && !(Audio = Open_Decoder(AudioStream)))
    AudioStream = -1;
  Packet = av_packet_alloc();
  Frame = av_frame_alloc();
  Rgba = malloc((size_t)Video->width * Video->height * 4);
  return Rgba ? 0 : -5;
}

EMSCRIPTEN_KEEPALIVE int bp_width(void) { return Video ? Video->width : 0; }
EMSCRIPTEN_KEEPALIVE int bp_height(void) { return Video ? Video->height : 0; }
EMSCRIPTEN_KEEPALIVE double bp_fps(void) {
  if (VideoStream < 0)
    return 0;
  AVRational r = Format->streams[VideoStream]->avg_frame_rate;
  if (!r.num)
    r = Format->streams[VideoStream]->r_frame_rate;
  return r.den ? (double)r.num / r.den : 15.0;
}
EMSCRIPTEN_KEEPALIVE int bp_sample_rate(void) { return Audio ? Audio->sample_rate : 0; }
EMSCRIPTEN_KEEPALIVE int bp_channels(void) { return Audio ? Audio->ch_layout.nb_channels : 0; }
EMSCRIPTEN_KEEPALIVE double bp_duration(void) { return Format && Format->duration > 0 ? Format->duration / 1e6 : 0; }
EMSCRIPTEN_KEEPALIVE uint8_t *bp_rgba(void) { return Rgba; }
EMSCRIPTEN_KEEPALIVE double bp_pts(void) { return Pts; }
EMSCRIPTEN_KEEPALIVE float *bp_audio(void) { return Pcm; }
EMSCRIPTEN_KEEPALIVE int bp_audio_samples(void) { return PcmSamples; }

static inline uint8_t Clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void To_Rgba(const AVFrame *f) {
  const int w = f->width, h = f->height;
  for (int y = 0; y < h; y++) {
    const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
    const uint8_t *pu = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
    const uint8_t *pv = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
    uint8_t *out = Rgba + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      int c = (py[x] - 16) * 298, d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
      out[0] = Clamp((c + 409 * e + 128) >> 8);
      out[1] = Clamp((c - 100 * d - 208 * e + 128) >> 8);
      out[2] = Clamp((c + 516 * d + 128) >> 8);
      out[3] = 255;
      out += 4;
    }
  }
}

static void To_Pcm(const AVFrame *f) {
  const int ch = f->ch_layout.nb_channels, n = f->nb_samples;
  if (PcmCapacity < n * ch) {
    free(Pcm);
    PcmCapacity = n * ch;
    Pcm = malloc(sizeof(float) * PcmCapacity);
  }
  PcmSamples = n;
  for (int c = 0; c < ch; c++) {
    if (f->format == AV_SAMPLE_FMT_FLTP) {
      const float *src = (const float *)f->extended_data[c];
      for (int i = 0; i < n; i++)
        Pcm[i * ch + c] = src[i];
    } else if (f->format == AV_SAMPLE_FMT_FLT) {
      memcpy(Pcm, f->data[0], sizeof(float) * n * ch);
      break;
    } else if (f->format == AV_SAMPLE_FMT_S16P) {
      const int16_t *src = (const int16_t *)f->extended_data[c];
      for (int i = 0; i < n; i++)
        Pcm[i * ch + c] = src[i] / 32768.0f;
    } else if (f->format == AV_SAMPLE_FMT_S16) {
      const int16_t *src = (const int16_t *)f->data[0];
      for (int i = 0; i < n * ch; i++)
        Pcm[i] = src[i] / 32768.0f;
      break;
    }
  }
}

// Returns a frame from the decoder that last received a packet, if any.
static int Drain(void) {
  AVCodecContext *ctx = Pending == 1 ? Video : Audio;
  int r = avcodec_receive_frame(ctx, Frame);
  if (r < 0) {
    Pending = 0;
    return 0;
  }
  if (Pending == 1) {
    AVRational tb = Format->streams[VideoStream]->time_base;
    int64_t ts = Frame->best_effort_timestamp != AV_NOPTS_VALUE ? Frame->best_effort_timestamp : Frame->pts;
    Pts = ts == AV_NOPTS_VALUE ? 0 : ts * av_q2d(tb);
    To_Rgba(Frame);
    av_frame_unref(Frame);
    return 1;
  }
  To_Pcm(Frame);
  av_frame_unref(Frame);
  return 2;
}

EMSCRIPTEN_KEEPALIVE int bp_next(void) {
  if (!Format)
    return -1;
  for (;;) {
    if (Pending) {
      int r = Drain();
      if (r)
        return r;
    }
    int r = av_read_frame(Format, Packet);
    if (r < 0)
      return r == AVERROR_EOF ? 0 : r;
    if (Packet->stream_index == VideoStream && avcodec_send_packet(Video, Packet) >= 0)
      Pending = 1;
    else if (Packet->stream_index == AudioStream && avcodec_send_packet(Audio, Packet) >= 0)
      Pending = 2;
    av_packet_unref(Packet);
  }
}
