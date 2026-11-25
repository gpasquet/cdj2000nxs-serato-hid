/*
 * hdwave — grande waveform en mode HID pour le CDJ-2000NXS (firmware 1.44).
 *
 * Code SH-4 ajouté après le programme principal (0x043E0000). Il s'intercale à trois endroits,
 * par redirection d'entrées de tables d'adresses (aucune instruction d'origine modifiée) :
 *   - hd_send   : envoi d'un paquet vers l'affichage   (FUN_04310aca, 8 entrées)
 *   - hd_check  : réception d'une réponse de l'affichage (FUN_04311216, 1 entrée)
 *   - hd_hid    : analyse d'un paquet HID de l'ordinateur (FUN_0435785e, 1 entrée)
 *
 * Principe (mis au point dans l'émulateur, voir EXPERIENCE.md) : quand l'ordinateur a envoyé une
 * waveform détaillée (paquets HID 0x30), les messages vers l'affichage décrivent un morceau « avec
 * waveform » (comme en mode USB) ; l'affichage réclame alors la waveform (demande 20) et la grille
 * (demande 21), que ce code lui envoie à la place des paquets de routine, après les avoir annoncés.
 *
 * Paquets HID 0x30 (64 octets, ordinateur → platine) :
 *   30 00                         effacer (retour au comportement d'origine)
 *   30 01 L:u32 min:u16 s256:u16  début de waveform : L octets PWV3 (150/s), durée du morceau
 *   30 02 k:u16 60 octets         waveform, octets k*60 .. k*60+59
 *   30 03 N:u32                   début de grille : N temps
 *   30 04 k:u16 60 octets         grille, 15 entrées de 4 octets (temps 1-4, ms >> 16, ms bas 16 bits)
 *   30 05                         valider : la waveform est utilisable
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#define ORIG_SEND  ((int (*)(const u8 *, int, int, int))0x04310aca)
#define ORIG_CHECK ((int (*)(u8 *, int, int, int))0x04311216)
#define ORIG_HID   ((int (*)(u8 *, int, int, int))0x0435785e)
#define ORIG_INSEND ((int (*)(u8 *, int, int, int))0x04353f80)   /* émission du rapport d'entrée HID */

#define WAVE_MAX 900000          /* = tampon de l'affichage : 100 min en PWV3, 50 min en couleur (PWV5) */
#define GRID_MAX 4096           /* temps */

/* Tampons hors image, dans la RAM libre à partir de 0x04400000 (section .hdbss). */
static u8 wave[WAVE_MAX] __attribute__((section(".hdbss")));
/* waveform compressée (30 0a, V36) : décodée au fil de l'arrivée dans wave[] (voir hdcz.h) */
#include "hdcz.h"
#define CZ_RING 4096
static u8 cz_ring[CZ_RING] __attribute__((section(".hdbss")));
static cz_dec_t cz __attribute__((section(".hdbss")));
static u8 grid[GRID_MAX * 4] __attribute__((section(".hdbss")));
static u8 out[896] __attribute__((section(".hdbss")));
static u8 fiche[896] __attribute__((section(".hdbss")));   /* dernière fiche 0x10 envoyée par MAIN */
static u8 rout[64] __attribute__((section(".hdbss")));     /* routine modifiée en cours */
static u8 pend[64] __attribute__((section(".hdbss")));     /* routine du principal mise de côté */
static u8 held[896] __attribute__((section(".hdbss")));    /* paquet du principal mis de côté */
static u8 xbuf[896] __attribute__((section(".hdbss")));    /* paquet du principal modifié */
static u8 fbuf[896] __attribute__((section(".hdbss")));    /* fiche préparée (tonalité devant le titre) */

static struct {
    u32 magic;          /* la RAM n'est pas remise à zéro au démarrage : état valide si == MAGIC */
    u32 n_send, n_check, n_hid, n_req;   /* compteurs de diagnostic (lisibles par le débogueur) */
    u8 last_ans[16];
    u32 wave_len, grid_n;
    u16 dur_min, dur_s256;
    u8 ready;
    /* envoi d'une série de paquets (0x20 ou 0x21) à l'affichage */
    u8 srv_type;        /* 0 : rien en cours */
    u16 srv_seq;        /* prochain numéro de morceau (1..) */
    u16 srv_count;      /* nombre de morceaux */
    u8 gap;             /* routines à laisser passer avant le prochain morceau */
    u8 announced;       /* la dernière routine a annoncé le morceau suivant */
    u16 fiche_len;      /* 0 : aucune fiche en mémoire */
    /* échange avec les annonces du principal (voir hd_send) */
    u8 pend_ann;        /* routine du principal portant une annonce, remplacée, à envoyer */
    u16 held_len;       /* paquet annoncé du principal, retardé d'un envoi (0 : aucun) */
    u8 ack_on, ack_exp, ack_act;   /* accusé à traduire : reçu ack_act, attendu ack_exp */
    u8 seq_pending;     /* séquence de chargement USB à jouer au prochain morceau chargé */
    u8 shown;           /* le morceau est chargé et décrit à l'affichage comme un morceau USB */
    u8 finish;          /* patch retiré, mais un envoi 0x20 / 0x21 commencé est mené à son terme */
    u8 seq_i, seq_k;    /* séquence de chargement USB en cours : modèle (1..), répétition */
    /* garde-fou : après la séquence de chargement, l'affichage doit réclamer la waveform */
    /* vue liste (BROWSE) : le patch est suspendu, l'affichage revient au mode ordinateur
     * d'origine ; la waveform reçue est gardée pour le retour à la vue morceau */
    u8 susp;            /* vue liste en cours */
    u8 rot_init;        /* compteur de molette déjà vu */
    u8 rot_k;           /* tampon d'envoi courant (0 / 1) */
    short rot_last;     /* dernier compteur de molette du principal */
    short rot_virt;     /* compteur de molette présenté à l'ordinateur */
    u8 back_prev;       /* touche BACK au rapport précédent */
    /* repères (hot cues, boucle) à ajouter au paquet 0x05 de l'affichage (30 10) */
    u8 cue_n;
    u16 cue_kind[8];
    u32 cue_hf[8];      /* position en demi-images (1/150 s) */
    u8 cue_rgb[24];     /* couleurs des repères, RVB (30 11), dans le même ordre */
    u8 back_pulse;      /* appui BROWSE à envoyer à l'affichage */
    u8 have;            /* waveform complète en mémoire (30 05 reçu) */
    u8 dbg_n, dbg_off[6], dbg_val[6];   /* essais : octets de routine forcés (30 0e) */
    /* tonalité (30 07), ajoutée devant le titre dans la fiche du morceau */
    u8 key_n, key[6];
    u8 key_pending;     /* fiche à (re)servir avec la tonalité */
    u16 idle;           /* routines écoulées sans aucun envoi de notre part */
    u16 fout_len;       /* longueur de la fiche préparée dans fbuf */
    signed char zoom;   /* zoom de la grande waveform : octet 33 des routines, comme en mode USB */
    u8 got20;           /* demande 0x20 reçue pour ce morceau */
    u8 tries;           /* séquences rejouées faute de demande */
    u16 wd;             /* routines écoulées depuis la fin de la séquence */
    /* lissage de la position (voir smooth_pos) */
    s32 sm_e, sm_v;     /* position estimée et vitesse par routine, en 1/256 de demi-image */
    u32 sm_tick, sm_s;  /* compteur de routines ; dernière position reçue */
    u32 sm_ht[8]; s32 sm_hs[8];   /* derniers changements : routine, position */
    u8 sm_hn, sm_still, sm_on, no_smooth;
    u8 refresh;         /* bits à poser une fois dans l'octet 37 (30 13) */
    u8 back_hold;       /* BACK appuyé en vue waveform : retenu jusqu'au relâchement */
    u8 early, partial;  /* 30 14 reçu ; chargement lancé avant la fin de la waveform */
    u32 wave_rx;        /* octets de waveform reçus (envoi séquentiel) */
    u8 cz_on, cz_go;    /* waveform compressée annoncée (30 01, octet 10) ; décodeur lancé */
    u32 cz_len, cz_w, cz_j, cz_n;   /* octets compressés : total, reçus ; points décodés, total */
    u8 quant;           /* QUANTIZE de Serato (rapport 01, octet 8, bit 0x04) */
    u8 spos_ok;         /* position de Serato reçue (rapport 01) */
    u32 spos;           /* position de Serato, demi-images (rapport 01, octets 12-14 : min, s, cs) */
    u8 grow, grow_lead, grow_k;   /* 30 15 : fin de boucle à la tête de lecture (IN posé) */
    /* retour de la liste : l'écran a gardé la waveform, on ne lui envoie qu'une marque */
    u8 delivered;       /* waveform de ce morceau entièrement servie à l'écran */
    u8 short20;         /* prochain envoi 0x20 : la marque « HDCACHE » seulement */
    u8 adj;             /* 30 16 : réglage de la boucle à la molette (1 IN, 2 OUT, 0 aucun) */
    /* molette en vue liste : un cran à la fois vers l'ordinateur (V39, voir hd_inrep) */
    signed char rot_pend;   /* crans reçus, pas encore transmis */
    u32 rot_t;          /* TMU3 au dernier cran transmis */
    u32 rot_tlast;      /* TMU3 au dernier rapport (compteur arrêté : crans transmis d'un coup) */
} st __attribute__((section(".hdbss")));

static u32 dur_sec(void);
static u32 dur_hf(void);
#define KEY_IN_TITLE 0
#define MAGIC 0x48445756u   /* « HDWV » */

static void zero(u8 *d, u32 n) { while (n--) *d++ = 0; }
static void init(void)
{
    if (st.magic == MAGIC) return;
    zero((u8 *)&st, sizeof st);
    st.magic = MAGIC;
}
static void copy(u8 *d, const u8 *s, u32 n) { while (n--) *d++ = *s++; }
static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static u16 crc16(const u8 *b, u32 n)        /* CRC16-XModem, comme la liaison */
{
    u16 c = 0;
    while (n--) {
        c ^= (u16)*b++ << 8;
        for (int k = 0; k < 8; k++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}
static void seal(u8 *p, u32 len) { wr16(p + len - 2, crc16(p, len - 2)); }

static void start_serving(u8 type);

/* Retour au comportement d'origine. Un envoi de waveform ou de grille déjà commencé est mené à
 * son terme : interrompu, il laisserait l'affichage en attente des morceaux manquants. */
static void go_passive(void)
{
    st.ready = 0; st.seq_pending = 0; st.seq_i = 0; st.shown = 0; st.have = 0;
    if (st.srv_type == 0x20 || st.srv_type == 0x21) {
        st.finish = 1;
    } else {
        st.srv_type = 0; st.announced = 0; st.finish = 0;
    }
}

/* ------------------------------------------------------- rapport d'entrée HID ---- */

/* Le principal construit le rapport de 20 octets pour l'ordinateur en 0x04358ab6 (octets 10-11 :
 * compteur de molette ; octet 6 bit 7 : « panneau ouvert ») et l'émet par 0x04353f80, appelée
 * depuis ce seul endroit (littéral 0x04358d0c). Ce bit ne suit pas l'écran (il reste à 1 après
 * la fermeture du panneau INFO, essai V13) : la partie Mac ne peut pas savoir si la waveform est
 * affichée. Le patch, lui, le sait. Quand elle l'est, les crans de molette changent le zoom et
 * ne sont pas transmis à l'ordinateur (le compteur qu'il reçoit n'avance pas) ; sinon ils lui
 * sont transmis comme à l'origine. Le rapport d'origine n'est pas modifié (le principal le
 * compare au suivant) : c'est une copie qui part.
 *
 * V39 — un cran = un morceau. Serato accélère dans ses listes quand les crans lui arrivent à
 * ~30 ms d'intervalle (sessions des 18 au 22/11 : 2 crans → 5 morceaux, 3 → 5 ou 6, 7 → 9) et
 * suit cran par cran à 63 ms et plus. En vue liste, les crans sont donc mis en
 * attente et transmis un par un, à 60 ms au moins l'un de l'autre (horloge : TMU3, compteur libre
 * du système à 13,5 MHz, TCOR0 = 13499 pour son tic de 1 ms). Pas besoin de provoquer d'envoi :
 * le principal renvoie le rapport de lui-même après 33 passages sans changement (compteur en
 * 0x0a7bb0f4, un passage par ms environ, d'où le pas de ~32 ms vu dans les sessions), ce qui
 * vide l'attente au rythme d'un cran toutes les 64 ms. Au plus 6 crans d'avance (0,4 s de
 * rattrapage quand on lâche la molette) ; l'appui sur la molette ou BACK vide l'attente : le
 * morceau chargé est celui que montre l'écran. */
#define TMU3_TCNT (*(volatile u32 *)0xFFDC000Cu)
#define ROT_GAP   (60u * 13500u)
#define ROT_MAX   6
static u8 inrep[2][20] __attribute__((section(".hdbss")));

int hd_inrep(u8 *rep, int len, int a, int b)
{
    init();
    if (len != 20) return ORIG_INSEND(rep, len, a, b);
    short cnt = (short)(rep[10] | rep[11] << 8);
    if (!st.rot_init) { st.rot_init = 1; st.rot_last = cnt; st.rot_virt = cnt; }
    short d = (short)(cnt - st.rot_last);
    st.rot_last = cnt;
    /* (Le panneau INFO, superposé à la vue morceau, n'est pas distingué : son suivi par la
     * touche se décalait sur platine, essai V14, et la molette restait à Serato après sa
     * fermeture.) */
    int wave = st.ready && st.shown && !st.susp;
    if (wave) {
        st.zoom = (signed char)(st.zoom + d);
        st.rot_pend = 0;
    } else {
        s32 p = st.rot_pend + d;
        st.rot_pend = (signed char)(p > ROT_MAX ? ROT_MAX : p < -ROT_MAX ? -ROT_MAX : p);
    }
    /* appui sur la molette (chargement) ou BACK : on part de la sélection affichée */
    if (rep[3] & 0x60) st.rot_pend = 0;
    if (st.rot_pend) {
        u32 now = TMU3_TCNT;
        if (now == st.rot_tlast) {          /* compteur arrêté : comportement d'origine */
            st.rot_virt = (short)(st.rot_virt + st.rot_pend);
            st.rot_pend = 0;
        } else if (st.rot_t - now >= ROT_GAP) {     /* décompte : temps écoulé = avant - maintenant */
            signed char s = st.rot_pend > 0 ? 1 : -1;
            st.rot_virt = (short)(st.rot_virt + s);
            st.rot_pend = (signed char)(st.rot_pend - s);
            st.rot_t = now;
        }
        st.rot_tlast = now;
    }
    u8 *o = inrep[st.rot_k ^= 1];
    copy(o, rep, 20);
    /* Serato n'accepte la bande tactile (Needle Search) que si le bit 7 de l'octet 6 est à 1
     * (toutes les sessions du 14 et du 15/11 : 7 sauts avec le bit, 21 touches sans effet
     * sans lui). Le principal le met à 0 en vue morceau : on le remet, la molette étant de
     * toute façon retenue dans cet état. */
    /* BACK en vue waveform : retour à la liste, comme BROWSE. L'affichage change d'écran sur
     * l'impulsion 0x80 de l'octet 31 d'une routine (le principal en mode ordinateur ne gère
     * pas lui-même la vue) ; elle part avec la prochaine routine (hd_send). */
    /* BACK = octet 3 bit 0x40 : rafales de 0x40 avant les chargements dans les sessions
     * réelles (remontée dans les listes), et 0x40 dans l'émulateur. (L'analyse statique
     * donnait 0x10 ; 0x10 n'apparaît qu'une ou deux fois par session.) */
    u8 back = rep[3] & 0x40;
    if (wave && back && !st.back_prev) { st.back_pulse = 1; st.back_hold = 1; }
    if (!back) st.back_hold = 0;
    st.back_prev = back;
    if (wave) {
        o[6] |= 0x80;
        /* Serato se croyant alors en vue liste, l'appui sur la molette (octet 3, 0x20)
         * chargerait le morceau sélectionné et BACK (0x40) changerait de liste : ces deux
         * touches ne lui sont pas transmises tant que la waveform est à l'écran. */
        o[3] &= (u8)~0x60;
    }
    /* L'impulsion fait quitter la vue waveform alors que la touche est encore enfoncée : sans
     * cette retenue, la fin de l'appui partait vers Serato, qui remontait d'une liste en plus
     * (essai V19 : « comme si j'appuyais deux fois », pas à chaque fois). */
    if (st.back_hold) o[3] &= (u8)~0x40;
    o[10] = (u8)st.rot_virt; o[11] = (u8)((u16)st.rot_virt >> 8);
    return ORIG_INSEND(o, len, a, b);
}

/* ---------------------------------------------------------------- paquets HID ---- */

/* Morceau prêt à être présenté à l'affichage (fin de l'envoi, ou début avec 30 14). */
static void begin_track(void)
{
    /* la waveform doit arriver AVANT que l'ordinateur charge le morceau sur la platine :
     * l'affichage ne réclame la waveform que pendant sa séquence de chargement (un envoi
     * tardif n'a pas pu la relancer, voir EXPERIENCE.md) */
    st.have = st.wave_len != 0;
    st.delivered = 0; st.short20 = 0;
    /* un nouveau morceau s'affiche toujours en vue morceau, même chargé depuis la liste
     * (comme en mode USB) : la suspension « vue liste » prend fin */
    st.susp = 0;
    st.ready = st.have;
    if (!st.finish) { st.srv_type = 0; st.announced = 0; }
    st.shown = 0;
    /* au chargement du morceau (prochaine routine « morceau de l'ordinateur chargé ») :
     * rejouer la séquence de chargement d'un morceau USB, sans quoi, après un premier
     * morceau, l'affichage garde l'ancien et ne réclame pas la nouvelle waveform */
    st.seq_pending = st.ready; st.seq_i = 0;
    st.got20 = 0; st.tries = 0; st.wd = 0;
}

int hd_hid(u8 *pkt, int a, int b, int c)
{
    init();
    st.n_hid++;
    /* QUANTIZE : Serato bascule le bit 0x04 de l'octet 8 de son rapport d'état à chaque appui
     * sur la touche (session du 18/11 au soir). L'affichage le montre en rouge par le bit 0x04
     * de l'octet 23 des routines, comme en mode USB (gris sans lui). */
    if (pkt[0] == 0x01) {
        st.quant = pkt[8] & 0x04;
        /* V36 : position prise dans le rapport de Serato. Le temps des routines du principal est
         * extrapolé entre deux rapports : à la pause il dépassait de 4 à 8 demi-images puis
         * revenait (vidéo 5194 : temps affiché 4:17.08 → 4:17.05 → 4:17.08 ; rejeu dans
         * l'émulateur, session de 18 h 12), et le lissage en faisait une lente glissade arrière */
        st.spos = ((u32)pkt[12] * 60 + pkt[13]) * 150 + ((u32)pkt[14] * 3 + 1) / 2;
        st.spos_ok = 1;
    }
    if (pkt[0] != 0x30) return ORIG_HID(pkt, a, b, c);
    switch (pkt[1]) {
    case 0x00:
        st.grow = 0; st.adj = 0; st.delivered = 0; st.short20 = 0;
        go_passive(); st.fiche_len = 0; st.key_n = 0; st.key_pending = 0; st.cue_n = 0; st.early = 0; st.partial = 0;
        zero(st.cue_rgb, sizeof st.cue_rgb); break;
    case 0x01:
        /* nouveau morceau : la fiche mémorisée est celle du précédent ; l'oublier, pour que la
         * demande de fiche de l'affichage parte vers le principal, qui envoie la nouvelle */
        go_passive(); st.fiche_len = 0;
        st.wave_len = rd32(pkt + 2);
        if (st.wave_len > WAVE_MAX) st.wave_len = WAVE_MAX;
        st.dur_min = rd16(pkt + 6); st.dur_s256 = rd16(pkt + 8);
        st.grid_n = 0; st.wave_rx = 0; st.early = 0; st.partial = 0;
        /* V36 : octet 10 = 1, waveform compressée (30 0a) : octets 12-15 longueur compressée,
         * 16-23 en-tête de la waveform (« HDRGB5 », options), recopié tel quel */
        st.cz_on = pkt[10] == 1 && st.wave_len >= 8;
        if (st.cz_on) {
            st.cz_len = rd32(pkt + 12); st.cz_w = 0; st.cz_j = 0; st.cz_go = 0;
            st.cz_n = (st.wave_len - 8) / 2;
            copy(wave, pkt + 16, 8);
            st.wave_rx = 8;
            cz_dec_init(&cz, cz_ring, CZ_RING - 1);
        }
        break;
    case 0x14:
        st.early = 1;
        break;
    case 0x02: {
        u32 off = (u32)rd16(pkt + 2) * 60;
        if (off + 60 <= WAVE_MAX) copy(wave + off, pkt + 4, 60);
        st.wave_rx = off + 60;
        /* 30 14 reçu avant la waveform : le chargement à l'écran commence dès le premier
         * paquet (l'en-tête « HDRG » sert à marquer la petite waveform) ; les morceaux de
         * waveform ne sont annoncés à l'affichage qu'une fois reçus (hd_send) */
        if (st.early && off == 0 && st.wave_len) { st.early = 0; begin_track(); st.partial = 1; }
        break;
    }
    case 0x0a: {
        /* 30 0a k : octets compressés k×60 à k×60+59 (V36), dans l'ordre */
        u32 off = (u32)rd16(pkt + 2) * 60;
        if (!st.cz_on || off != st.cz_w || off >= st.cz_len) break;
        u32 n = st.cz_len - off < 60 ? st.cz_len - off : 60;
        for (u32 i = 0; i < n; i++) cz_ring[(off + i) & (CZ_RING - 1)] = pkt[4 + i];
        st.cz_w = off + n;
        if (!st.cz_go && st.cz_w >= 5) { cz_dec_start(&cz); st.cz_go = 1; }
        /* un point lit 15 octets au plus : 32 d'avance suffisent, sauf à la fin du flux */
        while (st.cz_go && st.cz_j < st.cz_n && (st.cz_w >= st.cz_len || st.cz_w - cz.pos >= 32)) {
            u32 w = cz_point(&cz);
            wave[8 + 2 * st.cz_j] = (u8)(w >> 8); wave[9 + 2 * st.cz_j] = (u8)w;
            st.cz_j++;
        }
        st.wave_rx = 8 + 2 * st.cz_j;
        if (st.early && off == 0 && st.wave_len) { st.early = 0; begin_track(); st.partial = 1; }
        break;
    }
    case 0x03:
        st.grid_n = rd32(pkt + 2);
        if (st.grid_n > GRID_MAX) st.grid_n = GRID_MAX;
        break;
    case 0x04: {
        u32 off = (u32)rd16(pkt + 2) * 60;
        if (off + 60 <= sizeof grid) copy(grid + off, pkt + 4, 60);
        break;
    }
    case 0x07: {
        /* 30 07 n c… : tonalité du morceau (ASCII, 6 caractères au plus), lue par le Mac dans les
         * tags du fichier — Serato ne l'envoie pas à la platine */
        u32 n = pkt[2] > 6 ? 6 : pkt[2];
        for (u32 i = 0; i < n; i++) st.key[i] = pkt[3 + i];
        st.key_n = (u8)n; st.key_pending = KEY_IN_TITLE;
        break;
    }
    case 0x10: {
        /* 30 10 n (genre:u16 ms:u32)×n : repères du morceau (hot cues, boucle), lus par le Mac
         * dans les tags Serato ; ajoutés à la liste de repères (paquet 0x05) servie à
         * l'affichage, qui les dessine comme ceux d'un morceau rekordbox. 8 au plus. */
        u32 n = pkt[2] > 8 ? 8 : pkt[2];
        for (u32 i = 0; i < n; i++) {
            const u8 *e = pkt + 3 + 6 * i;
            st.cue_kind[i] = rd16(e);
            st.cue_hf[i] = rd32(e + 2) * 3 / 20;          /* ms → 1/150 s */
        }
        st.cue_n = (u8)n;
        break;
    }
    case 0x15:
        /* 30 15 g : IN posé, OUT pas encore appuyé. Le masque de boucle doit finir à la tête de
         * lecture, comme en mode USB. Envoyée par le Mac (V26), puis placée ici toutes les trois
         * routines (V27), sa fin était en retard ou saccadée. L'écran l'étend désormais lui-même
         * à chaque dessin (rgbwave.c, rgb_wframe) ; l'état lui est passé par la marque de la
         * petite waveform (colonne 31), renvoyée à l'écran ici (bit 3 de l'octet 37). */
        st.grow = pkt[2]; st.grow_lead = pkt[3]; st.grow_k = 0;
        break;
    case 0x16:
        /* 30 16 m : un point de la boucle est réglé à la molette (1 IN, 2 OUT ; 0 : fini). L'écran
         * fige la grande waveform sur ce point, comme en mode USB (V31) ; l'état lui passe par la
         * marque de la petite waveform (colonne 31 : 'I' ou 'O'), redemandée ici (octet 37, bit 3). */
        if (st.adj != pkt[2]) { st.adj = pkt[2]; st.refresh |= 0x08; }
        break;
    case 0x13:
        /* 30 13 m : repères ou boucle changés en cours de lecture (30 10 / 30 11 renvoyés juste
         * avant). L'octet 37 de la routine fait redemander des paquets à l'affichage, une fois
         * par routine qui le porte : bit 7 → liste des repères (05), bit 3 → petite waveform
         * (04, qui porte les couleurs). Vu dans le code de l'écran (0xD0E75A, 0xCF6DD4) et dans
         * la capture USB d'origine. */
        st.refresh |= pkt[2] & 0x88;          /* sans effacer un 0x08 demandé par 30 15 */
        break;
    case 0x11: {
        /* 30 11 n (R V B)×n : couleurs des repères de 30 10, transmises à l'affichage dans les
         * octets hauts des colonnes 8..31 de la petite waveform (voir edit_other) */
        u32 n = pkt[2] > 8 ? 8 : pkt[2];
        zero(st.cue_rgb, sizeof st.cue_rgb);
        copy(st.cue_rgb, pkt + 3, 3 * n);
        break;
    }
    case 0x0e:
        /* essais : 30 0e off val force l'octet « off » des routines déguisées ; 30 0e ff efface */
        if (pkt[2] == 0xfe) { st.no_smooth = pkt[3]; break; }    /* 30 0e fe 1 : sans lissage */
        if (pkt[2] == 0xff) st.dbg_n = 0;
        else if (st.dbg_n < 6) { st.dbg_off[st.dbg_n] = pkt[2]; st.dbg_val[st.dbg_n] = pkt[3]; st.dbg_n++; }
        break;
    case 0x06: {
        /* 30 06 d : la molette a tourné de d crans (signé) en vue morceau. En mode USB le
         * principal tient le niveau de zoom et l'envoie dans l'octet 33 des routines (8 bits signé ; 0 au
         * chargement, puis −1, −2, +1…) ; en mode ordinateur il ne le fait pas. */
        /* compteur libre, comme la molette : l'affichage n'en lit que les variations et borne
         * lui-même le zoom (5 niveaux, vérifié dans l'émulateur) */
        /* la partie Mac envoie tous les crans : le zoom ne change que si la waveform est à
         * l'écran (patch actif, morceau affiché, pas en vue liste). L'indicateur « vue liste »
         * que voit le Mac ne suit pas l'écran après un chargement (essai V11). */
        if (st.ready && st.shown && !st.susp)
            st.zoom = (signed char)(st.zoom + (signed char)pkt[2]);
        break;
    }
    case 0x05:
        if (st.partial) { st.partial = 0; break; }      /* déjà lancé au premier paquet (30 14) */
        st.early = 0;
        begin_track();
        break;
    }
    return 0;
}

/* -------------------------------------------------- réponses de l'affichage ---- */

static void start_serving(u8 type)
{
    if (type == 0x10) {
        st.srv_type = 0x10; st.srv_seq = 1; st.srv_count = 1; st.gap = 1; st.announced = 0;
        return;
    }
    u32 len = type == 0x20 ? (st.short20 ? 16 : st.wave_len) : st.grid_n * 4;
    u32 first = type == 0x20 ? 880 : 878;
    if (type == 0x20) st.got20 = 1;
    st.srv_type = type;
    st.srv_seq = 1;
    st.srv_count = len <= first ? 1 : 1 + (len - first + 887) / 888;
    st.gap = 1;
    st.announced = 0;
}

int hd_check(u8 *ans, int a, int b, int c)
{
    init();
    st.n_check++;
    if (ans[2]) { st.n_req++; copy(st.last_ans, ans, 16); }
    int changed = 0;
    /* accusé du paquet que nous avons mis à la place de celui du principal : le principal doit
     * recevoir l'accusé du paquet qu'il croit avoir envoyé (l'affichage répond à chaque paquet) */
    if (st.ack_on) {
        st.ack_on = 0;
        if (ans[0] == st.ack_act && ans[0] != st.ack_exp) { ans[0] = st.ack_exp; changed = 1; }
    }
    if (st.ready || st.finish) {
        /* demandes de waveform / grille : servies ici, masquées au processeur principal */
        if (!st.ready) {
            /* envoi en cours de fin : les demandes vont au principal */
        } else if (ans[2] == 0x20 && ans[4] == 0x20) {
            if (ans[3] == 0 && st.srv_type != 0x20) start_serving(0x20);
            zero(ans + 2, 14); changed = 1;
        } else if (ans[2] == 0x01 && ans[4] == 0x00 && ans[6] == 0x07 && ans[8] == 0x00 && st.fiche_len) {
            /* (octet 8 = 02 : l'affichage demande la LISTE de l'ordinateur, après BROWSE ; cette
             * demande va au principal, qui répond par le paquet 0x10 / 06 — voir hd_send) */
            /* demande de la fiche du morceau : le principal ne la renvoie pas, on la sert modifiée */
            if (st.srv_type == 0) start_serving(0x10);
            zero(ans + 2, 14); changed = 1;
        } else if (ans[2] == 0x21 && ans[4] == 0x21) {
            if (ans[3] == 0 && st.srv_type != 0x21) start_serving(0x21);
            zero(ans + 2, 14); changed = 1;
        }
    }
    if (changed) wr16(ans + 0x2e, crc16(ans, 0x2e));
    return ORIG_CHECK(ans, a, b, c);
}

/* -------------------------------------------------- paquets vers l'affichage ---- */

static void edit_fiche(u8 *p, u32 len)   /* fiche du mode PC → fiche « morceau USB » */
{
    p[4] = 0x0a; p[18] = 0x54;
    seal(p, len);
}

/* Fiche « morceau USB » avec la tonalité devant le titre. La fiche est une suite de champs
 * (octet 16 = nombre) : type u16, 0000, longueur u16 en caractères, texte UTF-16 ; type 04 = titre.
 * Renvoie la longueur du paquet (multiple de 16, CRC compris) ; sans tonalité ou en cas de doute :
 * la fiche d'origine, seulement passée en contexte USB. */
static u32 make_fiche(u8 *dst, const u8 *src, u32 len)
{
    /* KEY_IN_TITLE = 0 : la tonalité est dessinée par l'affichage à la place du numéro de piste
     * (rgbwave.c) ; le préfixe dans le titre reste disponible */
    u32 pos = 18, n = src[16], ins = 0, end = 0, k = KEY_IN_TITLE && st.key_n ? st.key_n + 1u : 0;
    for (u32 i = 0; i < n && k; i++) {
        if (pos + 6 > len - 2) { k = 0; break; }
        u32 l = rd16(src + pos + 4);
        if (rd16(src + pos) == 0x04 && !ins) ins = pos;
        pos += 6 + 2 * l;
        if (pos > len - 2) { k = 0; break; }
    }
    end = pos;
    u32 total = (end + 2 * k + 2 + 15) & ~15u;
    if (!k || !ins || total > 896) { copy(dst, src, len); edit_fiche(dst, len); return len; }
    copy(dst, src, ins + 6);
    wr16(dst + ins + 4, rd16(src + ins + 4) + k);
    u32 o = ins + 6;
    for (u32 i = 0; i + 1 < k; i++) { dst[o++] = st.key[i]; dst[o++] = 0; }
    dst[o++] = ' '; dst[o++] = 0;
    copy(dst + o, src + ins + 6, end - (ins + 6));
    o += end - (ins + 6);
    zero(dst + o, total - o);
    edit_fiche(dst, total);
    return total;
}

static void put_time(u8 *p, u32 hf)      /* temps restant, au format de la routine USB */
{
    wr16(p + 10, hf / 9000);
    p[13] = hf % 9000 / 150;
    p[12] = hf % 150;
}

static u32 build_chunk(void)       /* fabrique le prochain morceau dans out[], renvoie sa taille */
{
    if (st.srv_type == 0x10) {
        copy(out, fbuf, st.fout_len);            /* préparée à l'annonce (même longueur) */
        return st.fout_len;
    }
    u8 t = st.srv_type;
    const u8 *src = t == 0x20 ? wave : grid;
    u32 len = t == 0x20 ? st.wave_len : st.grid_n * 4;
    static u8 mark[16];
    if (t == 0x20 && st.short20) {
        /* « HDCACHE\0 », nombre de points, empreinte (calculée de même par l'écran) */
        u32 n = (st.wave_len - 8) / 2, id = n, m = 2 * n < 4096 ? 2 * n : 4096;
        for (u32 i = 0; i < m; i++) id = id * 31 + wave[8 + i];
        static const u8 MK[8] = {'H', 'D', 'C', 'A', 'C', 'H', 'E', 0};
        copy(mark, MK, 8); wr32(mark + 8, n); wr32(mark + 12, id);
        src = mark; len = 16;
    }
    u32 hdr, room, off;
    zero(out, 896);
    out[0] = t; out[2] = st.srv_seq; out[3] = st.srv_seq >> 8;
    if (st.srv_seq == 1) {
        wr32(out + 6, len);
        /* durée : la même, octet pour octet, que dans les routines (l'affichage compare) */
        if (t == 0x20) { wr16(out + 10, st.dur_min); out[12] = (u8)dur_hf(); out[13] = (u8)dur_sec(); hdr = 14; }
        else           { wr16(out + 12, st.dur_min); out[14] = (u8)dur_hf(); out[15] = (u8)dur_sec(); hdr = 16; }
        off = 0;
    } else {
        hdr = 6;
        off = (t == 0x20 ? 880 : 878) + (u32)(st.srv_seq - 2) * 888;
    }
    room = 894 - hdr;
    if (t == 0x21) {
        /* grille : 2 octets nuls, puis 4 octets par temps (numéro 1..4, ms bits 16-23, ms bits
         * 0-15). La longueur annoncée (4 par temps) ne compte pas les 2 derniers octets, comme
         * dans les paquets d'un morceau USB (essai V12 : sans ces 2 octets de tête, le rouge
         * tombait sur le 2e temps et le premier trait était faux). */
        for (u32 i = 0; i < room && off + i < len + 2; i++)
            out[hdr + i] = off + i < 2 ? 0 : src[off + i - 2];
    } else if (off < len) copy(out + hdr, src + off, len - off < room ? len - off : room);
    seal(out, 896);
    return 896;
}

/* séquence d'un chargement USB (firmware d'origine, capture link-ram.cap, paquets 9502-9566) :
 * octets 3..57 de chaque routine et nombre de répétitions */
#define USBSEQ_N 7
/* la 5e forme (octet 37 = 88 : « redemander 04 et 05 ») durait 57 routines dans la capture USB,
 * le temps de lire la clé ; tenue aussi longtemps elle ne produit que des redemandes en chaîne
 * (environ 1 s perdue par chargement) : 4 routines suffisent */
static const u8 usbseq_rep[USBSEQ_N] = {1, 1, 2, 2, 4, 1, 1};
static const u8 usbseq[USBSEQ_N][55] = {
    {0x88, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x81, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x02, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x40, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00},
    {0x88, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x62, 0x22, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00, 0x02, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00},
    {0x88, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x62, 0x22, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x02, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc0, 0x40, 0x10, 0x01, 0x00, 0x1e, 0x00},
    {0x88, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0x00, 0x02, 0x06, 0x00, 0x00, 0x02, 0x00, 0x00, 0xff, 0xff, 0x62, 0x22, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0x12, 0x11, 0x02, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc0, 0x40, 0x10, 0x01, 0x00, 0x1e, 0x00},
    {0x88, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0x00, 0x02, 0x06, 0x00, 0x00, 0x02, 0x00, 0x00, 0xff, 0xff, 0x62, 0x22, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x12, 0x88, 0x02, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc0, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00},
    {0x88, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0x84, 0x02, 0x06, 0x00, 0x84, 0x02, 0x00, 0x00, 0x7f, 0x00, 0x62, 0x26, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x12, 0x99, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x40, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00},
    {0x89, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x84, 0x02, 0x06, 0x00, 0x84, 0x02, 0x00, 0x00, 0x7f, 0x00, 0x62, 0x26, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x12, 0x80, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x24, 0x08, 0x40, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00},
};

static int is_routine(const u8 *p, int len)
{
    return len == 64 && !p[0] && !p[1] && !p[2] && (p[3] & 0xf8) == 0x88;
}

/* Routine : octet 24 = mode (bit 0 à 1 : morceau de l'ordinateur ; 0x80 : support de la platine),
 * octet 8 = numéro de piste (00 : pas de morceau, ee / ff : chargement en cours).
 * (Attention : l'octet 8 n'est PAS la source ; il vaut ce que l'ordinateur envoie, 1..n.) */
/* Durée annoncée à l'affichage : minutes, secondes, demi-images (0..149), arrondie au-dessus
 * (l'affichage refuse une waveform plus longue que la durée) à partir de la durée reçue du Mac
 * (minutes, secondes × 256). */
static u32 dur_sec(void) { return st.dur_s256 >> 8; }
static u32 dur_hf(void)
{
    u32 h = ((u32)(st.dur_s256 & 0xff) * 150 + 255) / 256 + 1;
    return h > 149 ? 149 : h;
}
static int pc_mode(const u8 *p) { return p[24] & 0x01; }
static int has_track(const u8 *p) { return p[8] != 0x00 && p[8] != 0xee && p[8] != 0xff; }
/* Morceau de l'ordinateur chargé. Le numéro de piste ne le dit pas : Serato envoie celui des tags
 * du fichier, donc 0 pour la plupart des morceaux (session réelle du 13/11). Ce qui le dit :
 * mode 0x01 exactement (0x81 : connexion, 0x11 : rien de chargé), octet 8 hors ee / ff, et une
 * durée non nulle (octets 14-17 ; deck vide : zéro). */
static int pc_loaded(const u8 *p)
{
    return (p[24] & 0x91) == 0x01 && p[8] != 0xee && p[8] != 0xff && (p[14] | p[15] | p[16] | p[17]);
}

/* Routine du mode ordinateur → routine d'un morceau USB (o : copie de pkt, modifiée). */
/* ---- lissage de la position de lecture ----
 * Serato envoie la position environ 40 fois par seconde, par pas de 2 à 4 centièmes et à
 * intervalles irréguliers (5 à 50 ms, session du 16/11) : recopiée telle quelle dans la routine
 * (une toutes les 20 ms environ), la waveform avance par à-coups. La position transmise à
 * l'écran est donc interpolée : elle avance à vitesse constante (pente mesurée sur les derniers
 * changements) en restant deux routines derrière la dernière position reçue, sans jamais la
 * dépasser : à l'arrêt elle rejoint exactement la position de Serato, sans rebond. Un écart de
 * plus de 100 ms (saut, scratch rapide) recale d'un coup. Unité : demi-images (1/150 s). */
static u32 smooth_pos(u32 s)
{
    s32 S = (s32)(s << 8);
    st.sm_tick++;
    if (!st.sm_on) { st.sm_on = 1; st.sm_e = S; st.sm_v = 0; st.sm_s = s; st.sm_hn = 0; st.sm_still = 0; return s; }
    if (s != st.sm_s) {
        s32 d = (s32)s - (s32)st.sm_s;
        st.sm_s = s;
        /* à l'arrêt (position inchangée depuis plus de 6 routines), un déplacement (CUE tout proche,
         * molette, reprise) est pris tel quel : sous 15 demi-images il était rattrapé d'une
         * demi-image par routine, et la waveform glissait lentement à l'arrêt (V35) */
        /* V36 : de même pour un mouvement à contresens de la vitesse lissée (correction après une
         * pause, fin d'un aperçu CUE, molette) : sous 15 demi-images il était rattrapé pas à pas
         * (émulateur : 38554 → 38562 en 8 routines, l'écran oscillait autour) */
        if (st.sm_still > 6 || (st.sm_v > 0 && d < 0) || (st.sm_v < 0 && d > 0)) {
            st.sm_hn = 0; st.sm_still = 0; st.sm_v = 0; st.sm_e = S; return s;
        }
        if (d > 60 || d < -60) st.sm_hn = 0;      /* saut : historique oublié */
        st.sm_still = 0;
        if (st.sm_hn == 8) { for (u32 i = 0; i < 7; i++) { st.sm_ht[i] = st.sm_ht[i + 1]; st.sm_hs[i] = st.sm_hs[i + 1]; } st.sm_hn = 7; }
        st.sm_ht[st.sm_hn] = st.sm_tick; st.sm_hs[st.sm_hn] = (s32)s; st.sm_hn++;
        s32 m;
        u32 dt = st.sm_tick - st.sm_ht[0];
        if (st.sm_hn < 2 || !dt) m = d > 0 ? 768 : -768;             /* premier pas : vitesse normale */
        else {
            static const u16 inv[33] = {0, 4096, 2048, 1365, 1024, 819, 683, 585, 512, 455, 410, 372, 341, 315, 293,
                273, 256, 241, 228, 216, 205, 195, 186, 178, 171, 164, 158, 152, 146, 141, 137, 132, 128};
            if (dt > 32) dt = 32;
            m = (((s32)s - st.sm_hs[0]) * (s32)inv[dt]) >> 4;        /* × 256 / dt */
        }
        s32 dv = m - st.sm_v, av = st.sm_v < 0 ? -st.sm_v : st.sm_v;
        if (dv < 0) dv = -dv;
        if ((m ^ st.sm_v) < 0 || dv > av / 2 + 128) st.sm_v = m; else st.sm_v += (m - st.sm_v) / 8;
    } else if (st.sm_still < 255) st.sm_still++;
    s32 v = st.sm_v, err = S - st.sm_e;
    if (err > 15 * 256 || err < -15 * 256) { st.sm_e = S; return s; }
    /* cible : deux routines derrière la dernière position reçue, dans le sens du mouvement */
    s32 step = v + (err - 2 * v) / 24;
    /* arrêt (plus de 6 routines sans changement) : calé sur la position reçue (V36 ; avant, une
     * demi-image par routine, glissade visible) */
    if (st.sm_still > 6) { st.sm_v = 0; st.sm_e = S; return s; }
    s32 e = st.sm_e + step;
    if (step > 0) { if (e > S) e = S; }
    else if (step < 0) { if (e < S) e = S; }
    st.sm_e = e;
    return (u32)((e + 128) >> 8);
}

static void convert_routine(u8 *o, const u8 *pkt)
{
        /* temps en demi-images (150 par seconde), l'unité de l'affichage en mode USB :
         * routine USB : octets 10-11 minutes, 13 secondes, 12 demi-images (0..149) ;
         * routine du mode ordinateur : octets 10-11 minutes, 13 secondes, 12 centièmes (0..99) ;
         * durée reçue du Mac : minutes et secondes × 256 */
        u32 dur = (u32)st.dur_min * 9000 + dur_sec() * 150 + dur_hf();
        int loaded = pc_loaded(pkt);                       /* morceau de l'ordinateur chargé */
        u32 el = loaded ? (u32)rd16(pkt + 10) * 9000 + (u32)pkt[13] * 150 + ((u32)pkt[12] * 3 + 1) / 2 : 0;
        u32 remain = pkt[22] & 0x20;
        /* V36 : temps écoulé selon Serato (voir hd_hid), dans les deux modes d'affichage */
        int ser = loaded && st.spos_ok;
        if (ser) el = st.spos;
        if (loaded && !st.no_smooth) el = smooth_pos(el); else st.sm_on = 0;
        /* TIME MODE sur REMAIN (octet 22, bit 0x20) : le principal envoie déjà le temps restant
         * (essai V11 du 15/11 : pris pour du temps écoulé, la waveform défilait à l'envers) */
        /* L'affichage suit ce choix : temps restant (bit 0x20, libellé REMAIN) ou temps écoulé
         * (sans le bit) ; dans les deux cas il en déduit la position (essai V12). */
        u32 tm = el < dur ? el : dur;
        if (remain && ser) tm = dur > el ? dur - el : 0;
        else if (remain && loaded) {
            /* Le temps restant du principal est compté sur la durée de Serato (octets 14-17, en
             * centièmes), qui diffère de la nôtre (I Follow Rivers : 299,07 s contre 299,02 s).
             * L'écran en déduisant la position sur NOTRE durée, la tête était décalée d'autant
             * (essai V13 : 50 ms). On repasse par le temps écoulé. */
            u32 sd = (u32)rd16(pkt + 14) * 9000 + (u32)pkt[17] * 150 + ((u32)pkt[16] * 3 + 1) / 2;
            u32 e = sd > el ? sd - el : 0;
            tm = dur > e ? dur - e : 0;
        }
        if (st.seq_pending && loaded) { st.seq_pending = 0; st.seq_i = st.short20 ? 4 : 1; st.seq_k = 0; st.shown = 1; }
        if (st.seq_i) {
            /* 2) séquence de chargement USB (changement de morceau), avec la durée, le temps
             * restant et le BPM du morceau */
            copy(o + 3, usbseq[st.seq_i - 1], 55);
            if (o[8] == 0x01) { o[8] = loaded ? pkt[8] : 1; wr16(o + 14, st.dur_min); }
            if (st.seq_i >= 6) {
                put_time(o, tm);
                o[22] = (u8)((o[22] & ~0x20) | remain);
                o[16] = (u8)dur_hf(); o[17] = (u8)dur_sec();
                if (loaded) { o[20] = pkt[20]; o[21] = pkt[21]; o[9] = pkt[9]; }
            }
            o[23] = (u8)((o[23] & ~0x04) | st.quant);
            o[33] = (u8)st.zoom;
            if (++st.seq_k >= usbseq_rep[st.seq_i - 1]) {
                st.seq_k = 0;
                if (++st.seq_i > USBSEQ_N) st.seq_i = 0;
            }
        }
        /* 2a) chargement d'un morceau par l'ordinateur (mode ordinateur, octet 8 = ee, ff ou 00 :
         * vide) : routine « chargement en cours » d'un morceau USB, pour que l'affichage oublie le
         * morceau précédent et réclame la waveform du nouveau (sans cela, seul le premier
         * chargement après la mise sous tension la réclame) */
        else if (pc_mode(pkt) && !loaded) {
            static const u8 usb_ff[50] = {
                0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
                0x62, 0x22, 0x80, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x12, 0x00, 0x02, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
                0x00, 0xc0, 0x40, 0x00, 0x01, 0x00, 0x1e, 0x00};
            o[3] = 0x88;
            copy(o + 8, usb_ff, sizeof usb_ff);
        }
        /* 2b) morceau de l'ordinateur chargé : le décrire comme un morceau USB « avec waveform » */
        else if (loaded) {
            /* (octets 20-23 : BPM et tempo, 48 et 54 : temps de la mesure, envoyés par l'ordinateur
             * et gardés tels quels) */
            /* (octet 8 : numéro de piste de l'ordinateur, gardé) */
            /* (octet 50 : laissé à la valeur du mode ordinateur, 04 ; 08 affiche « MASTER » à la
             * place du libellé BPM ; octet 8 = ff : champ TRACK vide — il ne sait afficher que
             * des chiffres, et Serato y envoie le numéro des tags, 0 le plus souvent) */
            /* (octet 9 : centièmes de BPM, gardé — l'écran en affiche le chiffre des dixièmes ;
             * il était mis à zéro jusqu'à la version 18, d'où le « .0 » permanent) */
            static const u8 off[] = {3, 8, 24, 35, 36, 38, 49, 52};
            static const u8 val[] = {0x89, 0xff, 0x80, 0x02, 0x12, 0x02, 0x04, 0x40};
            for (u32 i = 0; i < sizeof off; i++) o[off[i]] = val[i];
            /* octets 48-49 : position du repère de l'overview, 0..399 sur 10 bits. Les bits hauts
             * étaient écrasés : au-delà de 256 px le repère repartait du début (essai V11) */
            o[49] = (u8)(0x04 | (pkt[49] & 0x03));
            o[23] = (u8)((o[23] & ~0x04) | st.quant);   /* QUANTIZE de Serato */
            o[33] = (u8)st.zoom;            /* niveau de zoom (8 bits signé, 0 au départ en USB) */
            put_time(o, tm);                /* temps écoulé ou restant, comme l'ordinateur l'envoie */
            if (st.refresh) { o[37] |= st.refresh; st.refresh = 0; }
            wr16(o + 14, st.dur_min); o[16] = (u8)dur_hf(); o[17] = (u8)dur_sec();              /* durée */
        }
}

static const u8 *edit_other(const u8 *pkt, int len);

int hd_send(const u8 *pkt, int len, int a, int b)
{
    init();
    st.n_send++;
    /* Vue demandée par le principal (touche BROWSE) : paquet 0x10, octet 4 = 05 (vue morceau) ou
     * 06 (liste de l'ordinateur). En vue liste le patch est suspendu : l'affichage doit être dans
     * son contexte « ordinateur » d'origine pour montrer la liste et rendre la main à la molette.
     * Au retour en vue morceau, la séquence de chargement USB est rejouée et la waveform revient. */
    if (pkt[0] == 0x10 && len >= 20 && pkt[4] == 0x06 && !st.susp) {
        u8 h = st.have;
        st.susp = 1;
        if (st.ready) go_passive();
        st.have = h;
    } else if (pkt[0] == 0x10 && len >= 20 && pkt[4] == 0x05 && st.susp) {
        st.susp = 0;
        if (st.have) {
            st.ready = 1; st.seq_pending = 1; st.seq_i = 0; st.shown = 0;
            st.got20 = 0; st.tries = 0; st.wd = 0;
            /* l'écran a encore la waveform (rgbwave.c, rgb_decode) : marque seulement (essai V25 :
             * retour de la liste aussi long qu'un chargement, toute la waveform était renvoyée) */
            st.short20 = st.delivered;
        }
    }
    if (pkt[0] == 0x10 && pkt[4] == 0x05 && len >= 20 && len <= 896) {   /* mémoriser la fiche PC */
        copy(fiche, pkt, len); st.fiche_len = len;
    }
    /* le patch se retire (comportement d'origine) :
     *  - morceau chargé depuis un support de la platine (mode 0x80 et numéro de piste), pour que le
     *    mode USB reste celui d'origine même après une session Serato ;
     *  - morceau de l'ordinateur déchargé après avoir été affiché (le suivant arrivera avec sa
     *    propre waveform, précédée de 30 00) */
    if (is_routine(pkt, len) && st.ready &&
        ((!pc_mode(pkt) && has_track(pkt)) || (st.shown && pc_mode(pkt) && !pc_loaded(pkt))))
        go_passive();
    if (!st.ready && !st.finish && !st.pend_ann && !st.held_len) return ORIG_SEND(pkt, len, a, b);

    /* Chaque routine peut porter une annonce (octet 58 = type, 60-61 = longueur en mots) : le
     * paquet annoncé part juste après, à la place de la routine suivante. Nous y mettons aussi nos
     * propres paquets (waveform, grille, fiche), à la place de routines. Une routine remplacée qui
     * portait une annonce du principal n'est pas perdue : elle part à la place du paquet annoncé,
     * qui part lui-même à la place de la routine suivante. Les accusés sont traduits (hd_check).
     * Sans cela, un paquet du principal non annoncé fait refuser la liaison à l'affichage
     * (erreur, E-8709, écran figé), dès que le principal parle pendant un envoi de waveform. */
    if (is_routine(pkt, len)) {
        copy(rout, pkt, 64);
        /* Garde-fou : si l'affichage n'a pas réclamé la waveform 3 s (180 routines) après la
         * séquence de chargement, on la rejoue (deux fois au plus), puis le patch se retire :
         * affichage d'origine du mode ordinateur plutôt qu'un écran sans waveform. */
        if (st.ready && st.shown && !st.seq_i && !st.got20 && ++st.wd > 180) {
            st.wd = 0;
            if (st.tries < 2) { st.tries++; st.seq_i = 1; st.seq_k = 0; }
            else go_passive();
        }
        /* fiche avec tonalité : seulement au calme, 2 s après le dernier envoi (waveform puis
         * grille se suivent ; s'intercaler entre les deux casse la liaison, E-8709) */
        if (st.srv_type || st.seq_i || !st.shown || pkt[58]) st.idle = 0;
        else if (st.idle < 1000) st.idle++;
        if (st.key_pending && st.ready && st.shown && st.fiche_len && st.got20 && st.idle > 120) {
            st.key_pending = 0; st.idle = 0;
            start_serving(0x10);
        }
        if (st.back_pulse) { rout[31] |= 0x80; st.back_pulse = 0; }
        if (st.ready) {
            convert_routine(rout, pkt);
            if (st.shown && !st.seq_i)
                for (u32 i = 0; i < st.dbg_n; i++) if (st.dbg_off[i] < 58) rout[st.dbg_off[i]] = st.dbg_val[i];
        }
        u8 *send = 0; u32 n = 0;
        if (st.held_len) {                          /* paquet du principal mis de côté */
            send = held; n = st.held_len; st.held_len = 0;
        } else if (st.srv_type && st.announced) {   /* notre paquet annoncé */
            n = build_chunk(); send = out;
            st.announced = 0; st.gap = 1;
            if (++st.srv_seq > st.srv_count) {
                if (st.srv_type == 0x20) { if (!st.short20) st.delivered = 1; st.short20 = 0; }
                st.srv_type = 0; st.finish = 0;
            }
        }
        if (send) {
            if (pkt[58]) { copy(pend, rout, 64); seal(pend, 64); st.pend_ann = 1; }
            st.ack_on = 1; st.ack_exp = 0x00; st.ack_act = send[0];
            return ORIG_SEND(send, n, a, b);
        }
        /* annonce de notre prochain paquet, si le principal n'annonce rien */
        if (st.srv_type && !pkt[58] && !st.pend_ann) {
            if (st.gap) st.gap--;
            /* waveform encore en cours d'arrivée : n'annoncer que ce qui est déjà là */
            u32 end = 880 + (st.srv_seq - 1) * 888;
            if (end > st.wave_len) end = st.wave_len;
            int wait = st.srv_type == 0x20 && st.partial && st.wave_rx < end;
            if (!st.gap && !wait) {
                u32 m = 896;
                if (st.srv_type == 0x10) m = st.fout_len = (u16)make_fiche(fbuf, fiche, st.fiche_len);
                rout[58] = st.srv_type == 0x10 ? 0x01 : st.srv_type; rout[59] = 0; wr16(rout + 60, m / 2);
                st.announced = 1;
            }
        }
        seal(rout, 64);
        return ORIG_SEND(rout, 64, a, b);
    }
    const u8 *x = pkt;
    if (st.ready) x = edit_other(pkt, len);
    if (st.pend_ann) {      /* sa routine d'annonce avait été remplacée : elle part d'abord */
        st.pend_ann = 0;
        copy(held, x, len); st.held_len = len;
        st.ack_on = 1; st.ack_exp = pkt[0]; st.ack_act = 0x00;
        return ORIG_SEND(pend, 64, a, b);
    }
    return ORIG_SEND(x, len, a, b);
}

/* Paquets du principal autres que les routines, modifiés quand le patch est actif. */
static const u8 *edit_other(const u8 *pkt, int len)
{
    /* petite waveform (overview) d'un morceau dont la waveform est en couleur : marque pour
     * l'affichage dans les octets hauts (indice de couleur, toujours 0 en mode ordinateur) des
     * colonnes 0..7 (tonalité, nombre de points, 'H' 'D'). Voir rgbwave.c. */
    if (pkt[0] == 0x04 && len == 816 && st.wave_len > 8 && wave[0] == 'H' && wave[1] == 'D' &&
        wave[2] == 'R' && wave[3] == 'G' && !(pkt[9] | pkt[11] | pkt[13] | pkt[15] | pkt[17] | pkt[19] | pkt[21] | pkt[23])) {
        u32 n = (st.wave_len - 8) / 2;
        copy(xbuf, pkt, len);
/* octets hauts des colonnes : 0..2 tonalité (3 caractères au plus, 0 sinon), 3..5 nombre
         * de points, 6..7 'H' 'D'. La marque est en DERNIER : l'affichage range les colonnes dans
         * l'ordre, et lisait parfois une marque dont la suite n'était pas encore arrivée
         * (tonalité perdue une fois sur trois dans l'émulateur). */
        for (u32 i = 0; i < 3; i++) xbuf[9 + 2 * i] = i < st.key_n ? st.key[i] : 0;
        xbuf[15] = n; xbuf[17] = n >> 8; xbuf[19] = n >> 16;
        for (u32 i = 0; i < 24; i++) xbuf[9 + 2 * (8 + i)] = st.cue_rgb[i];   /* colonnes 8..31 */
        if (st.adj) xbuf[9 + 2 * 31] = st.adj == 1 ? 'I' : 'O';   /* réglage en cours (30 16) */
        else if (st.grow) xbuf[9 + 2 * 31] = 'G'; /* IN posé : l'écran étend la boucle (30 15) */
        xbuf[21] = 'H'; xbuf[23] = 'D';
        seal(xbuf, len);
        return xbuf;
    }
    /* (un état « ordinateur » avec une routine « USB » fait redemander l'état à l'affichage en
     * boucle, jusqu'au blocage : il doit toujours être modifié tant que le patch est actif) */
    if (pkt[0] == 0x05 && len >= 16 && pkt[2] == 0x09) {         /* état du lecteur : source USB */
        copy(xbuf, pkt, len);
        /* durée : minutes (16 bits), demi-images 0..149, secondes — PAS secondes × 256. L'écran
         * en déduit la position (durée − temps restant) : avec × 256 la waveform était en avance
         * de 0 à 0,7 s selon le morceau (essai V6 du 14/11, MASTER_1 : 0,4 s). */
        xbuf[2] = 0x01; wr16(xbuf + 6, st.dur_min);
        xbuf[8] = (u8)dur_hf(); xbuf[9] = (u8)dur_sec();
        /* repères : enregistrements de 3 mots après celui de la durée (genre 0x00ff) : genre,
         * minutes, secondes << 8 | demi-images (notes-cue.md) */
        u32 n = 0;
        zero(xbuf + 10, len - 12);
        for (u32 i = 0; i < st.cue_n && 10 + 6 * (n + 1) <= len - 2; i++, n++) {
            u8 *r = xbuf + 10 + 6 * n;
            u32 hf = st.cue_hf[i];
            wr16(r, st.cue_kind[i]); wr16(r + 2, hf / 9000);
            r[4] = (u8)(hf % 150); r[5] = (u8)(hf % 9000 / 150);
        }
        xbuf[2] = (u8)(1 + n);
        seal(xbuf, len);
        return xbuf;
    }
    if (pkt[0] == 0x10 && len >= 20 && len <= 896 && pkt[4] == 0x05) {   /* fiche : contexte USB */
        copy(xbuf, pkt, len);
        edit_fiche(xbuf, len);
        if (st.key_n && KEY_IN_TITLE) st.key_pending = 1;      /* la version avec tonalité suit, à sa longueur */
        return xbuf;
    }
    return pkt;
}
