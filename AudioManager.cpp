#include "AudioManager.h"
#include "VideoPlayerInstance.h"
#include "Utils.h"
#include "MediaFoundationManager.h"
#include <algorithm>
#include <cmath>

using namespace VideoPlayerUtils;

namespace AudioManager {

HRESULT InitWASAPI(VideoPlayerInstance* pInstance, const WAVEFORMATEX* pSourceFormat) {
    // Validation and check if already initialized
    if (!pInstance)
        return E_INVALIDARG;
    if (pInstance->pAudioClient && pInstance->pRenderClient) {
        pInstance->bAudioInitialized = TRUE;
        return S_OK;
    }

    HRESULT hr = S_OK;
    WAVEFORMATEX* pwfxDevice = nullptr;

    // Get device enumerator if needed
    IMMDeviceEnumerator* pEnumerator = MediaFoundation::GetDeviceEnumerator();
    if (!pEnumerator)
        return E_FAIL;

    // Get default audio endpoint
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pInstance->pDevice);
    if (FAILED(hr))
        return hr;

    // Activate audio interfaces
    hr = pInstance->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&pInstance->pAudioClient));
    if (FAILED(hr))
        return hr;

    hr = pInstance->pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&pInstance->pAudioEndpointVolume));
    if (FAILED(hr))
        return hr;

    // Get audio format if not specified
    if (!pSourceFormat) {
        hr = pInstance->pAudioClient->GetMixFormat(&pwfxDevice);
        if (FAILED(hr))
            return hr;
        pSourceFormat = pwfxDevice;
    }

    // Create audio notification event
    if (!pInstance->hAudioSamplesReadyEvent) {
        pInstance->hAudioSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!pInstance->hAudioSamplesReadyEvent) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        }
    }

    // Initialize audio client
    REFERENCE_TIME hnsBufferDuration = 2000000; // 200ms
    hr = pInstance->pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration,
        0,
        pSourceFormat,
        nullptr);
    if (FAILED(hr))
        goto cleanup;

    // Set notification event
    hr = pInstance->pAudioClient->SetEventHandle(pInstance->hAudioSamplesReadyEvent);
    if (FAILED(hr))
        goto cleanup;

    // Get render client service
    hr = pInstance->pAudioClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&pInstance->pRenderClient));
    if (FAILED(hr))
        goto cleanup;

    // Initialization successful
    pInstance->bAudioInitialized = TRUE;

cleanup:
    if (pwfxDevice)
        CoTaskMemFree(pwfxDevice);

    return hr;
}

DWORD WINAPI AudioThreadProc(LPVOID lpParam) {
    auto* pInstance = static_cast<VideoPlayerInstance*>(lpParam);
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

        // Audio/video synchronization
        if (llTimeStamp > 0 && pInstance->llPlaybackStartTime > 0) {
            auto sampleTimeMs = static_cast<ULONGLONG>(llTimeStamp / 10000);
            ULONGLONG currentTime = GetCurrentTimeMs();

            // Calculate elapsed time adjusted by playback speed
            auto effectiveElapsed = static_cast<ULONGLONG>(
                (currentTime - pInstance->llPlaybackStartTime - pInstance->llTotalPauseTime)
                * pInstance->playbackSpeed);

            // Calculate difference between sample time and elapsed time
            auto diff = static_cast<int64_t>(sampleTimeMs - effectiveElapsed);

            // Handle different synchronization cases
            if (diff > 15) {
                // Audio ahead: wait
                double waitTime = std::min<int64_t>(diff, 100) / pInstance->playbackSpeed;
                PreciseSleepHighRes(waitTime);
            }
            else if (diff < -50) {
                // Audio very late: skip sample
                pSample->Release();
                continue;
            }
            else if (diff < -15) {
                // Audio slightly late: adjust clock
                EnterCriticalSection(&pInstance->csClockSync);
                pInstance->llMasterClock += static_cast<LONGLONG>((diff * 5000) / pInstance->playbackSpeed);
                LeaveCriticalSection(&pInstance->csClockSync);
            }
            else if (diff > 0) {
                // Small adjustment for minimal differences
                PreciseSleepHighRes(diff / pInstance->playbackSpeed);
            }
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
            UINT32 framesToWrite = std::min(framesInBuffer, bufferFramesAvailable);
            BYTE* pDataRender = nullptr;
            hr = pInstance->pRenderClient->GetBuffer(framesToWrite, &pDataRender);
            if (SUCCEEDED(hr) && pDataRender) {
                DWORD bytesToCopy = framesToWrite * blockAlign;

                // Copy audio data
                memcpy(pDataRender, pAudioData, bytesToCopy);

                // Apply volume if needed (only for 16-bit PCM formats)
                if (pInstance->instanceVolume < 0.999f &&
                    pInstance->pSourceAudioFormat &&
                    pInstance->pSourceAudioFormat->wBitsPerSample == 16) {

                    auto* pSamples = reinterpret_cast<int16_t*>(pDataRender);
                    for (UINT32 i = 0; i < bytesToCopy / sizeof(int16_t); i++) {
                        pSamples[i] = static_cast<int16_t>(pSamples[i] * pInstance->instanceVolume);
                    }
                }

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

HRESULT StartAudioThread(VideoPlayerInstance* pInstance) {
    if (!pInstance || !pInstance->bHasAudio || !pInstance->bAudioInitialized)
        return E_INVALIDARG;

    // Ensure old thread is terminated
    if (pInstance->hAudioThread) {
        WaitForSingleObject(pInstance->hAudioThread, 5000);
        CloseHandle(pInstance->hAudioThread);
        pInstance->hAudioThread = nullptr;
    }

    // Start new audio thread
    pInstance->bAudioThreadRunning = TRUE;
    pInstance->hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, pInstance, 0, nullptr);
    if (!pInstance->hAudioThread) {
        pInstance->bAudioThreadRunning = FALSE;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (pInstance->hAudioReadyEvent) {
        SetEvent(pInstance->hAudioReadyEvent);
    }

    return S_OK;
}

void StopAudioThread(VideoPlayerInstance* pInstance) {
    if (!pInstance)
        return;

    pInstance->bAudioThreadRunning = FALSE;
    if (pInstance->hAudioThread) {
        // Wait for thread to terminate with short timeout
        if (WaitForSingleObject(pInstance->hAudioThread, 1000) == WAIT_TIMEOUT) {
            TerminateThread(pInstance->hAudioThread, 0);
        }
        CloseHandle(pInstance->hAudioThread);
        pInstance->hAudioThread = nullptr;
    }

    if (pInstance->pAudioClient) {
        pInstance->pAudioClient->Stop();
    }
}

HRESULT SetVolume(VideoPlayerInstance* pInstance, float volume) {
    if (!pInstance)
        return E_INVALIDARG;

    // Limit volume between 0.0 and 1.0
    volume = std::max(0.0f, std::min(volume, 1.0f));

    // Store volume in instance
    pInstance->instanceVolume = volume;

    return S_OK;
}

HRESULT GetVolume(const VideoPlayerInstance* pInstance, float* volume) {
    if (!pInstance || !volume)
        return E_INVALIDARG;

    // Return instance-specific volume
    *volume = pInstance->instanceVolume;

    return S_OK;
}

HRESULT GetAudioLevels(const VideoPlayerInstance* pInstance, float* pLeftLevel, float* pRightLevel) {
    if (!pInstance || !pLeftLevel || !pRightLevel)
        return E_INVALIDARG;

    // Check if audio device is available
    if (!pInstance->pDevice)
        return E_FAIL;

    IAudioMeterInformation* pAudioMeterInfo = nullptr;
    HRESULT hr = pInstance->pDevice->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, 
                                             reinterpret_cast<void**>(&pAudioMeterInfo));
    if (FAILED(hr))
        return hr;

    float peaks[2] = { 0.0f, 0.0f };
    hr = pAudioMeterInfo->GetChannelsPeakValues(2, peaks);

    // Always release resource, even on error
    if (pAudioMeterInfo) {
        pAudioMeterInfo->Release();
    }

    if (FAILED(hr))
        return hr;

    auto convertToPercentage = [](float level) -> float {
        if (level <= 0.f)
            return 0.f;
        float db = 20.f * log10(level);
        float normalized = (db + 60.f) / 60.f;
        normalized = std::max(0.f, std::min(normalized, 1.f));
        return normalized * 100.f;
    };

    *pLeftLevel = convertToPercentage(peaks[0]);
    *pRightLevel = convertToPercentage(peaks[1]);
    return S_OK;
}

} // namespace AudioManager