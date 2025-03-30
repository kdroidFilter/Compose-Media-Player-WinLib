#include "OffscreenPlayer.h"
#include <cstdio>
#include <new>
#include <vector>
#include <cstring>      // for memcpy
#include <windows.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

// ----------------------------------------------------------------------
// High resolution timing helper
// ----------------------------------------------------------------------
static inline ULONGLONG GetCurrentTimeMs() {
    return GetTickCount64();
}

// ----------------------------------------------------------------------
// Global variables for Media Foundation and playback state
// ----------------------------------------------------------------------
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

// Audio-related globals
static IMFSourceReader* g_pSourceReaderAudio = nullptr;
static bool g_bAudioPlaying = false;
static bool g_bAudioInitialized = false;
static bool g_bHasAudio = false;

// WASAPI globals
static IAudioClient* g_pAudioClient = nullptr;
static IAudioRenderClient* g_pRenderClient = nullptr;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static IMMDevice* g_pDevice = nullptr;
static WAVEFORMATEX* g_pSourceAudioFormat = nullptr;
static HANDLE g_hAudioSamplesReadyEvent = nullptr;  // Event for WASAPI callback

// Audio thread globals
static HANDLE g_hAudioThread = nullptr;
static bool g_bAudioThreadRunning = false;
static HANDLE g_hAudioReadyEvent = nullptr;  // Synchronization event for audio thread

// Playback clock globals
static ULONGLONG g_llPlaybackStartTime = 0;
static ULONGLONG g_llTotalPauseTime = 0;
static ULONGLONG g_llPauseStart = 0;

// ----------------------------------------------------------------------
// Helper function for logging HRESULT values (debug mode)
// ----------------------------------------------------------------------
static void PrintHR(const char* msg, HRESULT hr)
{
#ifdef _DEBUG
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
#else
    (void)msg; (void)hr;
#endif
}

// ----------------------------------------------------------------------
// High precision sleep helper (simple wrapper around Sleep)
// ----------------------------------------------------------------------
static void PreciseSleep(DWORD milliseconds)
{
    if(milliseconds == 0) return;
    Sleep(milliseconds);
}

// ----------------------------------------------------------------------
// Audio thread procedure: optimized for reduced CPU usage
// ----------------------------------------------------------------------
static DWORD WINAPI AudioThreadProc(LPVOID /*lpParam*/)
{
    if (!g_pAudioClient || !g_pRenderClient || !g_pSourceReaderAudio) {
        PrintHR("AudioThreadProc: Missing AudioClient/RenderClient/SourceReaderAudio", E_FAIL);
        return 0;
    }

    // Wait for the audio ready event before processing
    if (g_hAudioReadyEvent) {
        WaitForSingleObject(g_hAudioReadyEvent, INFINITE);
    }

    HRESULT hr = g_pAudioClient->Start();
    if (FAILED(hr)) {
        PrintHR("AudioClient->Start failed", hr);
        return 0;
    }

    const DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    UINT32 bufferFrameCount = 0;
    hr = g_pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        PrintHR("GetBufferSize failed", hr);
        return 0;
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread started. Buffer frame count: %u\n", bufferFrameCount);
#endif

    while (g_bAudioThreadRunning)
    {
        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;

        hr = g_pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr)) {
            PrintHR("ReadSample(audio) failed", hr);
            break;
        }
        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
#ifdef _DEBUG
            fprintf(stderr, "Audio stream ended\n");
#endif
            if (pSample) pSample->Release();
            break;
        }
        if (!pSample) {
            PreciseSleep(1);
            continue;
        }

        // Synchronize audio sample presentation using high-resolution time
        if (llTimeStamp > 0 && g_llPlaybackStartTime > 0) {
            ULONGLONG sampleTimeMs = (ULONGLONG)(llTimeStamp / 10000);
            ULONGLONG currentTime = GetCurrentTimeMs();
            ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
            if (sampleTimeMs > effectiveElapsedTime) {
                PreciseSleep((DWORD)(sampleTimeMs - effectiveElapsedTime));
            }
        }

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

        UINT32 numFramesPadding = 0;
        hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
        if (FAILED(hr)) {
            pBuf->Unlock();
            pBuf->Release();
            pSample->Release();
            continue;
        }
        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;
        const UINT32 blockAlign = (g_pSourceAudioFormat) ? g_pSourceAudioFormat->nBlockAlign : 4;
        UINT32 framesInBuffer = (blockAlign > 0) ? cbCurrLen / blockAlign : 0;
        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
            UINT32 framesToWrite = (framesInBuffer > bufferFramesAvailable) ? bufferFramesAvailable : framesInBuffer;
            BYTE* pDataRender = nullptr;
            hr = g_pRenderClient->GetBuffer(framesToWrite, &pDataRender);
            if (SUCCEEDED(hr) && pDataRender) {
                DWORD bytesToCopy = framesToWrite * blockAlign;
#ifdef _DEBUG
                fprintf(stderr, "Audio: Copying %u bytes (frames=%u)\n", bytesToCopy, framesToWrite);
#endif
                memcpy(pDataRender, pAudioData, bytesToCopy);
                g_pRenderClient->ReleaseBuffer(framesToWrite, 0);
            } else {
                PrintHR("GetBuffer failed", hr);
            }
        }
        pBuf->Unlock();
        pBuf->Release();
        pSample->Release();

        // Use event-driven waiting for the next sample with minimal delay
        WaitForSingleObject(g_hAudioSamplesReadyEvent, 5);
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread stopping\n");
#endif
    g_pAudioClient->Stop();
    return 0;
}

// ----------------------------------------------------------------------
// Initialize WASAPI in event-driven mode with optimized settings
// ----------------------------------------------------------------------
static HRESULT InitWASAPI(WAVEFORMATEX* pSourceFormat = nullptr)
{
    g_bAudioInitialized = false;
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

#ifdef _DEBUG
    if (pSourceFormat) {
        fprintf(stderr, "InitWASAPI: channels=%u, samples/sec=%u, bits=%u, blockAlign=%u, avgBytes/sec=%u\n",
                pSourceFormat->nChannels, pSourceFormat->nSamplesPerSec,
                pSourceFormat->wBitsPerSample, pSourceFormat->nBlockAlign,
                pSourceFormat->nAvgBytesPerSec);
    }
#endif

    if (!g_hAudioSamplesReadyEvent) {
        g_hAudioSamplesReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioSamplesReadyEvent) {
            PrintHR("CreateEvent for audio samples failed", HRESULT_FROM_WIN32(GetLastError()));
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    // Initialize audio client with a 200ms buffer duration in shared mode with event callbacks
    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBufferDuration, 0, pSourceFormat, nullptr);
    if (FAILED(hr)) {
        PrintHR("AudioClient->Initialize failed", hr);
        if (pwfxDevice) {
            CoTaskMemFree(pwfxDevice);
        }
        return hr;
    }

    hr = g_pAudioClient->SetEventHandle(g_hAudioSamplesReadyEvent);
    if (FAILED(hr)) {
        PrintHR("SetEventHandle failed", hr);
        if (pwfxDevice) {
            CoTaskMemFree(pwfxDevice);
        }
        return hr;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_pRenderClient));
    if (FAILED(hr)) {
        PrintHR("GetService(IAudioRenderClient) failed", hr);
    } else {
        g_bAudioInitialized = true;
    }

    if (pwfxDevice) {
        CoTaskMemFree(pwfxDevice);
    }
    return hr;
}

// ----------------------------------------------------------------------
// Initialize Media Foundation and create synchronization events
// ----------------------------------------------------------------------
HRESULT InitMediaFoundation()
{
    if (g_bMFInitialized)
        return OP_E_ALREADY_INITIALIZED;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        hr = MFStartup(MF_VERSION);
    }
    if (SUCCEEDED(hr)) {
        g_bMFInitialized = true;
        g_hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioReadyEvent) {
            PrintHR("CreateEvent failed", HRESULT_FROM_WIN32(GetLastError()));
        }
    } else {
        PrintHR("InitMediaFoundation failed", hr);
    }
    return hr;
}

// ----------------------------------------------------------------------
// Open media and set up video/audio decoding with optimizations
// ----------------------------------------------------------------------
HRESULT OpenMedia(const wchar_t* url)
{
    if (!g_bMFInitialized)
        return OP_E_NOT_INITIALIZED;
    if (!url)
        return OP_E_INVALID_PARAMETER;

    // Close any previous media and reset state
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

    hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(ENABLE_ADVANCED_VIDEO_PROCESSING) failed", hr);
        pAttributes->Release();
        return hr;
    }
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(DISABLE_DXVA) failed", hr);
        pAttributes->Release();
        return hr;
    }

    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) failed", hr);
        return hr;
    }

    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Video StreamSelection failed", hr);
        return hr;
    }

    // Set video output to RGB32 without forcing frame rate changes
    {
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
        IMFMediaType* pCurrent = nullptr;
        hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
        if (SUCCEEDED(hr) && pCurrent) {
            MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &g_videoWidth, &g_videoHeight);
#ifdef _DEBUG
            fprintf(stderr, "Video size: %u x %u\n", g_videoWidth, g_videoHeight);
#endif
            pCurrent->Release();
        }
    }

    // Create audio source reader
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) failed", hr);
        g_pSourceReaderAudio = nullptr;
        return S_OK; // Continue with video only
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

    // Force audio to standard PCM format (48kHz, 16-bit, stereo)
    {
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
            PrintHR("SetCurrentMediaType(audio forced PCM) failed", hr);
            g_pSourceReaderAudio->Release();
            g_pSourceReaderAudio = nullptr;
            return S_OK;
        }
    }

    // Retrieve actual audio format and initialize WASAPI accordingly
    {
        IMFMediaType* pActualType = nullptr;
        hr = g_pSourceReaderAudio->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
        if (SUCCEEDED(hr) && pActualType) {
            WAVEFORMATEX* pWfx = nullptr;
            UINT32 size = 0;
            HRESULT hr2 = MFCreateWaveFormatExFromMFMediaType(pActualType, &pWfx, &size);
            if (SUCCEEDED(hr2) && pWfx) {
                hr2 = InitWASAPI(pWfx);
                if (FAILED(hr2)) {
                    PrintHR("InitWASAPI with forced PCM failed", hr2);
                    g_pSourceReaderAudio->Release();
                    g_pSourceReaderAudio = nullptr;
                    CoTaskMemFree(pWfx);
                    pActualType->Release();
                    return S_OK;
                }
                if (g_pSourceAudioFormat) {
                    CoTaskMemFree(g_pSourceAudioFormat);
                }
                g_pSourceAudioFormat = pWfx;
            }
            pActualType->Release();
        }
    }

    g_bHasAudio = true;
    return S_OK;
}

// ----------------------------------------------------------------------
// Read a video frame (RGB32) with improved synchronization for 4K
// ----------------------------------------------------------------------
HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    DWORD streamIndex = 0;
    DWORD dwFlags = 0;
    LONGLONG llTimestamp = 0;
    IMFSample* pSample = nullptr;

    HRESULT hr = g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &dwFlags, &llTimestamp, &pSample);
    if (FAILED(hr)) {
        PrintHR("ReadSample(video) failed", hr);
        return hr;
    }
    if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        g_bEOF = TRUE;
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

    g_llCurrentPosition = llTimestamp;
    ULONGLONG frameTimeMs = (ULONGLONG)(llTimestamp / 10000);
    ULONGLONG currentTime = GetCurrentTimeMs();
    ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
    if (frameTimeMs > effectiveElapsedTime) {
        PreciseSleep((DWORD)(frameTimeMs - effectiveElapsedTime));
    }

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

    g_pLockedBuffer = pBuffer;
    g_pLockedBytes = pBytes;
    g_lockedMaxSize = cbMaxLen;
    g_lockedCurrSize = cbCurrLen;

    *pData = pBytes;
    *pDataSize = cbCurrLen;

    pSample->Release();
    return S_OK;
}

// ----------------------------------------------------------------------
// Unlock the video frame buffer if locked
// ----------------------------------------------------------------------
HRESULT UnlockVideoFrame()
{
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

// ----------------------------------------------------------------------
// Check if end-of-stream has been reached
// ----------------------------------------------------------------------
BOOL IsEOF()
{
    return g_bEOF;
}

// ----------------------------------------------------------------------
// Start or resume audio playback with updated timing management
// ----------------------------------------------------------------------
HRESULT StartAudioPlayback()
{
    if (!g_pSourceReaderAudio) {
#ifdef _DEBUG
        fprintf(stderr, "No audio source to start\n");
#endif
        return E_FAIL;
    }
    if (g_bAudioPlaying) {
        return S_OK;
    }
    if (!g_bAudioInitialized) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot start audio - WASAPI not initialized\n");
#endif
        return E_FAIL;
    }

    if (g_llPlaybackStartTime == 0) {
         g_llPlaybackStartTime = GetCurrentTimeMs();
         g_llTotalPauseTime = 0;
         g_llPauseStart = 0;
    } else if (g_llPauseStart != 0) {
         g_llTotalPauseTime += (GetCurrentTimeMs() - g_llPauseStart);
         g_llPauseStart = 0;
    }

    g_bAudioThreadRunning = true;
    g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    if (!g_hAudioThread) {
        g_bAudioThreadRunning = false;
        PrintHR("CreateThread(audio) failed", HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (g_hAudioReadyEvent) {
        SetEvent(g_hAudioReadyEvent);
    }
#ifdef _DEBUG
    fprintf(stderr, "Audio playback started\n");
#endif
    g_bAudioPlaying = true;
    return S_OK;
}

// ----------------------------------------------------------------------
// Stop (pause) audio playback and record pause start time
// ----------------------------------------------------------------------
HRESULT StopAudioPlayback()
{
    if (!g_bAudioPlaying) {
        return S_OK;
    }
#ifdef _DEBUG
    fprintf(stderr, "Stopping audio playback\n");
#endif
    g_bAudioThreadRunning = false;
    if (g_hAudioThread) {
        WaitForSingleObject(g_hAudioThread, 5000);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = nullptr;
    }
    if (g_llPauseStart == 0)
         g_llPauseStart = GetCurrentTimeMs();
    g_bAudioPlaying = false;
    return S_OK;
}

// ----------------------------------------------------------------------
// Close the media and release all allocated resources
// ----------------------------------------------------------------------
void CloseMedia()
{
    StopAudioPlayback();
    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }
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

// ----------------------------------------------------------------------
// Retrieve the video dimensions (width and height)
// ----------------------------------------------------------------------
void GetVideoSize(UINT32* pWidth, UINT32* pHeight)
{
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

// ----------------------------------------------------------------------
// Retrieve the video frame rate (numerator and denominator)
// ----------------------------------------------------------------------
HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom)
{
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

// ----------------------------------------------------------------------
// Seek to a specified position (in 100-nanosecond units)
// ----------------------------------------------------------------------
HRESULT SeekMedia(LONGLONG llPosition)
{
    if (!g_pSourceReader)
        return OP_E_NOT_INITIALIZED;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = llPosition;

    HRESULT hr = g_pSourceReader->SetCurrentPosition(GUID_NULL, var);
    if (FAILED(hr)) {
        PrintHR("SetCurrentPosition failed", hr);
    }
    PropVariantClear(&var);
    g_bEOF = FALSE;  // Reset EOF flag after seeking
    return hr;
}

// ----------------------------------------------------------------------
// Get the total media duration (in 100-nanosecond units)
// ----------------------------------------------------------------------
HRESULT GetMediaDuration(LONGLONG* pDuration)
{
    if (!g_pSourceReader || !pDuration)
        return OP_E_NOT_INITIALIZED;

    IMFMediaSource* pMediaSource = nullptr;
    IMFPresentationDescriptor* pPresentationDescriptor = nullptr;
    HRESULT hr = g_pSourceReader->GetServiceForStream(
        MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&pMediaSource));
    if (SUCCEEDED(hr))
    {
        hr = pMediaSource->CreatePresentationDescriptor(&pPresentationDescriptor);
        if (SUCCEEDED(hr))
        {
            hr = pPresentationDescriptor->GetUINT64(MF_PD_DURATION, reinterpret_cast<UINT64*>(pDuration));
            pPresentationDescriptor->Release();
        }
        pMediaSource->Release();
    }
    if (FAILED(hr))
    {
        PrintHR("GetMediaDuration failed", hr);
    }
    return hr;
}

// ----------------------------------------------------------------------
// Get the current playback position (in 100-nanosecond units)
// ----------------------------------------------------------------------
HRESULT GetMediaPosition(LONGLONG* pPosition)
{
    if (!g_pSourceReader || !pPosition)
        return OP_E_NOT_INITIALIZED;
    *pPosition = g_llCurrentPosition;
    return S_OK;
}
