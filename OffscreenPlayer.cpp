#define WIN32_LEAN_AND_MEAN
#include "OffscreenPlayer.h"
#include <cstdio>
#include <new>
#include <vector>

// ------------------------------------------------------
// Global variables (simple demo, not "clean"!)
// ------------------------------------------------------
static bool g_bMFInitialized = false;

// Video
static IMFSourceReader* g_pSourceReader = nullptr;
static BOOL g_bEOF = FALSE;
static IMFMediaBuffer* g_pLockedBuffer = nullptr;
static BYTE* g_pLockedBytes = nullptr;
static DWORD g_lockedMaxSize = 0;
static DWORD g_lockedCurrSize = 0;

// Actual video size
static UINT32 g_videoWidth = 0;
static UINT32 g_videoHeight = 0;

// Audio
static IMFSourceReader* g_pSourceReaderAudio = nullptr;
static bool g_bAudioPlaying = false;

// WASAPI
static IAudioClient* g_pAudioClient = nullptr;
static IAudioRenderClient* g_pRenderClient = nullptr;
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static IMMDevice* g_pDevice = nullptr;
static WAVEFORMATEX* g_pSourceAudioFormat = nullptr;

// Audio thread
static HANDLE g_hAudioThread = nullptr;
static bool g_bAudioThreadRunning = false;
static HANDLE g_hAudioReadyEvent = nullptr;  // Add synchronization event

// =============================================================
// Internal functions
// =============================================================

static void PrintHR(const char* msg, HRESULT hr) {
#ifdef _DEBUG
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
#else
    (void)msg; (void)hr;
#endif
}

// Thread to read audio and send it to WASAPI
static DWORD WINAPI AudioThreadProc(LPVOID /*lpParam*/)
{
    if (!g_pAudioClient || !g_pRenderClient || !g_pSourceReaderAudio) {
        PrintHR("AudioThreadProc: Missing AudioClient/RenderClient/SourceReaderAudio", E_FAIL);
        return 0;
    }

    HRESULT hr = g_pAudioClient->Start();
    if (FAILED(hr)) {
        PrintHR("AudioClient->Start failed", hr);
        return 0;
    }

    // Use the correct stream index for audio
    const DWORD audioStreamIndex = MF_SOURCE_READER_FIRST_AUDIO_STREAM;

    // Get buffer frame count
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
            PrintHR("ReadSample(audio) fail", hr);
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
#ifdef _DEBUG
            fprintf(stderr, "Audio stream ended\n");
#endif
            break; // End of audio stream
        }

        if (!pSample) {
            Sleep(5);
            continue;
        }

        IMFMediaBuffer* pBuf = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuf);
        if (SUCCEEDED(hr) && pBuf) {
            BYTE* pAudioData = nullptr;
            DWORD cbMaxLen = 0, cbCurrLen = 0;
            hr = pBuf->Lock(&pAudioData, &cbMaxLen, &cbCurrLen);
            if (SUCCEEDED(hr)) {
                // Get current padding
                UINT32 numFramesPadding = 0;
                hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
                if (SUCCEEDED(hr)) {
                    // Calculate available frames
                    UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;

                    // Get source format block align
                    UINT32 blockAlign = g_pSourceAudioFormat ? g_pSourceAudioFormat->nBlockAlign : 4;

                    // Calculate frames in our audio data
                    UINT32 framesInBuffer = cbCurrLen / blockAlign;

                    // Limit to available space
                    if (framesInBuffer > numFramesAvailable) {
                        framesInBuffer = numFramesAvailable;
                    }

                    if (framesInBuffer > 0) {
                        // Get buffer to write to
                        BYTE* pDataRender = nullptr;
                        hr = g_pRenderClient->GetBuffer(framesInBuffer, &pDataRender);
                        if (SUCCEEDED(hr) && pDataRender) {
                            DWORD bytesToCopy = framesInBuffer * blockAlign;
#ifdef _DEBUG
                            fprintf(stderr, "Audio: Copying %u bytes (frames=%u)\n", bytesToCopy, framesInBuffer);
#endif
                            memcpy(pDataRender, pAudioData, bytesToCopy);
                            g_pRenderClient->ReleaseBuffer(framesInBuffer, 0);
                        }
                        else {
                            PrintHR("GetBuffer failed", hr);
                        }
                    }
                    else {
                        // Buffer full, wait a bit
                        Sleep(10);
                    }
                }
                else {
                    PrintHR("GetCurrentPadding failed", hr);
                }

                pBuf->Unlock();
            }
            pBuf->Release();
        }
        pSample->Release();

        // Small sleep to avoid tight loop
        Sleep(1);
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio thread stopping\n");
#endif
    g_pAudioClient->Stop();
    return 0;
}

static HRESULT InitWASAPI(WAVEFORMATEX* pSourceFormat = nullptr)
{
    if (g_pAudioClient && g_pRenderClient) {
        return S_OK; // Already initialized
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&g_pEnumerator));
    if (FAILED(hr)) {
        PrintHR("CoCreateInstance(MMDeviceEnumerator) failed", hr);
        return hr;
    }

    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) {
        PrintHR("GetDefaultAudioEndpoint failed", hr);
        return hr;
    }

    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&g_pAudioClient));
    if (FAILED(hr)) {
        PrintHR("Activate(IAudioClient) failed", hr);
        return hr;
    }

    // Get Windows mix format
    WAVEFORMATEX* pwfx = nullptr;
    hr = g_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        PrintHR("GetMixFormat failed", hr);
        return hr;
    }

    // Use source format if provided, otherwise use mix format
    WAVEFORMATEX* pFormatToUse = pwfx;
    if (pSourceFormat) {
#ifdef _DEBUG
        fprintf(stderr, "Audio: Source format - channels: %u, samples/sec: %u, bits/sample: %u\n",
            pSourceFormat->nChannels, pSourceFormat->nSamplesPerSec, pSourceFormat->wBitsPerSample);
        fprintf(stderr, "Audio: Device format - channels: %u, samples/sec: %u, bits/sample: %u\n",
            pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);
#endif
        // Save source format for later use
        if (g_pSourceAudioFormat) {
            CoTaskMemFree(g_pSourceAudioFormat);
        }
        g_pSourceAudioFormat = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (g_pSourceAudioFormat) {
            memcpy(g_pSourceAudioFormat, pSourceFormat, sizeof(WAVEFORMATEX));
        }

        // Try with source format first
        hr = g_pAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, pSourceFormat, nullptr);
        if (hr == S_OK) {
            pFormatToUse = pSourceFormat;
#ifdef _DEBUG
            fprintf(stderr, "Audio: Using source format\n");
#endif
        }
        else {
#ifdef _DEBUG
            fprintf(stderr, "Audio: Source format not supported, using device format\n");
#endif
        }
    }

    // Buffer of about 200ms
    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = g_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        hnsBufferDuration,
        0,
        pFormatToUse,
        nullptr
    );
    if (FAILED(hr)) {
        PrintHR("AudioClient->Initialize failed", hr);
        CoTaskMemFree(pwfx);
        return hr;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&g_pRenderClient));
    if (FAILED(hr)) {
        PrintHR("GetService(IAudioRenderClient) failed", hr);
    }

    CoTaskMemFree(pwfx);
    return hr;
}

// =============================================================
// Implementation of exported functions
// =============================================================
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

        // Create synchronization event
        g_hAudioReadyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    } else {
        PrintHR("InitMediaFoundation failed", hr);
    }
    return hr;
}

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

    HRESULT hr = S_OK;

    // 1) Create attributes for SourceReader
    IMFAttributes* pAttributes = nullptr;
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) {
        PrintHR("MFCreateAttributes fail", hr);
        return hr;
    }

    // Enable hardware decoding for H.264
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(ENABLE_ADVANCED_VIDEO_PROCESSING) fail", hr);
        pAttributes->Release();
        return hr;
    }

    // Disable transformations (important for MP4)
    hr = pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    if (FAILED(hr)) {
        PrintHR("SetUINT32(DISABLE_DXVA) fail", hr);
        pAttributes->Release();
        return hr;
    }

    // 2) Video reader with attributes
    hr = MFCreateSourceReaderFromURL(url, pAttributes, &g_pSourceReader);
    pAttributes->Release();

    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) fail", hr);
        return hr;
    }

    // Configure streams - disable unused streams
    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(hr)) {
        PrintHR("SetStreamSelection(ALL_STREAMS, FALSE) fail", hr);
        return hr;
    }

    // Enable only video stream
    hr = g_pSourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) {
        PrintHR("SetStreamSelection(FIRST_VIDEO_STREAM, TRUE) fail", hr);
        return hr;
    }

    // 3) Set video output to RGB32
    {
        IMFMediaType* pType = nullptr;
        hr = MFCreateMediaType(&pType);
        if (SUCCEEDED(hr)) {
            hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            // Setting a fixed frame rate helps for some formats
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

        // Get actual size
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

    // 4) Audio reader (same URL, new instance)
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) fail", hr);
        // Continue anyway, video can work without audio
        g_pSourceReaderAudio = nullptr;
    }
    else {
        // Configure audio streams
        hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) {
            hr = g_pSourceReaderAudio->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        }

        // Get native audio stream format to check if it exists
        IMFMediaType* pNativeType = nullptr;
        hr = g_pSourceReaderAudio->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &pNativeType);
        if (FAILED(hr) || !pNativeType) {
            PrintHR("No audio stream found", hr);
            if (g_pSourceReaderAudio) {
                g_pSourceReaderAudio->Release();
                g_pSourceReaderAudio = nullptr;
            }
            return S_OK; // Continue with video only
        }

        if (pNativeType) {
            pNativeType->Release();
        }

        // Set audio output to PCM 16-bit, 2 channels, 44100 Hz
        IMFMediaType* pTypeAudio = nullptr;
        hr = MFCreateMediaType(&pTypeAudio);
        if (SUCCEEDED(hr)) {
            hr = pTypeAudio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
            if (SUCCEEDED(hr)) hr = pTypeAudio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

            if (SUCCEEDED(hr)) {
                hr = g_pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pTypeAudio);
            }

            if (SUCCEEDED(hr)) {
                // Get the actual format created
                IMFMediaType* pActualType = nullptr;
                hr = g_pSourceReaderAudio->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pActualType);
                if (SUCCEEDED(hr) && pActualType) {
                    // Extract audio format from media type
                    UINT32 size = 0;
                    WAVEFORMATEX* pSourceWfx = nullptr;
                    hr = MFCreateWaveFormatExFromMFMediaType(pActualType, &pSourceWfx, &size);
                    if (SUCCEEDED(hr) && pSourceWfx) {
                        // Initialize WASAPI with this format
                        hr = InitWASAPI(pSourceWfx);
                        CoTaskMemFree(pSourceWfx);
                    }
                    pActualType->Release();
                }
            }

            pTypeAudio->Release();
        }

        if (FAILED(hr)) {
            PrintHR("Audio setup failed", hr);
            // Continue anyway, video can work without audio
            if (g_pSourceReaderAudio) {
                g_pSourceReaderAudio->Release();
                g_pSourceReaderAudio = nullptr;
            }
        }
    }

    return S_OK;  // Return S_OK even if audio fails
}

HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    // Unlock if necessary
    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    // Read video sample
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
        // No frame available, stream in progress
        *pData = nullptr;
        *pDataSize = 0;
        return S_OK;
    }

    // Contiguous buffer
    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) {
        PrintHR("ConvertToContiguousBuffer fail", hr);
        pSample->Release();
        return hr;
    }

    // Lock
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

    g_pLockedBuffer = pBuffer;
    g_pLockedBytes = pBytes;
    g_lockedMaxSize = cbMaxLen;
    g_lockedCurrSize = cbCurrLen;

    *pData = pBytes;
    *pDataSize = cbCurrLen;

    pSample->Release();
    return S_OK;
}

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

BOOL IsEOF()
{
    return g_bEOF;
}

HRESULT StartAudioPlayback()
{
    if (!g_pSourceReaderAudio) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot start audio playback - no audio source\n");
#endif
        return E_FAIL;
    }

    if (g_bAudioPlaying) {
        return S_OK;
    }

    HRESULT hr = S_OK;

    // Check if WASAPI is initialized
    if (!g_pAudioClient || !g_pRenderClient) {
        hr = InitWASAPI();
        if (FAILED(hr)) {
            PrintHR("InitWASAPI fail", hr);
            return hr;
        }
    }

    // Create and start audio thread
    g_bAudioThreadRunning = true;
    g_hAudioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    if (!g_hAudioThread) {
        g_bAudioThreadRunning = false;
        PrintHR("CreateThread(audio) fail", HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }

#ifdef _DEBUG
    fprintf(stderr, "Audio playback started\n");
#endif
    g_bAudioPlaying = true;
    return S_OK;
}

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
        // Wait for thread to exit with timeout
        WaitForSingleObject(g_hAudioThread, 5000);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = nullptr;
    }
    g_bAudioPlaying = false;
    return S_OK;
}

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

    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;
}

// Return actual detected size
void GetVideoSize(UINT32* pWidth, UINT32* pHeight)
{
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}

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