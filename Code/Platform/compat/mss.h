/*
** Rebel audio interface.
**
** Source-compatible stand-in for the subset of the Miles Sound System API
** that WWAudio calls, written from the engine's call sites. It is not the
** RAD SDK header. The implementation (Platform/audio/mss_openal.cpp) plays
** through OpenAL, which Emscripten maps onto Web Audio.
*/
#pragma once

#include "windows.h"

typedef unsigned int U32;
typedef int S32;
typedef unsigned short U16;
typedef short S16;
typedef unsigned char U8;
typedef signed char S8;
typedef float F32;

#define AILCALLBACK
#define AILEXPORT

/* Opaque handles. Each is a pointer to an implementation object. */
struct RebelAudioDriver {
    S32 emulated_ds; /* read by WWAudio: TRUE if DirectSound is emulated (never here) */
    void *impl;
};
typedef struct RebelAudioDriver *HDIGDRIVER;
typedef struct RebelAudioSample *HSAMPLE;
typedef struct RebelAudioSample *H3DSAMPLE;
typedef struct RebelAudioStream *HSTREAM;
typedef void *H3DPOBJECT;
typedef U32 HPROVIDER;
typedef U32 HPROENUM;
typedef S32 HTIMER;
#define HPROENUM_FIRST 0

/* Error codes */
#define AIL_NO_ERROR 0
#define M3D_NOERR 0

/* Preferences */
#define DIG_USE_WAVEOUT 15
#define AIL_LOCK_PROTECTION 18
#define NO 0
#define YES 1

/* 3D speaker layouts */
#define AIL_3D_2_SPEAKER 0
#define AIL_3D_HEADPHONE 1
#define AIL_3D_SURROUND 2
#define AIL_3D_4_SPEAKER 3

/* EAX reverb room presets (WWAudio stores the room type) */
enum { ENVIRONMENT_GENERIC = 0, ENVIRONMENT_COUNT = 26 };

/* Sample processor stages */
#define DP_FILTER 0

/* File callback seek origins */
#define AIL_FILE_SEEK_BEGIN 0
#define AIL_FILE_SEEK_CURRENT 1
#define AIL_FILE_SEEK_END 2

/* Description of a RIFF WAVE image in memory. */
typedef struct {
    S32 format;          /* WAVE_FORMAT_* tag */
    void const *data_ptr; /* first byte of sample data */
    U32 data_len;        /* bytes of sample data */
    U32 rate;            /* samples per second */
    S32 bits;            /* bits per sample (4 for IMA ADPCM) */
    S32 channels;
    U32 samples;         /* sample frames */
    U32 block_size;      /* ADPCM block alignment */
    void const *initial_ptr; /* start of the file image */
} AILSOUNDINFO;

typedef U32 (*AIL_file_open_callback)(char const *filename, U32 *file_handle);
typedef void (*AIL_file_close_callback)(U32 file_handle);
typedef S32 (*AIL_file_seek_callback)(U32 file_handle, S32 offset, U32 type);
typedef U32 (*AIL_file_read_callback)(U32 file_handle, void *buffer, U32 bytes);

#ifdef __cplusplus
extern "C" {
#endif

/* system */
S32 AIL_startup(void);
void AIL_shutdown(void);
S32 AIL_set_preference(U32 number, S32 value);
char *AIL_last_error(void);
void AIL_lock(void);
void AIL_unlock(void);
void AIL_set_file_callbacks(AIL_file_open_callback open, AIL_file_close_callback close,
                            AIL_file_seek_callback seek, AIL_file_read_callback read);
S32 AIL_WAV_info(void const *data, AILSOUNDINFO *info);
void AIL_stop_timer(HTIMER timer);
void AIL_release_timer_handle(HTIMER timer);

/* 2D driver */
S32 AIL_waveOutOpen(HDIGDRIVER *drvr, LPHWAVEOUT *waveout, S32 device_id, LPWAVEFORMAT format);
void AIL_waveOutClose(HDIGDRIVER drvr);

/* 2D samples */
HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER dig);
void AIL_release_sample_handle(HSAMPLE s);
void AIL_init_sample(HSAMPLE s);
S32 AIL_set_named_sample_file(HSAMPLE s, char const *file_type_suffix, void const *file_image, U32 file_size,
                              S32 block);
void AIL_start_sample(HSAMPLE s);
void AIL_stop_sample(HSAMPLE s);
void AIL_resume_sample(HSAMPLE s);
void AIL_end_sample(HSAMPLE s);
void AIL_set_sample_volume(HSAMPLE s, S32 volume);
S32 AIL_sample_volume(HSAMPLE s);
void AIL_set_sample_pan(HSAMPLE s, S32 pan);
S32 AIL_sample_pan(HSAMPLE s);
void AIL_set_sample_playback_rate(HSAMPLE s, S32 rate);
S32 AIL_sample_playback_rate(HSAMPLE s);
void AIL_set_sample_loop_count(HSAMPLE s, S32 count);
S32 AIL_sample_loop_count(HSAMPLE s);
void AIL_set_sample_ms_position(HSAMPLE s, S32 ms);
void AIL_sample_ms_position(HSAMPLE s, S32 *total_ms, S32 *current_ms);
void AIL_set_sample_user_data(HSAMPLE s, U32 index, S32 value);
S32 AIL_sample_user_data(HSAMPLE s, U32 index);
HPROVIDER AIL_set_sample_processor(HSAMPLE s, U32 stage, HPROVIDER provider);

/* filters (reverb) */
S32 AIL_enumerate_filters(HPROENUM *next, HPROVIDER *dest, char **name);
void AIL_set_filter_sample_preference(HSAMPLE s, char const *name, void const *value);

/* streams */
HSTREAM AIL_open_stream(HDIGDRIVER dig, char const *filename, S32 stream_mem);
HSTREAM AIL_open_stream_by_sample(HDIGDRIVER dig, HSAMPLE s, char const *filename, S32 stream_mem);
void AIL_close_stream(HSTREAM st);
void AIL_start_stream(HSTREAM st);
void AIL_pause_stream(HSTREAM st, S32 onoff);
void AIL_set_stream_volume(HSTREAM st, S32 volume);
S32 AIL_stream_volume(HSTREAM st);
void AIL_set_stream_pan(HSTREAM st, S32 pan);
S32 AIL_stream_pan(HSTREAM st);
void AIL_set_stream_playback_rate(HSTREAM st, S32 rate);
S32 AIL_stream_playback_rate(HSTREAM st);
void AIL_set_stream_loop_count(HSTREAM st, S32 count);
S32 AIL_stream_loop_count(HSTREAM st);
void AIL_set_stream_loop_block(HSTREAM st, S32 loop_start, S32 loop_end);
void AIL_set_stream_ms_position(HSTREAM st, S32 ms);
void AIL_stream_ms_position(HSTREAM st, S32 *total_ms, S32 *current_ms);

/* 3D providers and listener */
S32 AIL_enumerate_3D_providers(HPROENUM *next, HPROVIDER *dest, char **name);
S32 AIL_open_3D_provider(HPROVIDER lib);
void AIL_close_3D_provider(HPROVIDER lib);
void AIL_set_3D_speaker_type(HPROVIDER lib, S32 speaker_type);
H3DPOBJECT AIL_3D_open_listener(HPROVIDER lib);

/* 3D samples */
H3DSAMPLE AIL_allocate_3D_sample_handle(HPROVIDER lib);
void AIL_release_3D_sample_handle(H3DSAMPLE s);
U32 AIL_set_3D_sample_file(H3DSAMPLE s, void const *file_image);
void AIL_start_3D_sample(H3DSAMPLE s);
void AIL_stop_3D_sample(H3DSAMPLE s);
void AIL_resume_3D_sample(H3DSAMPLE s);
void AIL_end_3D_sample(H3DSAMPLE s);
void AIL_set_3D_sample_volume(H3DSAMPLE s, S32 volume);
S32 AIL_3D_sample_volume(H3DSAMPLE s);
void AIL_set_3D_sample_playback_rate(H3DSAMPLE s, S32 rate);
S32 AIL_3D_sample_playback_rate(H3DSAMPLE s);
void AIL_set_3D_sample_loop_count(H3DSAMPLE s, U32 loops);
U32 AIL_3D_sample_loop_count(H3DSAMPLE s);
void AIL_set_3D_sample_offset(H3DSAMPLE s, U32 offset);
U32 AIL_3D_sample_offset(H3DSAMPLE s);
U32 AIL_3D_sample_length(H3DSAMPLE s);
void AIL_set_3D_sample_distances(H3DSAMPLE s, F32 max_dist, F32 min_dist);
void AIL_set_3D_sample_effects_level(H3DSAMPLE s, F32 effects_level);
void AIL_set_3D_object_user_data(H3DPOBJECT obj, U32 index, S32 value);
S32 AIL_3D_object_user_data(H3DPOBJECT obj, U32 index);
void AIL_set_3D_position(H3DPOBJECT obj, F32 x, F32 y, F32 z);
void AIL_set_3D_orientation(H3DPOBJECT obj, F32 x_face, F32 y_face, F32 z_face, F32 x_up, F32 y_up, F32 z_up);
void AIL_set_3D_velocity_vector(H3DPOBJECT obj, F32 dx_per_ms, F32 dy_per_ms, F32 dz_per_ms);

#ifdef __cplusplus
}
#endif
