// serato_hd — bibliothèque chargée dans Serato DJ Pro (DYLD_INSERT_LIBRARIES) pour le firmware
// modifié du CDJ-2000NXS (patch « hdwave » + « rgbwave », firmware/patch/) : envoie à la platine
// la waveform complète en couleur et la grille des temps du morceau chargé, en paquets HID 0x30.
//
// Principe :
//  1. au premier paquet du titre (02 xx) d'un chargement, 30 00 est envoyé : la platine oublie la
//     waveform du morceau précédent ;
//  2. un fil d'exécution repère le fichier audio ouvert par Serato, l'analyse (hdcore.c) et envoie
//     la waveform, la grille, puis 30 05, entre les paquets de Serato, qui ne sont ni retenus ni
//     retardés ;
//  3. à la réception de 30 05, la platine rejoue une séquence de chargement : son affichage réclame
//     la waveform et la dessine. (Jusqu'au 13/11/2026, les paquets de Serato étaient retenus
//     pendant l'envoi : inutile depuis que le firmware rejoue cette séquence, et gênant à la
//     connexion, où Serato recharge seul le morceau de son deck.)
// Si le fichier est introuvable ou illisible, ou si sa durée n'est pas celle que Serato annonce,
// rien d'autre n'est envoyé : la platine se comporte comme d'origine (logo, petite waveform de
// Serato), jamais avec la waveform d'un autre morceau.
//
// Variables d'environnement :
//   SERATO_HD_LOG   journal (défaut /tmp/serato_hd.log)
//   SERATO_HD_REC   enregistrement de session : tous les rapports HID échangés entre Serato et
//                   les platines, dans les deux sens, rejouables par hdtest replay
//   HD_PASSIF=1     n'envoie rien à la platine (enregistrement seul, firmware d'origine)
//   HD_DECKS        platines au firmware modifié, par ordre de détection (ex. « 1 » ou « 1,2 ») ;
//                   les autres sont laissées telles quelles (défaut : toutes)
//   HD_LOOP_BLINK   demi-période (s) du clignotement de LOOP IN / OUT en boucle (0,25 ; 0 : fixe)
//   HD_GAINS, HD_GAMMA, HD_HMAX… : réglages des couleurs et des hauteurs (voir hdcore.c)
//
// Compilé aussi dans l'outil de test hdtest (HD_HARNESS), qui fait tourner exactement cette
// logique contre l'émulateur, en rejouant une session Serato enregistrée.

#include "serato_hd.h"

#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// MARK: - journal et enregistrement

__thread int hd_quiet;
static FILE *hlog, *hrec;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER, rec_mu = PTHREAD_MUTEX_INITIALIZER;
static double t0;
static int passive;

#ifdef HD_HARNESS
double hd_fake_now = -1;               // horloge simulée (banc d'essai hors ligne)
#endif
double hd_now(void) {
#ifdef HD_HARNESS
    if (hd_fake_now >= 0) return hd_fake_now;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void hd_log(const char *fmt, ...) {
    if (!hlog) return;
    pthread_mutex_lock(&log_mu);
    fprintf(hlog, "%10.4f ", hd_now() - t0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(hlog, fmt, ap);
    va_end(ap);
    fputc('\n', hlog);
    fflush(hlog);
    pthread_mutex_unlock(&log_mu);
}

void hd_init(FILE *log) {
    if (hlog) return;
    hlog = log;
    t0 = hd_now();
    passive = getenv("HD_PASSIF") != NULL;
    const char *r = getenv("SERATO_HD_REC");
    if (r) hrec = fopen(r, "a");
}

// Une ligne par rapport HID : « temps HOST>CDJ dN len=64 xx xx … » (ou CDJ>HOST). Le format est
// celui que relit hdtest replay.
void hd_rec(const char *dir, int deck, const uint8_t *d, size_t len) {
    if (!hrec) return;
    char line[3 * 64 + 64];
    if (len > 64) len = 64;
    int n = snprintf(line, sizeof line, "%10.4f %s d%d len=%zu", hd_now() - t0, dir, deck, len);
    for (size_t i = 0; i < len; i++) n += snprintf(line + n, sizeof line - n, " %02x", d[i]);
    pthread_mutex_lock(&rec_mu);
    fputs(line, hrec);
    fputc('\n', hrec);
    pthread_mutex_unlock(&rec_mu);
}

void hd_rec_flush(void) {
    if (!hrec) return;
    pthread_mutex_lock(&rec_mu);
    fflush(hrec);
    pthread_mutex_unlock(&rec_mu);
}

// MARK: - platines

// Une platine débranchée puis rebranchée (ou redémarrée) est vue comme une nouvelle platine : de
// la place pour 14 reconnexions par séance avec deux platines (4 jusqu'au 24/11 : 2 seulement).
#define MAX_DECKS 16

struct hd_deck {
    int num;
    void *dev;
    pthread_mutex_t send_mu;          // un seul envoi à la fois vers cette platine
    int prio;                         // rapports de Serato en attente du verrou (passent avant nous)
    volatile int generation;          // incrémenté à chaque chargement
    char title_buf[512];
    size_t title_len;
    uint8_t last_in[64];              // dernier rapport reçu de la platine (enregistrement)
    size_t last_in_len;
    int rot_last, rot_init;    // compteur de la molette (zoom)
    int rot_virt;              // compteur de molette montré à Serato
    int list_last;             // dernier état « vue liste » annoncé par la platine
    volatile int wave_over_list;   // waveform à l'écran alors que la platine se dit en vue liste
    // repères et boucle suivis en direct (voir live_report)
    pthread_mutex_t live_mu;
    int live_gen, live_ok, led_init;
    uint32_t cue_n; uint8_t cue_idx[HD_CUE_MAX]; uint8_t cue_rgb[HD_CUE_MAX][3]; uint32_t cue_ms[HD_CUE_MAX];
    uint8_t led_prev, loop_prev;
    double gone_t[3];              // voyant de hot cue éteint depuis (0 : allumé ou déjà traité)
    int loop_on, loop_have, in_manual;
    double loop_led_t;             // dernier rapport où Serato allumait un voyant de boucle
    uint32_t loop_in, loop_out, pos_prev;
    uint32_t jump_o, jump_to; double jump_t;   // dernier retour en arrière avant le voyant de boucle (OUT)
    uint32_t fin_cand;                 // boucle courte : nouvelle fin mesurée une fois, à confirmer
    // touches de la platine pendant une boucle (rapport 00, octet 2) : 0x20 réduit de moitié
    // (relevé le 16/11 : 4 appuis, 4 réductions), 0x08 double (supposé : l'autre touche
    // CUE/LOOP CALL, jamais vue pendant une boucle). Comptées ici, appliquées dans live_report.
    uint8_t btn2_prev; int loop_btn;
    uint8_t btn0_prev; int in_press; double grow_t; uint32_t lb_in, lb_out;
    // OUT et RELOOP/EXIT (rapport 00, octet 1). IN ou OUT pendant une boucle active : réglage du
    // point ; Serato retire alors 0x08 et fait clignoter le voyant réglé (30/20 ou 30/10), la
    // boucle continue (session du 18/11 au soir). On garde donc la boucle (adjusting).
    int out_press, exit_press, adjusting, adj_out, inside; double adj_press_t, lp_t, reloop_t, inside_t;
    // longueurs calées sur les temps (BPM du morceau, analyse) ; boucle lancée depuis Serato :
    // longueur supposée (celle de la dernière boucle de ce genre) jusqu'au premier retour
    double bpm; int guess; uint32_t auto_len;
    // les touches de la platine priment (essai V27) : dernier appui IN / OUT / RELOOP, premier
    // retour après OUT (il donne les vrais points de Serato, calés sur la grille si QUANTIZE)
    double btn_t; int out_fix, quant;
    // réglage d'un point à la molette (rapport 00, octets 14-15 : compteur) : le point suit les
    // crans (environ 44 par milliseconde chez Serato, relevé sur 9 réglages), recalé à chaque tour
    int jog_init; uint16_t jog_last; int32_t jog_acc; uint32_t adj_base; double jog_send_t;
    uint32_t *grid_ms, grid_n;
    int grow_on;                       // le principal fait suivre la fin à la tête de lecture
    int adj_sent;                      // état de réglage envoyé (30 16) : 0, 1 IN, 2 OUT
    uint32_t cand_in, cand_out;    // retour en arrière qui contredit la boucle connue : à confirmer
    // clignotement de LOOP IN / OUT pendant une boucle active (voir hd_tweak, blinker)
    pthread_mutex_t rep_mu;
    uint8_t rep[64]; size_t rep_len;   // dernier rapport 01 de Serato
    uint32_t sdur_ms; double sdur_t;   // durée du morceau dans le dernier rapport 01 (sous rep_mu)
    uint32_t spos_ms;                  // position dans ce même rapport
    uint32_t in_press_pos;             // position estimée au moment de l'appui sur IN (0 : inconnue)
    // hot cue de la platine (rapport 00, octet 5 : 0x80 A, 0x40 B, 0x20 C ; séance du 25/11,
    // 447 s) : le saut qui suit n'est pas un retour de boucle
    uint8_t btn5_prev; double hc_t;
    uint64_t track_h;                  // empreinte du fichier chargé (0 : inconnu ; voir loader)
    // boucle agrandie par la règle « fin dépassée » sans retour depuis (grown ; fin d'avant :
    // grow_base) ; lecture sortie de la boucle que Serato dit encore active (left) : plus
    // d'agrandissement jusqu'au prochain retour au début
    int grown, left; uint32_t grow_base;
    int dbl;                           // boucle reprise au chargement, à confirmer au 1er rapport
    double rep_t, sent_t;              // reçu de Serato / dernier envoi à la platine
    double blink_t0, adj_t;            // début de la boucle / dernier état « réglage »
    int blink_th;
    // voyants calculés d'après notre suivi de boucle (essai V28 : Serato les désynchronise, voir
    // led_byte) : 0 repos, 1 IN posé, 2 boucle active, 3 réglage de IN, 4 réglage de OUT
    volatile int led_mode, led_live; volatile double led_t0;
};

static hd_deck_t decks[MAX_DECKS];
static int deck_n;
static pthread_mutex_t decks_mu = PTHREAD_MUTEX_INITIALIZER;
static hd_send_fn sink;               // envoi réel d'un paquet de 64 octets
static void (*deck_new_hook)(void *dev);   // nouvelle platine (bibliothèque : garde sa référence)
static hd_find_fn finder;             // fichier audio d'un titre

void hd_set_io(hd_send_fn s, hd_find_fn f) { sink = s; finder = f; }

hd_deck_t *hd_deck_for(void *dev) {
    for (int i = 0; i < deck_n; i++)
        if (decks[i].dev == dev) return &decks[i];
    pthread_mutex_lock(&decks_mu);
    for (int i = 0; i < deck_n; i++)
        if (decks[i].dev == dev) { pthread_mutex_unlock(&decks_mu); return &decks[i]; }
    if (deck_n == MAX_DECKS) { pthread_mutex_unlock(&decks_mu); return NULL; }
    hd_deck_t *k = &decks[deck_n];
    memset(k, 0, sizeof *k);
    k->num = deck_n + 1;
    k->dev = dev;
    pthread_mutex_init(&k->send_mu, NULL);
    pthread_mutex_init(&k->live_mu, NULL);
    pthread_mutex_init(&k->rep_mu, NULL);
    if (deck_new_hook) deck_new_hook(dev);
    deck_n++;
    pthread_mutex_unlock(&decks_mu);
    hd_log("CDJ %d détectée", k->num);
    return k;
}

int hd_deck_num(const hd_deck_t *k) { return k->num; }

static int deck_enabled(const hd_deck_t *k) {
    if (passive) return 0;
    const char *d = getenv("HD_DECKS");
    if (!d) return 1;
    for (const char *p = d; *p; p++)
        if (*p - '0' == k->num && (p == d || p[-1] == ',') && (!p[1] || p[1] == ',')) return 1;
    return 0;
}

// Envoi d'un paquet à la platine k (les nôtres comme ceux de Serato).
int hd_pass(hd_deck_t *k, const uint8_t *r) {
    pthread_mutex_lock(&k->send_mu);
    int ret = sink(k->dev, r);
    pthread_mutex_unlock(&k->send_mu);
    return ret;
}

void hd_lock_send(hd_deck_t *k) { pthread_mutex_lock(&k->send_mu); }
void hd_unlock_send(hd_deck_t *k) { pthread_mutex_unlock(&k->send_mu); }
// Rapport de Serato (position, voyants…) : prioritaire sur nos envois en série. Pendant l'envoi
// d'une waveform (1 500 paquets, un par milliseconde), notre boucle reprenait le verrou aussitôt
// rendu et Serato n'envoyait presque plus rien : position figée 1,7 s après chaque chargement,
// puis rattrapage (session du 20/11, 15 h 21 : 0,97 s → 1,05 s pendant l'envoi).
void hd_lock_send_prio(hd_deck_t *k) {
    __atomic_add_fetch(&k->prio, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&k->send_mu);
    __atomic_sub_fetch(&k->prio, 1, __ATOMIC_SEQ_CST);
}
int hd_pass_prio(hd_deck_t *k, const uint8_t *r) {
    hd_lock_send_prio(k);
    int ret = sink(k->dev, r);
    pthread_mutex_unlock(&k->send_mu);
    return ret;
}

// Rapport reçu de la platine : enregistré s'il a changé.
// Molette → zoom : octets 10-11 = compteur de la molette (16 bits signé) ; octet 6 bit 7 = la
// platine est en vue liste (Serato ne fait défiler sa bibliothèque que dans ce cas). En vue
// morceau la molette est donc libre : chaque cran est envoyé à la platine (30 06 d), dont le
// patch tient le niveau de zoom de la grande waveform. HD_ZOOM=0 désactive, HD_ZOOM=-1 inverse.
static void hd_rec_in(hd_deck_t *k, const uint8_t *r, size_t len);
static void live_send(hd_deck_t *k, int colors);
static double jog_per_ms(void) {
    const char *e = getenv("HD_JOG");
    double v = e ? atof(e) : 44.0;
    return v > 1 ? v : 44.0;
}
void hd_on_input(hd_deck_t *k, uint8_t *r, size_t len) {
    if (len > 64) len = 64;
    // Depuis la version 14 la molette est traitée dans le firmware (zoom et masquage à Serato) :
    // cette partie ne sert plus que si HD_ZOOM_MAC est défini (firmware plus ancien).
    static int mac_zoom = -1;
    if (mac_zoom < 0) mac_zoom = getenv("HD_ZOOM_MAC") != NULL;
    if (len >= 12 && mac_zoom) {
        int cnt = (int16_t)(r[10] | r[11] << 8);
        int list = r[6] & 0x80;
        // Après un chargement fait depuis la liste, l'écran passe à la waveform mais la platine
        // se dit toujours en vue liste, jusqu'à la touche BROWSE suivante (essai V12) : dans cet
        // état la molette ne doit que zoomer. Serato ne voit donc pas ces crans : le compteur
        // qu'il reçoit (rot_virt) n'avance pas.
        if (k->rot_init && list != k->list_last) k->wave_over_list = 0;
        k->list_last = list;
        if (!k->rot_init) k->rot_virt = cnt;
        if (k->rot_init) {
            int d = cnt - k->rot_last;
            if (d > 100 || d < -100) d = 0;                 // compteur remis à zéro
            int hide = k->wave_over_list && list && !passive && deck_enabled(k)
                       && !(getenv("HD_ZOOM") && !atoi(getenv("HD_ZOOM")));
            if (!hide) k->rot_virt += cnt - k->rot_last;
            static int sens = 2;
            if (sens == 2) sens = getenv("HD_ZOOM") ? atoi(getenv("HD_ZOOM")) : 1;
            if (d && sens && !passive && deck_enabled(k)) {
                uint8_t z[64] = {0x30, 0x06, (uint8_t)(int8_t)(d * sens)};
                hd_pass(k, z);
                hd_log("CDJ %d ZOOM %+d%s", k->num, d * sens, hide ? " (liste masquée à Serato)" : list ? " (liste)" : "");
            }
        }
        k->rot_last = cnt;
        k->rot_init = 1;
    }
    if (len >= 3) {
        // octet 1 : 0x80 LOOP IN, 0x40 LOOP OUT, 0x20 RELOOP/EXIT (octet 0 : 0x80 PLAY, 0x40 CUE ;
        // relevé 17/11). L'appui sur IN donne le début : après une sortie de boucle, le voyant IN de
        // Serato reste allumé, son front ne vient pas.
        uint8_t on0 = r[1] & ~k->btn0_prev;
        k->btn0_prev = r[1];
        if (on0 & 0xe0) {
            pthread_mutex_lock(&k->live_mu);
            if (on0 & 0x80) {
                k->in_press = 1;
                // position à l'instant de l'appui : dernier rapport de Serato + temps écoulé depuis
                // (vitesse normale) ; Serato arrondit son IN depuis cet instant, pas depuis le
                // rapport suivant, déjà jusqu'à 25 ms plus loin
                pthread_mutex_lock(&k->rep_mu);
                double el = hd_now() - k->sdur_t;
                k->in_press_pos = k->sdur_t > 0 && el >= 0 && el < 0.1 ? k->spos_ms + (uint32_t)lrint(el * 1000) : 0;
                pthread_mutex_unlock(&k->rep_mu);
            }
            if (on0 & 0x40) k->out_press = 1;
            if (on0 & 0x20) k->exit_press = 1;
            pthread_mutex_unlock(&k->live_mu);
        }
    }
    if (len >= 16 && r[0] == 0) {
        uint16_t c = (uint16_t)(r[14] | r[15] << 8);
        int16_t d = k->jog_init ? (int16_t)(c - k->jog_last) : 0;
        k->jog_init = 1; k->jog_last = c;
        if (d) {
            pthread_mutex_lock(&k->live_mu);
            if (k->adjusting && k->loop_on && k->loop_have) {
                k->jog_acc += d;
                int32_t ms = (int32_t)lrint(k->jog_acc / jog_per_ms());
                int64_t v = (int64_t)k->adj_base + ms;
                if (k->adj_out) {
                    if (v < (int64_t)k->loop_in + 10) v = k->loop_in + 10;
                    k->loop_out = (uint32_t)v;
                } else {
                    if (v > (int64_t)k->loop_out - 10) v = (int64_t)k->loop_out - 10;
                    if (v < 0) v = 0;
                    k->loop_in = (uint32_t)v;
                }
                double now = hd_now();
                if (now - k->jog_send_t > 0.04) { k->jog_send_t = now; live_send(k, 0); }
            }
            pthread_mutex_unlock(&k->live_mu);
        }
    }
    if (len >= 6 && r[0] == 0) {
        uint8_t on5 = r[5] & ~k->btn5_prev;
        k->btn5_prev = r[5];
        if (on5 & 0xe0) {
            pthread_mutex_lock(&k->live_mu);
            k->hc_t = hd_now();
            pthread_mutex_unlock(&k->live_mu);
        }
    }
    if (len >= 3 && r[0] == 0) {
        uint8_t on = r[2] & ~k->btn2_prev;
        k->btn2_prev = r[2];
        if (on & 0x60) {
            pthread_mutex_lock(&k->live_mu);
            if (on & 0x20) k->loop_btn--;
            if (on & 0x40) k->loop_btn++;
            pthread_mutex_unlock(&k->live_mu);
        }
    }
    hd_rec_in(k, r, len);
    if (len >= 12 && mac_zoom) { r[10] = (uint8_t)k->rot_virt; r[11] = (uint8_t)(k->rot_virt >> 8); }
}

static void hd_rec_in(hd_deck_t *k, const uint8_t *r, size_t len) {
    if (!hrec) return;
    if (len == k->last_in_len && !memcmp(r, k->last_in, len)) return;
    memcpy(k->last_in, r, len);
    k->last_in_len = len;
    hd_rec("CDJ>HOST", k->num, r, len);
}

// MARK: - chargement

typedef struct { hd_deck_t *deck; char title[512]; int gen; double t_title; uint32_t old_dur; } load_req_t;
typedef struct { hd_deck_t *deck; int gen; uint32_t sent; } up_ctx_t;

static int up_send(void *ctx, const uint8_t pkt[64]) {
    up_ctx_t *c = ctx;
    if (c->gen != c->deck->generation) return -1;   // un autre morceau arrive : on abandonne
    // un rapport de Serato attend le verrou : il passe d'abord (5 ms au plus)
    for (int i = 0; i < 25 && __atomic_load_n(&c->deck->prio, __ATOMIC_SEQ_CST); i++) usleep(200);
    if (hd_pass(c->deck, pkt)) return -1;           // verrou par paquet : Serato n'attend pas
    c->sent++;
    return 0;
}

// MARK: - analyses d'avance (V36)
// Serato ouvre le fichier audio quelques secondes avant de le charger sur la platine (session du
// 20/11 à 18 h 12 : « FICHIER ouvert » 5 s avant « CHARGEMENT »). Il est analysé aussitôt, à
// priorité basse, et gardé : au chargement, l'envoi commence sans les 0,3 à 0,4 s d'analyse.
#include <sys/stat.h>
#define PRE_N 8                      // analyses gardées
#define PRE_Q 4                      // fichiers en attente (le plus récent d'abord)
typedef struct { char path[1024]; time_t mtime; off_t size; hd_track_t *t; double used; } pre_t;
static pre_t pre[PRE_N];
static char pre_q[PRE_Q][1024];
static int pre_qn, pre_started;
static char pre_busy[1024];          // fichier en cours d'analyse
static pthread_mutex_t pre_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pre_cv = PTHREAD_COND_INITIALIZER;

static int file_id(const char *p, time_t *mt, off_t *sz) {
    struct stat st;
    if (stat(p, &st)) return 0;
    *mt = st.st_mtime; *sz = st.st_size;
    return 1;
}

static hd_track_t *track_dup(const hd_track_t *s) {
    hd_track_t *t = malloc(sizeof *t);
    if (!t) return NULL;
    *t = *s;
    t->cz = NULL; t->cz_len = 0;
    t->wave = malloc(s->wave_len ? s->wave_len : 1);
    t->grid = malloc(HD_GRID_MAX * 4);
    if (!t->wave || !t->grid) { free(t->wave); free(t->grid); free(t); return NULL; }
    memcpy(t->wave, s->wave, s->wave_len);
    memcpy(t->grid, s->grid, HD_GRID_MAX * 4);
    return t;
}

// sous pre_mu : entrée du fichier, à jour (même date et taille), ou NULL
static pre_t *pre_find(const char *path) {
    time_t mt; off_t sz;
    if (!file_id(path, &mt, &sz)) return NULL;
    for (int i = 0; i < PRE_N; i++)
        if (pre[i].t && !strcmp(pre[i].path, path) && pre[i].mtime == mt && pre[i].size == sz) return &pre[i];
    return NULL;
}

static void pre_put(const char *path, const hd_track_t *t) {
    time_t mt; off_t sz;
    if (!t || !file_id(path, &mt, &sz)) return;
    hd_track_t *c = track_dup(t);
    if (!c) return;
    pthread_mutex_lock(&pre_mu);
    int k = 0;
    for (int i = 0; i < PRE_N; i++) {
        if (pre[i].t && !strcmp(pre[i].path, path)) { k = i; break; }
        if (!pre[i].t || pre[i].used < pre[k].used) k = i;
    }
    hd_free(pre[k].t);
    snprintf(pre[k].path, sizeof pre[k].path, "%s", path);
    pre[k].mtime = mt; pre[k].size = sz; pre[k].t = c; pre[k].used = hd_now();
    pthread_mutex_unlock(&pre_mu);
}

static void *pre_worker(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    hd_quiet = 1;                    // nos lectures ne sont pas des ouvertures de Serato
    char path[1024];
    for (;;) {
        pthread_mutex_lock(&pre_mu);
        while (!pre_qn) pthread_cond_wait(&pre_cv, &pre_mu);
        snprintf(path, sizeof path, "%s", pre_q[--pre_qn]);
        if (pre_find(path)) { pthread_mutex_unlock(&pre_mu); continue; }
        snprintf(pre_busy, sizeof pre_busy, "%s", path);
        pthread_mutex_unlock(&pre_mu);
        hd_track_t *t = hd_analyse(path);
        pre_put(path, t);
        hd_free(t);
        pthread_mutex_lock(&pre_mu);
        pre_busy[0] = 0;
        pthread_cond_broadcast(&pre_cv);
        pthread_mutex_unlock(&pre_mu);
    }
    return NULL;
}

// fichier ouvert par Serato : analysé d'avance (HD_AVANCE=0 : non)
void hd_pre_open(const char *path) {
    const char *e = getenv("HD_AVANCE");
    if (e && e[0] == '0') return;
    pthread_mutex_lock(&pre_mu);
    int dup = !strcmp(pre_busy, path) || pre_find(path) != NULL;
    for (int i = 0; i < pre_qn && !dup; i++) dup = !strcmp(pre_q[i], path);
    if (!dup) {
        if (pre_qn == PRE_Q) { memmove(pre_q[0], pre_q[1], (PRE_Q - 1) * sizeof pre_q[0]); pre_qn--; }
        snprintf(pre_q[pre_qn++], sizeof pre_q[0], "%s", path);
        pthread_cond_signal(&pre_cv);
        if (!pre_started) {
            pthread_t th;
            if (!pthread_create(&th, NULL, pre_worker, NULL)) { pthread_detach(th); pre_started = 1; }
        }
    }
    pthread_mutex_unlock(&pre_mu);
}

// analyse prête (ou en cours : attendue 3 s au plus), copie à libérer ; NULL sinon
static hd_track_t *pre_get(const char *path) {
    hd_track_t *t = NULL;
    pthread_mutex_lock(&pre_mu);
    for (int i = 0; i < 30 && !strcmp(pre_busy, path); i++) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&pre_cv, &pre_mu, &ts);
    }
    pre_t *e = pre_find(path);
    if (e) { e->used = hd_now(); t = track_dup(e->t); }
    pthread_mutex_unlock(&pre_mu);
    return t;
}

// Contrôle avant tout envoi (24/11) : la durée que Serato annonce pour ce chargement doit être
// celle du fichier trouvé, à 0,15 s près (écart mesuré sur 22 chargements de tes séances : −10 à
// +70 ms ; sur les 40 morceaux de la bibliothèque : −5 à +74 ms, silence d'encodage des MP3).
// Serato met la nouvelle durée dans son rapport d'état 9 à 98 ms après le titre (147 chargements) :
// on prend le premier rapport reçu après le titre qui porte une autre durée que le morceau
// précédent, ou, si elle n'a pas changé (même morceau rechargé), un rapport reçu 0,15 s au moins
// après le titre. 1 : conforme ; 0 : fichier écarté ; −1 : nouveau chargement entre-temps.
#define DUR_TOL_MS 150
static int check_duration(hd_deck_t *k, const load_req_t *req, double dur_s) {
    uint32_t mine = (uint32_t)lrint(dur_s * 1000);
    for (int i = 0; i < 300; i++) {                 // 3 s au plus
        if (req->gen != k->generation) return -1;
        pthread_mutex_lock(&k->rep_mu);
        uint32_t sd = k->sdur_ms;
        double st = k->sdur_t;
        pthread_mutex_unlock(&k->rep_mu);
        if (st > req->t_title && (sd != req->old_dur || st - req->t_title >= 0.15)) {
            int32_t d = (int32_t)sd - (int32_t)mine;
            if (d >= -DUR_TOL_MS && d <= DUR_TOL_MS) return 1;
            hd_log("CDJ %d « %s » : durée du fichier %.2f s, Serato %.2f s", k->num, req->title, mine / 1000.0, sd / 1000.0);
            return 0;
        }
        usleep(10 * 1000);
    }
    hd_log("CDJ %d « %s » : pas de durée reçue de Serato", k->num, req->title);
    return 0;
}

// empreinte d'un chemin (FNV-1a) : même fichier chargé sur deux platines
static uint64_t path_hash(const char *p) {
    uint64_t h = 1469598103934665603ull;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return h ? h : 1;
}

static void *loader(void *arg) {
    load_req_t *req = arg;
    hd_deck_t *k = req->deck;
    char path[1024] = "";
    const char *why = "aucun fichier ouvert à ce titre";
    hd_track_t *t = NULL;
    int early = 0;
    if (finder(k, req->title, path, sizeof path)) {
        t = pre_get(path);
        early = t != NULL;
        if (!t) {
            hd_quiet = 1;
            t = hd_analyse(path);
            hd_quiet = 0;
            pre_put(path, t);
        }
        why = t ? NULL : "analyse impossible";
    }
    if (req->gen != k->generation) {   // nouveau chargement pendant l'analyse : il prend le relais
        hd_free(t);
        free(req);
        return NULL;
    }
    if (t) {
        int v = check_duration(k, req, t->duration);
        if (v < 0) { hd_free(t); free(req); return NULL; }   // nouveau chargement entre-temps
        if (v == 0) { hd_free(t); t = NULL; why = "durée différente de celle de Serato : fichier écarté"; }
    }
    if (t) {
        hd_log("CDJ %d ANALYSE « %s » → %s : %.1f s, %u points, grille %u temps (%.1f BPM), recalage %.0f ms, %s %.2f s",
               k->num, req->title, path, t->duration, t->n, t->grid_n, t->bpm, t->shift_ms,
               early ? "faite d'avance en" : "en", t->analyse_s);
        up_ctx_t c = {k, req->gen, 0};
        double ts = hd_now();
        int r = hd_packets(t, up_send, &c);
        double dt = hd_now() - ts;
        if (r == 0) {
            hd_log("CDJ %d ENVOI %u paquets en %.2f s (%.0f/s)", k->num, c.sent, dt, c.sent / fmax(dt, 1e-3));
            if (req->gen == k->generation) k->wave_over_list = k->list_last != 0;
            // Même morceau déjà sur une autre platine (chargement en double) : sa boucle en cours,
            // ou son IN en attente de OUT, est reprise. C'est le seul lien entre les platines :
            // ensuite chacune suit sa boucle seule. (Copie prise avant de verrouiller cette
            // platine : jamais deux verrous de platine tenus à la fois.)
            uint64_t th = path_hash(path);
            int d_on = 0, d_have = 0, d_man = 0, d_from = 0; uint32_t d_in = 0, d_out = 0;
            for (int i = 0; i < deck_n && !d_from; i++) {
                hd_deck_t *j = &decks[i];
                if (j == k || j->track_h != th) continue;
                pthread_mutex_lock(&j->live_mu);
                if (j->live_ok && j->live_gen == j->generation && j->track_h == th
                    && ((j->loop_on && j->loop_have) || j->in_manual)) {
                    d_on = j->loop_on; d_have = j->loop_have; d_man = j->in_manual;
                    d_in = j->loop_in; d_out = j->loop_out; d_from = j->num;
                }
                pthread_mutex_unlock(&j->live_mu);
            }
            pthread_mutex_lock(&k->live_mu);
            k->cue_n = t->cue_n > HD_CUE_MAX ? HD_CUE_MAX : t->cue_n;
            k->bpm = t->bpm; k->guess = 0; k->grow_on = 0; k->adj_sent = 0; k->out_fix = 0;
            free(k->grid_ms); k->grid_ms = NULL; k->grid_n = 0;
            if (t->grid_n && (k->grid_ms = malloc(t->grid_n * sizeof(uint32_t)))) {
                for (uint32_t i = 0; i < t->grid_n; i++) {
                    const uint8_t *g = t->grid + 4 * i;
                    k->grid_ms[i] = (uint32_t)g[1] << 16 | (uint32_t)g[3] << 8 | g[2];
                }
                k->grid_n = t->grid_n;
            }
            if (!k->auto_len && t->bpm > 40) k->auto_len = (uint32_t)(4 * 60000.0 / t->bpm + 0.5);
            memcpy(k->cue_idx, t->cue_idx, sizeof k->cue_idx);
            memcpy(k->cue_rgb, t->cue_rgb, sizeof k->cue_rgb);
            memcpy(k->cue_ms, t->cue_ms, sizeof k->cue_ms);
            k->loop_on = k->loop_have = k->in_manual = 0; k->loop_btn = 0; k->cand_in = k->cand_out = 0; k->in_press = 0; k->lb_in = k->lb_out = 0; k->out_press = k->exit_press = k->adjusting = 0;
            k->grown = k->left = 0; k->track_h = th; k->dbl = 0;
            if (d_from) {
                k->loop_on = d_on; k->loop_have = d_have; k->in_manual = d_man;
                k->loop_in = d_in; k->loop_out = d_out; k->dbl = d_from;
            }
            k->led_init = 0; k->live_gen = req->gen; k->live_ok = 1; k->led_mode = 0;
            pthread_mutex_unlock(&k->live_mu);
        } else {
            why = "envoi interrompu";
            hd_log("CDJ %d envoi interrompu après %u paquets", k->num, c.sent);
            if (req->gen == k->generation) {       // erreur d'envoi : la platine oublie le début reçu
                uint8_t clr[64];
                hd_clear_packet(clr);
                hd_pass(k, clr);
            }
        }
    }
    if (why) hd_log("CDJ %d « %s » sans waveform couleur (%s)", k->num, req->title, why);
    hd_free(t);
    free(req);
    hd_rec_flush();
    return NULL;
}

// Titre : paquets 02 xx (UTF-16 LE), en 1 à 4 morceaux (quartet haut de l'octet 2 = index,
// quartet bas = nombre).
static void on_title(hd_deck_t *k, const uint8_t *r) {
    int idx = r[2] >> 4, cnt = r[2] & 0xF;
    if (idx == 1) {
        // nouveau titre : tout envoi en cours est abandonné et la platine oublie la waveform
        // précédente (sans effet sur un firmware d'origine)
        k->title_len = 0;
        k->generation++;
        uint8_t clr[64];
        hd_clear_packet(clr);
        hd_pass(k, clr);
    }
    for (int i = 4; i + 1 < 64; i += 2) {
        uint16_t u = r[i] | (r[i + 1] << 8);
        if (!u) break;
        if (k->title_len + 1 < sizeof k->title_buf) k->title_buf[k->title_len++] = u < 128 ? (char)u : '?';
    }
    k->title_buf[k->title_len] = 0;
    if (idx != cnt) return;
    int blank = 1;
    for (size_t i = 0; i < k->title_len; i++) if (k->title_buf[i] != ' ') blank = 0;
    if (blank) return;                // paquets d'initialisation : pas un chargement
    load_req_t *req = calloc(1, sizeof *req);
    req->deck = k;
    snprintf(req->title, sizeof req->title, "%s", k->title_buf);
    req->gen = k->generation;
    req->t_title = hd_now();
    pthread_mutex_lock(&k->rep_mu);
    req->old_dur = k->sdur_ms;
    pthread_mutex_unlock(&k->rep_mu);
    hd_log("CDJ %d CHARGEMENT « %s »", k->num, req->title);
    pthread_t th;
    pthread_create(&th, NULL, loader, req);
    pthread_detach(th);
}

// MARK: - repères et boucle en direct
//
// Serato n'écrit les repères dans le fichier qu'à l'éjection du morceau, et n'envoie jamais leurs
// positions à la platine. Mais son rapport d'état (01, ~40 par seconde) porte les voyants :
//   octet 7 : hot cues, 2 bits par bouton (bits 7-6 : A, 5-4 : B, 3-2 : C), non nul = posé ;
//   octet 4 : 0x20 LOOP IN, 0x10 LOOP OUT, 0x08 boucle active ;
//   octets 12-14 : position (minutes, secondes, centièmes) ; 15-17 : durée.
// (session du 16/11 : hot cue posé à 16,89 s → 41 devient 51 ; boucle 75,87–76,90 s → e4 puis fc,
// la position revient de 76,88 à 75,88 s ; fc dès le départ pour une boucle automatique.)
// La position au moment où un voyant change donne donc le repère ou la borne. La liste est
// renvoyée à la platine (30 10, 30 11) suivie de 30 13, qui la fait relire par l'écran.
// Longueur mesurée (à un intervalle de rapport près, 25 ms environ) calée sur un nombre de temps
// puissance de 2 (1/32 à 64) quand elle en est à moins de 40 ms : boucles de Serato, CALL ×2 / ÷2.
static uint32_t snap_len(const hd_deck_t *k, uint32_t len) {
    if (k->bpm < 40 || len < 10) return len;
    double beat = 60000.0 / k->bpm, n = pow(2, round(log2(len / beat)));
    if (n < 1.0 / 32 || n > 64 || fabs(len - n * beat) > 40) return len;
    return (uint32_t)(n * beat + 0.5);
}
// Résolution du QUANTIZE de Serato, en temps (réglage « quantize_value » de ses préférences :
// 0.25 = quart de temps, réglage de l'utilisateur au 24/11) ; HD_QUANTIZE la remplace. Relue au
// plus une fois par seconde (Serato tourne dans le même processus : sa valeur en mémoire).
static double quant_beats(void) {
    static double q = 1, t = -10;
    const char *e = getenv("HD_QUANTIZE");
    if (e) return atof(e) > 0 ? atof(e) : 1;
    double now = hd_now();
    if (now - t < 1) return q;
    t = now;
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("quantize_value"), CFSTR("com.serato.seratodj"));
    if (v) {
        double x = 0;
        if (CFGetTypeID(v) == CFStringGetTypeID()) x = CFStringGetDoubleValue((CFStringRef)v);
        else if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &x);
        CFRelease(v);
        if (x >= 0.03 && x <= 16) q = x;
    }
    return q;
}
// IN calé comme Serato quand QUANTIZE est actif : sur la subdivision de la grille la plus proche
// (quart de temps pour 0.25). Jusqu'au 24/11 : sur le temps entier le plus proche à moins de
// 150 ms, ce qui pouvait placer l'IN 140 ms après celui de Serato (28,40 → 28,54 s).
static uint32_t snap_beat(const hd_deck_t *k, uint32_t ms) {
    if (!k->quant || k->grid_n < 2) return ms;
    uint32_t lo = 0, hi = k->grid_n - 1;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (k->grid_ms[m] <= ms) lo = m + 1; else hi = m; }
    uint32_t i = lo ? lo - 1 : 0;                   // temps qui précède ms (ou le premier)
    if (i + 1 >= k->grid_n) i = k->grid_n - 2;
    double a = k->grid_ms[i], beat = (double)k->grid_ms[i + 1] - a;
    if (beat <= 0) return ms;
    double q = quant_beats(), step = beat * (q < 1 ? q : 1);
    double x = a + floor(((double)ms - a) / step + 0.5) * step;
    return x < 0 ? ms : (uint32_t)lrint(x);
}
// 30 15 g lead : pendant que la fin de boucle n'est pas connue (IN posé, en attente de OUT), le
// principal place lui-même la fin à la position affichée, plus « lead » routines d'avance (le
// temps que l'écran redessine) : plus d'espace entre le masque et la tête de lecture (essai V26).
static void grow_set(hd_deck_t *k, int on) {
    if (k->grow_on == on) return;
    k->grow_on = on;
    const char *e = getenv("HD_GROW_LEAD");
    uint8_t p[64] = {0x30, 0x15, (uint8_t)on, (uint8_t)(e ? atoi(e) : 5)};
    hd_pass(k, p);
}
// 30 16 m : réglage d'un point de la boucle à la molette en cours (1 : IN, 2 : OUT, 0 : fini).
// L'écran fige alors la grande waveform sur ce point (le masque grandit ou rétrécit sous la tête
// de lecture, comme en mode USB) ; il le lit dans la marque de la petite waveform (V31).
static void adj_set(hd_deck_t *k, int m) {
    if (k->adj_sent == m) return;
    k->adj_sent = m;
    uint8_t p[64] = {0x30, 0x16, (uint8_t)m};
    hd_pass(k, p);
    hd_log("CDJ %d réglage %s", k->num, m == 1 ? "IN" : m == 2 ? "OUT" : "fini");
}
static void live_send(hd_deck_t *k, int colors) {
    // liste rangée par position : boucle (genres 3 et 17) et hot cues (genre 0)
    struct { uint32_t ms; uint16_t kind; uint8_t rgb[3]; } e[HD_CUE_MAX + 2];
    uint32_t n = 0;
    int loop = (k->loop_on || k->in_manual) && (k->loop_out > k->loop_in || k->grow_on);   // premier tour : début → lecture
    if (loop) {
        e[n].ms = k->loop_in; e[n].kind = HD_KIND_LOOP_IN; n++;
        e[n].ms = k->loop_out > k->loop_in ? k->loop_out : k->loop_in + 10; e[n].kind = HD_KIND_LOOP_OUT; n++;
    }
    for (uint32_t i = 0; i < k->cue_n && n < HD_CUE_MAX; i++) {
        e[n].ms = k->cue_ms[i]; e[n].kind = HD_KIND_MEMORY; memcpy(e[n].rgb, k->cue_rgb[i], 3); n++;
    }
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && e[j - 1].ms > e[j].ms; j--) {
            __typeof__(e[0]) x = e[j]; e[j] = e[j - 1]; e[j - 1] = x;
        }
    uint8_t p[64];
    memset(p, 0, 64); p[0] = 0x30; p[1] = 0x10; p[2] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) { memcpy(p + 3 + 6 * i, &e[i].kind, 2); memcpy(p + 5 + 6 * i, &e[i].ms, 4); }
    hd_pass(k, p);
    memset(p, 0, 64); p[0] = 0x30; p[1] = 0x11;
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) if (e[i].kind == HD_KIND_MEMORY) { memcpy(p + 3 + 3 * c, e[i].rgb, 3); c++; }
    p[2] = (uint8_t)c;
    hd_pass(k, p);
    memset(p, 0, 64); p[0] = 0x30; p[1] = 0x13; p[2] = colors ? 0x88 : 0x80;
    hd_pass(k, p);
}

static void led_update(hd_deck_t *k, uint8_t lp, double now);
static void live_report(hd_deck_t *k, const uint8_t *r) {
    static const uint8_t DEF[3][3] = {{0xCC, 0x00, 0x00}, {0xCC, 0x88, 0x00}, {0x00, 0x00, 0xCC}};
    const char *off = getenv("HD_LIVE");
    if (off && *off == '0') return;
    if (!k->live_ok || k->live_gen != k->generation) return;
    uint32_t t = ((uint32_t)r[12] * 60 + r[13]) * 1000 + (uint32_t)r[14] * 10;
    uint32_t dur = ((uint32_t)r[15] * 60 + r[16]) * 1000 + (uint32_t)r[17] * 10;
    // La position de Serato est toujours le temps écoulé, même TIME MODE sur REMAIN (bit 0x02 de
    // l'octet 4) : la retrancher de la durée plaçait repères et boucles depuis la fin (essai V20).
    uint32_t pos = t; (void)dur;
    uint8_t led = r[7], lp = r[4] & 0x38;
    k->quant = r[8] & 0x04;
    int cues = 0, loop = 0;
    double now = hd_now();
    pthread_mutex_lock(&k->live_mu);
    if (!k->led_init) {
        k->led_init = 1; k->led_prev = led; k->loop_prev = lp; k->pos_prev = pos;
        memset(k->gone_t, 0, sizeof k->gone_t);
        // boucle reprise au chargement : gardée si Serato l'a reprise aussi (voyant de boucle ;
        // voyant IN seul pour un IN en attente de OUT), oubliée sinon
        if (k->dbl) {
            int from = k->dbl; k->dbl = 0;
            int keep = k->in_manual ? (lp & 0x20) && !(lp & 0x08) : (lp & 0x08) != 0;
            if (!keep) {
                k->loop_on = k->loop_have = k->in_manual = 0;
                hd_log("CDJ %d boucle de CDJ %d non reprise par Serato", k->num, from);
            } else {
                k->inside = pos >= k->loop_in && pos <= k->loop_out; k->inside_t = now; k->grow_t = now;
                if (k->in_manual) grow_set(k, 1);
                hd_log("CDJ %d %s %.2f–%.2f s reprise de CDJ %d (même morceau)", k->num,
                       k->in_manual ? "IN en attente" : "boucle", k->loop_in / 1000.0, k->loop_out / 1000.0, from);
                live_send(k, 0);
            }
        }
        pthread_mutex_unlock(&k->live_mu);
        return;
    }
    for (int s = 0; s < 3; s++) {
        int f = (led >> (6 - 2 * s)) & 3, pf = (k->led_prev >> (6 - 2 * s)) & 3;
        if (pf && !f) k->gone_t[s] = now;                 // éteint : supprimé s'il le reste 0,3 s
        if (f && k->gone_t[s] != 0) k->gone_t[s] = 0;     // rallumé aussitôt : clignotement
        else if (f && !pf) {                              // posé : à la position de lecture
            uint32_t i;
            for (i = 0; i < k->cue_n && k->cue_idx[i] != s; i++) ;
            if (i == k->cue_n && k->cue_n < HD_CUE_MAX) k->cue_n++;
            if (i < k->cue_n) {
                k->cue_idx[i] = (uint8_t)s; k->cue_ms[i] = pos; memcpy(k->cue_rgb[i], DEF[s], 3);
                cues = 1;
                hd_log("CDJ %d hot cue %c posé à %.2f s", k->num, 'A' + s, pos / 1000.0);
            }
        }
        if (!f && k->gone_t[s] != 0 && now - k->gone_t[s] > 0.3) {
            k->gone_t[s] = 0;
            for (uint32_t i = 0; i < k->cue_n; i++)
                if (k->cue_idx[i] == s) {
                    for (uint32_t j = i; j + 1 < k->cue_n; j++) {
                        k->cue_idx[j] = k->cue_idx[j + 1]; k->cue_ms[j] = k->cue_ms[j + 1];
                        memcpy(k->cue_rgb[j], k->cue_rgb[j + 1], 3);
                    }
                    k->cue_n--; cues = 1;
                    hd_log("CDJ %d hot cue %c supprimé", k->num, 'A' + s);
                    break;
                }
        }
    }
    // boucle
    if (lp != k->loop_prev) k->lp_t = now;
    // IN ou OUT pendant une boucle (active ou en réglage) : réglage du point, la boucle continue.
    // RELOOP/EXIT : sortie (ou retour de la dernière boucle) ; pendant un réglage, la boucle n'est
    // finie que si Serato cesse de faire clignoter (état tenu 0,7 s) ou éteint tout.
    if (k->exit_press) k->reloop_t = now;
    if (k->in_press || k->out_press || k->exit_press) k->btn_t = now;
    int reloop = now - k->reloop_t < 0.4;
    if ((k->in_press || k->out_press) && k->loop_on) {
        // point réglé : celui de la touche (IN pendant le réglage de OUT : Serato passe au réglage
        // de IN, session du 18/11 à 22 h 25, 293 s ; la même touche termine le réglage)
        int o = k->out_press ? 1 : 0;
        if (k->adjusting && o == k->adj_out && now - k->adj_press_t > 0.15) {
            // même touche : réglage fini tout de suite (V31 ; avant, 0,3 s après le retour du voyant
            // de boucle de Serato) : la waveform reprend la position de lecture
            k->adjusting = 0;
        } else {
            k->adj_out = o;
            k->adjusting = 1; k->adj_press_t = now;
            k->adj_base = k->adj_out ? k->loop_out : k->loop_in; k->jog_acc = 0;
        }
    } else if (k->in_press && !(lp & 0x08)) {
        uint32_t at = k->in_press_pos && k->in_press_pos + 60 >= pos && k->in_press_pos <= pos + 60 ? k->in_press_pos : pos;
        k->loop_in = k->loop_out = snap_beat(k, at); k->in_manual = 1; k->grow_t = now;
        hd_log("CDJ %d IN posé à %.2f s", k->num, k->loop_in / 1000.0);
        grow_set(k, 1); live_send(k, 0);
    }
    k->in_press = k->out_press = k->exit_press = 0;
    if ((lp & 0x08) && k->adjusting && now - k->adj_press_t > 0.3) k->adjusting = 0;   // réglage fini
    // (l'état 00 seul ne suffit pas : Serato désynchronisé fait alterner 10 / 00 en réglage de OUT,
    // session du 18/11, 22 h 25, 182 s)
    if (k->adjusting && !(lp & 0x08) && now - k->lp_t > 0.7) {
        k->adjusting = 0;
        if (k->loop_on && k->loop_have) { k->lb_in = k->loop_in; k->lb_out = k->loop_out; }
        k->loop_on = 0; k->loop_have = 0; k->in_manual = 0; loop = 1;
    }
    int act = (lp & 0x08) || (k->adjusting && k->loop_on);
    // lecture continue depuis la position précédente (ni saut, ni needle search) : seule
    // condition pour agrandir le masque (essai V25 : boucle « 35,77–169,15 s » après un needle search)
    int cont = pos + 30 >= k->pos_prev && pos <= k->pos_prev + 150;
    // un retour au début depuis la fin connue n'interrompt pas la lecture continue de la boucle
    if (!cont && k->loop_have && (pos > k->loop_in ? pos - k->loop_in : k->loop_in - pos) <= 40
        && k->pos_prev + 250 >= k->loop_out && k->pos_prev <= k->loop_out + 250) cont = 1;
    // lecture continue dans la boucle depuis au moins 0,2 s (un needle search qui s'arrête un
    // instant dans la boucle ne compte pas : essai V26, fin fausse de 24,47 s)
    int was_inside = k->inside && now - k->inside_t > 0.2;
    // saut vers un hot cue : touche de la platine, ou arrivée sur un repère connu hors de la boucle
    // (table de mixage, Serato). Ni retour ni changement de boucle : Serato garde la boucle active
    // et y revient si la lecture l'atteint. (Séance du 25/11, 447-500 s : appuis répétés sur A
    // et B pris pour une boucle de 0,1 s au repère, puis agrandie à chaque fin dépassée.)
    int hcj = 0;
    if (pos + 30 < k->pos_prev || pos > k->pos_prev + 150) {
        if (now - k->hc_t < 0.3) hcj = 1;
        else if (k->loop_have && (pos + 40 < k->loop_in || pos > k->loop_out + 40))
            for (uint32_t i = 0; i < k->cue_n; i++)
                if ((pos > k->cue_ms[i] ? pos - k->cue_ms[i] : k->cue_ms[i] - pos) <= 40) { hcj = 1; break; }
    }
    if (hcj) { k->cand_in = k->cand_out = 0; k->fin_cand = 0; }
    if (!cont) k->inside = 0;
    else if (!k->inside && pos >= k->loop_in && pos <= k->loop_out) { k->inside = 1; k->inside_t = now; }
    // OUT : Serato revient au début de la boucle, parfois un rapport avant d'allumer le voyant de
    // boucle (séance du 24/11, platine 2, 691,5 s) ; ce retour est noté pour le front du voyant
    if (k->in_manual && !(lp & 0x08) && !hcj && pos + 30 < k->pos_prev) { k->jump_o = k->pos_prev; k->jump_to = pos; k->jump_t = now; }
    // IN posé sans OUT : abandon si la lecture repart nettement avant le IN (CUE, hot cue…) ou s'en
    // éloigne de plus de 70 s. Marge de 0,3 s (30 ms jusqu'au 24/11) : le début de boucle de Serato
    // peut précéder notre IN (QUANTIZE : 30,23 s pour un IN noté à 30,30 s ; IN calé sur un temps
    // jusqu'à 150 ms plus loin), et le retour au début à l'appui sur OUT annulait alors l'IN : la
    // boucle n'était reconnue qu'au premier retour, un tour plus tard.
    // (pos > loop_in d'abord : la soustraction non signée débordait dès que la lecture revenait
    // avant le IN, et l'IN était abandonné comme s'il était 70 s plus loin)
    if (k->in_manual && !k->loop_on && !(lp & 0x08) && (pos + 300 < k->loop_in || (pos > k->loop_in && pos - k->loop_in > 70000))) {
        k->in_manual = 0; k->loop_out = k->loop_in; grow_set(k, 0); live_send(k, 0);
    }
    // CALL ×2 / ÷2 de la platine : avant l'examen des retours (Serato recule déjà d'une nouvelle
    // longueur dans le rapport qui suit l'appui : ne pas compter la réduction deux fois)
    if (k->loop_btn) {
        if (act && k->loop_on && k->loop_have && k->loop_out > k->loop_in) {
            uint32_t len = snap_len(k, k->loop_out - k->loop_in);
            while (k->loop_btn < 0 && len >= 20) { len /= 2; k->loop_btn++; }
            while (k->loop_btn > 0 && len < 600000) { len *= 2; k->loop_btn--; }
            k->loop_out = k->loop_in + snap_len(k, len); loop = 1;
            k->grown = 0;
        }
        k->loop_btn = 0;
    }
    if ((lp & 0x08) && !(k->loop_prev & 0x08) && k->loop_on) {
        // fin d'un réglage : la boucle continue
    } else if ((lp & 0x08) && !(k->loop_prev & 0x08)) {
        k->loop_on = 1; k->guess = 0;
        // OUT : Serato revient au début dans le même rapport, la fin est la position d'avant ; ou
        // dans le rapport précédent (retour noté plus haut)
        uint32_t o = k->pos_prev > pos ? k->pos_prev : pos, land = pos;
        if (k->in_manual && now - k->jump_t < 0.2 && k->jump_o > o) { o = k->jump_o; land = k->jump_to; }
        uint32_t di = pos > k->lb_in ? pos - k->lb_in : k->lb_in - pos;
        (void)di;
        if (k->in_manual && o > k->loop_in + 20) {
            // début posé par Serato avant notre IN (QUANTIZE) : celui où la lecture est revenue
            if (land + 20 < k->loop_in && k->loop_in - land < 400) k->loop_in = land;
            k->loop_out = k->loop_in + snap_len(k, o - k->loop_in); k->loop_have = 1; loop = 1;
            k->out_fix = 1;            // le premier retour donnera les points exacts de Serato
        } else if (k->lb_out > k->lb_in && (reloop || di <= 60)) {
            // RELOOP (touche de la platine) : la dernière boucle revient, où que soit la lecture
            // (Serato saute au début dans le rapport suivant ; essai V27 : masque au mauvais endroit)
            k->loop_in = k->lb_in; k->loop_out = k->lb_out; k->loop_have = 1; loop = 1;
        } else if (now - k->btn_t < 0.6) {
            // boucle posée depuis la platine sans IN vu : le premier retour la donne
            k->loop_in = pos; k->loop_out = pos; k->loop_have = 0; k->grow_t = now;
        } else if (k->auto_len) {
            // boucle lancée depuis Serato (table de mixage, clavier) ou 4BEAT : longueur inconnue
            // jusqu'au premier retour ; on suppose celle de la dernière boucle de ce genre
            k->loop_in = pos; k->loop_out = pos + k->auto_len; k->loop_have = 1; k->guess = 1; loop = 1;
        } else { k->loop_in = pos; k->loop_out = pos; k->loop_have = 0; k->grow_t = now; } // fin au premier retour
        k->in_manual = 0; k->inside = 1; k->inside_t = now; k->grown = k->left = 0; grow_set(k, 0);
    } else if (!(lp & 0x08) && (k->loop_prev & 0x08) && !k->adjusting) {
        if (k->loop_on && k->loop_have) { loop = 1; k->lb_in = k->loop_in; k->lb_out = k->loop_out; }
        k->loop_on = 0; k->loop_have = 0; k->in_manual = 0; k->guess = 0; k->out_fix = 0;
        k->grown = k->left = 0;
    } else if (act && k->loop_on && !hcj && pos + 30 < k->pos_prev && k->pos_prev - pos < 70000) {
        // retour en arrière pendant la boucle : fin = position d'avant, début = position d'arrivée.
        uint32_t o = k->pos_prev;
        uint32_t d = pos > k->loop_in ? pos - k->loop_in : k->loop_in - pos;
        uint32_t od = o > k->loop_out ? o - k->loop_out : k->loop_out - o;
        int from_end = k->pos_prev + 250 >= k->loop_out && k->pos_prev <= k->loop_out + 250;
        int from_in = o > k->loop_in && o < k->loop_out;       // parti de l'intérieur de la boucle
        int fix = k->out_fix; k->out_fix = 0;
        if (d <= 40) k->grown = k->left = 0;   // retour au début : Serato tient bien la boucle
        if (fix && (o > pos ? o - pos : 0) > 20 && (pos > k->loop_in ? pos - k->loop_in : k->loop_in - pos) < 600) {
            // premier retour après OUT : début et fin tels que Serato les a posés
            k->loop_in = pos; k->loop_out = pos + snap_len(k, o - pos); k->loop_have = 1; loop = 1;
        } else if ((!k->loop_have || k->guess) && (was_inside || !k->guess)) {
            // premier retour : la vraie boucle (longueur supposée corrigée)
            k->loop_in = pos; k->loop_out = pos + snap_len(k, o > pos ? o - pos : 0); k->loop_have = 1; loop = 1;
            if (k->guess || now - k->btn_t >= 0.6) k->auto_len = k->loop_out - k->loop_in;   // boucles de Serato seulement
            k->guess = 0;
        } else if (k->adjusting && d <= 3000 && od <= 3000) {
            // réglage de OUT (Serato fait clignoter OUT : 30 / 20) ou de IN (30 / 10)
            // point suivi à la molette : recalé sur la mesure si elle s'en écarte de plus de 40 ms
            // (la mesure elle-même est à un rapport près, 25 ms)
            if (k->adj_out && od > 40) { k->loop_out = o; loop = 1; }
            if (!k->adj_out && d > 40) { k->loop_in = pos; loop = 1; }
            k->adj_base = k->adj_out ? k->loop_out : k->loop_in; k->jog_acc = 0;
        } else if (d <= 40 && was_inside && (from_end || from_in)) {
            // fin changée : agrandie sans touche vue, ou raccourcie depuis Serato (le retour arrive
            // sur le début depuis l'intérieur : la nouvelle fin est atteinte dès ce tour-ci)
            // (fin connue calée sur les temps : un écart de moins de 150 ms qui ne tombe pas sur un
            // temps n'est que l'imprécision du dernier rapport avant le retour)
            uint32_t L = k->loop_out - k->loop_in, raw = o - k->loop_in, nl = snap_len(k, raw);
            // boucle courte : la fin mesurée (dernier rapport avant le retour) arrive jusqu'à 25 ms
            // trop tôt ; sans correction, ¼ de temps (113 ms, mesures de 80 à 110) était pris pour ⅛
            if (L < 400) nl = snap_len(k, raw + 20);
            uint32_t ne = k->loop_in + nl;
            int noise = nl == raw && snap_len(k, L) != L ? 0 : nl == raw && (raw > L ? raw - L : L - raw) < 150;
            if (!noise && (ne > k->loop_out ? ne - k->loop_out : k->loop_out - ne) > 30) {
                // boucle courte : une seule mesure est trop imprécise (un rapport toutes les 25 ms) ;
                // la même nouvelle fin deux tours de suite (séance du 24/11, 262-265 s : boucle de ¼
                // de temps prise un tour sur deux pour ⅛, le masque alternait)
                if ((L < 400 || nl < 400) && (k->fin_cand > ne ? k->fin_cand - ne : ne - k->fin_cand) > 10) k->fin_cand = ne;
                else { k->loop_out = ne; loop = 1; k->fin_cand = 0; }
            } else k->fin_cand = 0;
        } else {
            // boucle réduite alors que la lecture avait passé la nouvelle fin : Serato recule d'une
            // nouvelle longueur (moitié, quart…) ; sinon, à confirmer par un second retour identique
            uint32_t L = k->loop_out - k->loop_in, back = o - pos, m;
            for (m = L / 2; m >= 10; m /= 2)
                if ((back > m ? back - m : m - back) <= 40 && pos >= k->loop_in) break;
            if (m >= 10 && from_end && was_inside) { k->loop_out = k->loop_in + snap_len(k, m); loop = 1; }
            else {
                uint32_t ci = pos > k->cand_in ? pos - k->cand_in : k->cand_in - pos;
                uint32_t co = o > k->cand_out ? o - k->cand_out : k->cand_out - o;
                if (ci <= 40 && co <= 40) { k->loop_in = pos; k->loop_out = o; loop = 1; k->cand_in = k->cand_out = 0; }
                else { k->cand_in = pos; k->cand_out = o; }
            }
        }
        if (loop) k->grown = k->left = 0;   // boucle mesurée sur un retour
    }
    // la lecture dépasse la fin connue, en continu depuis l'intérieur de la boucle :
    //  - réglage de OUT à la molette : le masque suit la lecture ;
    //  - sinon boucle doublée depuis Serato (table de mixage, clavier) : longueur × 2 tout de suite
    //    (essai V26 : le masque s'agrandissait jusqu'à atteindre la mesure) ; longueur supposée
    //    trop courte : de même. (150 ms : le retour de Serato peut arriver un rapport après la
    //    fin mesurée, sans quoi le masque doublait un instant.)
    // Une seule fois sans retour : si la lecture dépasse aussi la fin agrandie, Serato ne tient plus
    // la boucle alors que son voyant reste allumé (séance du 25/11 : même morceau sur les deux
    // platines, une boucle posée sur l'une libère parfois celle de l'autre ; 149,4 s et 95,9 s).
    // Le masque revient alors à la boucle d'avant et ne bouge plus jusqu'au prochain retour au
    // début (avant : doublé à chaque fin dépassée, jusqu'à 30 s). Pas pour une longueur supposée
    // (guess) : elle peut être fausse de plus du double.
    if (act && k->loop_on && k->loop_have && k->inside && !k->left && pos > k->loop_out + (k->adjusting ? 30 : 150)
        && pos - k->loop_in < 600000 && now - k->grow_t > 0.1) {
        if (k->adjusting) { k->loop_out = pos; k->grow_t = now; loop = 1; }
        else if (k->grown && !k->guess) {
            // marge d'au moins 0,5 s : la fin agrandie vient d'une longueur mesurée, parfois trop
            // courte (rejeu du 25/11, 167 s : 37,00–38,64 doublée, retour réel à 38,88)
            uint32_t L = k->loop_out - k->loop_in, m = L / 4 > 500 ? L / 4 : 500;
            if (pos > k->loop_out + m) {
                k->loop_out = k->grow_base; k->grown = 0; k->left = 1; k->grow_t = now; loop = 1;
                hd_log("CDJ %d lecture sortie de la boucle (voyant toujours allumé)", k->num);
            }
        } else {
            uint32_t L = k->loop_out - k->loop_in;
            if (L < 10) L = pos - k->loop_in;
            k->grow_base = k->loop_out; k->grown = 1;
            while (k->loop_in + L <= pos) L *= 2;
            k->loop_out = k->loop_in + snap_len(k, L);
            k->grow_t = now; loop = 1;
        }
    }
    // premier tour sans longueur supposée (pas de BPM) : le masque suit la lecture
    if ((lp & 0x08) && k->loop_on && !k->loop_have && k->inside && pos > k->loop_out + 20
        && pos - k->loop_in < 70000 && now - k->grow_t > 0.12) {
        k->loop_out = pos; k->grow_t = now; live_send(k, 0);
    }
    // IN posé, en attente de OUT : le principal fait suivre la fin (grow_set) ; il n'y a rien à
    // envoyer ici
    if (!k->in_manual) grow_set(k, 0);
    adj_set(k, k->adjusting && k->loop_on && k->loop_have ? (k->adj_out ? 2 : 1) : 0);
    led_update(k, lp, now);
    if (loop) hd_log("CDJ %d boucle %s%.2f–%.2f s", k->num, k->loop_on ? "" : "terminée ", k->loop_in / 1000.0, k->loop_out / 1000.0);
    k->led_prev = led; k->loop_prev = lp; k->pos_prev = pos;
    if (cues || loop) live_send(k, cues);
    pthread_mutex_unlock(&k->live_mu);
}

// Voyants LOOP IN / OUT / RELOOP (rapport 01, octet 4 : 0x20 IN, 0x10 OUT, 0x08 boucle active).
// Serato les tient lui-même, mais se désynchronise : un appui sur IN pendant le réglage de OUT
// éteint OUT pour tout le reste du morceau (session du 18/11, 22 h 25 : états 18, 28, 20 / 00
// au lieu de 38, 30 / 10 ; seul un nouveau chargement remet les choses). On les calcule donc
// d'après notre suivi de boucle (led_mode, posé par live_report), comme en mode USB :
//   repos : IN et OUT allumés (30) ; IN posé : IN seul (20) ; boucle active : IN, OUT, RELOOP
//   (38), IN et OUT clignotant ensemble (0,25 s, HD_LOOP_BLINK, 0 : fixes) ; réglage de IN :
//   IN clignote, OUT allumé (30 / 10) ; réglage de OUT : OUT clignote (30 / 20).
// Sans suivi (morceau sans waveform couleur, HD_LIVE=0) : l'état de Serato, 00 remplacé par 30.
static double blink_half(void) {
    const char *e = getenv("HD_LOOP_BLINK");
    return e ? atof(e) : 0.25;
}
static long led_phase(const hd_deck_t *k, double t) {
    int m = k->led_mode;
    double h = m == 2 ? blink_half() : 0.25;
    if (m < 2 || h <= 0) return 0;
    return (long)((t - k->led_t0) / h);
}
static uint8_t led_byte(const hd_deck_t *k, double now) {
    long ph = led_phase(k, now) & 1;
    switch (k->led_mode) {
    case 1: return 0x20;
    case 2: return ph ? 0x08 : 0x38;
    case 3: return ph ? 0x10 : 0x30;
    case 4: return ph ? 0x20 : 0x30;
    default: return 0x30;
    }
}
static const uint8_t *led_apply(const hd_deck_t *k, const uint8_t *r, size_t len, uint8_t *buf, double now) {
    uint8_t v = led_byte(k, now);
    if ((r[4] & 0x38) == v) return r;
    memcpy(buf, r, len);
    buf[4] = (uint8_t)((r[4] & ~0x38) | v);
    return buf;
}
// Serato n'envoie plus rien à l'arrêt : pendant un clignotement (boucle active, réglage), le
// dernier rapport est renvoyé ici à chaque changement de phase, au plus 30 s après le dernier
// rapport de Serato.
static void *blinker(void *arg) {
    hd_deck_t *k = arg;
    for (;;) {
        usleep(20000);
        double now = hd_now();
        if (!k->led_live || k->led_mode < 2) continue;
        uint8_t pkt[64] = {0}, out[64]; size_t len;
        hd_lock_send(k);
        pthread_mutex_lock(&k->rep_mu);
        len = k->rep_len;
        int go = len && now - k->rep_t < 30 && now - k->sent_t > 0.05 &&
                 led_phase(k, now) != led_phase(k, k->sent_t);
        if (go) { memcpy(pkt, k->rep, len); k->sent_t = now; }
        pthread_mutex_unlock(&k->rep_mu);
        if (go && deck_enabled(k)) {
            const uint8_t *s = led_apply(k, pkt, len, out, now);
            if (s != pkt) memcpy(pkt, s, len);
            sink(k->dev, pkt);
        }
        hd_unlock_send(k);
    }
    return 0;
}
static void blink_start(hd_deck_t *k) {
    if (k->blink_th) return;
    k->blink_th = 1;
#ifndef HD_HARNESS
    pthread_t th;
    pthread_create(&th, NULL, blinker, k);
    pthread_detach(th);
#else
    (void)blinker;
#endif
}
// Mode des voyants d'après le suivi de boucle (appelée par live_report, sous live_mu).
static void led_update(hd_deck_t *k, uint8_t lp, double now) {
    int m = k->adjusting && k->loop_on ? (k->adj_out ? 4 : 3)
          : (lp & 0x08) ? 2 : k->in_manual ? 1 : 0;
    if (m != k->led_mode) {
        k->led_t0 = m >= 3 ? k->adj_press_t : now;
        k->led_mode = m;
    }
    k->led_live = 1;
    if (m >= 2) blink_start(k);
}

// Renvoie le rapport à envoyer à la platine (r, ou copie modifiée dans buf).
const uint8_t *hd_tweak(hd_deck_t *k, const uint8_t *r, size_t len, uint8_t *buf) {
    const char *e = getenv("HD_LEDS");
    if (len < 16 || len > 64 || r[0] != 0x01 || (e && *e == '0') || !deck_enabled(k)) return r;
    double now = hd_now();
    pthread_mutex_lock(&k->rep_mu);
    memcpy(k->rep, r, len); k->rep_len = len; k->rep_t = now; k->sent_t = now;
    pthread_mutex_unlock(&k->rep_mu);
    if (k->led_live && k->live_ok && k->live_gen == k->generation) return led_apply(k, r, len, buf, now);
    uint8_t lp = r[4] & 0x38;
    if (lp) { k->loop_led_t = now; return r; }
    if (now - k->loop_led_t < 0.6) return r;
    memcpy(buf, r, len);
    buf[4] |= 0x30;
    return buf;
}

#ifdef HD_HARNESS
void hd_harness_live(hd_deck_t *k) { if (k->live_ok && k->live_gen == k->generation) return; k->loop_on = k->loop_have = k->in_manual = k->adjusting = 0; k->lb_in = k->lb_out = 0; k->live_ok = 1; k->live_gen = k->generation; k->led_init = 0; k->guess = 0; k->grown = k->left = 0; k->dbl = 0; k->grow_on = 0; k->adj_sent = 0;
    const char *b = getenv("HD_TEST_BPM"); k->bpm = b ? atof(b) : 0; k->auto_len = k->bpm > 40 ? (uint32_t)(4 * 60000.0 / k->bpm + 0.5) : 0; }
#endif

// Paquet de Serato vers la platine k, vu juste avant son envoi (jamais retenu).
void hd_on_report(hd_deck_t *k, const uint8_t *r, size_t len) {
    hd_rec("HOST>CDJ", k->num, r, len);
    if (len < 4 || !deck_enabled(k)) return;
    if (r[0] == 0x01 && len >= 18) {   // durée du morceau (octets 15-17 : min, s, centièmes)
        pthread_mutex_lock(&k->rep_mu);
        k->sdur_ms = ((uint32_t)r[15] * 60 + r[16]) * 1000 + (uint32_t)r[17] * 10;
        k->spos_ms = ((uint32_t)r[12] * 60 + r[13]) * 1000 + (uint32_t)r[14] * 10;
        k->sdur_t = hd_now();
        pthread_mutex_unlock(&k->rep_mu);
    }
    if (r[0] == 0x02) on_title(k, r);
    else if (r[0] == 0x01 && len >= 18) live_report(k, r);
}

// MARK: - bibliothèque injectée dans Serato

#ifndef HD_HARNESS
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>
#include <sys/stat.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                  \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement,                             \
        (const void *)(unsigned long)&_replacee}

// Fichiers audio ouverts récemment par Serato (le titre seul ne donne pas le chemin).
#define RECENT 32
static struct { char path[1024]; double t; } recent[RECENT];
static int recent_n;
static pthread_mutex_t recent_mu = PTHREAD_MUTEX_INITIALIZER;

static int is_audio(const char *p) {
    const char *dot = strrchr(p, '.');
    if (!dot) return 0;
    static const char *ext[] = {".mp3", ".wav", ".aif", ".aiff", ".m4a", ".aac", ".flac", ".alac", ".mp4", NULL};
    for (int i = 0; ext[i]; i++)
        if (strcasecmp(dot, ext[i]) == 0) return 1;
    return 0;
}

static void note_open(const char *path) {
    if (!path || hd_quiet || !is_audio(path)) return;
    pthread_mutex_lock(&recent_mu);
    int last = (recent_n + RECENT - 1) % RECENT;
    if (recent_n == 0 || strcmp(recent[last].path, path) != 0) {
        snprintf(recent[recent_n % RECENT].path, sizeof recent[0].path, "%s", path);
        recent[recent_n % RECENT].t = hd_now();
        recent_n++;
        pthread_mutex_unlock(&recent_mu);
        hd_log("FICHIER ouvert : %s", path);
        hd_pre_open(path);
        return;
    }
    recent[last].t = hd_now();
    pthread_mutex_unlock(&recent_mu);
}

static int z_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int fd = open(path, flags, mode);
    if (fd >= 0) note_open(path);
    return fd;
}
DYLD_INTERPOSE(z_open, open);

static FILE *z_fopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f) note_open(path);
    return f;
}
DYLD_INTERPOSE(z_fopen, fopen);

extern FILE *fopen_darwin_extsn(const char *, const char *) __asm("_fopen$DARWIN_EXTSN");
static FILE *z_fopen_ext(const char *path, const char *mode) {
    FILE *f = fopen_darwin_extsn(path, mode);
    if (f) note_open(path);
    return f;
}
DYLD_INTERPOSE(z_fopen_ext, fopen_darwin_extsn);

static void squash(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (; *in && j + 1 < cap; in++)
        if (isalnum((unsigned char)*in)) out[j++] = (char)tolower((unsigned char)*in);
    out[j] = 0;
}

// Titre inscrit dans un fichier audio (étiquette lue par AudioToolbox : ID3, AIFF, WAV…), réduit
// comme le titre que Serato envoie à la platine (paquet 02 : UTF-16, hors ASCII → « ? », puis
// lettres et chiffres en minuscules). Lu une fois par fichier, dans le fil du chargeur (jamais dans
// celui de Serato), hors du verrou des fichiers récents. Mesuré sur les 40 morceaux de la
// bibliothèque : 0,05 à 0,4 ms par fichier, 25 ms au pire.
#define TAGS 64
static struct { char path[1024]; char key[512]; } tags[TAGS];
static int tags_n;
static pthread_mutex_t tags_mu = PTHREAD_MUTEX_INITIALIZER;

static void file_title_key(const char *path, char *key, size_t cap) {
    key[0] = 0;
    pthread_mutex_lock(&tags_mu);
    for (int j = 0; j < TAGS && j < tags_n; j++)
        if (!strcmp(tags[j].path, path)) {
            snprintf(key, cap, "%s", tags[j].key);
            pthread_mutex_unlock(&tags_mu);
            return;
        }
    pthread_mutex_unlock(&tags_mu);
    char title[512] = "";
    int quiet = hd_quiet;
    hd_quiet = 1;                      // cette ouverture n'est pas une ouverture de Serato
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    AudioFileID f;
    if (u && AudioFileOpenURL(u, kAudioFileReadPermission, 0, &f) == noErr) {
        CFDictionaryRef d = NULL;
        UInt32 sz = sizeof d;
        if (AudioFileGetProperty(f, kAudioFilePropertyInfoDictionary, &sz, &d) == noErr && d) {
            CFStringRef t = CFDictionaryGetValue(d, CFSTR(kAFInfoDictionary_Title));
            if (t && CFGetTypeID(t) == CFStringGetTypeID()) {
                CFIndex n = CFStringGetLength(t), w = 0;
                for (CFIndex i = 0; i < n && w + 1 < (CFIndex)sizeof title; i++) {
                    UniChar c = CFStringGetCharacterAtIndex(t, i);
                    title[w++] = c < 128 ? (char)c : '?';
                }
                title[w] = 0;
            }
            CFRelease(d);
        }
        AudioFileClose(f);
    }
    if (u) CFRelease(u);
    hd_quiet = quiet;
    squash(title, key, cap);
    pthread_mutex_lock(&tags_mu);
    int i = tags_n++ % TAGS;
    snprintf(tags[i].path, sizeof tags[i].path, "%s", path);
    snprintf(tags[i].key, sizeof tags[i].key, "%s", key);
    pthread_mutex_unlock(&tags_mu);
}

// Fichier d'un titre, parmi les fichiers audio ouverts récemment par Serato (le plus récent
// d'abord ; Serato rouvre le fichier juste avant d'annoncer le titre) :
//  1) dont le nom contient le titre (Serato a pu l'ouvrir bien avant, en parcourant sa
//     bibliothèque) ;
//  2) dont le titre inscrit est le titre (fichier nommé autrement : « Pitbull Ft. T-Pain - Hey
//     Baby, Drop It ToThe Floor… » pour « Hey Baby, Drop It On The Floor - … ») ;
// Plus de règle de secours (« le dernier fichier ouvert », jusqu'au 24/11) : elle pouvait donner la
// waveform d'un autre morceau si la liste défilait ou si l'autre platine chargeait au même moment.
// Sans nom ni titre inscrit reconnu, la platine garde l'affichage d'origine pour ce morceau.
// Attend un peu que Serato ait ouvert le fichier.
static int find_recent(hd_deck_t *k, const char *title, char *out, size_t cap) {
    char want[512], cand[1024];
    static char paths[RECENT][1024];   // candidats de la règle 2 (un chargeur à la fois, voir paths_mu)
    static pthread_mutex_t paths_mu = PTHREAD_MUTEX_INITIALIZER;
    squash(title, want, sizeof want);
    for (int attempt = 0; attempt < 10; attempt++) {
        int np = 0;
        out[0] = 0;
        pthread_mutex_lock(&paths_mu);
        pthread_mutex_lock(&recent_mu);
        for (int j = 0; j < RECENT && j < recent_n; j++) {
            int i = (recent_n - 1 - j) % RECENT;
            const char *base = strrchr(recent[i].path, '/');
            squash(base ? base + 1 : recent[i].path, cand, sizeof cand);
            if (want[0] && strstr(cand, want)) { snprintf(out, cap, "%s", recent[i].path); break; }
            snprintf(paths[np++], sizeof paths[0], "%s", recent[i].path);
        }
        pthread_mutex_unlock(&recent_mu);
        for (int j = 0; !out[0] && want[0] && j < np; j++) {
            char key[512];
            file_title_key(paths[j], key, sizeof key);
            if (key[0] && !strcmp(key, want)) {
                snprintf(out, cap, "%s", paths[j]);
                hd_log("CDJ %d « %s » : fichier trouvé par son titre inscrit", hd_deck_num(k), title);
            }
        }
        pthread_mutex_unlock(&paths_mu);
        if (out[0]) return 1;
        usleep(100 * 1000);
    }
    return 0;
}

static int hid_sink(void *dev, const uint8_t *p) {
    IOReturn r = IOHIDDeviceSetReport((IOHIDDeviceRef)dev, kIOHIDReportTypeOutput, 0, p, 64);
    return r == kIOReturnSuccess ? 0 : -1;
}

// La platine débranchée, Serato libère sa référence : nos fils (envoi de la waveform, clignotement
// des voyants) pourraient encore écrire quelques instants vers un objet libéré. Une référence à
// nous le garde valide ; l'écriture échoue alors proprement (envoi interrompu).
static void retain_dev(void *dev) { CFRetain((CFTypeRef)dev); }

__attribute__((constructor)) static void serato_hd_init(void) {
    const char *p = getenv("SERATO_HD_LOG");
    hd_init(fopen(p ? p : "/tmp/serato_hd.log", "a"));
    hd_set_io(hid_sink, find_recent);
    deck_new_hook = retain_dev;
    hd_log("# serato_hd chargé, pid %d%s%s", getpid(), passive ? ", passif (rien n'est envoyé)" : "",
           getenv("SERATO_HD_REC") ? ", enregistrement de session" : "");
}

static int is_pioneer(IOHIDDeviceRef d) {
    int v = 0;
    CFTypeRef n = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDVendorIDKey));
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &v);
    return v == 0x08E4 || v == 0x2B73;
}

static IOReturn z_IOHIDDeviceSetReport(IOHIDDeviceRef d, IOHIDReportType t, CFIndex id,
                                       const uint8_t *r, CFIndex len) {
    hd_deck_t *k = NULL;
    if (!is_pioneer(d) || t != kIOHIDReportTypeOutput || len < 16 || !(k = hd_deck_for((void *)d)))
        return IOHIDDeviceSetReport(d, t, id, r, len);
    hd_on_report(k, r, (size_t)len);
    uint8_t buf[64];
    r = hd_tweak(k, r, (size_t)len, buf);
    hd_lock_send_prio(k);
    IOReturn ret = IOHIDDeviceSetReport(d, t, id, r, len);
    hd_unlock_send(k);
    return ret;
}
DYLD_INTERPOSE(z_IOHIDDeviceSetReport, IOHIDDeviceSetReport);

// Rapports reçus de la platine (boutons, jog, sélecteur…) : enregistrés, puis remis à Serato.
typedef struct { IOHIDReportCallback cb; void *ctx; hd_deck_t *deck; } in_tramp_t;

static void z_input(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id,
                    uint8_t *report, CFIndex len) {
    in_tramp_t *t = ctx;
    if (result == kIOReturnSuccess && len > 0) hd_on_input(t->deck, report, (size_t)len);
    t->cb(t->ctx, result, sender, type, id, report, len);
}

static void z_IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef d, uint8_t *report, CFIndex len,
                                                     IOHIDReportCallback cb, void *ctx) {
    hd_deck_t *k = NULL;
    if (!cb || !getenv("SERATO_HD_REC") || !is_pioneer(d) || !(k = hd_deck_for((void *)d))) {
        IOHIDDeviceRegisterInputReportCallback(d, report, len, cb, ctx);
        return;
    }
    in_tramp_t *t = malloc(sizeof *t);   // jamais libéré : un par platine et par session
    t->cb = cb; t->ctx = ctx; t->deck = k;
    IOHIDDeviceRegisterInputReportCallback(d, report, len, z_input, t);
}
DYLD_INTERPOSE(z_IOHIDDeviceRegisterInputReportCallback, IOHIDDeviceRegisterInputReportCallback);
#endif
