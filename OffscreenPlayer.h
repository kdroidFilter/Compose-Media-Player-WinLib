#pragma once

#ifndef OFFSCREEN_PLAYER_H
#define OFFSCREEN_PLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef OFFSCREENPLAYER_EXPORTS
#define OFFSCREENPLAYER_API __declspec(dllexport)
#else
#define OFFSCREENPLAYER_API __declspec(dllimport)
#endif
#else
#define OFFSCREENPLAYER_API
#endif

// Codes d'erreur custom (exemple)
#define OP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define OP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define OP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

// ====================================================================
// Fonctions exportées
// ====================================================================

//
// 1) Initialiser Media Foundation (à appeler UNE SEULE FOIS).
//
OFFSCREENPLAYER_API HRESULT InitMediaFoundation();

//
// 2) Ouvrir un fichier (ou URL) et préparer le décodage video+audio.
//
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t *url);

//
// 3) Lire une frame vidéo en mémoire (RGB32).
//    - pData : pointeur de pointeur vers les pixels
//    - pDataSize : taille en octets
//    Retourne : S_OK si ok, S_FALSE si fin de flux, ou E_FAIL si erreur.
//
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE **pData, DWORD *pDataSize);

//
// 4) Libère la frame précédente (déverrouille le buffer).
//
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame();

//
// 5) Fermer le flux et libérer les ressources Media Foundation.
//
OFFSCREENPLAYER_API void CloseMedia();

//
// 6) Contrôles basiques
//
OFFSCREENPLAYER_API BOOL IsEOF(); // Fin de la vidéo ?
OFFSCREENPLAYER_API HRESULT StartAudioPlayback(); // Démarre l'audio (thread interne)
OFFSCREENPLAYER_API HRESULT StopAudioPlayback(); // Stoppe l'audio

//
// 7) Récupérer la taille réelle de la vidéo (width, height).
//
OFFSCREENPLAYER_API void GetVideoSize(UINT32 *pWidth, UINT32 *pHeight);

OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT *pNum, UINT *pDenom);

#ifdef __cplusplus
}
#endif

#endif // OFFSCREEN_PLAYER_H
