// NativeVideoPlayer.cpp
#include "NativeVideoPlayer.h"
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
#include <endpointvolume.h>
#include <algorithm>
#include <chrono>
#include <thread>
using std::min;

#ifdef _DEBUG
#define PrintHR(msg, hr) fprintf(stderr, "%s (hr=0x%08x)\n", msg, static_cast<unsigned int>(hr))
#else
#define PrintHR(msg, hr) ((void)0)
#endif

// Ressources globales partagées entre toutes les instances
static bool g_bMFInitialized = false;
static ID3D11Device* g_pD3DDevice = nullptr;
static IMFDXGIDeviceManager* g_pDXGIDeviceManager = nullptr;
static UINT32 g_dwResetToken = 0;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static int g_instanceCount = 0;

// Structure pour encapsuler l'état d'une instance
struct VideoPlayerInstance {
    IMFSourceReader* pSourceReader = nullptr;
    IMFSourceReader* pSourceReaderAudio = nullptr;
    BOOL bEOF = FALSE;
    IMFMediaBuffer* pLockedBuffer = nullptr;
    BYTE* pLockedBytes = nullptr;
    DWORD lockedMaxSize = 0;
    DWORD lockedCurrSize = 0;
    UINT32 videoWidth = 0;
    UINT32 videoHeight = 0;
    LONGLONG llCurrentPosition = 0;
    BOOL bHasAudio = FALSE;
    BOOL bAudioInitialized = FALSE;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;
    IMMDevice* pDevice = nullptr;
    WAVEFORMATEX* pSourceAudioFormat = nullptr;
    HANDLE hAudioSamplesReadyEvent = nullptr;
    HANDLE hAudioThread = nullptr;
    BOOL bAudioThreadRunning = FALSE;
    HANDLE hAudioReadyEvent = nullptr;
    ULONGLONG llPlaybackStartTime = 0;
    ULONGLONG llTotalPauseTime = 0;
    ULONGLONG llPauseStart = 0;
    LONGLONG llMasterClock = 0;
    CRITICAL_SECTION csClockSync;
    BOOL bSeekInProgress = FALSE;
    IAudioEndpointVolume* pAudioEndpointVolume = nullptr;
};

// Fonctions utilitaires
static inline ULONGLONG GetCurrentTimeMs() {
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void PreciseSleepHighRes(double ms) {
    if (ms <= 0.1)
        return;
    HANDLE hTimer = CreateWaitableTimer(nullptr, TRUE, nullptr);
    if (!hTimer) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
        return;
    }
    LARGE_INTEGER liDueTime;
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

static HRESULT InitWASAPI(VideoPlayerInstance* pInstance, const WAVEFORMATEX* pSourceFormat = nullptr) {
    if (!pInstance)
        return E_INVALIDARG;
    if (pInstance->pAudioClient && pInstance->pRenderClient) {
        pInstance->bAudioInitialized = TRUE;
        return S_OK;
    }

    if (!g_pEnumerator) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_pEnumerator));
        if (FAILED(hr)) return hr;
    }

    HRESULT hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pInstance->pDevice);
    if (FAILED(hr)) return hr;

    hr = pInstance->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pInstance->pAudioClient));
    if (FAILED(hr)) return hr;

    hr = pInstance->pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pInstance->pAudioEndpointVolume));
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* pwfxDevice = nullptr;
    if (!pSourceFormat) {
        hr = pInstance->pAudioClient->GetMixFormat(&pwfxDevice);
        if (FAILED(hr)) return hr;
        pSourceFormat = pwfxDevice;
    }

    if (!pInstance->hAudioSamplesReadyEvent) {
        pInstance->hAudioSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!pInstance->hAudioSamplesReadyEvent) return HRESULT_FROM_WIN32(GetLastError());
    }

    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = pInstance->pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             hnsBufferDuration, 0, pSourceFormat, nullptr);
    if (FAILED(hr)) {
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = pInstance->pAudioClient->SetEventHandle(pInstance->hAudioSamplesReadyEvent);
    if (FAILED(hr)) {
        if (pwfxDevice) CoTaskMemFree(pwfxDevice);
        return hr;
    }

    hr = pInstance->pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pInstance->pRenderClient));
    if (FAILED(hr)) return hr;

    pInstance->bAudioInitialized = TRUE;
    if (pwfxDevice) CoTaskMemFree(pwfxDevice);
    return hr;
}

static DWORD WINAPI AudioThreadProc(LPVOID lpParam) {
    VideoPlayerInstance* pInstance = static_cast<VideoPlayerInstance*>(lpParam);
    if (!pInstance || !pInstance->pAudioClient || !pInstance->pRenderClient || !pInstance->pSourceReaderAudio)
        return 0;

    if (pInstance->hAudioReadyEvent)
        WaitForSingleObject(pInstance->hAudioReadyEvent, INFINITE);

    constexpr DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    UINT32 bufferFrameCount = 0;
    HRESULT hr = pInstance->pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr))
        return 0;

    while (pInstance->bAudioThreadRunning) {
        bool seekInProgress = false;
        EnterCriticalSection(&pInstance->csClockSync);
        seekInProgress = pInstance->bSeekInProgress;
        LeaveCriticalSection(&pInstance->csClockSync);
        if (seekInProgress || pInstance->llPauseStart != 0) {
            PreciseSleepHighRes(10);
            continue;
        }

        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;
        hr = pInstance->pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr))
            break;

        EnterCriticalSection(&pInstance->csClockSync);
        seekInProgress = pInstance->bSeekInProgress;
        LeaveCriticalSection(&pInstance->csClockSync);
        if (seekInProgress) {
            if (pSample) pSample->Release();
            PreciseSleepHighRes(10);
            continue;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample) pSample->Release();
            break;
        }

        if (!pSample) {
            PreciseSleepHighRes(1);
            continue;
        }

        if (llTimeStamp > 0 && pInstance->llPlaybackStartTime > 0) {
            auto sampleTimeMs = static_cast<ULONGLONG>(llTimeStamp / 10000);
            ULONGLONG currentTime = GetCurrentTimeMs();
            ULONGLONG effectiveElapsed = currentTime - pInstance->llPlaybackStartTime - pInstance->llTotalPauseTime;
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
        hr = pInstance->pAudioClient->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            pBuf->Unlock();
            pBuf->Release();
            pSample->Release();
            continue;
        }

        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;
        UINT32 blockAlign = pInstance->pSourceAudioFormat ? pInstance->pSourceAudioFormat->nBlockAlign : 4;
        UINT32 framesInBuffer = (blockAlign > 0) ? cbCurr / blockAlign : 0;
        double bufferFullness = static_cast<double>(numFramesPadding) / bufferFrameCount;

        if (bufferFullness > 0.8)
            PreciseSleepHighRes(1);
        else if (bufferFullness < 0.2 && framesInBuffer < bufferFramesAvailable)
            PreciseSleepHighRes(3);

        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
            UINT32 framesToWrite = min(framesInBuffer, bufferFramesAvailable);
            BYTE* pDataRender = nullptr;
            hr = pInstance->pRenderClient->GetBuffer(framesToWrite, &pDataRender);
            if (SUCCEEDED(hr) && pDataRender) {
                DWORD bytesToCopy = framesToWrite * blockAlign;
                memcpy(pDataRender, pAudioData, bytesToCopy);
                pInstance->pRenderClient->ReleaseBuffer(framesToWrite, 0);
            }
        }

        pBuf->Unlock();
        pBuf->Release();
        pSample->Release();

        if (llTimeStamp > 0) {
            EnterCriticalSection(&pInstance->csClockSync);
            pInstance->llMasterClock = llTimeStamp;
            LeaveCriticalSection(&pInstance->csClockSync);
        }

        WaitForSingleObject(pInstance->hAudioSamplesReadyEvent, 5);
    }

    pInstance->pAudioClient->Stop();
    return 0;
}

// Implémentation des fonctions exportées
NATIVEVIDEOPLAYER_API HRESULT InitMediaFoundation() {
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

    g_bMFInitialized = true;
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT CreateVideoPlayerInstance(VideoPlayerInstance** ppInstance) {
    if (!ppInstance)
        return E_INVALIDARG;

    if (!g_bMFInitialized) {
        HRESULT hr = InitMediaFoundation();
        if (FAILED(hr))
            return hr;
    }

    VideoPlayerInstance* pInstance = new VideoPlayerInstance();
    if (!pInstance)
        return E_OUTOFMEMORY;

    memset(pInstance, 0, sizeof(VideoPlayerInstance));
    InitializeCriticalSection(&pInstance->csClockSync);
    pInstance->hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!pInstance->hAudioReadyEvent) {
        DeleteCriticalSection(&pInstance->csClockSync);
        delete pInstance;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_instanceCount++;
    *ppInstance = pInstance;
    return S_OK;
}

NATIVEVIDEOPLAYER_API void DestroyVideoPlayerInstance(VideoPlayerInstance* pInstance) {
    if (pInstance) {
        CloseMedia(pInstance);
        if (pInstance->hAudioReadyEvent) {
            CloseHandle(pInstance->hAudioReadyEvent);
        }
        DeleteCriticalSection(&pInstance->csClockSync);
        delete pInstance;
        g_instanceCount--;
    }
}

NATIVEVIDEOPLAYER_API HRESULT OpenMedia(VideoPlayerInstance* pInstance, const wchar_t* url) {
    if (!pInstance || !url)
        return OP_E_INVALID_PARAMETER;
    if (!g_bMFInitialized)
        return OP_E_NOT_INITIALIZED;

    CloseMedia(pInstance);
    pInstance->bEOF = FALSE;
    pInstance->videoWidth = pInstance->videoHeight = 0;
    pInstance->bHasAudio = FALSE;

    HRESULT hr = S_OK;
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 4);
    if (FAILED(hr))
        return hr;

    pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    pAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_pDXGIDeviceManager);
    pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromURL(url, pAttributes, &pInstance->pSourceReader);
    pAttributes->Release();
    if (FAILED(hr))
        return hr;

    hr = pInstance->pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = pInstance->pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr))
        return hr;

    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (SUCCEEDED(hr)) {
        hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr))
            hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr))
            hr = pInstance->pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        pType->Release();
    }
    if (FAILED(hr))
        return hr;

    IMFMediaType* pCurrent = nullptr;
    hr = pInstance->pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
    if (SUCCEEDED(hr) && pCurrent) {
        MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &pInstance->videoWidth, &pInstance->videoHeight);
        pCurrent->Release();
    }

    hr = MFCreateSourceReaderFromURL(url, nullptr, &pInstance->pSourceReaderAudio);
    if (FAILED(hr)) { pInstance->pSourceReaderAudio = nullptr; return S_OK; }

    hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) { pInstance->pSourceReaderAudio->Release(); pInstance->pSourceReaderAudio = nullptr; return S_OK; }

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
            hr = pInstance->pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedType);
        pWantedType->Release();
    }
    if (FAILED(hr)) { pInstance->pSourceReaderAudio->Release(); pInstance->pSourceReaderAudio = nullptr; return S_OK; }

    IMFMediaType* pActualType = nullptr;
    hr = pInstance->pSourceReaderAudio->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
    if (SUCCEEDED(hr) && pActualType) {
        WAVEFORMATEX* pWfx = nullptr;
        UINT32 size = 0;
        hr = MFCreateWaveFormatExFromMFMediaType(pActualType, &pWfx, &size);
        if (SUCCEEDED(hr) && pWfx) {
            hr = InitWASAPI(pInstance, pWfx);
            if (FAILED(hr)) {
                PrintHR("InitWASAPI failed", hr);
                if (pWfx) CoTaskMemFree(pWfx);
                pActualType->Release();
                pInstance->pSourceReaderAudio->Release();
                pInstance->pSourceReaderAudio = nullptr;
                return S_OK;
            }
            if (pInstance->pSourceAudioFormat)
                CoTaskMemFree(pInstance->pSourceAudioFormat);
            pInstance->pSourceAudioFormat = pWfx;
        }
        pActualType->Release();
    }

    pInstance->bHasAudio = TRUE;
    if (pInstance->bHasAudio && pInstance->bAudioInitialized) {
        if (pInstance->hAudioThread) {
            WaitForSingleObject(pInstance->hAudioThread, 5000);
            CloseHandle(pInstance->hAudioThread);
            pInstance->hAudioThread = nullptr;
        }
        pInstance->bAudioThreadRunning = TRUE;
        pInstance->hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, pInstance, 0, nullptr);
        if (!pInstance->hAudioThread) {
            pInstance->bAudioThreadRunning = FALSE;
            PrintHR("CreateThread(audio) failed", HRESULT_FROM_WIN32(GetLastError()));
        } else if (pInstance->hAudioReadyEvent)
            SetEvent(pInstance->hAudioReadyEvent);
    }

    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT ReadVideoFrame(VideoPlayerInstance* pInstance, BYTE** pData, DWORD* pDataSize) {
    if (!pInstance || !pInstance->pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    if (pInstance->pLockedBuffer)
        UnlockVideoFrame(pInstance);

    if (pInstance->bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    DWORD streamIndex = 0, dwFlags = 0;
    LONGLONG llTimestamp = 0;
    IMFSample* pSample = nullptr;
    HRESULT hr = pInstance->pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &dwFlags, &llTimestamp, &pSample);
    if (FAILED(hr))
        return hr;

    if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        pInstance->bEOF = TRUE;
        if (pSample) pSample->Release();
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    if (!pSample) { *pData = nullptr; *pDataSize = 0; return S_OK; }

    LONGLONG masterClock = 0;
    EnterCriticalSection(&pInstance->csClockSync);
    masterClock = pInstance->llMasterClock;
    LeaveCriticalSection(&pInstance->csClockSync);

    UINT frameRateNum = 30, frameRateDenom = 1;
    GetVideoFrameRate(pInstance, &frameRateNum, &frameRateDenom);
    double frameTimeMs = 1000.0 * frameRateDenom / frameRateNum;
    auto skipThreshold = static_cast<LONGLONG>(-frameTimeMs * 3 * 10000);

    if (pInstance->bHasAudio && masterClock > 0) {
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
        ULONGLONG effectiveElapsed = currentTime - pInstance->llPlaybackStartTime - pInstance->llTotalPauseTime;
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

    pInstance->llCurrentPosition = llTimestamp;
    pInstance->pLockedBuffer = pBuffer;
    pInstance->pLockedBytes = pBytes;
    pInstance->lockedMaxSize = cbMax;
    pInstance->lockedCurrSize = cbCurr;
    *pData = pBytes;
    *pDataSize = cbCurr;
    pSample->Release();
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT UnlockVideoFrame(VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return E_INVALIDARG;
    if (pInstance->pLockedBuffer) {
        pInstance->pLockedBuffer->Unlock();
        pInstance->pLockedBuffer->Release();
        pInstance->pLockedBuffer = nullptr;
    }
    pInstance->pLockedBytes = nullptr;
    pInstance->lockedMaxSize = pInstance->lockedCurrSize = 0;
    return S_OK;
}

NATIVEVIDEOPLAYER_API BOOL IsEOF(VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return FALSE;
    return pInstance->bEOF;
}

NATIVEVIDEOPLAYER_API void GetVideoSize(VideoPlayerInstance* pInstance, UINT32* pWidth, UINT32* pHeight) {
    if (!pInstance)
        return;
    if (pWidth)  *pWidth = pInstance->videoWidth;
    if (pHeight) *pHeight = pInstance->videoHeight;
}

NATIVEVIDEOPLAYER_API HRESULT GetVideoFrameRate(VideoPlayerInstance* pInstance, UINT* pNum, UINT* pDenom) {
    if (!pInstance || !pInstance->pSourceReader || !pNum || !pDenom)
        return OP_E_NOT_INITIALIZED;

    IMFMediaType* pType = nullptr;
    HRESULT hr = pInstance->pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (SUCCEEDED(hr)) {
        hr = MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, pNum, pDenom);
        pType->Release();
    }
    return hr;
}

NATIVEVIDEOPLAYER_API HRESULT SeekMedia(VideoPlayerInstance* pInstance, LONGLONG llPositionIn100Ns) {
    if (!pInstance || !pInstance->pSourceReader)
        return OP_E_NOT_INITIALIZED;

    EnterCriticalSection(&pInstance->csClockSync);
    pInstance->bSeekInProgress = TRUE;
    LeaveCriticalSection(&pInstance->csClockSync);

    if (pInstance->llPauseStart != 0) {
        pInstance->llTotalPauseTime += (GetCurrentTimeMs() - pInstance->llPauseStart);
        pInstance->llPauseStart = GetCurrentTimeMs();
    }

    if (pInstance->pLockedBuffer)
        UnlockVideoFrame(pInstance);

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = llPositionIn100Ns;

    bool wasPlaying = false;
    if (pInstance->bHasAudio && pInstance->pAudioClient) {
        wasPlaying = (pInstance->llPauseStart == 0);
        pInstance->pAudioClient->Stop();
        Sleep(5);
    }

    HRESULT hr = pInstance->pSourceReader->SetCurrentPosition(GUID_NULL, var);
    if (FAILED(hr)) {
        EnterCriticalSection(&pInstance->csClockSync);
        pInstance->bSeekInProgress = FALSE;
        LeaveCriticalSection(&pInstance->csClockSync);
        PropVariantClear(&var);
        return hr;
    }

    if (pInstance->bHasAudio && pInstance->pSourceReaderAudio) {
        hr = pInstance->pSourceReaderAudio->SetCurrentPosition(GUID_NULL, var);
        if (FAILED(hr)) {
            PrintHR("Failed to seek audio stream", hr);
        }
        if (pInstance->pRenderClient && pInstance->pAudioClient) {
            UINT32 bufferFrameCount = 0;
            if (SUCCEEDED(pInstance->pAudioClient->GetBufferSize(&bufferFrameCount))) {
                pInstance->pAudioClient->Reset();
            }
        }
    }

    PropVariantClear(&var);

    EnterCriticalSection(&pInstance->csClockSync);
    pInstance->llMasterClock = llPositionIn100Ns;
    pInstance->llCurrentPosition = llPositionIn100Ns;
    pInstance->bSeekInProgress = FALSE;
    LeaveCriticalSection(&pInstance->csClockSync);

    pInstance->bEOF = FALSE;

    ULONGLONG currentTime = GetCurrentTimeMs();
    pInstance->llPlaybackStartTime = currentTime - static_cast<ULONGLONG>(llPositionIn100Ns / 10000);

    if (pInstance->bHasAudio && pInstance->pAudioClient && wasPlaying) {
        Sleep(5);
        pInstance->pAudioClient->Start();
    }

    if (pInstance->hAudioReadyEvent)
        SetEvent(pInstance->hAudioReadyEvent);

    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT GetMediaDuration(VideoPlayerInstance* pInstance, LONGLONG* pDuration) {
    if (!pInstance || !pInstance->pSourceReader || !pDuration)
        return OP_E_NOT_INITIALIZED;

    IMFMediaSource* pMediaSource = nullptr;
    IMFPresentationDescriptor* pPresentationDescriptor = nullptr;
    HRESULT hr = pInstance->pSourceReader->GetServiceForStream(MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&pMediaSource));
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

NATIVEVIDEOPLAYER_API HRESULT GetMediaPosition(VideoPlayerInstance* pInstance, LONGLONG* pPosition) {
    if (!pInstance || !pPosition)
        return OP_E_NOT_INITIALIZED;

    *pPosition = pInstance->llCurrentPosition;
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT SetPlaybackState(VideoPlayerInstance* pInstance, BOOL bPlaying) {
    if (!pInstance)
        return OP_E_NOT_INITIALIZED;

    if (bPlaying) {
        if (pInstance->llPlaybackStartTime == 0)
            pInstance->llPlaybackStartTime = GetCurrentTimeMs();
        else if (pInstance->llPauseStart != 0) {
            pInstance->llTotalPauseTime += (GetCurrentTimeMs() - pInstance->llPauseStart);
            pInstance->llPauseStart = 0;
        }
        if (pInstance->bHasAudio && pInstance->pAudioClient)
            pInstance->pAudioClient->Start();
    } else {
        if (pInstance->llPauseStart == 0)
            pInstance->llPauseStart = GetCurrentTimeMs();
        if (pInstance->bHasAudio && pInstance->pAudioClient)
            pInstance->pAudioClient->Stop();
    }
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT ShutdownMediaFoundation() {
    if (g_instanceCount > 0)
        return E_FAIL; // Instances encore actives

    HRESULT hr = S_OK;
    if (g_bMFInitialized) {
        hr = MFShutdown();
        g_bMFInitialized = false;
    }
    if (g_pDXGIDeviceManager) {
        g_pDXGIDeviceManager->Release();
        g_pDXGIDeviceManager = nullptr;
    }
    if (g_pD3DDevice) {
        g_pD3DDevice->Release();
        g_pD3DDevice = nullptr;
    }
    if (g_pEnumerator) {
        g_pEnumerator->Release();
        g_pEnumerator = nullptr;
    }
    CoUninitialize();
    return hr;
}

NATIVEVIDEOPLAYER_API void CloseMedia(VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return;

    pInstance->bAudioThreadRunning = FALSE;
    if (pInstance->hAudioThread) {
        WaitForSingleObject(pInstance->hAudioThread, 5000);
        CloseHandle(pInstance->hAudioThread);
        pInstance->hAudioThread = nullptr;
    }
    if (pInstance->pLockedBuffer)
        UnlockVideoFrame(pInstance);
    if (pInstance->pAudioClient) {
        pInstance->pAudioClient->Stop();
        pInstance->pAudioClient->Release();
        pInstance->pAudioClient = nullptr;
    }
    if (pInstance->pRenderClient) {
        pInstance->pRenderClient->Release();
        pInstance->pRenderClient = nullptr;
    }
    if (pInstance->pDevice) {
        pInstance->pDevice->Release();
        pInstance->pDevice = nullptr;
    }
    if (pInstance->pSourceAudioFormat) {
        CoTaskMemFree(pInstance->pSourceAudioFormat);
        pInstance->pSourceAudioFormat = nullptr;
    }
    if (pInstance->pSourceReader) {
        pInstance->pSourceReader->Release();
        pInstance->pSourceReader = nullptr;
    }
    if (pInstance->pSourceReaderAudio) {
        pInstance->pSourceReaderAudio->Release();
        pInstance->pSourceReaderAudio = nullptr;
    }
    if (pInstance->hAudioSamplesReadyEvent) {
        CloseHandle(pInstance->hAudioSamplesReadyEvent);
        pInstance->hAudioSamplesReadyEvent = nullptr;
    }
    if (pInstance->pAudioEndpointVolume) {
        pInstance->pAudioEndpointVolume->Release();
        pInstance->pAudioEndpointVolume = nullptr;
    }
    pInstance->bEOF = FALSE;
    pInstance->videoWidth = pInstance->videoHeight = 0;
    pInstance->bHasAudio = FALSE;
    pInstance->bAudioInitialized = FALSE;
    pInstance->llPlaybackStartTime = pInstance->llTotalPauseTime = pInstance->llPauseStart = 0;
}

NATIVEVIDEOPLAYER_API HRESULT SetAudioVolume(VideoPlayerInstance* pInstance, float volume) {
    if (!pInstance || !pInstance->pAudioEndpointVolume)
        return OP_E_NOT_INITIALIZED;

    HRESULT hr = pInstance->pAudioEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
    return hr;
}

NATIVEVIDEOPLAYER_API HRESULT GetAudioVolume(VideoPlayerInstance* pInstance, float* volume) {
    if (!pInstance || !pInstance->pAudioEndpointVolume || !volume)
        return OP_E_INVALID_PARAMETER;

    HRESULT hr = pInstance->pAudioEndpointVolume->GetMasterVolumeLevelScalar(volume);
    return hr;
}

NATIVEVIDEOPLAYER_API HRESULT GetAudioLevels(VideoPlayerInstance* pInstance, float* pLeftLevel, float* pRightLevel) {
    if (!pInstance || !pLeftLevel || !pRightLevel)
        return OP_E_INVALID_PARAMETER;

    IAudioMeterInformation* pAudioMeterInfo = nullptr;
    HRESULT hr = pInstance->pDevice->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pAudioMeterInfo));
    if (FAILED(hr))
        return hr;

    float peaks[2] = { 0.0f, 0.0f };
    hr = pAudioMeterInfo->GetChannelsPeakValues(2, peaks);
    pAudioMeterInfo->Release();
    if (FAILED(hr))
        return hr;

    auto convertToPercentage = [](float level) -> float {
        if (level <= 0.f)
            return 0.f;
        float db = 20.f * log10(level);
        float normalized = (db + 60.f) / 60.f;
        if (normalized < 0.f)
            normalized = 0.f;
        if (normalized > 1.f)
            normalized = 1.f;
        return normalized * 100.f;
    };

    *pLeftLevel = convertToPercentage(peaks[0]);
    *pRightLevel = convertToPercentage(peaks[1]);
    return S_OK;
}