#pragma once
#include "windows.h"
/* Wave format descriptors (only the fields the audio code reads). */
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IMA_ADPCM 0x11
typedef struct waveformat_tag {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
} WAVEFORMAT, *LPWAVEFORMAT;
typedef struct pcmwaveformat_tag {
    WAVEFORMAT wf;
    WORD wBitsPerSample;
} PCMWAVEFORMAT, *LPPCMWAVEFORMAT;
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;
DECLARE_HANDLE(HWAVEOUT);
typedef HWAVEOUT *LPHWAVEOUT;
