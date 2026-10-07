/* Video for Windows: only the AVI writer the developer frame grabber uses.
** On the web every call fails, so the grabber never records. */
#pragma once
#include "windows.h"
#include "wingdi.h"

typedef struct RebelAviFile *PAVIFILE;
typedef struct RebelAviStream *PAVISTREAM;
typedef struct _AVISTREAMINFOA {
    DWORD fccType, fccHandler, dwFlags, dwCaps;
    WORD wPriority, wLanguage;
    DWORD dwScale, dwRate, dwStart, dwLength, dwInitialFrames, dwSuggestedBufferSize, dwQuality, dwSampleSize;
    RECT rcFrame;
    DWORD dwEditCount, dwFormatChangeCount;
    char szName[64];
} AVISTREAMINFO, AVISTREAMINFOA;
#define streamtypeVIDEO MAKEFOURCC('v', 'i', 'd', 's')
#define OF_WRITE 0x00000001
#define OF_CREATE 0x00001000
#define AVIIF_KEYFRAME 0x00000010L
#define AVIERR_OK 0L

inline void AVIFileInit(void) {}
inline void AVIFileExit(void) {}
inline HRESULT AVIFileOpen(PAVIFILE *f, LPCSTR, UINT, void *) { *f = nullptr; return E_NOTIMPL; }
inline ULONG AVIFileRelease(PAVIFILE) { return 0; }
inline HRESULT AVIFileCreateStream(PAVIFILE, PAVISTREAM *s, AVISTREAMINFO *) { *s = nullptr; return E_NOTIMPL; }
inline HRESULT AVIStreamSetFormat(PAVISTREAM, LONG, void *, LONG) { return E_NOTIMPL; }
inline HRESULT AVIStreamWrite(PAVISTREAM, LONG, LONG, void *, LONG, DWORD, LONG *, LONG *) { return E_NOTIMPL; }
inline ULONG AVIStreamRelease(PAVISTREAM) { return 0; }
