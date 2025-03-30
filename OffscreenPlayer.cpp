#define WIN32_LEAN_AND_MEAN
#include "OffscreenPlayer.h"
#include <cstdio>
#include <new>
#include <vector>

// ------------------------------------------------------
// Variables globales (simple démo, pas "clean" !)
// ------------------------------------------------------
static bool g_bMFInitialized = false;

// Vidéo
static IMFSourceReader* g_pSourceReader = nullptr;
static BOOL g_bEOF = FALSE;
static IMFMediaBuffer* g_pLockedBuffer = nullptr;
static BYTE* g_pLockedBytes = nullptr;
static DWORD g_lockedMaxSize = 0;
static DWORD g_lockedCurrSize = 0;

// Taille de la vidéo réelle
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

// Thread audio
static HANDLE g_hAudioThread = NULL;
static bool g_bAudioThreadRunning = false;

// =============================================================
// Fonctions internes
// =============================================================

static void PrintHR(const char* msg, HRESULT hr) {
#ifdef _DEBUG
    fprintf(stderr, "%s (hr=0x%08x)\n", msg, (unsigned int)hr);
#else
    (void)msg; (void)hr;
#endif
}

// Thread qui lit l'audio et l'envoie à WASAPI
static DWORD WINAPI AudioThreadProc(LPVOID /*lpParam*/)
{
    if (!g_pAudioClient || !g_pRenderClient) {
        PrintHR("AudioThreadProc: Missing AudioClient/RenderClient", E_FAIL);
        return 0;
    }
    HRESULT hr = g_pAudioClient->Start();
    if (FAILED(hr)) {
        PrintHR("AudioClient->Start failed", hr);
        return 0;
    }

    // On suppose que la piste audio est le flux #1
    const DWORD audioStreamIndex = 1;

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
            break; // fin du flux audio
        }
        if (pSample) {
            IMFMediaBuffer* pBuf = nullptr;
            hr = pSample->ConvertToContiguousBuffer(&pBuf);
            if (SUCCEEDED(hr) && pBuf) {
                BYTE* pAudioData = nullptr;
                DWORD cbMaxLen = 0, cbCurrLen = 0;
                hr = pBuf->Lock(&pAudioData, &cbMaxLen, &cbCurrLen);
                if (SUCCEEDED(hr)) {
                    // On déverse dans le buffer WASAPI
                    WAVEFORMATEX* pwfx = nullptr;
                    UINT32 numFramesPadding = 0;
                    UINT32 numFramesAvailable = 0;

                    hr = g_pAudioClient->GetMixFormat(&pwfx);
                    if (SUCCEEDED(hr) && pwfx) {
                        hr = g_pAudioClient->GetCurrentPadding(&numFramesPadding);
                        if (SUCCEEDED(hr)) {
                            const UINT32 bufferFrameCount = pwfx->nSamplesPerSec; // simpliste
                            numFramesAvailable = bufferFrameCount - numFramesPadding;

                            // Nb de frames dans notre chunk
                            UINT32 framesInBuffer = cbCurrLen / pwfx->nBlockAlign;
                            if (framesInBuffer > numFramesAvailable) {
                                framesInBuffer = numFramesAvailable;
                            }

                            BYTE* pDataRender = nullptr;
                            hr = g_pRenderClient->GetBuffer(framesInBuffer, &pDataRender);
                            if (SUCCEEDED(hr) && pDataRender) {
                                memcpy(pDataRender, pAudioData, framesInBuffer * pwfx->nBlockAlign);
                                g_pRenderClient->ReleaseBuffer(framesInBuffer, 0);
                            }
                        }
                        CoTaskMemFree(pwfx);
                    }
                    pBuf->Unlock();
                }
                pBuf->Release();
            }
            pSample->Release();
        }
        Sleep(5);
    }

    g_pAudioClient->Stop();
    return 0;
}

static HRESULT InitWASAPI()
{
    if (g_pAudioClient && g_pRenderClient) {
        return S_OK; // Déjà initialisé
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  IID_PPV_ARGS(&g_pEnumerator));
    if (FAILED(hr)) return hr;

    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) return hr;

    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pAudioClient);
    if (FAILED(hr)) return hr;

    // Récupère format "mix" Windows
    WAVEFORMATEX* pwfx = nullptr;
    hr = g_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) return hr;

    // Buffer d'environ 200ms
    REFERENCE_TIME hnsBufferDuration = 2000000;
    hr = g_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        hnsBufferDuration,
        0,
        pwfx,
        NULL
    );
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        return hr;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_pRenderClient);
    CoTaskMemFree(pwfx);
    return hr;
}

// =============================================================
// Implémentation des fonctions exportées
// =============================================================
HRESULT InitMediaFoundation()
{
    if (g_bMFInitialized)
        return OP_E_ALREADY_INITIALIZED;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        hr = MFStartup(MF_VERSION);
    }
    if (SUCCEEDED(hr)) {
        g_bMFInitialized = true;
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

    // Fermer un précédent média le cas échéant
    CloseMedia();
    g_bEOF = FALSE;
    g_videoWidth = 0;
    g_videoHeight = 0;

    HRESULT hr = S_OK;

    // 1) Reader vidéo
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReader);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(video) fail", hr);
        return hr;
    }

    // 2) Sortie vidéo RGB32
    {
        IMFMediaType* pType = nullptr;
        hr = MFCreateMediaType(&pType);
        if (SUCCEEDED(hr)) {
            hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

            if (SUCCEEDED(hr)) {
                hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
            }
            pType->Release();
        }
        if (FAILED(hr)) {
            PrintHR("SetCurrentMediaType(RGB32) fail", hr);
            return hr;
        }

        // Récupération de la taille réelle
        IMFMediaType* pCurrent = nullptr;
        hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
        if (SUCCEEDED(hr) && pCurrent) {
            MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &g_videoWidth, &g_videoHeight);
            pCurrent->Release();
        }
    }

    // 3) Reader audio
    hr = MFCreateSourceReaderFromURL(url, nullptr, &g_pSourceReaderAudio);
    if (FAILED(hr)) {
        PrintHR("MFCreateSourceReaderFromURL(audio) fail", hr);
        return hr;
    }

    // Sortie audio en PCM 16 bits, 2 canaux, 44100 Hz
    {
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
                hr = g_pSourceReaderAudio->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pTypeAudio);
            }
            pTypeAudio->Release();
        }
        if (FAILED(hr)) {
            PrintHR("SetCurrentMediaType(PCM) fail", hr);
            return hr;
        }
    }

    return hr;
}

HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize)
{
    if (!g_pSourceReader || !pData || !pDataSize)
        return OP_E_NOT_INITIALIZED;

    // Unlock si jamais
    if (g_pLockedBuffer) {
        UnlockVideoFrame();
    }

    if (g_bEOF) {
        *pData = nullptr;
        *pDataSize = 0;
        return S_FALSE;
    }

    // Lecture de l'échantillon vidéo
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
        // Pas de frame dispo, flux en cours
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
    if (g_bAudioPlaying) {
        return S_OK;
    }

    HRESULT hr = InitWASAPI();
    if (FAILED(hr)) {
        PrintHR("InitWASAPI fail", hr);
        return hr;
    }

    g_bAudioThreadRunning = true;
    g_hAudioThread = CreateThread(NULL, 0, AudioThreadProc, NULL, 0, NULL);
    if (!g_hAudioThread) {
        g_bAudioThreadRunning = false;
        PrintHR("CreateThread(audio) fail", HRESULT_FROM_WIN32(GetLastError()));
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_bAudioPlaying = true;
    return S_OK;
}

HRESULT StopAudioPlayback()
{
    if (!g_bAudioPlaying) {
        return S_OK;
    }
    g_bAudioThreadRunning = false;
    if (g_hAudioThread) {
        WaitForSingleObject(g_hAudioThread, INFINITE);
        CloseHandle(g_hAudioThread);
        g_hAudioThread = NULL;
    }
    g_bAudioPlaying = false;
    return S_OK;
}

void CloseMedia()
{
    StopAudioPlayback();

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

    if (g_pLockedBuffer) {
        UnlockVideoFrame();
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

// Renvoyer la taille réelle détectée
void GetVideoSize(UINT32* pWidth, UINT32* pHeight)
{
    if (pWidth)  *pWidth = g_videoWidth;
    if (pHeight) *pHeight = g_videoHeight;
}
