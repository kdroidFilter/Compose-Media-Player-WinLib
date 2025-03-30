#pragma once
#ifndef OFFSCREEN_PLAYER_H
#define OFFSCREEN_PLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

// Export macro for Windows DLL
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

#ifdef __cplusplus
extern "C" {
#endif

// ====================================================================
// Exported Functions for Offscreen Media Playback
// ====================================================================

// 1) Initialize Media Foundation (call once per process)
OFFSCREENPLAYER_API HRESULT InitMediaFoundation();

// 2) Open a media file or URL and prepare for decoding
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t* url);

// 3) Read the next video frame in RGB32 format.
//    pData: receives pointer to the frame data
//    pDataSize: receives the size in bytes of the frame buffer
//    Returns: S_OK if a frame is read, S_FALSE if end-of-stream, or an error code.
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize);

// 4) Unlock the video frame buffer previously locked by ReadVideoFrame.
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame();

// 5) Close the media and free all associated resources.
OFFSCREENPLAYER_API void CloseMedia();

// 6) Basic controls for audio playback and end-of-file check.
OFFSCREENPLAYER_API BOOL IsEOF();
OFFSCREENPLAYER_API HRESULT StartAudioPlayback();
OFFSCREENPLAYER_API HRESULT StopAudioPlayback();

// 7) Retrieve the video dimensions (width and height).
OFFSCREENPLAYER_API void GetVideoSize(UINT32* pWidth, UINT32* pHeight);

// 8) Retrieve the current video frame rate (numerator and denominator).
OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom);

// 9) Seek to a specific position (in 100-nanosecond units)
OFFSCREENPLAYER_API HRESULT SeekMedia(LONGLONG llPosition);

// 10) Get the total duration of the media (in 100-nanosecond units)
OFFSCREENPLAYER_API HRESULT GetMediaDuration(LONGLONG* pDuration);

// 11) Get the current playback position (in 100-nanosecond units)
OFFSCREENPLAYER_API HRESULT GetMediaPosition(LONGLONG* pPosition);

#ifdef __cplusplus
}
#endif

#endif // OFFSCREEN_PLAYER_H
