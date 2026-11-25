// hdcore — calculs communs à la bibliothèque injectée dans Serato (serato_hd.dylib) et à l'outil
// de test (hdtest) : waveform couleur d'un fichier audio, grille des temps Serato, paquets HID 0x30
// pour le patch « hdwave » du CDJ-2000NXS (firmware 1.44 modifié, voir firmware/patch/).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HD_CUE_MAX 8                 // repères envoyés à la platine (place du paquet 0x05)
// genres de repères de l'affichage (vérifiés dans l'émulateur le 16/11)
#define HD_KIND_HOT 4                // + 0..2 : hot cues A, B, C
#define HD_KIND_MEMORY 0             // cue mémoire
#define HD_KIND_LOOP_IN 3
#define HD_KIND_LOOP_OUT 17
#define HD_RATE 150                  // points de waveform par seconde (comme rekordbox)
#define HD_WAVE_MAX 900000           // octets : tampon de la platine (en-tête 8 + 2 par point)
#define HD_POINTS_MAX ((HD_WAVE_MAX - 8) / 2)   // 449 996 points = 50 min en couleur
#define HD_GRID_MAX 4096             // temps

typedef struct {
    double duration;                 // secondes
    char key[8];                     // tonalité lue dans les tags (« 8A », « Gb »), vide si absente
    uint32_t n;                      // points
    uint8_t *wave;                   // 8 + 2n octets : "HDRGB5\0\1" puis PWV5 (16 bits gros-boutiste)
    uint32_t wave_len;
    uint32_t grid_n;                 // temps de la grille (0 : pas de grille Serato dans le fichier)
    uint8_t *grid;                   // 4 octets par temps : numéro 1..4, ms bits 16-23, ms bits 0-15
    double bpm;                      // BPM du dernier repère de la grille (0 sinon)
    double analyse_s;                // durée du calcul
    double shift_ms;                 // recalage MP3 appliqué à la waveform (ms)
    uint32_t cue_n;                  // repères Serato (hot cues, bornes de boucles)
    uint8_t cue_idx[HD_CUE_MAX];     // indice Serato (0 = A…)
    uint8_t cue_type[HD_CUE_MAX];    // 0 hot cue, 1 début de boucle enregistrée
    uint16_t cue_kind[HD_CUE_MAX];   // genre pour l'affichage de la platine
    uint8_t cue_rgb[HD_CUE_MAX][3];  // couleur Serato (RVB)
    uint32_t cue_ms[HD_CUE_MAX];     // position (ms, timeline Serato)
    uint8_t *cz;                     // waveform compressée (firmware ≥ v36), calculée à l'envoi
    uint32_t cz_len;                 // 0 : pas encore calculée ou envoi non compressé
} hd_track_t;

// Analyse un fichier audio (tout format lu par AudioToolbox). NULL en cas d'échec.
hd_track_t *hd_analyse(const char *path);
void hd_free(hd_track_t *t);

// Paquets HID 0x30 (64 octets) d'un morceau : 30 00, 30 01, 30 02…, 30 03, 30 04…, 30 05.
// Appelle send(ctx, paquet) pour chacun ; s'arrête et renvoie -1 si send renvoie une erreur.
int hd_packets(const hd_track_t *t, int (*send)(void *ctx, const uint8_t pkt[64]), void *ctx);
// Paquet 30 00 : effacer (la platine revient au comportement d'origine).
void hd_clear_packet(uint8_t pkt[64]);
uint32_t hd_packet_count(const hd_track_t *t);

// Grille Serato seule (pour les tests) : renvoie le nombre de temps écrits dans grid.
uint32_t hd_serato_grid(const char *path, double duration, uint8_t *grid, uint32_t max, double *bpm);
