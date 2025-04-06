#pragma once
#ifndef OFFSCREEN_PLAYER_H
#define OFFSCREEN_PLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

// Macro d'exportation pour la DLL Windows
#ifdef _WIN32
#ifdef OFFSCREENPLAYER_EXPORTS
#define OFFSCREENPLAYER_API __declspec(dllexport)
#else
#define OFFSCREENPLAYER_API __declspec(dllimport)
#endif
#else
#define OFFSCREENPLAYER_API
#endif

// Codes d'erreur personnalisés
#define OP_E_NOT_INITIALIZED     ((HRESULT)0x80000001L)
#define OP_E_ALREADY_INITIALIZED ((HRESULT)0x80000002L)
#define OP_E_INVALID_PARAMETER   ((HRESULT)0x80000003L)

#ifdef __cplusplus
extern "C" {
#endif

// ====================================================================
// Fonctions exportées pour la lecture multimédia Offscreen
// ====================================================================

/**
 * @brief Initialise Media Foundation, Direct3D11 et le gestionnaire DXGI.
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT InitMediaFoundation();

/**
 * @brief Ouvre un média (fichier ou URL) et prépare le décodage avec accélération matérielle.
 * @param url Chemin ou URL du média (chaine large).
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT OpenMedia(const wchar_t* url);

/**
 * @brief Lit la prochaine frame vidéo en format RGB32 (via conversion si nécessaire) avec synchronisation AV améliorée.
 * @param pData Reçoit un pointeur sur les données de la frame (à ne pas libérer).
 * @param pDataSize Reçoit la taille en octets du tampon.
 * @return S_OK si une frame est lue, S_FALSE en fin de flux, ou un code d'erreur.
 * @note Les données restent valides jusqu'à l'appel de UnlockVideoFrame.
 */
OFFSCREENPLAYER_API HRESULT ReadVideoFrame(BYTE** pData, DWORD* pDataSize);

/**
 * @brief Déverrouille le tampon de la frame vidéo précédemment verrouillé par ReadVideoFrame.
 * @return S_OK en cas de succès.
 */
OFFSCREENPLAYER_API HRESULT UnlockVideoFrame();

/**
 * @brief Ferme le média et libère toutes les ressources associées.
 */
OFFSCREENPLAYER_API void CloseMedia();

/**
 * @brief Indique si la fin du flux média a été atteinte.
 * @return TRUE si fin de flux, FALSE sinon.
 */
OFFSCREENPLAYER_API BOOL IsEOF();

/**
 * @brief Récupère les dimensions de la vidéo.
 * @param pWidth Pointeur pour recevoir la largeur en pixels.
 * @param pHeight Pointeur pour recevoir la hauteur en pixels.
 */
OFFSCREENPLAYER_API void GetVideoSize(UINT32* pWidth, UINT32* pHeight);

/**
 * @brief Récupère le taux de rafraîchissement (frame rate) de la vidéo.
 * @param pNum Pointeur pour recevoir le numérateur.
 * @param pDenom Pointeur pour recevoir le dénominateur.
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT GetVideoFrameRate(UINT* pNum, UINT* pDenom);

/**
 * @brief Recherche une position spécifique dans le média.
 * @param llPosition Position (en 100-ns) à atteindre.
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT SeekMedia(LONGLONG llPosition);

/**
 * @brief Obtient la durée totale du média.
 * @param pDuration Pointeur pour recevoir la durée (en 100-ns).
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT GetMediaDuration(LONGLONG* pDuration);

/**
 * @brief Obtient la position de lecture courante.
 * @param pPosition Pointeur pour recevoir la position (en 100-ns).
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT GetMediaPosition(LONGLONG* pPosition);

/**
 * @brief Définit l'état de lecture (lecture ou pause).
 * @param bPlaying TRUE pour lecture, FALSE pour pause.
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT SetPlaybackState(BOOL bPlaying);

/**
 * @brief Arrête Media Foundation et libère toutes les ressources.
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT ShutdownMediaFoundation();

/**
 * @brief Définit le niveau de volume audio.
 * @param volume Niveau de volume à appliquer (entre 0.0 pour muet et 1.0 pour le volume maximum).
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT SetAudioVolume(float volume);

/**
 * @brief Récupère le niveau de volume audio actuel.
 * @param volume Pointeur pour recevoir le niveau de volume (entre 0.0 et 1.0).
 * @return S_OK en cas de succès, ou un code d'erreur.
 */
OFFSCREENPLAYER_API HRESULT GetAudioVolume(float* volume);

/**
* @brief Récupère les niveaux audio pour les canaux gauche et droit.
* @param pLeftLevel Pointeur pour recevoir le niveau audio du canal gauche.
* @param pRightLevel Pointeur pour recevoir le niveau audio du canal droit.
* @return S_OK en cas de succès, ou un code d'erreur.
*/
OFFSCREENPLAYER_API HRESULT GetAudioLevels(float* pLeftLevel, float* pRightLevel);


#ifdef __cplusplus
}
#endif

#endif // OFFSCREEN_PLAYER_H
