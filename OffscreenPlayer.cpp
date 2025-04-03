#include "OffscreenPlayer.h"
#include <cstdio>
#include <cstring>      // for memcpy
#include <windows.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <utility>

// Global variables for Media Foundation and playback state
static bool g_bMFInitialized = false;               // Media Foundation initialization flag
static IMFSourceReader* g_pSourceReader = nullptr;    // Source reader for video
static BOOL g_bEOF = FALSE;                           // End-of-stream flag
static IMFMediaBuffer* g_pLockedBuffer = nullptr;     // Locked video frame buffer
static BYTE* g_pLockedBytes = nullptr;                // Pointer to locked frame data
static DWORD g_lockedMaxSize = 0;                     // Maximum size of locked buffer
static DWORD g_lockedCurrSize = 0;                    // Current size of locked buffer
static UINT32 g_videoWidth = 0;                       // Video width in pixels
static UINT32 g_videoHeight = 0;                      // Video height in pixels
static LONGLONG g_llCurrentPosition = 0;              // Current playback position (100-ns units)

// Audio-related globals
static IMFSourceReader* g_pSourceReaderAudio = nullptr; // Source reader for audio
static bool g_bAudioInitialized = false;                // WASAPI initialization state
static bool g_bHasAudio = false;                        // Indicates if media has audio
static IAudioClient* g_pAudioClient = nullptr;          // WASAPI audio client
static IAudioRenderClient* g_pRenderClient = nullptr;   // WASAPI render client
static IMMDeviceEnumerator* g_pEnumerator = nullptr;    // Audio device enumerator
static IMMDevice* g_pDevice = nullptr;                  // Default audio device
static WAVEFORMATEX* g_pSourceAudioFormat = nullptr;    // Audio format
static HANDLE g_hAudioSamplesReadyEvent = nullptr;      // Event for audio samples ready

// Audio thread globals
static HANDLE g_hAudioThread = nullptr;                 // Audio playback thread handle
static bool g_bAudioThreadRunning = false;              // Audio thread running state
static HANDLE g_hAudioReadyEvent = nullptr;             // Event for audio thread synchronization

// Playback clock globals
static ULONGLONG g_llPlaybackStartTime = 0;             // Start time of playback (ms)
static ULONGLONG g_llTotalPauseTime = 0;                // Total paused time (ms)
static ULONGLONG g_llPauseStart = 0;                    // Start time of current pause (ms)

// Added master clock and synchronization critical section for AV sync
static LONGLONG g_llMasterClock = 0;                    // Master clock position in 100ns
static CRITICAL_SECTION g_csClockSync;                // Critical section for clock access

// Helper: Get current time in milliseconds
static inline ULONGLONG GetCurrentTimeMs() {
    return GetTickCount64();
}

// Helper: Precise sleep function using Sleep()
static void PreciseSleep(DWORD milliseconds) {
    if (milliseconds > 0) Sleep(milliseconds);
}

// Logging helper (debug mode only)
#ifdef _DEBUG
static void PrintHR(const char* msg, HRESULT hr) {
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
}
#else
#define PrintHR(msg, hr) ((void)0)
#endif

// Audio thread procedure for low-latency playback
static DWORD WINAPI AudioThreadProc(LPVOID /*lpParam*/)
{
    // Vérifier que les composants audio sont disponibles
    if (!g_pAudioClient || !g_pRenderClient || !g_pSourceReaderAudio) {
        PrintHR("AudioThreadProc: missing audio components", E_FAIL);
        return 0;
    }

    // Attendre le signal de démarrage du thread audio
    if (g_hAudioReadyEvent) {
        WaitForSingleObject(g_hAudioReadyEvent, INFINITE);
    }

    // Obtenir la taille du buffer audio
    constexpr DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    UINT32 bufferFrameCount = 0;
    HRESULT hr = g_pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        PrintHR("GetBufferSize failed", hr);
        return 0;
    }

    while (g_bAudioThreadRunning) {
        // Si en pause, ne pas traiter l'audio
        if (g_llPauseStart != 0) {
            PreciseSleep(10);
            continue;
        }

        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;

        // Lire le prochain échantillon audio
        hr = g_pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr)) {
            PrintHR("ReadSample(audio) failed", hr);
            break;
        }
        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample)
                pSample->Release();
            break;
        }
        if (!pSample) {
            PreciseSleep(1); // Éviter une attente active
            continue;
        }

        // Synchronisation audio en fonction du clock de lecture
        if (llTimeStamp > 0 && g_llPlaybackStartTime > 0) {
            auto sampleTimeMs = static_cast<ULONGLONG>(llTimeStamp / 10000);
            ULONGLONG currentTime = GetCurrentTimeMs();
            ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
            auto diff = static_cast<int64_t>(sampleTimeMs - effectiveElapsedTime);

            if (abs(diff) > 30) {
                if (diff > 30) {
                    // L'audio est en avance : attendre la différence
                    PreciseSleep(static_cast<DWORD>(diff));
                } else {
                    // L'audio est en retard : ignorer cet échantillon
                    pSample->Release();
                    continue;
                }
            } else if (diff > 0) {
                PreciseSleep(static_cast<DWORD>(diff));
            }
        }

        // Convertir l'échantillon en buffer contigu
        IMFMediaBuffer* pBuf = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuf);
        if (FAILED(hr) || !pBuf) {
            pSample->Release();
            continue;
        }

        BYTE* pAudioData = nullptr;
        DWORD cbMaxLen = 0, cbCurrLen = 0;
        hr = pBuf->Lock(&pAudioData, &cbMaxLen, &cbCurrLen);
        if (FAILED(hr)) {
            pBuf->Release();
            pSample->Release();
            continue;
        }

        // Obtenir le padding actuel pour connaître l'espace restant dans le buffer
        UINT32 numFramesPadding = 0;
        hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            PrintHR("GetCurrentPadding failed", hr);
            pBuf->Unlock();
            pBuf->Release();
            pSample->Release();
            continue;
        }

        // Contrôle du niveau de remplissage du buffer audio
        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;
        UINT32 blockAlign = g_pSourceAudioFormat ? g_pSourceAudioFormat->nBlockAlign : 4;
        UINT32 framesInBuffer = (blockAlign > 0) ? cbCurrLen / blockAlign : 0;
        double bufferFullness = static_cast<double>(numFramesPadding) / bufferFrameCount;
        if (bufferFullness > 0.8) {
            PreciseSleep(1);
        } else if (bufferFullness < 0.2 && framesInBuffer < bufferFramesAvailable) {
            PreciseSleep(3);
        }

        // Écriture des données audio dans le buffer de rendu
        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
            UINT32 framesToWrite = std::min(framesInBuffer, bufferFramesAvailable);
            BYTE* pDataRender = nullptr;
            hr = g_pRenderClient->GetBuffer(framesToWrite, &pDataRender);
            if (SUCCEEDED(hr) && pDataRender) {
                DWORD bytesToCopy = framesToWrite * blockAlign;
                memcpy(pDataRender, pAudioData, bytesToCopy);
                g_pRenderClient->ReleaseBuffer(framesToWrite, 0);
            } else {
                PrintHR("GetBuffer failed", hr);
            }
        }

        pBuf->Unlock();
        pBuf->Release();
        pSample->Release();

        // **Mise à jour du master clock** avec le timestamp de l'échantillon audio
        if (llTimeStamp > 0) {
            EnterCriticalSection(&g_csClockSync);
            g_llMasterClock = llTimeStamp;
            LeaveCriticalSection(&g_csClockSync);
        }

        // Attendre brièvement pour réduire l'utilisation CPU
        WaitForSingleObject(g_hAudioSamplesReadyEvent, 5);
    }

    g_pAudioClient->Stop();
    return 0;
}

// Initialize WASAPI for audio playback (event-driven mode)
static HRESULT InitWASAPI(const WAVEFORMATEX* pSourceFormat = nullptr) {
    if (g_pAudioClient && g_pRenderClient) {
        g_bAudioInitialized = true;
        return S_OK;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_pEnumerator));
    if (FAILED(hr)) {
        PrintHR("CoCreateInstance(MMDeviceEnumerator) failed", hr);
        return hr;
    }

    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) {
        PrintHR("GetDefaultAudioEndpoint failed", hr);
        return hr;
    }

    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_pAudioClient));
    if (FAILED(hr)) {
        PrintHR("Activate(IAudioClient) failed", hr);
        return hr;
    }

    WAVEFORMATEX* pwfxDevice = nullptr;
    if (!pSourceFormat) {
        hr = g_pAudioClient->GetMixFormat(&pwfxDevice);
        if (FAILED(hr)) {
            PrintHR("GetMixFormat failed", hr);
            return hr;
        }
        pSourceFormat = pwfxDevice;
    }

    if (!g_hAudioSamplesReadyEvent) {
        g_hAudioSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioSamplesReadyEvent) {
            PrintHR("CreateEvent for audio samples failed", HRESULT_FROM_WIN32(GetLastError()));
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    // Initialize audio client with a 200ms buffer duration
    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBufferDuration, 0, pSourceFormat, nullptr);
    if (FAILED(hr)) {
        PrintHR("AudioClient->Initialize failed", hr);
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = g_pAudioClient->SetEventHandle(g_hAudioSamplesReadyEvent);
    if (FAILED(hr)) {
        PrintHR("SetEventHandle failed", hr);
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_pRenderClient));
    if (FAILED(hr)) {
        PrintHR("GetService(IAudioRenderClient) failed", hr);
    } else {
        g_bAudioInitialized = true;
    }

    if (pwfxDevice) CoTaskMemFree(pwfxDevice);
    return hr;
}

// Initialize Media Foundation with COM and synchronization events
OFFSCREENPLAYER_API HRESULT InitMediaFoundation() {
    if (g_bMFInitialized) {
        return OP_E_ALREADY_INITIALIZED;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        hr = MFStartup(MF_VERSION);
    }
    if (SUCCEEDED(hr)) {
        g_bMFInitialized = true;
        // Initialize critical section for master clock synchronization
        InitializeCriticalSection(&g_csClockSync);

        g_hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioReadyEvent) {
            PrintHR("CreateEvent for audio ready failed", HRESULT_FROM_WIN32(GetLastError()));
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    } else {
        PrintHR("InitMediaFoundation failed", hr);
    }
    return hr;
}

// Open media and configure for hardware-accelerated decoding
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t* url) {
    if (!g_bMFInitialized) return OP_E_NOT_INITIALIZED;
    if (!url) return OP_E_INVALID_PARAMETER;

    // Clean up any existing media state
    CloseMedia();
    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;
    g_bHasAudio = false;

    HRESULT hr = S_OK;
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) {
        PrintHR("MFCreateAttributes failed", hr);
        return hr;
    }

    // Enable advanced video processing and hardware acceleration (DXVA)
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(ENABLE_ADVANCED_VIDEO_PROCESSING) failed", hr);
        pAttributes->Release();
        return hr;
    }
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE); // Ensure DXVA is enabled
    if (FAILED(hr)) {
        PrintHR("SetUINT32(DISABLE_DXVA) failed", hr);
        pAttributes->Release();
        return hr;
    }

    // Create source reader for video
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) failed", hr);
        return hr;
    }

    // Select video stream only
    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Video StreamSelection failed", hr);
        return hr;
    }

    // Set video output to RGB32 (decoded by hardware if possible)
    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (SUCCEEDED(hr)) {
        hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) {
            hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        }
        pType->Release();
    }
    if (FAILED(hr)) {
        PrintHR("SetCurrentMediaType(RGB32) failed", hr);
        return hr;
    }

    // Retrieve video dimensions
    IMFMediaType* pCurrent = nullptr;
    hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
    if (SUCCEEDED(hr) && pCurrent) {
        MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &g_videoWidth, &g_videoHeight);
        pCurrent->Release();
    }

    // Create audio source reader
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) failed", hr);
        g_pSourceReaderAudio = nullptr;
        return S_OK; // Proceed with video only
    }

    hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Audio StreamSelection failed", hr);
        g_pSourceReaderAudio->Release();
        g_pSourceReaderAudio = nullptr;
        return S_OK;
    }

    // Set audio output to PCM (48kHz, 16-bit, stereo)
    IMFMediaType* pWantedType = nullptr;
    hr = MFCreateMediaType(&pWantedType);
    if (SUCCEEDED(hr)) {
        hr = pWantedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = pWantedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) {
            hr = g_pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedType);
        }
        pWantedType->Release();
    }
    if (FAILED(hr)) {
        PrintHR("SetCurrentMediaType(audio PCM) failed", hr);
        g_pSourceReaderAudio->Release();
        g_pSourceReaderAudio = nullptr;
        return S_OK;
    }

    // Initialize WASAPI with the actual audio format
    IMFMediaType* pActualType = nullptr;
    hr = g_pSourceReaderAudio->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
    if (SUCCEEDED(hr) && pActualType) {
        WAVEFORMATEX* pWfx = nullptr;
        UINT32 size = 0;
        hr = MFCreateWaveFormatExFromMFMediaType(pActualType, &pWfx, &size);
        if (SUCCEEDED(hr) && pWfx) {
            hr = InitWASAPI(pWfx);
            if (FAILED(hr)) {
                PrintHR("InitWASAPI failed", hr);
                g_pSourceReaderAudio->Release();
                g_pSourceReaderAudio = nullptr;
                CoTaskMemFree(pWfx);
                pActualType->Release();
                return S_OK;
            }
            if (g_pSourceAudioFormat) CoTaskMemFree(g_pSourceAudioFormat);
            g_pSourceAudioFormat = pWfx;
        }
        pActualType->Release();
    }

    g_bHasAudio = true;
    // If audio exists and WASAPI is ready, start the audio thread
    if (g_bHasAudio && g_bAudioInitialized) {
        // Ensure no old audio thread is running
        if (g_hAudioThread) {
            WaitForSingleObject(g_hAudioThread, 5000);
            CloseHandle(g_hAudioThread);
            g_hAudioThread = nullptr;
        }

        g_bAudioThreadRunning = true;
        g_hAudioThread = CreateThread(
            nullptr,
            0,
            AudioThreadProc,
            nullptr,
            0,
            nullptr
        );

        if (!g_hAudioThread) {
            g_bAudioThreadRunning = false;
            PrintHR("CreateThread(audio) failed", HRESULT_FROM_WIN32(GetLastError()));
        } else {
            // Signal the audio thread to start playback
            if (g_hAudioReadyEvent) {
                SetEvent(g_hAudioReadyEvent);
            }
        }
    }

    return S_OK;
}

// Modified PreciseSleepHighRes function for short durations
static void PreciseSleepHighRes(double milliseconds) {
    if (milliseconds <= 0.1) return; // Return immediately for very short durations

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    // For short durations (less than 5ms), use only busy-waiting for maximum precision
    if (milliseconds < 5.0) {
        double target = milliseconds * freq.QuadPart / 1000.0;
        do {
            QueryPerformanceCounter(&end);
            // Yield to avoid hogging the CPU
            YieldProcessor();
        } while ((end.QuadPart - start.QuadPart) < target);
    } else {
        // For longer durations, use Sleep for 80% of time
        // then busy-wait for the remainder for precision
        double sleepPortion = milliseconds * 0.8;
        Sleep(static_cast<DWORD>(sleepPortion));

        // Busy-wait for the rest
        QueryPerformanceCounter(&start); // Reset starting point
        double remainingTime = milliseconds - sleepPortion;
        double target = remainingTime * freq.QuadPart / 1000.0;

        do {
            QueryPerformanceCounter(&end);
            YieldProcessor();
        } while ((end.QuadPart - start.QuadPart) < target);
    }
}

// Read a video frame with hardware decoding support and improved AV sync
// Modify the ReadVideoFrame function to better handle high frame rates
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    if (g_pLockedBuffer)
        UnlockVideoFrame();

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    DWORD streamIndex = 0;
    DWORD dwFlags = 0;
    LONGLONG llTimestamp = 0;
    IMFSample* pSample = nullptr;

    // Read the next video frame
    HRESULT hr = g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &dwFlags, &llTimestamp, &pSample);
    if (FAILED(hr)) {
        PrintHR("ReadSample(video) failed", hr);
        return hr;
    }
    if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        g_bEOF = TRUE;
        if (pSample)
            pSample->Release();
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }
    if (!pSample) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_OK;
    }


    // Get the master clock safely
    LONGLONG masterClock = 0;
    EnterCriticalSection(&g_csClockSync);
    masterClock = g_llMasterClock;
    LeaveCriticalSection(&g_csClockSync);

    // Get frame rate to adjust synchronization thresholds
    UINT frameRateNum = 30, frameRateDenom = 1;
    GetVideoFrameRate(&frameRateNum, &frameRateDenom);
    double frameTimeMs = 1000.0 * frameRateDenom / frameRateNum;

    // Calculate threshold for frame skipping based on frame rate
    // For higher frame rates, be less aggressive with skipping
    LONGLONG skipThreshold = static_cast<LONGLONG>(-frameTimeMs * 3 * 10000); // 3 frame times in 100ns units

    // AV Sync based on master clock
    if (g_bHasAudio && masterClock > 0) {
        LONGLONG diff = llTimestamp - masterClock;
        if (diff > 0) {
            // Frame is ahead of audio - wait but don't wait more than one frame time
            DWORD sleepTime = static_cast<DWORD>(std::min(diff / 10000, static_cast<LONGLONG>(frameTimeMs)));
            if (sleepTime > 1)
                PreciseSleepHighRes(sleepTime);
        } else if (diff < skipThreshold) {
            // Frame is too far behind audio - skip it
            // For 60fps, this will be around -50ms instead of a fixed -500ms
            pSample->Release();
            *pData = nullptr;
            *pDataSize = 0;
            return S_OK;
        }
    } else {
        // For playback without audio, use system clock-based synchronization
        auto frameTimeMs = static_cast<ULONGLONG>(llTimestamp / 10000);
        ULONGLONG currentTime = GetCurrentTimeMs();
        ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
        if (frameTimeMs > effectiveElapsedTime) {
            DWORD sleepTime = static_cast<DWORD>(frameTimeMs - effectiveElapsedTime);
            // Cap sleep time to prevent excessive waits, but adjust based on frame rate
            DWORD maxSleepTime = static_cast<DWORD>(frameTimeMs * 1.5);
            if (sleepTime > maxSleepTime)
                sleepTime = maxSleepTime;
            PreciseSleepHighRes(sleepTime);
        }
    }

    // Convert sample to contiguous buffer
    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) {
        PrintHR("ConvertToContiguousBuffer failed", hr);
        pSample->Release();
        return hr;
    }

    BYTE* pBytes = nullptr;
    DWORD cbMaxLen = 0, cbCurrLen = 0;
    hr = pBuffer->Lock(&pBytes, &cbMaxLen, &cbCurrLen);
    if (FAILED(hr)) {
        PrintHR("Buffer->Lock failed", hr);
        pBuffer->Release();
        pSample->Release();
        return hr;
    }

    // Update global playback position
    g_llCurrentPosition = llTimestamp;

    // Save locked buffer information for later unlocking
    g_pLockedBuffer = pBuffer;
    g_pLockedBytes = pBytes;
    g_lockedMaxSize = cbMaxLen;
    g_lockedCurrSize = cbCurrLen;

    *pData = pBytes;
    *pDataSize = cbCurrLen;

    pSample->Release();
    return S_OK;
}

// Unlock the video frame buffer
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame() {
    if (g_pLockedBuffer) {
        g_pLockedBuffer->Unlock();
        g_pLockedBuffer->Release();
        g_pLockedBuffer = nullptr;
    }
    g_pLockedBytes = nullptr;
    g_lockedMaxSize = 0;
    g_lockedCurrSize = 0;
    return S_OK;
}

// Check end-of-stream status
OFFSCREENPLAYER_API BOOL IsEOF() {
    return g_bEOF;
}

// Clean up all resources
OFFSCREENPLAYER_API void CloseMedia() {
    // Stop audio thread if running
    g_bAudioThreadRunning = false;
    if (g_hAudioThread) {
        WaitForSingleObject(g_hAudioThread, 5000);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = nullptr;
    }

    // Release locked video buffer if any
    if (g_pLockedBuffer) UnlockVideoFrame();
    if (g_pAudioClient) {
        g_pAudioClient->Stop();
        g_pAudioClient->Release();
        g_pAudioClient = nullptr;
    }
    if (g_pRenderClient) {
        g_pRenderClient->Release();
        g_pRenderClient = nullptr;
    }
    if (g_pDevice) {
        g_pDevice->Release();
        g_pDevice = nullptr;
    }
    if (g_pEnumerator) {
        g_pEnumerator->Release();
        g_pEnumerator = nullptr;
    }
    if (g_pSourceAudioFormat) {
        CoTaskMemFree(g_pSourceAudioFormat);
        g_pSourceAudioFormat = nullptr;
    }
    if (g_pSourceReader) {
        g_pSourceReader->Release();
        g_pSourceReader = nullptr;
    }
    if (g_pSourceReaderAudio) {
        g_pSourceReaderAudio->Release();
        g_pSourceReaderAudio = nullptr;
    }
    if (g_hAudioSamplesReadyEvent) {
        CloseHandle(g_hAudioSamplesReadyEvent);
        g_hAudioSamplesReadyEvent = nullptr;
    }
    if (g_hAudioReadyEvent) {
        CloseHandle(g_hAudioReadyEvent);
        g_hAudioReadyEvent = nullptr;
    }

    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;
    g_bHasAudio = false;
    g_bAudioInitialized = false;
    g_llPlaybackStartTime = 0;
    g_llTotalPauseTime = 0;
    g_llPauseStart = 0;
}

// Get video dimensions
OFFSCREENPLAYER_API void GetVideoSize(UINT32* pWidth, UINT32* pHeight) {
    if (pWidth) *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

// Get video frame rate
OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom) {
    if (!g_pSourceReader || !pNum || !pDenom) return OP_E_NOT_INITIALIZED;

    IMFMediaType* pType = nullptr;
    HRESULT hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (SUCCEEDED(hr)) {
        hr = MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, pNum, pDenom);
        pType->Release();
    }
    return hr;
}

// Seek to a specific position
OFFSCREENPLAYER_API HRESULT SeekMedia(LONGLONG llPositionIn100Ns) {
    if (!g_pSourceReader)
        return OP_E_NOT_INITIALIZED;

    // 1. Libérer toute frame verrouillée
    UnlockVideoFrame();

    // 2. Mettre en pause si nécessaire
    BOOL wasPlaying = (g_llPauseStart == 0 && g_llPlaybackStartTime > 0);
    if (wasPlaying) {
        SetPlaybackState(FALSE);  // Pause
    }

    // 3. Flush avant le seek pour vider les buffers internes
    g_pSourceReader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (g_pSourceReaderAudio) {
        g_pSourceReaderAudio->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    }

    // Préparer le seek
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = llPositionIn100Ns;

    // Effectuer le seek sur la vidéo
    HRESULT hr = g_pSourceReader->SetCurrentPosition(GUID_NULL, var);
    if (SUCCEEDED(hr) && g_pSourceReaderAudio) {
        PROPVARIANT varAudio;
        PropVariantInit(&varAudio);
        varAudio.vt = VT_I8;
        varAudio.hVal.QuadPart = llPositionIn100Ns;
        hr = g_pSourceReaderAudio->SetCurrentPosition(GUID_NULL, varAudio);
        PropVariantClear(&varAudio);
    }

    if (SUCCEEDED(hr)) {
        // 4. Réinitialiser immédiatement les timers et la master clock
        EnterCriticalSection(&g_csClockSync);
        g_llMasterClock = llPositionIn100Ns;
        LeaveCriticalSection(&g_csClockSync);

        g_llCurrentPosition = llPositionIn100Ns;
        g_llPlaybackStartTime = GetCurrentTimeMs() - (llPositionIn100Ns / 10000);
        g_llTotalPauseTime = 0;

        // Réinitialiser le client audio et le thread audio
        if (g_pAudioClient) {
            g_pAudioClient->Stop();
            g_pAudioClient->Reset();
        }
        if (g_bHasAudio && g_pSourceReaderAudio) {
            g_bAudioThreadRunning = false;
            if (g_hAudioThread) {
                SetEvent(g_hAudioSamplesReadyEvent);
                WaitForSingleObject(g_hAudioThread, 5000);
                CloseHandle(g_hAudioThread);
                g_hAudioThread = nullptr;
            }
            g_bAudioThreadRunning = true;
            g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
            if (g_hAudioReadyEvent) {
                SetEvent(g_hAudioReadyEvent);
            }
        }
    }

    PropVariantClear(&var);
    g_bEOF = FALSE;

    // Restaurer l'état de lecture si nécessaire
    if (wasPlaying) {
        SetPlaybackState(TRUE);  // Reprendre la lecture
    }

    return hr;
}


// Get media duration
OFFSCREENPLAYER_API HRESULT GetMediaDuration(LONGLONG* pDuration) {
    if (!g_pSourceReader || !pDuration) return OP_E_NOT_INITIALIZED;

    IMFMediaSource* pMediaSource = nullptr;
    IMFPresentationDescriptor* pPresentationDescriptor = nullptr;
    HRESULT hr = g_pSourceReader->GetServiceForStream(MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&pMediaSource));
    if (SUCCEEDED(hr)) {
        hr = pMediaSource->CreatePresentationDescriptor(&pPresentationDescriptor);
        if (SUCCEEDED(hr)) {
            hr = pPresentationDescriptor->GetUINT64(MF_PD_DURATION, reinterpret_cast<UINT64*>(pDuration));
            pPresentationDescriptor->Release();
        }
        pMediaSource->Release();
    }
    if (FAILED(hr)) PrintHR("GetMediaDuration failed", hr);
    return hr;
}

// Get current playback position
OFFSCREENPLAYER_API HRESULT GetMediaPosition(LONGLONG* pPosition) {
    if (!g_pSourceReader || !pPosition) return OP_E_NOT_INITIALIZED;
    *pPosition = g_llCurrentPosition;
    return S_OK;
}

OFFSCREENPLAYER_API HRESULT SetPlaybackState(BOOL bPlaying) {
    if (!g_bMFInitialized) return OP_E_NOT_INITIALIZED;

    if (bPlaying) {
        // Start playback timing
        if (g_llPlaybackStartTime == 0) {
            g_llPlaybackStartTime = GetCurrentTimeMs();
        } else if (g_llPauseStart != 0) {
            // Accumulate pause duration
            g_llTotalPauseTime += (GetCurrentTimeMs() - g_llPauseStart);
            g_llPauseStart = 0;
        }

        // Start audio playback if available
        if (g_bHasAudio && g_pAudioClient) {
            g_pAudioClient->Start();
        }
    } else {
        // Record pause time
        if (g_llPauseStart == 0) {
            g_llPauseStart = GetCurrentTimeMs();
        }

        // Stop audio playback if available
        if (g_bHasAudio && g_pAudioClient) {
            g_pAudioClient->Stop();
        }
    }

    return S_OK;
}

// Shutdown Media Foundation and clean up all resources
OFFSCREENPLAYER_API HRESULT ShutdownMediaFoundation() {
    // First close any open media
    CloseMedia();

    HRESULT hr = S_OK;

    // Shutdown Media Foundation
    if (g_bMFInitialized) {
        hr = MFShutdown();
        if (FAILED(hr)) {
            PrintHR("MFShutdown failed", hr);
        }
        g_bMFInitialized = false;
    }

    // Delete critical section for master clock synchronization
    DeleteCriticalSection(&g_csClockSync);

    // Clean up COM
    CoUninitialize();

    return hr;
}
