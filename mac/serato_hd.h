// serato_hd — logique commune à la bibliothèque injectée dans Serato et à l'outil de test hdtest.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "hdcore.h"

typedef struct hd_deck hd_deck_t;
typedef int (*hd_send_fn)(void *dev, const uint8_t *pkt);                 // 0 : envoyé
typedef int (*hd_find_fn)(hd_deck_t *k, const char *title, char *path, size_t cap);   // 1 : trouvé

extern __thread int hd_quiet;         // 1 pendant nos propres lectures de fichiers audio

void hd_init(FILE *log);
void hd_set_io(hd_send_fn send, hd_find_fn find);
void hd_log(const char *fmt, ...);
double hd_now(void);
hd_deck_t *hd_deck_for(void *dev);
int hd_deck_num(const hd_deck_t *k);
void hd_on_report(hd_deck_t *k, const uint8_t *r, size_t len);   // paquet de Serato, avant envoi
void hd_on_input(hd_deck_t *k, uint8_t *r, size_t len);    // rapport reçu de la platine
void hd_rec(const char *dir, int deck, const uint8_t *d, size_t len);
void hd_rec_flush(void);
int hd_pass(hd_deck_t *k, const uint8_t *r);
int hd_pass_prio(hd_deck_t *k, const uint8_t *r);   // rapport de Serato : avant nos envois en série
const uint8_t *hd_tweak(hd_deck_t *k, const uint8_t *r, size_t len, uint8_t *buf);
void hd_lock_send(hd_deck_t *k);
void hd_unlock_send(hd_deck_t *k);
// Fichier audio ouvert par Serato : analysé d'avance pour un chargement plus rapide (V36).
void hd_pre_open(const char *path);
