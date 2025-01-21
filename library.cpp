#define WINVER _WIN32_WINNT_WIN10
#define MEDIAPLAYER_EXPORTS

#include "library.h"
#include <windows.h>
#include <mfplay.h>
#include <mferror.h>
#include <mfapi.h>
#include <cstdio>
#include <objbase.h>
#include <new>
#include <atomic>

// =============== Structures and Global Variables ====================

struct PlayerState {
    IMFPMediaPlayer*      player;
    MEDIA_PLAYER_CALLBACK userCallback;
    HWND                  hwnd;
    std::atomic<bool>     hasVideo;
    std::atomic<bool>     isInitialized;
    std::atomic<bool>     isPlaying;
    CRITICAL_SECTION      lock;
};

// Our global state
static PlayerState g_state = {
    nullptr,    // player
    nullptr,    // userCallback
    nullptr,    // hwnd
    false,      // hasVideo
    false,      // isInitialized
    false       // isPlaying
};

// Playback thread + message loop
static HANDLE g_hThread       = nullptr;
static DWORD  g_dwThreadId    = 0;
static bool   g_bThreadActive = false;  // To check if it's running

// To simplify, we'll store everything the thread needs
// before starting it.
static HWND                  g_hwndInit = nullptr;
static MEDIA_PLAYER_CALLBACK g_cbInit   = nullptr;

// =============== Utility Logging (debug) ====================
static void LogDebugW(const wchar_t* format, ...)
{
#ifdef _DEBUG
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, format, args);
    va_end(args);
    OutputDebugStringW(buffer);
#else
    (void)format; // no log in release mode
#endif
}

// =============== MFPlay Callback Class ======================
class MediaPlayerCallback : public IMFPMediaPlayerCallback {
private:
    long m_cRef;

public:
    MediaPlayerCallback() : m_cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IMFPMediaPlayerCallback || riid == IID_IUnknown) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) override {
        if (!pEventHeader) return;

        EnterCriticalSection(&g_state.lock);

        if (!g_state.userCallback) {
            LeaveCriticalSection(&g_state.lock);
            return;
        }

        if (FAILED(pEventHeader->hrEvent)) {
            // Notify an error occurred
            g_state.isPlaying = false;
            g_state.userCallback(MP_EVENT_PLAYBACK_ERROR, pEventHeader->hrEvent);
            LeaveCriticalSection(&g_state.lock);
            return;
        }

        switch (pEventHeader->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
        {
            auto pEvent = MFP_GET_MEDIAITEM_CREATED_EVENT(pEventHeader);
            LogDebugW(L"[Callback] MFP_EVENT_TYPE_MEDIAITEM_CREATED\n");

            if (SUCCEEDED(pEventHeader->hrEvent) && pEvent && pEvent->pMediaItem) {
                BOOL bHasVideo = FALSE, bIsSelected = FALSE;
                HRESULT hr = pEvent->pMediaItem->HasVideo(&bHasVideo, &bIsSelected);
                if (SUCCEEDED(hr)) {
                    g_state.hasVideo = (bHasVideo && bIsSelected);
                    if (g_state.player) {
                        g_state.player->SetMediaItem(pEvent->pMediaItem);
                    }
                }
            }
            g_state.userCallback(MP_EVENT_MEDIAITEM_CREATED, pEventHeader->hrEvent);
            break;
        }
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
        {
            LogDebugW(L"[Callback] MFP_EVENT_TYPE_MEDIAITEM_SET\n");
            g_state.userCallback(MP_EVENT_MEDIAITEM_SET, pEventHeader->hrEvent);

            // Start playback immediately
            if (g_state.player) {
                g_state.player->Play();
                g_state.isPlaying = true;
            }
            break;
        }
        case MFP_EVENT_TYPE_PLAY:
            LogDebugW(L"[Callback] MFP_EVENT_TYPE_PLAY -> PLAYBACK_STARTED\n");
            g_state.isPlaying = true;
            g_state.userCallback(MP_EVENT_PLAYBACK_STARTED, pEventHeader->hrEvent);
            break;

        case MFP_EVENT_TYPE_PAUSE:
        case MFP_EVENT_TYPE_STOP:
            LogDebugW(L"[Callback] MFP_EVENT_TYPE_STOP -> PLAYBACK_STOPPED\n");
            g_state.isPlaying = false;
            g_state.userCallback(MP_EVENT_PLAYBACK_STOPPED, pEventHeader->hrEvent);
            break;

        case MFP_EVENT_TYPE_ERROR:
            // Handled earlier with FAILED()
            break;
        }

        LeaveCriticalSection(&g_state.lock);
    }
};

// Keep a global pointer to our callback
static MediaPlayerCallback* g_pCallback = nullptr;

// =================================================================
// ThreadProc: Executes the Win32 message loop + initializes MFPlay
// =================================================================
static DWORD WINAPI MediaThreadProc(LPVOID lpParam)
{
    HRESULT hr = S_OK;

    // Initialize COM in STA
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        LogDebugW(L"[MediaThreadProc] CoInitializeEx failed: 0x%08x\n", hr);
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogDebugW(L"[MediaThreadProc] MFStartup failed: 0x%08x\n", hr);
        CoUninitialize();
        return 1;
    }

    // Create the callback object
    g_pCallback = new (std::nothrow) MediaPlayerCallback();
    if (!g_pCallback) {
        LogDebugW(L"[MediaThreadProc] new MediaPlayerCallback FAILED\n");
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    // Create the MFPlay player
    EnterCriticalSection(&g_state.lock);
    hr = MFPCreateMediaPlayer(
        nullptr,               // URL: unnecessary, we'll use CreateMediaItemFromURL later
        FALSE,              // Do not auto-start
        0,                  // Flags (optional for further use)
        g_pCallback,        // Callback pointer
        g_hwndInit,         // HWND to display the video
        &g_state.player
    );
    if (SUCCEEDED(hr)) {
        g_state.userCallback  = g_cbInit;
        g_state.hwnd          = g_hwndInit;
        g_state.isInitialized = true;
    } else {
        LogDebugW(L"[MediaThreadProc] MFPCreateMediaPlayer failed: 0x%08x\n", hr);
        delete g_pCallback;
        g_pCallback = nullptr;
        LeaveCriticalSection(&g_state.lock);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    LeaveCriticalSection(&g_state.lock);

    LogDebugW(L"[MediaThreadProc] Player created OK\n");

    // Win32 message loop required for MFPlay
    g_bThreadActive = true;
    MSG msg;
    while (g_bThreadActive)
    {
        // Wait for a message to be available
        DWORD dwResult = MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);
        if (dwResult == WAIT_FAILED) {
            // Unexpected error
            LogDebugW(L"[MediaThreadProc] WAIT_FAILED\n");
            break;
        }

        // Process all available messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_bThreadActive = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Shutdown
    LogDebugW(L"[MediaThreadProc] Exiting message loop...\n");

    // Cleanup
    if (g_state.player) {
        g_state.player->Shutdown();
        g_state.player->Release();
        g_state.player = nullptr;
    }
    if (g_pCallback) {
        g_pCallback->Release();
        g_pCallback = nullptr;
    }

    MFShutdown();
    CoUninitialize();

    return 0;
}

// =================================================================
// External API Functions
// =================================================================

// 1) InitializeMediaPlayer
HRESULT InitializeMediaPlayer(HWND hwnd, MEDIA_PLAYER_CALLBACK callback)
{
    EnterCriticalSection(&g_state.lock);

    if (g_state.isInitialized) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_ALREADY_INITIALIZED;
    }
    if (!hwnd || !callback) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_INVALID_PARAMETER;
    }

    // Temporarily store
    g_hwndInit = hwnd;
    g_cbInit   = callback;

    // Start the thread
    g_bThreadActive = false;
    g_hThread = CreateThread(
        nullptr,
        0,
        MediaThreadProc,
        nullptr,
        0,
        &g_dwThreadId
    );
    if (!g_hThread) {
        LeaveCriticalSection(&g_state.lock);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    LeaveCriticalSection(&g_state.lock);

    return S_OK;
}

// 2) PlayFile
HRESULT PlayFile(const wchar_t* filePath)
{
    EnterCriticalSection(&g_state.lock);
    if (!g_state.isInitialized || !g_state.player) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_NOT_INITIALIZED;
    }
    if (!filePath) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_INVALID_PARAMETER;
    }

    g_state.hasVideo = false;
    g_state.isPlaying = false;

    HRESULT hr = g_state.player->CreateMediaItemFromURL(filePath, FALSE, 0, nullptr);
    LogDebugW(L"[PlayFile] CreateMediaItemFromURL(%s) -> 0x%08x\n", filePath, hr);

    LeaveCriticalSection(&g_state.lock);
    return hr;
}

// 3) PausePlayback
HRESULT PausePlayback()
{
    EnterCriticalSection(&g_state.lock);
    if (!g_state.isInitialized || !g_state.player) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_NOT_INITIALIZED;
    }
    HRESULT hr = g_state.player->Pause();
    if (SUCCEEDED(hr)) {
        g_state.isPlaying = false;
    }
    LeaveCriticalSection(&g_state.lock);
    return hr;
}

// 4) ResumePlayback
HRESULT ResumePlayback()
{
    EnterCriticalSection(&g_state.lock);
    if (!g_state.isInitialized || !g_state.player) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_NOT_INITIALIZED;
    }
    HRESULT hr = g_state.player->Play();
    if (SUCCEEDED(hr)) {
        g_state.isPlaying = true;
    }
    LeaveCriticalSection(&g_state.lock);
    return hr;
}

// 5) StopPlayback
HRESULT StopPlayback()
{
    EnterCriticalSection(&g_state.lock);
    if (!g_state.isInitialized || !g_state.player) {
        LeaveCriticalSection(&g_state.lock);
        return MP_E_NOT_INITIALIZED;
    }
    HRESULT hr = g_state.player->Stop();
    if (SUCCEEDED(hr)) {
        g_state.isPlaying = false;
    }
    LeaveCriticalSection(&g_state.lock);
    return hr;
}

// 6) UpdateVideo
void UpdateVideo()
{
    EnterCriticalSection(&g_state.lock);
    if (g_state.player && g_state.hasVideo) {
        g_state.player->UpdateVideo();
    }
    LeaveCriticalSection(&g_state.lock);
}

// 7) CleanupMediaPlayer
void CleanupMediaPlayer()
{
    EnterCriticalSection(&g_state.lock);
    LogDebugW(L"[CleanupMediaPlayer] Called\n");

    // Signal the thread to exit the loop
    g_bThreadActive = false;
    LeaveCriticalSection(&g_state.lock);

    // Optionally send WM_QUIT
    if (g_dwThreadId != 0) {
        PostThreadMessage(g_dwThreadId, WM_QUIT, 0, 0);
    }

    // Wait for the thread to finish
    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }
    g_dwThreadId = 0;

    EnterCriticalSection(&g_state.lock);
    g_state.isInitialized = false;
    g_state.isPlaying     = false;
    g_state.hasVideo      = false;
    g_state.userCallback  = nullptr;
    g_state.hwnd          = nullptr;
    LeaveCriticalSection(&g_state.lock);
}

// 8) IsInitialized
BOOL IsInitialized()
{
    return g_state.isInitialized;
}

// 9) HasVideo
BOOL HasVideo()
{
    return g_state.hasVideo;
}

// ============================
// Static Initialization Block
// ============================
struct InitOnce {
    InitOnce() {
        InitializeCriticalSection(&g_state.lock);
    }
    ~InitOnce() {
        DeleteCriticalSection(&g_state.lock);
    }
} initOnce;
