#define WIN32_LEAN_AND_MEAN
#include "OffscreenPlayer.h"

#include <cstdio>
#include <new>
#include <vector>

// ------------------------------------------------------
// Global variables (for demonstration; not "clean" design)
// ------------------------------------------------------
static bool g_bMFInitialized = false;

// Video-related
static IMFSourceReader* g_pSourceReader = nullptr;
static BOOL g_bEOF = FALSE;
static IMFMediaBuffer* g_pLockedBuffer = nullptr;
static BYTE* g_pLockedBytes = nullptr;
static DWORD g_lockedMaxSize = 0;
static DWORD g_lockedCurrSize = 0;

// Actual video resolution
static UINT32 g_videoWidth = 0;
static UINT32 g_videoHeight = 0;

// Audio-related
static IMFSourceReader* g_pSourceReaderAudio = nullptr;
static bool g_bAudioPlaying = false;
static bool g_bAudioInitialized = false;
static bool g_bHasAudio = false;

// WASAPI
static IAudioClient* g_pAudioClient = nullptr;
static IAudioRenderClient* g_pRenderClient = nullptr;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static IMMDevice* g_pDevice = nullptr;
static WAVEFORMATEX* g_pSourceAudioFormat = nullptr;

// Audio thread
static HANDLE g_hAudioThread = nullptr;
static bool g_bAudioThreadRunning = false;
static HANDLE g_hAudioReadyEvent = nullptr;  // Synchronization event for audio thread

// ------------------------------------------------------
// Helper function for debug
// ------------------------------------------------------
static void PrintHR(const char* msg, HRESULT hr)
{
#ifdef _DEBUG
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
#else
    (void)msg; (void)hr;
#endif
}

// ------------------------------------------------------
// Thread procedure to process audio samples and feed WASAPI
// ------------------------------------------------------
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

    // Audio is the "first audio stream" in this simple approach
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

    // Main loop to read audio samples and deliver them to WASAPI
    while (g_bAudioThreadRunning)
    {
        IMFSample* pSample = nullptr;
        DWORD dwFlags = 0;
        LONGLONG llTimeStamp = 0;

        // Read one audio sample
        hr = g_pSourceReaderAudio->ReadSample(audioStreamIndex, 0, nullptr, &dwFlags, &llTimeStamp, &pSample);
        if (FAILED(hr)) {
            PrintHR("ReadSample(audio) fail", hr);
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
#ifdef _DEBUG
            fprintf(stderr, "Audio stream ended\n");
#endif
            break; // end of audio
        }

        if (!pSample) {
            // No new sample yet, wait briefly
            Sleep(5);
            continue;
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
                    // Get the current padding in WASAPI buffer
                    UINT32 numFramesPadding = 0;
                    hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
                    if (SUCCEEDED(hr)) {
                        // Number of frames available in WASAPI buffer
                        UINT32 bufferFramesAvailable = bufferFrameCount - numFramesPadding;

                        // Calculate how many frames are in our sample
                        // Using blockAlign from the source wave format
                        const UINT32 blockAlign = (g_pSourceAudioFormat) ? g_pSourceAudioFormat->nBlockAlign : 4;
                        UINT32 framesInBuffer = 0;
                        if (blockAlign > 0) {
                            framesInBuffer = cbCurrLen / blockAlign;
                        }

                        if (framesInBuffer > 0 && bufferFramesAvailable > 0) {
                            // Decide how many frames we can write now
                            UINT32 framesToWrite = (framesInBuffer > bufferFramesAvailable) ?
                                                  bufferFramesAvailable : framesInBuffer;

                            // Ask WASAPI for a pointer to the render buffer
                            BYTE* pDataRender = nullptr;
                            hr = g_pRenderClient->GetBuffer(framesToWrite, &pDataRender);
                            if (SUCCEEDED(hr) && pDataRender) {
                                const DWORD bytesToCopy = framesToWrite * blockAlign;
#ifdef _DEBUG
                                fprintf(stderr, "Audio: Copying %u bytes (frames=%u)\n",
                                        bytesToCopy, framesToWrite);
#endif
                                // Copy the audio data
                                memcpy(pDataRender, pAudioData, bytesToCopy);

                                // Release the buffer (commit the data)
                                g_pRenderClient->ReleaseBuffer(framesToWrite, 0);

                                // If we wrote all frames, this sample is fully processed
                                if (framesToWrite == framesInBuffer) {
                                    sampleProcessed = true;
                                } else {
                                    // In a more advanced approach, we would handle partial writing,
                                    // adjusting pointers, etc. For simplicity, mark as processed.
                                    sampleProcessed = true;
                                }
                            } else {
                                PrintHR("GetBuffer failed", hr);
                                sampleProcessed = true;
                            }
                        }
                        else if (framesInBuffer == 0) {
                            sampleProcessed = true;
                        }
                        else {
                            // Buffer is full, wait briefly and retry
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

        // Small sleep to avoid tight loop
        Sleep(1);
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread stopping\n");
#endif
    g_pAudioClient->Stop();
    return 0;
}

// ------------------------------------------------------
// Initialize WASAPI with a given WAVEFORMATEX (if provided)
// Otherwise, the device mix format is used (but here we
// force a shared-mode approach).
// ------------------------------------------------------
static HRESULT InitWASAPI(WAVEFORMATEX* pSourceFormat = nullptr)
{
    g_bAudioInitialized = false;

    // If we already have a valid IAudioClient, skip re-initialization
    if (g_pAudioClient && g_pRenderClient) {
        g_bAudioInitialized = true;
        return S_OK;
    }

    // Create the device enumerator
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  IID_PPV_ARGS(&g_pEnumerator));
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

    // Activate the IAudioClient
    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&g_pAudioClient));
    if (FAILED(hr)) {
        PrintHR("Activate(IAudioClient) failed", hr);
        return hr;
    }

    // If no source format is provided, get the device's mix format
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
        fprintf(stderr,
                "InitWASAPI: channels=%u, samples/sec=%u, bits=%u, blockAlign=%u, avgBytes/sec=%u\n",
                pSourceFormat->nChannels,
                pSourceFormat->nSamplesPerSec,
                pSourceFormat->wBitsPerSample,
                pSourceFormat->nBlockAlign,
                pSourceFormat->nAvgBytesPerSec);
    }
#endif

    // For shared mode, use a ~200ms buffer
    REFERENCE_TIME hnsBufferDuration = 2000000; // 2,000,000 x 100 ns = 200 ms
    hr = g_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0, // no special stream flags
        hnsBufferDuration,
        0, // periodicity
        pSourceFormat,
        nullptr
    );
    if (FAILED(hr)) {
        PrintHR("AudioClient->Initialize failed", hr);
        if (pwfxDevice) {
            CoTaskMemFree(pwfxDevice);
        }
        return hr;
    }

    // Retrieve the IAudioRenderClient from the IAudioClient
    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void**>(&g_pRenderClient));
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

// ------------------------------------------------------
// Initialize Media Foundation (call once).
// ------------------------------------------------------
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

        // Create an event for audio-thread synchronization
        g_hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_hAudioReadyEvent) {
            PrintHR("CreateEvent failed", HRESULT_FROM_WIN32(GetLastError()));
            // We'll continue even if this fails, just without the event
        }
    } else {
        PrintHR("InitMediaFoundation failed", hr);
    }
    return hr;
}

// ------------------------------------------------------
// Open a media (URL or file) and prepare video+audio
// ------------------------------------------------------
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

    // ------------------------------------------------------
    // 1) Create attributes for SourceReader (video)
    // ------------------------------------------------------
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) {
        PrintHR("MFCreateAttributes fail", hr);
        return hr;
    }

    // Enable advanced video processing (better video pipeline)
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(ENABLE_ADVANCED_VIDEO_PROCESSING) fail", hr);
        pAttributes->Release();
        return hr;
    }

    // Allow DXVA hardware acceleration
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(DISABLE_DXVA) fail", hr);
        pAttributes->Release();
        return hr;
    }

    // Create the video SourceReader
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();

    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) fail", hr);
        return hr;
    }

    // Disable all streams, enable only the first video stream
    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Video StreamSelection fail", hr);
        return hr;
    }

    // Force output to RGB32 for easy access
    {
        IMFMediaType* pType = nullptr;
        hr = MFCreateMediaType(&pType);
        if (SUCCEEDED(hr)) {
            hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            // Optionally force a nominal frame rate to help certain decoders
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, 30, 1);

            if (SUCCEEDED(hr)) {
                hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
            }
            pType->Release();
        }
        if (FAILED(hr)) {
            PrintHR("SetCurrentMediaType(RGB32) fail", hr);
            return hr;
        }

        // Retrieve the actual video size
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

    // ------------------------------------------------------
    // 2) Create audio SourceReader separately
    // ------------------------------------------------------
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) fail", hr);
        // No audio available, but we still succeed for video playback
        g_pSourceReaderAudio = nullptr;
        return S_OK;
    }

    // Disable all streams, enable only the first audio stream
    hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        PrintHR("Audio StreamSelection fail", hr);
        // We can continue with video only
        g_pSourceReaderAudio->Release();
        g_pSourceReaderAudio = nullptr;
        return S_OK;
    }

    // ------------------------------------------------------
    // 3) Force the audio to a standard PCM format (48kHz, 16-bit, stereo)
    //    This ensures we match a common WASAPI format in shared mode.
    // ------------------------------------------------------
    {
        IMFMediaType* pWantedType = nullptr;
        hr = MFCreateMediaType(&pWantedType);
        if (SUCCEEDED(hr)) {
            hr = pWantedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr)) hr = pWantedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);

            if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);       // 16-bit stereo => 4 bytes per frame
            if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000); // 48k * 4
            if (SUCCEEDED(hr)) hr = pWantedType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

            if (SUCCEEDED(hr)) {
                hr = g_pSourceReaderAudio->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                    nullptr,
                    pWantedType
                );
            }
            pWantedType->Release();
        }
        if (FAILED(hr)) {
            PrintHR("SetCurrentMediaType(audio forced PCM) fail", hr);
            // We can still continue without audio
            g_pSourceReaderAudio->Release();
            g_pSourceReaderAudio = nullptr;
            return S_OK;
        }
    }

    // ------------------------------------------------------
    // 4) Retrieve the chosen format from the SourceReader
    //    and initialize WASAPI accordingly
    // ------------------------------------------------------
    {
        IMFMediaType* pActualType = nullptr;
        hr = g_pSourceReaderAudio->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
        if (SUCCEEDED(hr) && pActualType) {
            WAVEFORMATEX* pWfx = nullptr;
            UINT32 size = 0;
            HRESULT hr2 = MFCreateWaveFormatExFromMFMediaType(pActualType, &pWfx, &size);
            if (SUCCEEDED(hr2) && pWfx) {
                // Initialize WASAPI with the new PCM format
                hr2 = InitWASAPI(pWfx);
                if (FAILED(hr2)) {
                    PrintHR("InitWASAPI with forced PCM failed", hr2);
                    // We'll continue with video only
                    g_pSourceReaderAudio->Release();
                    g_pSourceReaderAudio = nullptr;
                    CoTaskMemFree(pWfx);
                    pActualType->Release();
                    return S_OK;
                }

                // Store the wave format in a global pointer for reference
                if (g_pSourceAudioFormat) {
                    CoTaskMemFree(g_pSourceAudioFormat);
                }
                g_pSourceAudioFormat = pWfx;
            }
            if (pActualType) {
                pActualType->Release();
            }
        }
        // If it fails, we keep going with video only
    }

    g_bHasAudio = true;
    return S_OK;
}

// ------------------------------------------------------
// Read a video frame (RGB32). pData gets the pointer,
// pDataSize gets the size in bytes.
// ------------------------------------------------------
// ------------------------------------------------------
HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    // Unlock if still locked
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

    HRESULT hr = g_pSourceReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0,
        &streamIndex,
        &dwFlags,
        &llTimestamp,
        &pSample
    );
    if (FAILED(hr)) {
        PrintHR("ReadSample(video) fail", hr);
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
        // No frame available, so return OK but with no data
        *pData = nullptr;
        *pDataSize = 0;
        return S_OK;
    }

    // Convert to contiguous buffer
    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) {
        PrintHR("ConvertToContiguousBuffer fail", hr);
        pSample->Release();
        return hr;
    }

    // Lock the buffer
    BYTE* pBytes = nullptr;
    DWORD cbMaxLen = 0;
    DWORD cbCurrLen = 0;
    hr = pBuffer->Lock(&pBytes, &cbMaxLen, &cbCurrLen);
    if (FAILED(hr)) {
        PrintHR("Buffer->Lock fail", hr);
        pBuffer->Release();
        pSample->Release();
        return hr;
    }

    // Store references for later unlock
    g_pLockedBuffer = pBuffer;
    g_pLockedBytes = pBytes;
    g_lockedMaxSize = cbMaxLen;
    g_lockedCurrSize = cbCurrLen;

    // Provide data to the caller
    *pData = pBytes;
    *pDataSize = cbCurrLen;

    // Done with the sample, but keep pBuffer locked
    pSample->Release();
    return S_OK;
}

// ------------------------------------------------------
// Unlock the video frame buffer if locked
// ------------------------------------------------------
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

// ------------------------------------------------------
// Check if end-of-file was reached
// ------------------------------------------------------
BOOL IsEOF()
{
    return g_bEOF;
}

// ------------------------------------------------------
// Start audio playback (launches the audio thread)
// ------------------------------------------------------
HRESULT StartAudioPlayback()
{
    if (!g_pSourceReaderAudio) {
#ifdef _DEBUG
        fprintf(stderr, "No audio source to start\n");
#endif
        return E_FAIL;
    }

    if (g_bAudioPlaying) {
        // Already playing
        return S_OK;
    }

    // Ensure WASAPI is initialized
    if (!g_bAudioInitialized) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot start audio - WASAPI not initialized\n");
#endif
        return E_FAIL;
    }

    // Create and start the audio thread
    g_bAudioThreadRunning = true;
    g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    if (!g_hAudioThread) {
        g_bAudioThreadRunning = false;
        PrintHR("CreateThread(audio) fail", HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Signal the audio thread to begin (if event is valid)
    if (g_hAudioReadyEvent) {
        SetEvent(g_hAudioReadyEvent);
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio playback started\n");
#endif
    g_bAudioPlaying = true;
    return S_OK;
}

// ------------------------------------------------------
// Stop audio playback (stops the audio thread)
// ------------------------------------------------------
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
        // Wait for the thread to exit
        WaitForSingleObject(g_hAudioThread, 5000);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = nullptr;
    }
    g_bAudioPlaying = false;
    return S_OK;
}

// ------------------------------------------------------
// Close the media and release all resources
// ------------------------------------------------------
void CloseMedia()
{
    StopAudioPlayback();

    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    // Release WASAPI stuff
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

    // Release the two SourceReaders
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
}

// ------------------------------------------------------
// Return the detected video dimensions
// ------------------------------------------------------
void GetVideoSize(UINT32* pWidth, UINT32* pHeight)
{
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

// ------------------------------------------------------
// Retrieve the current video framerate (numerator, denominator)
// ------------------------------------------------------
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
