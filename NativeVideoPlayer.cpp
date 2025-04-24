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

    // 1. Configure and open media source with both audio and video streams
    // ------------------------------------------------------------------
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 5);
    if (FAILED(hr))
        return hr;

    // Configure attributes for hardware acceleration
    pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    pAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, GetDXGIDeviceManager());

    // Enable advanced video processing for better synchronization
    pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    // Create source reader for both audio and video
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &pInstance->pSourceReader);
    safeRelease(pAttributes);
    if (FAILED(hr))
        return hr;

    // 2. Configure video stream
    // ------------------------------------------
    // Enable video stream
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

    // 3. Configure audio stream (if available)
    // ------------------------------------------
    // Try to enable audio stream
    hr = pInstance->pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(hr)) {
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
            hr = pInstance->pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedType);
            safeRelease(pWantedType);
        }

        if (SUCCEEDED(hr)) {
            // Get the actual audio format for WASAPI
            IMFMediaType* pActualType = nullptr;
            hr = pInstance->pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
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
                    } else {
                        if (pInstance->pSourceAudioFormat)
                            CoTaskMemFree(pInstance->pSourceAudioFormat);
                        pInstance->pSourceAudioFormat = pWfx;
                        pInstance->bHasAudio = TRUE;
                    }
                }
                safeRelease(pActualType);
            }
        }

        // Create a separate audio source reader for the audio thread
        // This is needed even with automatic synchronization
        hr = MFCreateSourceReaderFromURL(url, nullptr, &pInstance->pSourceReaderAudio);
        if (SUCCEEDED(hr)) {
            // Select only audio stream
            hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            if (SUCCEEDED(hr))
                hr = pInstance->pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

            if (SUCCEEDED(hr)) {
                // Configure audio format (same as main reader)
                IMFMediaType* pWantedAudioType = nullptr;
                hr = MFCreateMediaType(&pWantedAudioType);
                if (SUCCEEDED(hr)) {
                    pWantedAudioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    pWantedAudioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    pWantedAudioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    pWantedAudioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                    pWantedAudioType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                    pWantedAudioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
                    pWantedAudioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    hr = pInstance->pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pWantedAudioType);
                    safeRelease(pWantedAudioType);
                }
            }

            if (FAILED(hr)) {
                PrintHR("Failed to configure audio source reader", hr);
                safeRelease(pInstance->pSourceReaderAudio);
                pInstance->pSourceReaderAudio = nullptr;
            }
        } else {
            PrintHR("Failed to create audio source reader", hr);
        }
    }

    // 4. Set up presentation clock for synchronization
    // ----------------------------------------------------------
    // Get the media source from the source reader
    hr = pInstance->pSourceReader->GetServiceForStream(
        MF_SOURCE_READER_MEDIASOURCE, 
        GUID_NULL, 
        IID_PPV_ARGS(&pInstance->pMediaSource));

    if (SUCCEEDED(hr)) {
        // Create the presentation clock
        hr = MFCreatePresentationClock(&pInstance->pPresentationClock);
        if (SUCCEEDED(hr)) {
            // Create a system time source
            IMFPresentationTimeSource* pTimeSource = nullptr;
            hr = MFCreateSystemTimeSource(&pTimeSource);
            if (SUCCEEDED(hr)) {
                // Set the time source on the presentation clock
                hr = pInstance->pPresentationClock->SetTimeSource(pTimeSource);
                if (SUCCEEDED(hr)) {
                    // Set the rate control on the presentation clock
                    IMFRateControl* pRateControl = nullptr;
                    hr = pInstance->pPresentationClock->QueryInterface(IID_PPV_ARGS(&pRateControl));
                    if (SUCCEEDED(hr)) {
                        // Explicitly set rate to 1.0 to ensure correct initial playback speed
                        hr = pRateControl->SetRate(FALSE, 1.0f);
                        if (FAILED(hr)) {
                            PrintHR("Failed to set initial presentation clock rate", hr);
                        }
                        pRateControl->Release();
                    }

                    // Get the media sink from the media source
                    IMFMediaSink* pMediaSink = nullptr;
                    hr = pInstance->pMediaSource->QueryInterface(IID_PPV_ARGS(&pMediaSink));
                    if (SUCCEEDED(hr)) {
                        // Set the presentation clock on the media sink
                        IMFClockStateSink* pClockStateSink = nullptr;
                        hr = pMediaSink->QueryInterface(IID_PPV_ARGS(&pClockStateSink));
                        if (SUCCEEDED(hr)) {
                            // Start the presentation clock
                            hr = pInstance->pPresentationClock->Start(0);
                            if (FAILED(hr)) {
                                PrintHR("Failed to start presentation clock", hr);
                            }
                            pClockStateSink->Release();
                        }
                        pMediaSink->Release();
                    } else {
                        PrintHR("Failed to get media sink from media source", hr);
                    }
                }
                safeRelease(pTimeSource);
            }
        }
    }

    // 5. Start audio thread for both manual and automatic synchronization
    // ----------------------------------------------------
    if (pInstance->bHasAudio && pInstance->bAudioInitialized && pInstance->pSourceReaderAudio) {
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

    // Store current position
    pInstance->llCurrentPosition = llTimestamp;

    // Automatic synchronization with presentation clock
    if (pInstance->pPresentationClock) {
        // With automatic synchronization, the presentation clock handles timing
        // We need to check if we should skip very late frames or wait for early frames

        // Get current presentation time
        MFTIME clockTime = 0;
        hr = pInstance->pPresentationClock->GetTime(&clockTime);

        if (SUCCEEDED(hr)) {
            // Calculate frame rate for skip threshold
            UINT frameRateNum = 30, frameRateDenom = 1;
            GetVideoFrameRate(pInstance, &frameRateNum, &frameRateDenom);
            double frameTimeMs = 1000.0 * frameRateDenom / frameRateNum;
            auto skipThreshold = static_cast<LONGLONG>(-frameTimeMs * 3 * 10000);

            // The presentation clock's rate already accounts for playback speed

            // Calculate difference between frame timestamp and clock
            LONGLONG diff = llTimestamp - clockTime;

            // If frame is very late, skip it
            if (diff < skipThreshold) {
                pSample->Release();
                *pData = nullptr;
                *pDataSize = 0;
                return S_OK;
            }
            // If frame is ahead of schedule, wait to maintain correct frame rate
            else if (diff > 0) {
                // Convert diff from 100ns units to milliseconds and apply playback speed
                double waitTime = diff / 10000.0;
                // Limit maximum wait time to avoid freezing if timestamps are far apart
                waitTime = std::min(waitTime, frameTimeMs * 2);
                if (waitTime > 1.0) {
                    PreciseSleepHighRes(waitTime);
                }
            }
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

    // Stop the presentation clock
    if (pInstance->pPresentationClock) {
        pInstance->pPresentationClock->Stop();
    }

    // Seek the main source reader
    HRESULT hr = pInstance->pSourceReader->SetCurrentPosition(GUID_NULL, var);
    if (FAILED(hr)) {
        EnterCriticalSection(&pInstance->csClockSync);
        pInstance->bSeekInProgress = FALSE;
        LeaveCriticalSection(&pInstance->csClockSync);
        PropVariantClear(&var);
        return hr;
    }

    // Also seek the audio source reader if available
    if (pInstance->pSourceReaderAudio) {
        PROPVARIANT varAudio;
        PropVariantInit(&varAudio);
        varAudio.vt = VT_I8;
        varAudio.hVal.QuadPart = llPositionIn100Ns;

        HRESULT hrAudio = pInstance->pSourceReaderAudio->SetCurrentPosition(GUID_NULL, varAudio);
        if (FAILED(hrAudio)) {
            PrintHR("Failed to seek audio source reader", hrAudio);
        }
        PropVariantClear(&varAudio);
    }


    // Reset audio client if needed
    if (pInstance->bHasAudio && pInstance->pRenderClient && pInstance->pAudioClient) {
        UINT32 bufferFrameCount = 0;
        if (SUCCEEDED(pInstance->pAudioClient->GetBufferSize(&bufferFrameCount))) {
            pInstance->pAudioClient->Reset();
        }
    }

    PropVariantClear(&var);

    // Update position and state
    EnterCriticalSection(&pInstance->csClockSync);
    pInstance->llCurrentPosition = llPositionIn100Ns;
    pInstance->bSeekInProgress = FALSE;
    LeaveCriticalSection(&pInstance->csClockSync);

    pInstance->bEOF = FALSE;

    // Restart the presentation clock at the new position
    if (pInstance->pPresentationClock) {
        hr = pInstance->pPresentationClock->Start(llPositionIn100Ns);
        if (FAILED(hr)) {
            PrintHR("Failed to restart presentation clock after seek", hr);
        }
    }

    // Restart audio if it was playing
    if (pInstance->bHasAudio && pInstance->pAudioClient && wasPlaying) {
        Sleep(5);
        pInstance->pAudioClient->Start();
    }

    // Signal audio thread to continue
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

    HRESULT hr = S_OK;

    if (bStop && !bPlaying) {
        // Stop playback completely
        if (pInstance->llPlaybackStartTime != 0) {
            pInstance->llTotalPauseTime = 0;
            pInstance->llPauseStart = 0;
            pInstance->llPlaybackStartTime = 0;

            // Stop presentation clock
            if (pInstance->pPresentationClock) {
                pInstance->pPresentationClock->Stop();
            }
        }
    } else if (bPlaying) {
        // Start or resume playback
        if (pInstance->llPlaybackStartTime == 0) {
            // First start
            pInstance->llPlaybackStartTime = GetCurrentTimeMs();
        } else if (pInstance->llPauseStart != 0) {
            // Resume from pause
            pInstance->llTotalPauseTime += (GetCurrentTimeMs() - pInstance->llPauseStart);
            pInstance->llPauseStart = 0;
        }

        // Start audio client if available
        if (pInstance->pAudioClient && pInstance->bAudioInitialized) {
            pInstance->pAudioClient->Start();
        }

        // Start or resume presentation clock
        if (pInstance->pPresentationClock) {
            MFTIME clockTime = 0;
            if (SUCCEEDED(pInstance->pPresentationClock->GetTime(&clockTime))) {
                hr = pInstance->pPresentationClock->Start(clockTime);
                if (FAILED(hr)) {
                    PrintHR("Failed to start presentation clock", hr);
                }
            }
        }
    } else {
        // Pause playback
        if (pInstance->llPauseStart == 0) {
            pInstance->llPauseStart = GetCurrentTimeMs();
        }

        // Pause audio client if available
        if (pInstance->pAudioClient && pInstance->bAudioInitialized) {
            pInstance->pAudioClient->Stop();
        }

        // Pause presentation clock
        if (pInstance->pPresentationClock) {
            hr = pInstance->pPresentationClock->Pause();
            if (FAILED(hr)) {
                PrintHR("Failed to pause presentation clock", hr);
            }
        }
    }
    return hr;
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

    // Stop and release presentation clock
    if (pInstance->pPresentationClock) {
        pInstance->pPresentationClock->Stop();
        SAFE_RELEASE(pInstance->pPresentationClock);
    }

    // Release media source
    SAFE_RELEASE(pInstance->pMediaSource);

    // Release other COM resources
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

    // Update the presentation clock rate
    if (pInstance->pPresentationClock) {
        // Get the rate control interface from the presentation clock
        IMFRateControl* pRateControl = nullptr;
        HRESULT hr = pInstance->pPresentationClock->QueryInterface(IID_PPV_ARGS(&pRateControl));
        if (SUCCEEDED(hr)) {
            // Set the playback rate
            hr = pRateControl->SetRate(FALSE, speed);
            if (FAILED(hr)) {
                PrintHR("Failed to set presentation clock rate", hr);
            }
            pRateControl->Release();
        }
    }

    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT GetPlaybackSpeed(const VideoPlayerInstance* pInstance, float* pSpeed) {
    if (!pInstance || !pSpeed)
        return OP_E_INVALID_PARAMETER;

    // Return instance-specific playback speed
    *pSpeed = pInstance->playbackSpeed;

    return S_OK;
}

NATIVEVIDEOPLAYER_API HRESULT GetVideoMetadata(const VideoPlayerInstance* pInstance, VideoMetadata* pMetadata) {
    if (!pInstance || !pMetadata)
        return OP_E_INVALID_PARAMETER;
    if (!pInstance->pSourceReader)
        return OP_E_NOT_INITIALIZED;

    // Initialize metadata structure with default values
    ZeroMemory(pMetadata, sizeof(VideoMetadata));

    HRESULT hr = S_OK;

    // Get media source for property access
    IMFMediaSource* pMediaSource = nullptr;
    IMFPresentationDescriptor* pPresentationDescriptor = nullptr;

    // Get media source from source reader
    hr = pInstance->pSourceReader->GetServiceForStream(
        MF_SOURCE_READER_MEDIASOURCE, 
        GUID_NULL, 
        IID_PPV_ARGS(&pMediaSource));

    if (SUCCEEDED(hr) && pMediaSource) {
        // Get presentation descriptor
        hr = pMediaSource->CreatePresentationDescriptor(&pPresentationDescriptor);

        if (SUCCEEDED(hr) && pPresentationDescriptor) {
            // Get duration
            UINT64 duration = 0;
            if (SUCCEEDED(pPresentationDescriptor->GetUINT64(MF_PD_DURATION, &duration))) {
                pMetadata->duration = static_cast<LONGLONG>(duration);
                pMetadata->hasDuration = TRUE;
            }

            // Get stream descriptors to access more metadata
            DWORD streamCount = 0;
            hr = pPresentationDescriptor->GetStreamDescriptorCount(&streamCount);

            if (SUCCEEDED(hr)) {
                // Try to get title and other metadata from attributes
                IMFAttributes* pAttributes = nullptr;
                if (SUCCEEDED(pPresentationDescriptor->QueryInterface(IID_PPV_ARGS(&pAttributes)))) {
                    // We can't directly access some metadata attributes due to missing definitions
                    // Set a default title based on the file path if available
                    if (pInstance->pSourceReader) {
                        // For now, we'll leave title empty as we can't reliably extract it
                        // without the proper attribute definitions
                        pMetadata->hasTitle = FALSE;
                    }

                    // Try to estimate bitrate from stream properties
                    UINT64 duration = 0;
                    if (SUCCEEDED(pPresentationDescriptor->GetUINT64(MF_PD_DURATION, &duration)) && duration > 0) {
                        // We'll try to estimate bitrate later from individual streams
                        pMetadata->hasBitrate = FALSE;
                    }

                    pAttributes->Release();
                }

                // Process each stream to get more metadata
                for (DWORD i = 0; i < streamCount; i++) {
                    BOOL selected = FALSE;
                    IMFStreamDescriptor* pStreamDescriptor = nullptr;

                    if (SUCCEEDED(pPresentationDescriptor->GetStreamDescriptorByIndex(i, &selected, &pStreamDescriptor))) {
                        // Get media type handler
                        IMFMediaTypeHandler* pHandler = nullptr;
                        if (SUCCEEDED(pStreamDescriptor->GetMediaTypeHandler(&pHandler))) {
                            // Get major type to determine if video or audio
                            GUID majorType;
                            if (SUCCEEDED(pHandler->GetMajorType(&majorType))) {
                                if (majorType == MFMediaType_Video) {
                                    // Get current media type
                                    IMFMediaType* pMediaType = nullptr;
                                    if (SUCCEEDED(pHandler->GetCurrentMediaType(&pMediaType))) {
                                        // Get video dimensions
                                        UINT32 width = 0, height = 0;
                                        if (SUCCEEDED(MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &width, &height))) {
                                            pMetadata->width = width;
                                            pMetadata->height = height;
                                            pMetadata->hasWidth = TRUE;
                                            pMetadata->hasHeight = TRUE;
                                        }

                                        // Get frame rate
                                        UINT32 numerator = 0, denominator = 1;
                                        if (SUCCEEDED(MFGetAttributeRatio(pMediaType, MF_MT_FRAME_RATE, &numerator, &denominator))) {
                                            if (denominator > 0) {
                                                pMetadata->frameRate = static_cast<float>(numerator) / static_cast<float>(denominator);
                                                pMetadata->hasFrameRate = TRUE;
                                            }
                                        }

                                        // Get subtype (format) for mime type
                                        GUID subtype;
                                        if (SUCCEEDED(pMediaType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                                            // Convert subtype to mime type string
                                            if (subtype == MFVideoFormat_H264) {
                                                wcscpy_s(pMetadata->mimeType, L"video/h264");
                                                pMetadata->hasMimeType = TRUE;
                                            }
                                            else if (subtype == MFVideoFormat_HEVC) {
                                                wcscpy_s(pMetadata->mimeType, L"video/hevc");
                                                pMetadata->hasMimeType = TRUE;
                                            }
                                            else if (subtype == MFVideoFormat_MPEG2) {
                                                wcscpy_s(pMetadata->mimeType, L"video/mpeg2");
                                                pMetadata->hasMimeType = TRUE;
                                            }
                                            else if (subtype == MFVideoFormat_WMV3) {
                                                wcscpy_s(pMetadata->mimeType, L"video/wmv");
                                                pMetadata->hasMimeType = TRUE;
                                            }
                                            else {
                                                wcscpy_s(pMetadata->mimeType, L"video/unknown");
                                                pMetadata->hasMimeType = TRUE;
                                            }
                                        }

                                        pMediaType->Release();
                                    }
                                }
                                else if (majorType == MFMediaType_Audio) {
                                    // Get current media type
                                    IMFMediaType* pMediaType = nullptr;
                                    if (SUCCEEDED(pHandler->GetCurrentMediaType(&pMediaType))) {
                                        // Get audio channels
                                        UINT32 channels = 0;
                                        if (SUCCEEDED(pMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))) {
                                            pMetadata->audioChannels = channels;
                                            pMetadata->hasAudioChannels = TRUE;
                                        }

                                        // Get audio sample rate
                                        UINT32 sampleRate = 0;
                                        if (SUCCEEDED(pMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))) {
                                            pMetadata->audioSampleRate = sampleRate;
                                            pMetadata->hasAudioSampleRate = TRUE;
                                        }

                                        pMediaType->Release();
                                    }
                                }
                            }
                            pHandler->Release();
                        }
                        pStreamDescriptor->Release();
                    }
                }
            }
            pPresentationDescriptor->Release();
        }
        pMediaSource->Release();
    }

    // If we couldn't get some metadata from the media source, try to get it from the instance
    if (!pMetadata->hasWidth || !pMetadata->hasHeight) {
        if (pInstance->videoWidth > 0 && pInstance->videoHeight > 0) {
            pMetadata->width = pInstance->videoWidth;
            pMetadata->height = pInstance->videoHeight;
            pMetadata->hasWidth = TRUE;
            pMetadata->hasHeight = TRUE;
        }
    }

    // If we couldn't get frame rate from media source, try to get it directly
    if (!pMetadata->hasFrameRate) {
        UINT numerator = 0, denominator = 1;
        if (SUCCEEDED(GetVideoFrameRate(pInstance, &numerator, &denominator)) && denominator > 0) {
            pMetadata->frameRate = static_cast<float>(numerator) / static_cast<float>(denominator);
            pMetadata->hasFrameRate = TRUE;
        }
    }

    // If we couldn't get duration from media source, try to get it directly
    if (!pMetadata->hasDuration) {
        LONGLONG duration = 0;
        if (SUCCEEDED(GetMediaDuration(pInstance, &duration))) {
            pMetadata->duration = duration;
            pMetadata->hasDuration = TRUE;
        }
    }

    // If we couldn't get audio channels, check if audio is available
    if (!pMetadata->hasAudioChannels && pInstance->bHasAudio) {
        if (pInstance->pSourceAudioFormat) {
            pMetadata->audioChannels = pInstance->pSourceAudioFormat->nChannels;
            pMetadata->hasAudioChannels = TRUE;

            pMetadata->audioSampleRate = pInstance->pSourceAudioFormat->nSamplesPerSec;
            pMetadata->hasAudioSampleRate = TRUE;
        }
    }

    return S_OK;
}
