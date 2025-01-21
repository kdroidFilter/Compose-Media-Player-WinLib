#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef __cplusplus
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

    // Some internal error codes (example)
#define MP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define MP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define MP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

    // Callback prototype
    typedef void (CALLBACK *MEDIA_PLAYER_CALLBACK)(int eventType, HRESULT hr);

    // Functions exposed by the DLL
    MEDIAPLAYER_API HRESULT InitializeMediaPlayer(HWND hwnd, MEDIA_PLAYER_CALLBACK callback);
    MEDIAPLAYER_API HRESULT PlayFile(const wchar_t* filePath);
    MEDIAPLAYER_API HRESULT PausePlayback();
    MEDIAPLAYER_API HRESULT ResumePlayback();
    MEDIAPLAYER_API HRESULT StopPlayback();
    MEDIAPLAYER_API void    UpdateVideo();
    MEDIAPLAYER_API void    CleanupMediaPlayer();
    MEDIAPLAYER_API BOOL    IsInitialized();
    MEDIAPLAYER_API BOOL    HasVideo();

#ifdef __cplusplus
}
#endif
