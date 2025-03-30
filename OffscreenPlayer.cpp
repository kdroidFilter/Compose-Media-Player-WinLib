#define WIN32_LEAN_AND_MEAN
#include "OffscreenPlayer.h"
#include <cstdio>
#include <new>
#include <vector>
#include <cstring> // for memcpy

// Global variables for Media Foundation initialization
static bool g_bMFInitialized = false;

// Video-related globals
static IMFSourceReader* g_pSourceReader = nullptr;
static BOOL g_bEOF = FALSE;
static IMFMediaBuffer* g_pLockedBuffer = nullptr;
static BYTE* g_pLockedBytes = nullptr;
static DWORD g_lockedMaxSize = 0;
static DWORD g_lockedCurrSize = 0;

// Actual video resolution
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

// Audio thread globals
static HANDLE g_hAudioThread = nullptr;
static bool g_bAudioThreadRunning = false;
static HANDLE g_hAudioReadyEvent = nullptr;  // Synchronization event for audio thread

// Playback clock globals
static ULONGLONG g_llPlaybackStartTime = 0;
// New globals to handle pause duration
static ULONGLONG g_llTotalPauseTime = 0;
static ULONGLONG g_llPauseStart = 0;

// Helper function for debugging
static void PrintHR(const char* msg, HRESULT hr)
{
#ifdef _DEBUG
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
#else
    (void)msg; (void)hr;
#endif
}

// ----------------------------------------------------------------------
// Audio thread procedure: reads audio samples and sends them to WASAPI.
// ----------------------------------------------------------------------
static DWORD WINAPI AudioThreadProc(LPVOID /*lpParam*/)
{
    if (!g_pAudioClient || !g_pRenderClient || !g_pSourceReaderAudio) {
        PrintHR("AudioThreadProc: Missing AudioClient/RenderClient/SourceReaderAudio", E_FAIL);
        return 0;
    }

    // Wait for the "ready" signal
    if (g_hAudioReadyEvent) {
        WaitForSingleObject(g_hAudioReadyEvent, INFINITE);
    }

    // Start the audio client
    HRESULT hr = g_pAudioClient->Start();
    if (FAILED(hr)) {
        PrintHR("AudioClient->Start failed", hr);
        return 0;
    }

    // Audio is read from the first audio stream
    const DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;

    // Retrieve the size of the WASAPI buffer
    UINT32 bufferFrameCount = 0;
    hr = g_pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        PrintHR("GetBufferSize failed", hr);
        return 0;
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread started. Buffer frame count: %u\n", bufferFrameCount);
#endif

    // Main loop: read and process audio samples
    while (g_bAudioThreadRunning)
    {
        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;

        // Read one audio sample
        hr = g_pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr)) {
            PrintHR("ReadSample(audio) failed", hr);
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
#ifdef _DEBUG
            fprintf(stderr, "Audio stream ended\n");
#endif
            break;
        }

        if (!pSample) {
            // No new sample available, wait briefly
            Sleep(5);
            continue;
        }

        // Synchronize audio sample presentation based on adjusted elapsed time
        if (llTimeStamp > 0 && g_llPlaybackStartTime > 0) {
            ULONGLONG sampleTimeMs = (ULONGLONG)(llTimeStamp / 10000);
            ULONGLONG currentTime = GetTickCount64();
            // Calculate effective elapsed time excluding pause durations
            ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
            if (sampleTimeMs > effectiveElapsedTime) {
                Sleep((DWORD)(sampleTimeMs - effectiveElapsedTime));
            }
        }

        bool sampleProcessed = false;
        while (!sampleProcessed && g_bAudioThreadRunning) {
            IMFMediaBuffer* pBuf = nullptr;
            hr = pSample->ConvertToContiguousBuffer(&pBuf);
            if (SUCCEEDED(hr) && pBuf) {
                BYTE* pAudioData = nullptr;
                DWORD cbMaxLen = 0, cbCurrLen = 0;
                hr = pBuf->Lock(&pAudioData, &cbMaxLen, &cbCurrLen);
                if (SUCCEEDED(hr)) {
                    // Get current padding in WASAPI buffer
                    UINT32 numFramesPadding = 0;
                    hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
                    if (SUCCEEDED(hr)) {
                        // Calculate available frames in WASAPI buffer
                        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;

                        // Calculate number of frames in our sample (using blockAlign from the source format)
                        const UINT32 blockAlign = (g_pSourceAudioFormat) ? g_pSourceAudioFormat->nBlockAlign : 4;
                        UINT32 framesInBuffer = (blockAlign > 0) ? cbCurrLen / blockAlign : 0;

                        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
                            // Determine how many frames to write
                            UINT32 framesToWrite = (framesInBuffer > bufferFramesAvailable) ? bufferFramesAvailable : framesInBuffer;

                            // Get pointer to the render buffer from WASAPI
                            BYTE* pDataRender = nullptr;
                            hr = g_pRenderClient->GetBuffer(framesToWrite, &pDataRender);
                            if (SUCCEEDED(hr) && pDataRender) {
                                const DWORD bytesToCopy = framesToWrite * blockAlign;
#ifdef _DEBUG
                                fprintf(stderr, "Audio: Copying %u bytes (frames=%u)\n", bytesToCopy, framesToWrite);
#endif
                                memcpy(pDataRender, pAudioData, bytesToCopy);
                                g_pRenderClient->ReleaseBuffer(framesToWrite, 0);
                                sampleProcessed = true;
                            } else {
                                PrintHR("GetBuffer failed", hr);
                                sampleProcessed = true;
                            }
                        }
                        else if (framesInBuffer == 0) {
                            sampleProcessed = true;
                        }
                        else {
                            // Buffer full, wait briefly and retry
                            Sleep(10);
                        }
                    }
                    else {
                        PrintHR("GetCurrentPadding failed", hr);
                        sampleProcessed = true;
                    }
                    pBuf->Unlock();
                } else {
                    sampleProcessed = true;
                }
                pBuf->Release();
            }
            else {
                sampleProcessed = true;
            }
        }

        if (pSample) {
            pSample->Release();
        }
        // Small sleep to avoid a tight loop
        Sleep(1);
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread stopping\n");
#endif
    g_pAudioClient->Stop();
    return 0;
}

// ----------------------------------------------------------------------
// Initialize WASAPI with a given WAVEFORMATEX (if provided).
// Otherwise, the device mix format is used (forcing shared mode).
// ----------------------------------------------------------------------
static HRESULT InitWASAPI(WAVEFORMATEX* pSourceFormat = nullptr)
{
    g_bAudioInitialized = false;

    // If already initialized, skip re-initialization
    if (g_pAudioClient && g_pRenderClient) {
        g_bAudioInitialized = true;
        return S_OK;
    }

    // Create device enumerator
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_pEnumerator));
    if (FAILED(hr)) {
        PrintHR("CoCreateInstance(MMDeviceEnumerator) failed", hr);
        return hr;
    }

    // Get default output device
    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) {
        PrintHR("GetDefaultAudioEndpoint failed", hr);
        return hr;
    }

    // Activate IAudioClient
    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_pAudioClient));
    if (FAILED(hr)) {
        PrintHR("Activate(IAudioClient) failed", hr);
        return hr;
    }

    // If no source format provided, get the device's mix format
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

    // For shared mode, use a ~200ms buffer duration
    REFERENCE_TIME hnsBufferDuration = 2000000; // 200 ms
    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, pSourceFormat, nullptr);
    if (FAILED(hr)) {
        PrintHR("AudioClient->Initialize failed", hr);
        if (pwfxDevice) {
            CoTaskMemFree(pwfxDevice);
        }
        return hr;
    }

    // Retrieve IAudioRenderClient from IAudioClient
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
// Initialize Media Foundation (call once per process).
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
        // Create event for audio thread synchronization
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
// Open media (URL or file) and prepare video and audio decoding.
// ----------------------------------------------------------------------
HRESULT OpenMedia(const wchar_t* url)
{
    if (!g_bMFInitialized)
        return OP_E_NOT_INITIALIZED;
    if (!url)
        return OP_E_INVALID_PARAMETER;

    // Close any previous media
    CloseMedia();

    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;
    g_bHasAudio = false;

    HRESULT hr = S_OK;

    // Create attributes for video SourceReader
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) {
        PrintHR("MFCreateAttributes failed", hr);
        return hr;
    }

    // Enable advanced video processing
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(ENABLE_ADVANCED_VIDEO_PROCESSING) failed", hr);
        pAttributes->Release();
        return hr;
    }

    // Allow DXVA hardware acceleration
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(DISABLE_DXVA) failed", hr);
        pAttributes->Release();
        return hr;
    }

    // Create video SourceReader
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();

    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) failed", hr);
        return hr;
    }

    // Disable all streams and enable only the first video stream
    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Video StreamSelection failed", hr);
        return hr;
    }

    // Force output to RGB32 format for easier processing
    {
        IMFMediaType* pType = nullptr;
        hr = MFCreateMediaType(&pType);
        if (SUCCEEDED(hr)) {
            hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            // Optionally set nominal frame rate (e.g., 30 fps)
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, 30, 1);

            if (SUCCEEDED(hr)) {
                hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
            }
            pType->Release();
        }
        if (FAILED(hr)) {
            PrintHR("SetCurrentMediaType(RGB32) failed", hr);
            return hr;
        }

        // Retrieve actual video size
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

    // Create audio SourceReader separately
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) failed", hr);
        // No audio available; continue with video playback
        g_pSourceReaderAudio = nullptr;
        return S_OK;
    }

    // Disable all streams and enable only the first audio stream
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

    // Retrieve the chosen audio format and initialize WASAPI accordingly
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
// Read a video frame (RGB32). This function synchronizes video presentation
// with audio playback, taking into account the total pause duration.
// ----------------------------------------------------------------------
// ----------------------------------------------------------------------
// Read a video frame (RGB32). This function synchronizes video presentation
// with audio playback, taking into account the total pause duration.
// ----------------------------------------------------------------------
HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    // Unlock any previously locked buffer
    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    // Read one video sample
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

    // Update current playback position
    g_llCurrentPosition = llTimestamp;

    // Synchronize video frame presentation
    ULONGLONG frameTimeMs = (ULONGLONG)(llTimestamp / 10000); // Convert from 100-ns to ms
    ULONGLONG currentTime = GetTickCount64();
    ULONGLONG effectiveElapsedTime = currentTime - g_llPlaybackStartTime - g_llTotalPauseTime;
    if (frameTimeMs > effectiveElapsedTime) {
        Sleep((DWORD)(frameTimeMs - effectiveElapsedTime));
    }

    // Convert the sample to a contiguous buffer
    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) {
        PrintHR("ConvertToContiguousBuffer failed", hr);
        pSample->Release();
        return hr;
    }

    // Lock the buffer to access raw pixel data
    BYTE* pBytes = nullptr;
    DWORD cbMaxLen = 0;
    DWORD cbCurrLen = 0;
    hr = pBuffer->Lock(&pBytes, &cbMaxLen, &cbCurrLen);
    if (FAILED(hr)) {
        PrintHR("Buffer->Lock failed", hr);
        pBuffer->Release();
        pSample->Release();
        return hr;
    }

    // Store the buffer references for later unlocking
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
// Unlock the video frame buffer if locked.
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
// Check if end-of-file is reached.
// ----------------------------------------------------------------------
BOOL IsEOF()
{
    return g_bEOF;
}

// ----------------------------------------------------------------------
// Start audio playback (or resume from pause). This function updates the
// total pause time so that synchronization remains accurate.
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

    // Ensure WASAPI is initialized
    if (!g_bAudioInitialized) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot start audio - WASAPI not initialized\n");
#endif
        return E_FAIL;
    }

    // If playback was already started, then we are resuming.
    // Update the total pause time using the recorded pause start.
    if (g_llPlaybackStartTime == 0) {
         // First time play: initialize timing variables.
         g_llPlaybackStartTime = GetTickCount64();
         g_llTotalPauseTime = 0;
         g_llPauseStart = 0;
    } else if (g_llPauseStart != 0) {
         // Resuming: add the paused duration to the total pause time.
         g_llTotalPauseTime += (GetTickCount64() - g_llPauseStart);
         g_llPauseStart = 0;
    }

    // Create and start the audio thread
    g_bAudioThreadRunning = true;
    g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    if (!g_hAudioThread) {
        g_bAudioThreadRunning = false;
        PrintHR("CreateThread(audio) failed", HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Signal the audio thread if the event is valid
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
// Stop audio playback (pause) and record the pause start time.
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
    // Record the time when playback is paused
    if (g_llPauseStart == 0)
         g_llPauseStart = GetTickCount64();
    g_bAudioPlaying = false;
    return S_OK;
}

// ----------------------------------------------------------------------
// Close media and release all associated resources.
// ----------------------------------------------------------------------
void CloseMedia()
{
    StopAudioPlayback();

    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    // Release WASAPI resources
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

    // Release SourceReaders
    if (g_pSourceReader) {
        g_pSourceReader->Release();
        g_pSourceReader = nullptr;
    }
    if (g_pSourceReaderAudio) {
        g_pSourceReaderAudio->Release();
        g_pSourceReaderAudio = nullptr;
    }

    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;
    g_bHasAudio = false;
    g_bAudioInitialized = false;

    // Reset playback timing variables
    g_llPlaybackStartTime = 0;
    g_llTotalPauseTime = 0;
    g_llPauseStart = 0;
}

// ----------------------------------------------------------------------
// Retrieve the detected video dimensions.
// ----------------------------------------------------------------------
void GetVideoSize(UINT32* pWidth, UINT32* pHeight)
{
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

// ----------------------------------------------------------------------
// Retrieve the current video frame rate (numerator and denominator).
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
// Seek to a specific position (in 100-nanosecond units).
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
    g_bEOF = FALSE;  // Réinitialiser EOF après Seek
    return hr;
}

// ----------------------------------------------------------------------
// Get the total duration of the media (in 100-nanosecond units).
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
// Get the current playback position (in 100-nanosecond units).
// ----------------------------------------------------------------------

HRESULT GetMediaPosition(LONGLONG* pPosition)
{
    if (!g_pSourceReader || !pPosition)
        return OP_E_NOT_INITIALIZED;

    *pPosition = g_llCurrentPosition;
    return S_OK;
}


