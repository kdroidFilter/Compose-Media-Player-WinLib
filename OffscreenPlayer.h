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

/**
 * @brief Initialize Media Foundation. Must be called once per process before using other functions.
 * @return S_OK on success, or an error code (e.g., OP_E_ALREADY_INITIALIZED).
 */
OFFSCREENPLAYER_API HRESULT InitMediaFoundation();

/**
 * @brief Open a media file or URL and prepare for decoding with hardware acceleration.
 * @param url The file path or URL of the media to open (wide string).
 * @return S_OK on success, or an error code (e.g., OP_E_NOT_INITIALIZED).
 */
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t *url);

/**
 * @brief Read the next video frame in RGB32 format, leveraging hardware decoding if available.
 * @param pData Receives a pointer to the frame data (caller must not free this).
 * @param pDataSize Receives the size in bytes of the frame buffer.
 * @return S_OK if a frame is read, S_FALSE if end-of-stream, or an error code.
 * @note Frame data is valid until UnlockVideoFrame is called.
 */
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE **pData, DWORD *pDataSize);

/**
 * @brief Unlock the video frame buffer previously locked by ReadVideoFrame.
 * @return S_OK on success.
 */
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame();

/**
 * @brief Close the media and free all associated resources.
 */
OFFSCREENPLAYER_API void CloseMedia();

/**
 * @brief Check if the end of the media stream has been reached.
 * @return TRUE if end-of-stream, FALSE otherwise.
 */
OFFSCREENPLAYER_API BOOL IsEOF();


/**
 * @brief Retrieve the video dimensions.
 * @param pWidth Pointer to receive the width in pixels.
 * @param pHeight Pointer to receive the height in pixels.
 */
OFFSCREENPLAYER_API void GetVideoSize(UINT32 *pWidth, UINT32 *pHeight);

/**
 * @brief Retrieve the video frame rate.
 * @param pNum Pointer to receive the numerator of the frame rate.
 * @param pDenom Pointer to receive the denominator of the frame rate.
 * @return S_OK on success, or an error code.
 */
OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT *pNum, UINT *pDenom);

/**
 * @brief Seek to a specific position in the media.
 * @param llPosition The position to seek to, in 100-nanosecond units.
 * @return S_OK on success, or an error code.
 */
OFFSCREENPLAYER_API HRESULT SeekMedia(LONGLONG llPosition);

/**
 * @brief Get the total duration of the media.
 * @param pDuration Pointer to receive the duration in 100-nanosecond units.
 * @return S_OK on success, or an error code.
 */
OFFSCREENPLAYER_API HRESULT GetMediaDuration(LONGLONG *pDuration);

/**
 * @brief Get the current playback position.
 * @param pPosition Pointer to receive the current position in 100-nanosecond units.
 * @return S_OK on success, or an error code.
 */
OFFSCREENPLAYER_API HRESULT GetMediaPosition(LONGLONG *pPosition);


OFFSCREENPLAYER_API HRESULT SetPlaybackState(BOOL bPlaying);

OFFSCREENPLAYER_API HRESULT ShutdownMediaFoundation();

#ifdef __cplusplus
}
#endif

#endif // OFFSCREEN_PLAYER_H
