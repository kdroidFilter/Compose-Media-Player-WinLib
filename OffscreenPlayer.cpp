#include "OffscreenPlayer.h"
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <thread>
using std::min;

#ifdef _DEBUG
#define PrintHR(msg, hr) fprintf(stderr, "%s (hr=0x%08x)\n", msg, static_cast<unsigned int>(hr))
#else
#define PrintHR(msg, hr) ((void)0)
#endif

static bool g_bMFInitialized = false;
static IMFSourceReader* g_pSourceReader = nullptr;
static BOOL g_bEOF = FALSE;
static IMFMediaBuffer* g_pLockedBuffer = nullptr;
static BYTE* g_pLockedBytes = nullptr;
static DWORD g_lockedMaxSize = 0;
static DWORD g_lockedCurrSize = 0;
static UINT32 g_videoWidth = 0;
static UINT32 g_videoHeight = 0;
static LONGLONG g_llCurrentPosition = 0;

static IMFSourceReader* g_pSourceReaderAudio = nullptr;
static bool g_bAudioInitialized = false;
static bool g_bHasAudio = false;
static IAudioClient* g_pAudioClient = nullptr;
static IAudioRenderClient* g_pRenderClient = nullptr;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static IMMDevice* g_pDevice = nullptr;
static WAVEFORMATEX* g_pSourceAudioFormat = nullptr;
static HANDLE g_hAudioSamplesReadyEvent = nullptr;

static HANDLE g_hAudioThread = nullptr;
static bool g_bAudioThreadRunning = false;
static HANDLE g_hAudioReadyEvent = nullptr;

static ULONGLONG g_llPlaybackStartTime = 0;
static ULONGLONG g_llTotalPauseTime = 0;
static ULONGLONG g_llPauseStart = 0;
static LONGLONG g_llMasterClock = 0;
static CRITICAL_SECTION g_csClockSync;

static ID3D11Device* g_pD3DDevice = nullptr;
static IMFDXGIDeviceManager* g_pDXGIDeviceManager = nullptr;
static UINT32 g_dwResetToken = 0;

static BOOL g_bSeekInProgress = FALSE;

// Remplacement de GetTickCount64() par std::chrono::steady_clock
static inline ULONGLONG GetCurrentTimeMs() {
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Implémentation modernisée de PreciseSleepHighRes utilisant CreateWaitableTimer pour éviter une busy-wait
static void PreciseSleepHighRes(double ms) {
    if (ms <= 0.1)
        return;

    // Créer un timer haute résolution
    HANDLE hTimer = CreateWaitableTimer(nullptr, TRUE, nullptr);
    if (!hTimer) {
        // En cas d'échec, on retombe sur sleep_for
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
        return;
    }

    LARGE_INTEGER liDueTime;
    // Convertir le temps en intervalles de 100 ns et utiliser une valeur négative pour un délai relatif
    liDueTime.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);
    if (SetWaitableTimer(hTimer, &liDueTime, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(hTimer, INFINITE);
    CloseHandle(hTimer);
}

static HRESULT CreateDX11Device() {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &g_pD3DDevice, nullptr, nullptr);
    if (FAILED(hr))
        return hr;

    ID3D10Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_pD3DDevice->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&pMultithread)))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
    }
    return hr;
}

static HRESULT InitWASAPI(const WAVEFORMATEX* pSourceFormat = nullptr) {
    if (g_pAudioClient && g_pRenderClient) {
        g_bAudioInitialized = true;
        return S_OK;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_pEnumerator));
    if (FAILED(hr)) return hr;

    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) return hr;

    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_pAudioClient));
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* pwfxDevice = nullptr;
    if (!pSourceFormat) {
        hr = g_pAudioClient->GetMixFormat(&pwfxDevice);
        if (FAILED(hr)) return hr;
        pSourceFormat = pwfxDevice;
    }

    if (!g_hAudioSamplesReadyEvent) {
        g_hAudioSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioSamplesReadyEvent) return HRESULT_FROM_WIN32(GetLastError());
    }

    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    hnsBufferDuration, 0, pSourceFormat, nullptr);
    if (FAILED(hr)) {
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = g_pAudioClient->SetEventHandle(g_hAudioSamplesReadyEvent);
    if (FAILED(hr)) {
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_pRenderClient));
    if (FAILED(hr)) return hr;

    g_bAudioInitialized = true;
    if (pwfxDevice) CoTaskMemFree(pwfxDevice);
    return hr;
}

static DWORD WINAPI AudioThreadProc(LPVOID) {
    if (!g_pAudioClient || !g_pRenderClient || !g_pSourceReaderAudio)
        return 0;

    if (g_hAudioReadyEvent)
        WaitForSingleObject(g_hAudioReadyEvent, INFINITE);

    constexpr DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    UINT32 bufferFrameCount = 0;
    HRESULT hr = g_pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr))
        return 0;

    while (g_bAudioThreadRunning) {
        // Vérifier si une opération de seek est en cours ou si le son est en pause
        bool seekInProgress = false;
        EnterCriticalSection(&g_csClockSync);
        seekInProgress = g_bSeekInProgress;
        LeaveCriticalSection(&g_csClockSync);
        if (seekInProgress || g_llPauseStart != 0) {
            PreciseSleepHighRes(10);
            continue;
        }

        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;
        hr = g_pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr))
            break;

        // Vérification additionnelle du seek après ReadSample
        EnterCriticalSection(&g_csClockSync);
        seekInProgress = g_bSeekInProgress;
        LeaveCriticalSection(&g_csClockSync);
        if (seekInProgress) {
            if (pSample)
                pSample->Release();
            PreciseSleepHighRes(10);
            continue;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample)
                pSample->Release();
            break;
        }

        if (!pSample) {
            PreciseSleepHighRes(1);
            continue;
        }

        if (llTimeStamp > 0 && g_llPlaybackStartTime > 0) {
            auto sampleTimeMs = static_cast<ULONGLONG>(llTimeStamp / 10000);
            ULONGLONG currentTime = GetCurrentTimeMs();
            ULONGLONG effectiveElapsed = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
            int64_t diff = static_cast<int64_t>(sampleTimeMs - effectiveElapsed);
            if (abs(diff) > 30) {
                if (diff > 30)
                    PreciseSleepHighRes(diff);
                else {
                    pSample->Release();
                    continue;
                }
            } else if (diff > 0)
                PreciseSleepHighRes(diff);
        }

        IMFMediaBuffer* pBuf = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuf);
        if (FAILED(hr) || !pBuf) {
            pSample->Release();
            continue;
        }

        BYTE* pAudioData = nullptr;
        DWORD cbMax = 0, cbCurr = 0;
        hr = pBuf->Lock(&pAudioData, &cbMax, &cbCurr);
        if (FAILED(hr)) {
            pBuf->Release();
            pSample->Release();
            continue;
        }

        UINT32 numFramesPadding = 0;
        hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            pBuf->Unlock();
            pBuf->Release();
            pSample->Release();
            continue;
        }

        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;
        UINT32 blockAlign = g_pSourceAudioFormat ? g_pSourceAudioFormat->nBlockAlign : 4;
        UINT32 framesInBuffer = (blockAlign > 0) ? cbCurr / blockAlign : 0;
        double bufferFullness = static_cast<double>(numFramesPadding) / bufferFrameCount;

        if (bufferFullness > 0.8)
            PreciseSleepHighRes(1);
        else if (bufferFullness < 0.2 && framesInBuffer < bufferFramesAvailable)
            PreciseSleepHighRes(3);

        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
            UINT32 framesToWrite = min(framesInBuffer, bufferFramesAvailable);
            BYTE* pDataRender = nullptr;
            hr = g_pRenderClient->GetBuffer(framesToWrite, &pDataRender);
            if (SUCCEEDED(hr) && pDataRender) {
                DWORD bytesToCopy = framesToWrite * blockAlign;
                memcpy(pDataRender, pAudioData, bytesToCopy);
                g_pRenderClient->ReleaseBuffer(framesToWrite, 0);
            }
        }

        pBuf->Unlock();
        pBuf->Release();
        pSample->Release();

        if (llTimeStamp > 0) {
            EnterCriticalSection(&g_csClockSync);
            g_llMasterClock = llTimeStamp;
            LeaveCriticalSection(&g_csClockSync);
        }

        WaitForSingleObject(g_hAudioSamplesReadyEvent, 5);
    }

    g_pAudioClient->Stop();
    return 0;
}

OFFSCREENPLAYER_API HRESULT InitMediaFoundation() {
    if (g_bMFInitialized)
        return OP_E_ALREADY_INITIALIZED;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE)
        hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
        return hr;

    hr = CreateDX11Device();
    if (FAILED(hr)) { MFShutdown(); return hr; }

    hr = MFCreateDXGIDeviceManager(&g_dwResetToken, &g_pDXGIDeviceManager);
    if (SUCCEEDED(hr))
        hr = g_pDXGIDeviceManager->ResetDevice(g_pD3DDevice, g_dwResetToken);
    if (FAILED(hr)) {
        if (g_pD3DDevice) { g_pD3DDevice->Release(); g_pD3DDevice = nullptr; }
        MFShutdown();
        return hr;
    }

    InitializeCriticalSection(&g_csClockSync);
    g_hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_hAudioReadyEvent) {
        DeleteCriticalSection(&g_csClockSync);
        MFShutdown();
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_bMFInitialized = true;
    return S_OK;
}

OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t* url) {
    if (!g_bMFInitialized)
        return OP_E_NOT_INITIALIZED;
    if (!url)
        return OP_E_INVALID_PARAMETER;

    CloseMedia();
    g_bEOF = FALSE;
    g_videoWidth = g_videoHeight = 0;
    g_bHasAudio = false;

    HRESULT hr = S_OK;
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 4);
    if (FAILED(hr))
        return hr;

    pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    pAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_pDXGIDeviceManager);
    pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();
    if (FAILED(hr))
        return hr;

    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr))
        return hr;

    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (SUCCEEDED(hr)) {
        hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr))
            hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr))
            hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        pType->Release();
    }
    if (FAILED(hr))
        return hr;

    IMFMediaType* pCurrent = nullptr;
    hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
    if (SUCCEEDED(hr) && pCurrent) {
        MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &g_videoWidth, &g_videoHeight);
        pCurrent->Release();
    }

    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) { g_pSourceReaderAudio = nullptr; return S_OK; }

    hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) { g_pSourceReaderAudio->Release(); g_pSourceReaderAudio = nullptr; return S_OK; }

    IMFMediaType* pWantedType = nullptr;
    hr = MFCreateMediaType(&pWantedType);
    if (SUCCEEDED(hr)) {
        hr = pWantedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        if (SUCCEEDED(hr))
            hr = pWantedType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr))
            hr = g_pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedType);
        pWantedType->Release();
    }
    if (FAILED(hr)) { g_pSourceReaderAudio->Release(); g_pSourceReaderAudio = nullptr; return S_OK; }

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
                if (pWfx) CoTaskMemFree(pWfx);
                pActualType->Release();
                g_pSourceReaderAudio->Release();
                g_pSourceReaderAudio = nullptr;
                return S_OK;
            }
            if (g_pSourceAudioFormat)
                CoTaskMemFree(g_pSourceAudioFormat);
            g_pSourceAudioFormat = pWfx;
        }
        pActualType->Release();
    }

    g_bHasAudio = true;
    if (g_bHasAudio && g_bAudioInitialized) {
        if (g_hAudioThread) {
            WaitForSingleObject(g_hAudioThread, 5000);
            CloseHandle(g_hAudioThread);
            g_hAudioThread = nullptr;
        }
        g_bAudioThreadRunning = true;
        g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
        if (!g_hAudioThread) {
            g_bAudioThreadRunning = false;
            PrintHR("CreateThread(audio) failed", HRESULT_FROM_WIN32(GetLastError()));
        } else if (g_hAudioReadyEvent)
            SetEvent(g_hAudioReadyEvent);
    }

    return S_OK;
}

OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize) {
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    if (g_pLockedBuffer)
        UnlockVideoFrame();

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    DWORD streamIndex = 0, dwFlags = 0;
    LONGLONG llTimestamp = 0;
    IMFSample* pSample = nullptr;
    HRESULT hr = g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &dwFlags, &llTimestamp, &pSample);
    if (FAILED(hr))
        return hr;

    if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        g_bEOF = TRUE;
        if (pSample) pSample->Release();
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    if (!pSample) { *pData = nullptr; *pDataSize = 0; return S_OK; }

    LONGLONG masterClock = 0;
    EnterCriticalSection(&g_csClockSync);
    masterClock = g_llMasterClock;
    LeaveCriticalSection(&g_csClockSync);

    UINT frameRateNum = 30, frameRateDenom = 1;
    GetVideoFrameRate(&frameRateNum, &frameRateDenom);
    double frameTimeMs = 1000.0 * frameRateDenom / frameRateNum;
    auto skipThreshold = static_cast<LONGLONG>(-frameTimeMs * 3 * 10000);

    if (g_bHasAudio && masterClock > 0) {
        LONGLONG diff = llTimestamp - masterClock;
        if (diff > 0) {
            DWORD sleepTime = static_cast<DWORD>(min(diff / 10000, static_cast<LONGLONG>(frameTimeMs)));
            if (sleepTime > 1)
                PreciseSleepHighRes(sleepTime);
        } else if (diff < skipThreshold) {
            pSample->Release();
            *pData = nullptr;
            *pDataSize = 0;
            return S_OK;
        }
    } else {
        auto frameTimeAbs = static_cast<ULONGLONG>(llTimestamp / 10000);
        ULONGLONG currentTime = GetCurrentTimeMs();
        ULONGLONG effectiveElapsed = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
        if (frameTimeAbs > effectiveElapsed) {
            auto sleepTime = static_cast<DWORD>(frameTimeAbs - effectiveElapsed);
            auto maxSleep = static_cast<DWORD>(frameTimeMs * 1.5);
            if (sleepTime > maxSleep)
                sleepTime = maxSleep;
            PreciseSleepHighRes(sleepTime);
        }
    }

    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) {
        PrintHR("ConvertToContiguousBuffer failed", hr);
        pSample->Release();
        return hr;
    }

    BYTE* pBytes = nullptr;
    DWORD cbMax = 0, cbCurr = 0;
    hr = pBuffer->Lock(&pBytes, &cbMax, &cbCurr);
    if (FAILED(hr)) {
        PrintHR("Buffer->Lock failed", hr);
        pBuffer->Release();
        pSample->Release();
        return hr;
    }

    g_llCurrentPosition = llTimestamp;
    g_pLockedBuffer = pBuffer;
    g_pLockedBytes = pBytes;
    g_lockedMaxSize = cbMax;
    g_lockedCurrSize = cbCurr;
    *pData = pBytes;
    *pDataSize = cbCurr;
    pSample->Release();
    return S_OK;
}

OFFSCREENPLAYER_API HRESULT UnlockVideoFrame() {
    if (g_pLockedBuffer) {
        g_pLockedBuffer->Unlock();
        g_pLockedBuffer->Release();
        g_pLockedBuffer = nullptr;
    }
    g_pLockedBytes = nullptr;
    g_lockedMaxSize = g_lockedCurrSize = 0;
    return S_OK;
}

OFFSCREENPLAYER_API BOOL IsEOF() {
    return g_bEOF;
}

OFFSCREENPLAYER_API void GetVideoSize(UINT32* pWidth, UINT32* pHeight) {
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom) {
    if (!g_pSourceReader || !pNum || !pDenom)
        return OP_E_NOT_INITIALIZED;

    IMFMediaType* pType = nullptr;
    HRESULT hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (SUCCEEDED(hr)) {
        hr = MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, pNum, pDenom);
        pType->Release();
    }
    return hr;
}

OFFSCREENPLAYER_API HRESULT SeekMedia(LONGLONG llPositionIn100Ns) {
    if (!g_bMFInitialized || !g_pSourceReader)
        return OP_E_NOT_INITIALIZED;

    // Set seek in progress flag with proper synchronization
    EnterCriticalSection(&g_csClockSync);
    g_bSeekInProgress = TRUE;
    LeaveCriticalSection(&g_csClockSync);

    if (g_llPauseStart != 0) {
        // If paused, update total pause time before seeking
        g_llTotalPauseTime += (GetCurrentTimeMs() - g_llPauseStart);
        g_llPauseStart = GetCurrentTimeMs(); // Reset pause start time
    }

    // Unlock any locked video frame
    if (g_pLockedBuffer)
        UnlockVideoFrame();

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = llPositionIn100Ns;

    // Stop audio rendering if active
    bool wasPlaying = false;
    if (g_bHasAudio && g_pAudioClient) {
        wasPlaying = (g_llPauseStart == 0);
        g_pAudioClient->Stop();

        // Add small delay to ensure audio processing has paused
        Sleep(5);
    }

    // Seek video stream
    HRESULT hr = g_pSourceReader->SetCurrentPosition(GUID_NULL, var);
    if (FAILED(hr)) {
        EnterCriticalSection(&g_csClockSync);
        g_bSeekInProgress = FALSE;
        LeaveCriticalSection(&g_csClockSync);
        PropVariantClear(&var);
        return hr;
    }

    // Seek audio stream if available
    if (g_bHasAudio && g_pSourceReaderAudio) {
        hr = g_pSourceReaderAudio->SetCurrentPosition(GUID_NULL, var);
        if (FAILED(hr)) {
            PrintHR("Failed to seek audio stream", hr);
            // Continue anyway, just log the error
        }

        // Clear audio buffers
        if (g_pRenderClient && g_pAudioClient) {
            UINT32 bufferFrameCount = 0;
            if (SUCCEEDED(g_pAudioClient->GetBufferSize(&bufferFrameCount))) {
                g_pAudioClient->Reset();
            }
        }
    }

    PropVariantClear(&var);

    // Reset timing information
    EnterCriticalSection(&g_csClockSync);
    g_llMasterClock = llPositionIn100Ns;
    g_llCurrentPosition = llPositionIn100Ns;
    g_bSeekInProgress = FALSE;
    LeaveCriticalSection(&g_csClockSync);

    // Reset EOF flag as we may have seeked away from the end
    g_bEOF = FALSE;

    // Adjust playback start time to maintain correct timing after seek
    ULONGLONG currentTime = GetCurrentTimeMs();
    g_llPlaybackStartTime = currentTime - static_cast<ULONGLONG>(llPositionIn100Ns / 10000);

    // Restart audio if it was playing before
    if (g_bHasAudio && g_pAudioClient && wasPlaying) {
        // Add small delay to ensure seeking has completed
        Sleep(5);
        g_pAudioClient->Start();
    }

    // Notify audio thread that we've performed a seek operation
    if (g_hAudioReadyEvent)
        SetEvent(g_hAudioReadyEvent);

    return S_OK;
}

OFFSCREENPLAYER_API HRESULT GetMediaDuration(LONGLONG* pDuration) {
    if (!g_pSourceReader || !pDuration)
        return OP_E_NOT_INITIALIZED;

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
    return hr;
}

OFFSCREENPLAYER_API HRESULT GetMediaPosition(LONGLONG* pPosition) {
    if (!g_pSourceReader || !pPosition)
        return OP_E_NOT_INITIALIZED;

    *pPosition = g_llCurrentPosition;
    return S_OK;
}

OFFSCREENPLAYER_API HRESULT SetPlaybackState(BOOL bPlaying) {
    if (!g_bMFInitialized)
        return OP_E_NOT_INITIALIZED;

    if (bPlaying) {
        if (g_llPlaybackStartTime == 0)
            g_llPlaybackStartTime = GetCurrentTimeMs();
        else if (g_llPauseStart != 0) {
            g_llTotalPauseTime += (GetCurrentTimeMs() - g_llPauseStart);
            g_llPauseStart = 0;
        }
        if (g_bHasAudio && g_pAudioClient)
            g_pAudioClient->Start();
    } else {
        if (g_llPauseStart == 0)
            g_llPauseStart = GetCurrentTimeMs();
        if (g_bHasAudio && g_pAudioClient)
            g_pAudioClient->Stop();
    }
    return S_OK;
}

OFFSCREENPLAYER_API HRESULT ShutdownMediaFoundation() {
    CloseMedia();
    HRESULT hr = S_OK;
    if (g_bMFInitialized) {
        hr = MFShutdown();
        g_bMFInitialized = false;
    }
    DeleteCriticalSection(&g_csClockSync);
    if (g_pDXGIDeviceManager) {
        g_pDXGIDeviceManager->Release();
        g_pDXGIDeviceManager = nullptr;
    }
    if (g_pD3DDevice) {
        g_pD3DDevice->Release();
        g_pD3DDevice = nullptr;
    }
    CoUninitialize();
    return hr;
}

OFFSCREENPLAYER_API void CloseMedia() {
    g_bAudioThreadRunning = false;
    if (g_hAudioThread) {
        WaitForSingleObject(g_hAudioThread, 5000);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = nullptr;
    }
    if (g_pLockedBuffer)
        UnlockVideoFrame();
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
    g_videoWidth = g_videoHeight = 0;
    g_bHasAudio = false;
    g_bAudioInitialized = false;
    g_llPlaybackStartTime = g_llTotalPauseTime = g_llPauseStart = 0;
}
