#pragma once

#ifndef OFFSCREEN_PLAYER_H
#define OFFSCREEN_PLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef OFFSCREENPLAYER_EXPORTS
#define OFFSCREENPLAYER_API __declspec(dllexport)
#else
#define OFFSCREENPLAYER_API __declspec(dllimport)
#endif
#else
#define OFFSCREENPLAYER_API
#endif

// Custom error codes
#define OP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define OP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define OP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

// ====================================================================
// Exported functions
// ====================================================================

// 1) Initialize Media Foundation once per process.
OFFSCREENPLAYER_API HRESULT InitMediaFoundation();

// 2) Open a file (or URL) and prepare for video and audio decoding.
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t* url);

// 3) Read the next video frame in memory (RGB32).
//    pData: pointer to a pointer that receives the pixel data
//    pDataSize: receives the size in bytes
//    Returns: S_OK if a frame is read, S_FALSE if end-of-stream, or an error code.
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize);

// 4) Unlock the previously read frame buffer.
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame();

// 5) Close the media and free all associated resources.
OFFSCREENPLAYER_API void CloseMedia();

// 6) Basic controls
OFFSCREENPLAYER_API BOOL IsEOF();                     // Checks if video has reached the end
OFFSCREENPLAYER_API HRESULT StartAudioPlayback();      // Starts/resumes audio playback in a dedicated thread
OFFSCREENPLAYER_API HRESULT StopAudioPlayback();       // Stops (pauses) audio playback

// 7) Retrieve the actual video dimensions (width, height).
OFFSCREENPLAYER_API void GetVideoSize(UINT32* pWidth, UINT32* pHeight);

// 8) Retrieve current video frame rate (numerator, denominator).
OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom);

#ifdef __cplusplus
}
#endif

#endif // OFFSCREEN_PLAYER_H
