// NativeVideoPlayer.cpp
#include "NativeVideoPlayer.h"
#include "VideoPlayerInstance.h"
#include "Utils.h"
#include "MediaFoundationManager.h"
#include "AudioManager.h"
#include <algorithm>
#include <cstring>

using namespace VideoPlayerUtils;
using namespace MediaFoundation;
using namespace AudioManager;

// Error code definitions from header
#define OP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define OP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define OP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

// Debug print macro
#ifdef _DEBUG
#define PrintHR(msg, hr) fprintf(stderr, "%s (hr=0x%08x)\n", msg, static_cast<unsigned int>(hr))
#else
#define PrintHR(msg, hr) ((void)0)
#endif

// API Implementation
NATIVEVIDEOPLAYER_API HRESULT InitMediaFoundation() {
    return Initialize();
}

NATIVEVIDEOPLAYER_API HRESULT CreateVideoPlayerInstance(VideoPlayerInstance** ppInstance) {
    // Parameter validation
    if (!ppInstance)
        return E_INVALIDARG;

    // Ensure Media Foundation is initialized
    if (!IsInitialized()) {
        HRESULT hr = Initialize();
        if (FAILED(hr))
            return hr;
    }

    // Allocate and initialize a new instance
    auto* pInstance = new (std::nothrow) VideoPlayerInstance();
    if (!pInstance)
        return E_OUTOFMEMORY;

    // Initialize critical section for synchronization
    InitializeCriticalSection(&pInstance->csClockSync);

    // Create audio synchronization event
    pInstance->hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!pInstance->hAudioReadyEvent) {
        DeleteCriticalSection(&pInstance->csClockSync);
        delete pInstance;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Increment instance count and return the instance
    IncrementInstanceCount();
    *ppInstance = pInstance;
    return S_OK;
}

NATIVEVIDEOPLAYER_API void DestroyVideoPlayerInstance(VideoPlayerInstance* pInstance) {
    if (pInstance) {
        // Ensure all media resources are released
        CloseMedia(pInstance);

        // Delete critical section
        DeleteCriticalSection(&pInstance->csClockSync);

        // Delete instance and decrement counter
        delete pInstance;
        DecrementInstanceCount();
    }
}

NATIVEVIDEOPLAYER_API HRESULT OpenMedia(VideoPlayerInstance* pInstance, const wchar_t* url) {
    // Parameter validation
    if (!pInstance || !url)
        return OP_E_INVALID_PARAMETER;
    if (!IsInitialized())
        return OP_E_NOT_INITIALIZED;

    // Close previous media and reset state
    CloseMedia(pInstance);
    pInstance->bEOF = FALSE;
    pInstance->videoWidth = pInstance->videoHeight = 0;
    pInstance->bHasAudio = FALSE;

    HRESULT hr = S_OK;

    // Helper function to safely release COM objects
    auto safeRelease = [](IUnknown* obj) { if (obj) obj->Release(); };

    // 1. Configure and open video stream
    // ------------------------------------------
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 4);
    if (FAILED(hr))
        return hr;

    // Configure attributes for hardware acceleration
    pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    pAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, GetDXGIDeviceManager());
    pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    // Create source reader for video
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &pInstance->pSourceReader);
    safeRelease(pAttributes);
    if (FAILED(hr))
        return hr;

    // Select only video stream
    hr = pInstance->pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = pInstance->pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr))
        return hr;

    // Configure video format (RGB32)
    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (SUCCEEDED(hr)) {
        hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr))
            hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr))
            hr = pInstance->pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        safeRelease(pType);
    }
    if (FAILED(hr))
        return hr;

    // Get video dimensions
    IMFMediaType* pCurrent = nullptr;
    hr = pInstance->pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
    if (SUCCEEDED(hr)) {
        hr = MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &pInstance->videoWidth, &pInstance->videoHeight);
        safeRelease(pCurrent);
    }

    // 2. Configure and open audio stream (if available)
    // ----------------------------------------------------------
    hr = MFCreateSourceReaderFromURL(url, nullptr, &pInstance->pSourceReaderAudio);
    if (FAILED(hr)) {
        pInstance->pSourceReaderAudio = nullptr;
        return S_OK; // Continue without audio
    }

    // Select only audio stream
    hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) {
        safeRelease(pInstance->pSourceReaderAudio);
        pInstance->pSourceReaderAudio = nullptr;
        return S_OK; // Continue without audio
    }

    // Configure audio format (PCM 16-bit stereo 48kHz)
    IMFMediaType* pWantedType = nullptr;
    hr = MFCreateMediaType(&pWantedType);
    if (SUCCEEDED(hr)) {
        pWantedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pWantedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pWantedType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pWantedType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pWantedType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pWantedType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        pWantedType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = pInstance->pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedType);
        safeRelease(pWantedType);
    }
    if (FAILED(hr)) {
        safeRelease(pInstance->pSourceReaderAudio);
        pInstance->pSourceReaderAudio = nullptr;
        return S_OK; // Continue without audio
    }

    // Initialize WASAPI with current audio format
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
                safeRelease(pActualType);
                safeRelease(pInstance->pSourceReaderAudio);
                pInstance->pSourceReaderAudio = nullptr;
                return S_OK; // Continue without audio
            }
            if (pInstance->pSourceAudioFormat)
                CoTaskMemFree(pInstance->pSourceAudioFormat);
            pInstance->pSourceAudioFormat = pWfx;
        }
        safeRelease(pActualType);
    }

    // 3. Start audio thread if ready
    // -------------------------------------------
    pInstance->bHasAudio = TRUE;
    if (pInstance->bAudioInitialized) {
        hr = StartAudioThread(pInstance);
        if (FAILED(hr)) {
            PrintHR("StartAudioThread failed", hr);
        }
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

    if (!pSample) { 
        *pData = nullptr; 
        *pDataSize = 0; 
        return S_OK; 
    }

    LONGLONG masterClock = 0;
    EnterCriticalSection(&pInstance->csClockSync);
    masterClock = pInstance->llMasterClock;
    LeaveCriticalSection(&pInstance->csClockSync);

    UINT frameRateNum = 30, frameRateDenom = 1;
    GetVideoFrameRate(pInstance, &frameRateNum, &frameRateDenom);
    double frameTimeMs = 1000.0 * frameRateDenom / frameRateNum;
    auto skipThreshold = static_cast<LONGLONG>(-frameTimeMs * 3 * 10000);

    // Video synchronization
    if (pInstance->bHasAudio && masterClock > 0) {
        // Sync with audio clock
        auto adjustedMasterClock = static_cast<LONGLONG>(masterClock * pInstance->playbackSpeed);
        LONGLONG diff = llTimestamp - adjustedMasterClock;

        if (diff > 0) {
            // Video ahead: wait
            double maxWaitTime = frameTimeMs * 2 / pInstance->playbackSpeed;
            double waitTime = std::min<double>(diff / 10000.0, maxWaitTime);
            if (waitTime > 1.0)
                PreciseSleepHighRes(waitTime);
        } 
        else if (diff < skipThreshold) {
            // Video very late: skip frame
            pSample->Release();
            *pData = nullptr;
            *pDataSize = 0;
            return S_OK;
        }
        // If slightly late, play frame normally
    } 
    else {
        // No audio or no clock: sync with system time
        auto frameTimeAbs = static_cast<ULONGLONG>(llTimestamp / 10000);
        ULONGLONG currentTime = GetCurrentTimeMs();
        auto effectiveElapsed = static_cast<ULONGLONG>(
            (currentTime - pInstance->llPlaybackStartTime - pInstance->llTotalPauseTime) 
            * pInstance->playbackSpeed);

        if (frameTimeAbs > effectiveElapsed) {
            // Limit wait time
            double waitTime = std::min<double>(
                (frameTimeAbs - effectiveElapsed) / pInstance->playbackSpeed,
                frameTimeMs * 1.5 / pInstance->playbackSpeed);
            PreciseSleepHighRes(waitTime);
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

NATIVEVIDEOPLAYER_API BOOL IsEOF(const VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return FALSE;
    return pInstance->bEOF;
}

NATIVEVIDEOPLAYER_API void GetVideoSize(const VideoPlayerInstance* pInstance, UINT32* pWidth, UINT32* pHeight) {
    if (!pInstance)
        return;
    if (pWidth)  *pWidth = pInstance->videoWidth;
    if (pHeight) *pHeight = pInstance->videoHeight;
}

NATIVEVIDEOPLAYER_API HRESULT GetVideoFrameRate(const VideoPlayerInstance* pInstance, UINT* pNum, UINT* pDenom) {
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

NATIVEVIDEOPLAYER_API HRESULT GetMediaDuration(const VideoPlayerInstance* pInstance, LONGLONG* pDuration) {
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

NATIVEVIDEOPLAYER_API HRESULT GetMediaPosition(const VideoPlayerInstance* pInstance, LONGLONG* pPosition) {
    if (!pInstance || !pPosition)
        return OP_E_NOT_INITIALIZED;

    *pPosition = pInstance->llCurrentPosition;
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT SetPlaybackState(VideoPlayerInstance* pInstance, BOOL bPlaying, BOOL bStop) {
    if (!pInstance)
        return OP_E_NOT_INITIALIZED;

    if (bStop && !bPlaying) {
        if (pInstance->llPlaybackStartTime != 0) {
            pInstance->llTotalPauseTime = 0;
            pInstance->llPauseStart = 0;
            pInstance->llPlaybackStartTime = 0;

            // Reset master clock to avoid synchronization issues
            EnterCriticalSection(&pInstance->csClockSync);
            pInstance->llMasterClock = 0;
            LeaveCriticalSection(&pInstance->csClockSync);
        }
    } else if (bPlaying) {
        if (pInstance->llPlaybackStartTime == 0)
            pInstance->llPlaybackStartTime = GetCurrentTimeMs();
        else if (pInstance->llPauseStart != 0) {
            pInstance->llTotalPauseTime += (GetCurrentTimeMs() - pInstance->llPauseStart);
            pInstance->llPauseStart = 0;
        }
        if (pInstance->pAudioClient && pInstance->bAudioInitialized)
            pInstance->pAudioClient->Start();
    } else {
        if (pInstance->llPauseStart == 0)
            pInstance->llPauseStart = GetCurrentTimeMs();
        if (pInstance->pAudioClient && pInstance->bAudioInitialized)
            pInstance->pAudioClient->Stop();
    }
    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT ShutdownMediaFoundation() {
    return Shutdown();
}

NATIVEVIDEOPLAYER_API void CloseMedia(VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return;

    // Stop audio thread
    StopAudioThread(pInstance);

    // Release video buffer
    if (pInstance->pLockedBuffer) {
        UnlockVideoFrame(pInstance);
    }

    // Macro for safely releasing COM interfaces
    #define SAFE_RELEASE(obj) if (obj) { obj->Release(); obj = nullptr; }

    // Stop and release audio resources
    if (pInstance->pAudioClient) {
        pInstance->pAudioClient->Stop();
        SAFE_RELEASE(pInstance->pAudioClient);
    }

    SAFE_RELEASE(pInstance->pRenderClient);
    SAFE_RELEASE(pInstance->pDevice);
    SAFE_RELEASE(pInstance->pAudioEndpointVolume);
    SAFE_RELEASE(pInstance->pSourceReader);
    SAFE_RELEASE(pInstance->pSourceReaderAudio);

    // Release audio format
    if (pInstance->pSourceAudioFormat) {
        CoTaskMemFree(pInstance->pSourceAudioFormat);
        pInstance->pSourceAudioFormat = nullptr;
    }

    // Close event handles
    #define SAFE_CLOSE_HANDLE(handle) if (handle) { CloseHandle(handle); handle = nullptr; }

    SAFE_CLOSE_HANDLE(pInstance->hAudioSamplesReadyEvent);
    SAFE_CLOSE_HANDLE(pInstance->hAudioReadyEvent);

    // Reset state variables
    pInstance->bEOF = FALSE;
    pInstance->videoWidth = pInstance->videoHeight = 0;
    pInstance->bHasAudio = FALSE;
    pInstance->bAudioInitialized = FALSE;
    pInstance->llPlaybackStartTime = 0;
    pInstance->llTotalPauseTime = 0;
    pInstance->llPauseStart = 0;
    pInstance->llCurrentPosition = 0;
    pInstance->bSeekInProgress = FALSE;
    pInstance->playbackSpeed = 1.0f;

    #undef SAFE_RELEASE
    #undef SAFE_CLOSE_HANDLE
}

NATIVEVIDEOPLAYER_API HRESULT SetAudioVolume(VideoPlayerInstance* pInstance, float volume) {
    return SetVolume(pInstance, volume);
}

NATIVEVIDEOPLAYER_API HRESULT GetAudioVolume(const VideoPlayerInstance* pInstance, float* volume) {
    return GetVolume(pInstance, volume);
}

NATIVEVIDEOPLAYER_API HRESULT GetAudioLevels(const VideoPlayerInstance* pInstance, float* pLeftLevel, float* pRightLevel) {
    return AudioManager::GetAudioLevels(pInstance, pLeftLevel, pRightLevel);
}

NATIVEVIDEOPLAYER_API HRESULT SetPlaybackSpeed(VideoPlayerInstance* pInstance, float speed) {
    if (!pInstance)
        return OP_E_NOT_INITIALIZED;

    // Limit speed between 0.5 and 2.0
    speed = std::max(0.5f, std::min(speed, 2.0f));

    // Store speed in instance
    pInstance->playbackSpeed = speed;

    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT GetPlaybackSpeed(const VideoPlayerInstance* pInstance, float* pSpeed) {
    if (!pInstance || !pSpeed)
        return OP_E_INVALID_PARAMETER;

    // Return instance-specific playback speed
    *pSpeed = pInstance->playbackSpeed;

    return S_OK;
}