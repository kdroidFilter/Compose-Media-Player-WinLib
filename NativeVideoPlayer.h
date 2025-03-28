#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef __cplusplus

// Template to safely release COM interfaces
template <class T>
void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

// Structure for video dimensions
struct VideoSize {
    int width;    // Width
    int height;   // Height
    float ratio;  // Aspect ratio (width/height)
};

extern "C" {
#endif

#ifdef _WIN32
#ifdef MEDIAPLAYER_EXPORTS
#define MEDIAPLAYER_API __declspec(dllexport)
#else
#define MEDIAPLAYER_API __declspec(dllimport)
#endif
#else
#define MEDIAPLAYER_API
#endif

#include <windows.h>

// Event types for the callback
#define MP_EVENT_MEDIAITEM_CREATED    1
#define MP_EVENT_MEDIAITEM_SET        2
#define MP_EVENT_PLAYBACK_STARTED     3
#define MP_EVENT_PLAYBACK_STOPPED     4
#define MP_EVENT_PLAYBACK_ERROR       5
#define MP_EVENT_PLAYBACK_PAUSED      6
#define MP_EVENT_LOADING_STARTED      7
#define MP_EVENT_LOADING_COMPLETE     8
#define MP_EVENT_PLAYBACK_ENDED       9

// Some internal error codes (example)
#define MP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define MP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define MP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

// Callback prototype
typedef void (CALLBACK *MEDIA_PLAYER_CALLBACK)(int eventType, HRESULT hr);

// Functions exposed by the DLL
MEDIAPLAYER_API HRESULT InitializeMediaPlayer(HWND hwnd, MEDIA_PLAYER_CALLBACK callback);
MEDIAPLAYER_API HRESULT PlayFile(const wchar_t* filePath);
MEDIAPLAYER_API HRESULT PlayURL(const wchar_t* url);
MEDIAPLAYER_API HRESULT PausePlayback();
MEDIAPLAYER_API HRESULT ResumePlayback();
MEDIAPLAYER_API HRESULT StopPlayback();
MEDIAPLAYER_API void    UpdateVideo();
MEDIAPLAYER_API void    CleanupMediaPlayer();

// Player states
MEDIAPLAYER_API BOOL    IsInitialized();
MEDIAPLAYER_API BOOL    HasVideo();
MEDIAPLAYER_API BOOL    IsLoading();
MEDIAPLAYER_API BOOL    IsPlaying();

// Volume
MEDIAPLAYER_API HRESULT SetVolume(float level);    // Level between 0.0 and 1.0
MEDIAPLAYER_API HRESULT GetVolume(float* pLevel);
MEDIAPLAYER_API HRESULT SetMute(BOOL bMute);
MEDIAPLAYER_API HRESULT GetMute(BOOL* pbMute);
MEDIAPLAYER_API HRESULT GetChannelLevels(float* pLeft, float* pRight);

// Slider
MEDIAPLAYER_API HRESULT GetDuration(LONGLONG* pDuration);  // Total duration in 100ns units
MEDIAPLAYER_API HRESULT GetCurrentPosition(LONGLONG* pPosition); // Current position
MEDIAPLAYER_API HRESULT SetPosition(LONGLONG position); // Set the position

// Function to get the full video dimensions
MEDIAPLAYER_API HRESULT GetVideoSize(VideoSize* pSize);

// Simplified function to get only the aspect ratio
MEDIAPLAYER_API HRESULT GetVideoAspectRatio(float* pRatio);

#ifdef __cplusplus
}
#endif
