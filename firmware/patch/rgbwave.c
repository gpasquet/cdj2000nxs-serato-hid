/* Grande waveform en couleur (RGB) pour le programme d'affichage (Blackfin BF531) du CDJ-2000NXS 1.44.
 *
 * Trois appels du programme d'origine sont redirigés ici :
 *   - 0xD0F538 : décodage de la waveform reçue (paquets 0x20) → rgb_decode(longueur)
 *   - 0xD2F3B6 : tracé d'une colonne de la grande waveform   → rgb_column(x, i0, i1, a4, mode)
 *   - 0xD3058A : tracé de la petite waveform (overview)      → rgb_overview()
 *
 * Format couleur (envoyé par le patch du processeur principal en mode ordinateur) : le tampon des
 * paquets 0x20 commence par les 8 octets MAGIC, suivis de 2 octets par point au format PWV5 de
 * rekordbox (16 bits gros-boutiste : rouge 15-13, vert 12-10, bleu 9-7, hauteur 6-2).
 * Sans ces 8 octets (mode USB, données PWV3 d'origine), tout se passe comme dans le firmware
 * d'origine.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define WAVE_BUF   ((u8 *)0x01A65168)      /* tampon des paquets 0x20 (900 000 octets) */
#define WAVE_LEN   (*(volatile u32 *)0x006D413C)   /* longueur annoncée par l'en-tête 0x20 */
#define POINTS     ((u8 *)0x01015180)      /* table des points : 12 octets par point (déplacée, V33) */
#define COLOR_TAB  ((const u32 *)0x00CD3928)   /* 8 couleurs RGB565 d'origine (indice = octet 1) */
#define IMAGE      ((u16 *)0x01000000)     /* image 400 × 90 RGB565 de la grande waveform */
#define IMG_W      400
#define BASE_ROW   44                      /* ligne de base : la colonne monte à partir d'ici */
#define BUF_MAX    900000u

#define ORIG_DECODE ((void (*)(u32))0x00D2F51C)
#define ORIG_COLUMN ((void (*)(int, int, int, int, int))0x00D2E230)

#define RGB_INDEX  5                       /* indice de couleur d'origine utilisé pour nos points */
#define RGB_MAX    ((BUF_MAX - 8) / 2)     /* 449 996 points = 50 min à 150 points/s */
/* Couleurs RGB555, une par point. La table des points prévoit 900 000 entrées (12 octets) et les
 * entrées ne servent que jusqu'à la longueur du morceau : pour un morceau en couleur (≤ RGB_MAX
 * points) les 900 Ko de la fin de la table sont libres. (Les octets 3..11 des entrées servent aux
 * repères de cue : ils ne peuvent pas porter la couleur.) */
#define COLORS_FAR ((u16 *)(0x01011940 + 900000 * 12 - RGB_MAX * 2))   /* (adresse inchangée : la table déplacée finit au même endroit) */
/* Couleurs des points : dans le tampon de réception, juste après les données (V29). À la fin de
 * la table des points (COLORS_FAR) elles étaient écrasées pendant la vue liste, ce qui empêchait
 * de réutiliser la waveform au retour (barres noires). COLORS_FAR seulement au-delà de 25 min. */
static u16 *colors_p __attribute__((section(".data"))) = 0;
#define COLORS     colors_p

static const u8 MAGIC[8] = {'H', 'D', 'R', 'G', 'B', '5', 0x00, 0x01};

/* nombre de points en couleur du morceau chargé (0 : morceau d'origine). En .data : initialisé
 * par le chargement du programme. */
static u32 rgb_points __attribute__((section(".data"))) = 0;

static const u8 L5[8] = {0, 4, 9, 13, 18, 22, 27, 31};     /* 3 bits → 5 bits */

/* Les images de l'affichage sont en RGB555 (rouge 14-10, vert 9-5, bleu 4-0), PAS en RGB565 :
 * établi par une mire (HD_MIRE) le 14/11 — en 565, l'orange sortait jaune et le violet bleu.
 * Le bit 15 est repris du pixel ou de la couleur d'origine. */
static u16 rgb555(u32 r3, u32 g3, u32 b3)
{
    return (u16)(L5[r3] << 10 | L5[g3] << 5 | L5[b3]);
}

/* ---- petite waveform (overview) ----
 * Les 400 colonnes reçues (paquet 0x04) sont rangées en 0x600CE2 : u16 = hauteur (octet bas) +
 * indice de couleur 0..5 (octet haut, toujours 0 en mode ordinateur). La fonction d'origine
 * 0xD2F7C6 les trace dans une image RGB565 de 400 × 27 (0x01A63924 = ligne du bas, colonne 0 ;
 * on monte de 400 pixels par ligne ; pixels hors waveform = 0), puis 0xD30546 l'affiche.
 * Marque posée par le patch du processeur principal en mode ordinateur, quand la waveform est en
 * couleur : octets hauts des colonnes 0..7 = tonalité (3), nombre de points (24 bits), 'H', 'D'. Elle permet de
 * ne colorer l'overview qu'avec les couleurs du même morceau (l'overview arrive avant la grande
 * waveform). Sans marque (mode USB), tout se passe comme dans le firmware d'origine. */
#define OV_COLS    ((u16 *)0x00600CE2)
#define OV_IMG     ((u16 *)0x01A63924)
#define OV_W       400
#define OV_ROWS    27
#define ORIG_OV_DRAW   ((void (*)(void))0x00D2F7C6)
#define ORIG_OV_UPDATE ((int (*)(void))0x00D30546)    /* trace l'overview et l'affiche */

static u32 ov_n __attribute__((section(".data"))) = 0;    /* points annoncés par la marque */
static u8 ov_grow __attribute__((section(".data"))) = 0; /* IN posé : boucle étendue jusqu'à la tête (colonne 31 = 'G') */
static u8 ov_adj __attribute__((section(".data"))) = 0;  /* réglage IN (1) / OUT (2) en cours (colonne 31 = 'I' / 'O') */
static u16 cue_col[8] __attribute__((section(".data"))) = {0};   /* couleurs RGB555 des repères */
static int wc_x __attribute__((section(".data"))) = 0;          /* dernière colonne tracée … */
static int wc_i0 __attribute__((section(".data"))) = 0;         /* … son premier point … */
static int wc_ppc __attribute__((section(".data"))) = 0;        /* … et points par colonne */
static u32 ov_sum __attribute__((section(".data"))) = 0;  /* empreinte des colonnes marquées */
static u32 ov_redraw __attribute__((section(".data"))) = 0;   /* overview à redessiner en couleur */

/* Overview d'un morceau USB analysé par rekordbox : indices de couleur (octets hauts) au-delà
 * des 8 colonnes de la marque. En mode ordinateur ils sont tous nuls. Sans ce test, l'overview
 * d'un morceau USB chargé après Serato gardait les couleurs et la tonalité du morceau précédent
 * jusqu'à l'action suivante (essai V11). */
static u32 ov_indexed(void)
{
    for (u32 k = 32; k < OV_W; k++) if (OV_COLS[k] >> 8) return 1;     /* 8..31 : couleurs des repères */
    return 0;
}

static u32 ov_checksum(void)
{
    u32 s = 0;
    for (u32 k = 0; k < OV_W; k++) s = s * 31 + OV_COLS[k];
    return s;
}

/* couleur d'une colonne : moyenne des couleurs de ses points, pondérée par le carré de leur
 * hauteur (les coups forts dominent, comme dans l'overview de Serato), puis resaturée (la moyenne
 * de teintes différentes tire sur le pastel) et ramenée à une luminosité fixe */
static u16 ov_color(u32 lo, u32 hi)
{
    u32 r = 0, g = 0, b = 0, w;
    for (u32 k = lo; k < hi; k++) {
        u32 c = COLORS[k];
        w = POINTS[k * 12] & 0x1f;
        w = w * w + 1;
        r += (c >> 10 & 31) * w; g += (c >> 5 & 31) * w; b += (c & 31) * w;
    }
    u32 m = r, mn = r;
    if (g > m) m = g;
    if (b > m) m = b;
    if (g < mn) mn = g;
    if (b < mn) mn = b;
    if (!m) return 0x0421;
    mn = mn * 3 / 4;                       /* on retire les trois quarts de la part grise */
    m -= mn;
    r = (r - mn) * 27 / m; g = (g - mn) * 27 / m; b = (b - mn) * 27 / m;
    return (u16)(r << 10 | g << 5 | b);
}

/* ---- tonalité à la place du numéro de piste ----
 * Le bandeau du bas est recomposé à chaque image : chaque ligne de chaque élément est copiée vers
 * l'écran (RGB555, 480 pixels par ligne à 0x658C88) par la routine de copie 0xD49394. Elle est détournée ici (rgb_memcpy) : après la copie d'une ligne qui couvre les chiffres du champ TRACK (x 48..79,
 * y 157..182), la zone est noircie et la tonalité y est dessinée (police de keyfont.h).
 * La tonalité arrive par la marque de l'overview (octets hauts des colonnes 0..2), posée par le
 * patch du processeur principal ; sans marque, tout est d'origine. */
#define SCREEN     ((u8 *)0x00658C88)
#define SCREEN2    ((u8 *)0x006966C8)      /* tampon de composition (SCREEN + 263 lignes) */
#define ORIG_BLIT  ((void (*)(u8 *, const u8 *, u32))0x00D49394)
/* Profileur (mesures de fluidité) : temps passé dans chaque partie, au compteur de cycles du
 * processeur (CYCLES, activé par SYSCFG.CCEN). prof[i] = {cumul, maximum, nombre}. Lu au débogueur
 * dans l'émulateur, ou affiché à l'écran (prof_show) quand le Mac le demande (HD_DEBUG). */
typedef struct { unsigned acc, max, n; } prof_t;
prof_t prof[20] __attribute__((section(".data"))) = {{0, 0, 0}};
static inline unsigned cyc_now(void)
{
    unsigned c, f;
    __asm__ volatile("%0 = SYSCFG;" : "=d"(f));
    if (!(f & 2)) { f |= 2; __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(f)); }
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}
/* mesures seulement en mode HD_DEBUG : sur la platine, le profileur lui-même coûtait (V29 : notre
 * code mesuré à 5 M cycles par image, plus que le dessin d'origine) */
static u8 prof_on __attribute__((section(".data"))) = 0;
static inline unsigned prof_t0(void) { return prof_on ? cyc_now() : 0; }
static inline void prof_add(unsigned i, unsigned t0)
{
    if (!prof_on) return;
    unsigned d = cyc_now() - t0;
    prof[i].acc += d; prof[i].n++;
    if (d > prof[i].max) prof[i].max = d;
}
static void key_paint(void);
static void tall_apply(void);
static u32 fcount __attribute__((section(".data"))) = 0;   /* dessins de la grande waveform */
static void hdr_pre_i(void);
void hdr_pre(void) { unsigned t = prof_t0(); hdr_pre_i(); prof_add(6, t); }
#define KEY_X      48
#define KEY_W      36
#define KEY_Y      157
#define KEY_H      26
#define KEY_TOP    1            /* les lettres (24 lignes) commencent à la ligne KEY_Y + 1 */
static u8 key_n __attribute__((section(".data"))) = 0;
static u8 key_c[3] __attribute__((section(".data"))) = {0, 0, 0};

#include "keyfont.h"

static int glyph(u8 c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'G') return 10 + c - 'A';
    if (c >= 'a' && c <= 'g' && c != 'b') return 10 + c - 'a';
    if (c == 'm') return 17;
    if (c == 'b') return 18;
    if (c == '#') return 19;
    return -1;
}

/* Écrit le pixel (x, relatif au champ) s'il est dans la portion [x0, x1[ de la ligne */
#define PUT(base, x, v) do { u32 ax_ = (base) + (x); if (ax_ >= x0 && ax_ < x1) line[ax_] = (v); } while (0)

/* Dessine la part du champ (libellé y 148..152, lettres y 157..182) qui tombe sur la ligne y,
 * entre les colonnes x0 et x1. line = colonne 0 de cette ligne d'écran. */
static void key_row(u16 *line, u32 y, u32 x0, u32 x1)
{
    if (y < 153) {                                   /* libellé « TRACK » remplacé par « KEY » */
        static const u8 KEY5[5][3] = {{0x12,0x1e,0x11}, {0x14,0x10,0x0a}, {0x18,0x1c,0x04},
                                      {0x14,0x10,0x04}, {0x12,0x1e,0x04}};
        for (u32 x = 0; x < 34; x++) PUT(47, x, 0);
        for (u32 i = 0; i < 3; i++)
            for (u32 b = 0; b < 5; b++)
                if (KEY5[y - 148][i] & (0x10u >> b)) PUT(47, i * 7 + b, 0x7fff);
        return;
    }
    if (y < KEY_Y) return;
    for (u32 x = 0; x < KEY_W; x++) PUT(KEY_X, x, 0);
    y -= KEY_Y;
    if (y < KEY_TOP || y >= KEY_TOP + 24) return;
    y -= KEY_TOP;
    /* 2 caractères : police large de 14 px, aux positions des deux chiffres d'origine (pas de
     * 18) ; 3 caractères : police étroite de 10 px, pas de 12 */
    u32 w = key_n <= 2 ? 14 : 10, pitch = key_n <= 2 ? 18 : 12;
    for (u32 i = 0; i < key_n; i++) {
        int g = glyph(key_c[i]);
        if (g < 0) continue;
        u32 bits = key_n <= 2 ? FONT_L[g][y] : FONT_E[g][y];
        for (u32 b = 0; b < w; b++)
            if (bits & (1u << (w - 1 - b))) PUT(KEY_X, i * pitch + b, 0x7fff);
    }
}

/* Le bandeau n'est recomposé que lorsqu'un de ses éléments change (platine à l'arrêt : jamais).
 * À l'arrivée d'une tonalité, elle est donc aussi dessinée tout de suite, directement à l'écran. */
static void key_paint_i(void)
{
    u16 *line = (u16 *)(SCREEN + 960 * 148), *line2 = (u16 *)(SCREEN2 + 960 * 148);
    for (u32 y = 148; y < KEY_Y + KEY_H; y++, line += 480, line2 += 480) {
        key_row(line, y, 0, 480);
        key_row(line2, y, 0, 480);
    }
}

/* ---- grande waveform étirée en hauteur ----
 * L'écran a deux tampons RGB555 (SCREEN et SCREEN2, 960 octets par ligne) affichés à tour de
 * rôle. À chaque image, l'interface recopie dans le tampon à composer les lignes 53 à 142 de
 * l'autre, puis y pose l'image de la grande waveform (IMAGE, 400 × 90) ligne par ligne : chaque
 * ligne (406 pixels, colonnes 38 à 443 : 2 noirs, les 400 de l'image, 4 noirs) passe par un
 * tampon temporaire puis par la routine de copie générale. Pour un morceau de l'ordinateur,
 * la destination de ces copies est redirigée (rgb_dst) : les 90 lignes sont étalées sur 122
 * (lignes 21 à 142, jusque sous le bandeau, à la place du phase meter), une ligne sur trois
 * environ étant doublée (rgb_after). Grille, repères, boucle et tête de lecture font partie de
 * l'image et suivent.
 * À l'arrêt l'interface ne recompose rien, et elle ne restaure sous une fenêtre que la zone
 * d'origine : à chaque changement d'état (fenêtre ouverte ou fermée, morceau USB, liste) et
 * après chaque conversion du calque, la zone est donc refaite ici d'après IMAGE (tall_apply).
 * Le calque 8 bits (OVL) est affiché OY lignes plus bas que ses propres coordonnées. */
#define OVL    ((u8 *)0x00000028)
#define OY     3
static u8 ph_hide __attribute__((section(".data"))) = 0;
static u8 ph_soft __attribute__((section(".data"))) = 0;
u8 tall_opt __attribute__((section(".data"))) = 1;      /* 0 : waveform de hauteur d'origine */
static u8 dbg_on __attribute__((section(".data"))) = 0;  /* mesures affichées dans le bandeau (HD_DEBUG) */
static u8 tall_live __attribute__((section(".data"))) = 0;   /* étirement en cours (vue morceau Serato) */
static u8 tall_todo __attribute__((section(".data"))) = 0;   /* passages de rgb_time où refaire la zone */
static u8 pose_seen __attribute__((section(".data"))) = 0;   /* une image a été posée depuis rgb_time */
/* colonne de fin de l'étirement : 444 (toute la largeur) ou 227 (fenêtre MENU « LOAD PREVIOUS
 * TRACK » sur la moitié droite : seule la partie gauche est étirée, la fenêtre reste en place) */
static u32 tall_x1 __attribute__((section(".data"))) = 444;
/* ---- pleine largeur (V33) ----
 * Pour un morceau de l'ordinateur, l'image de la grande waveform passe de 400 à 480 colonnes
 * (toute la largeur de l'écran, tête de lecture au centre, colonne 240) : les constantes du dessin
 * d'origine sont basculées à l'exécution (geo_set), et la pose étirée prend ses lignes directement
 * dans l'image (le calque de la waveform reste celui d'origine, 406 × 90 en colonne 38). En pleine
 * largeur, colonne d'écran = colonne de l'image. La marge noire de gauche et la molette de zoom
 * de droite sont recouvertes. Un morceau USB remet les constantes d'origine. */
static u8 fw __attribute__((section(".data"))) = 0;          /* 480 colonnes en cours */
static u8 fw_opt __attribute__((section(".data"))) = 1;      /* HD_LARGE=0 : 400 colonnes */
static u8 col_ok __attribute__((section(".data"))) = 0;      /* couleurs pour le morceau à l'écran */
static u32 g_imgw __attribute__((section(".data"))) = 400;   /* largeur de l'image, en pixels */
static u32 g_c0 __attribute__((section(".data"))) = 38;      /* colonne d'écran du début des lignes posées */
static u32 g_lw __attribute__((section(".data"))) = 406;     /* pixels des lignes posées */
static int g_x0 __attribute__((section(".data"))) = 40;      /* colonne d'écran de la colonne 0 de l'image */
static u32 g_xe __attribute__((section(".data"))) = 444;     /* fin de l'étirement sur toute la largeur */
static u8 *tall_half_dst __attribute__((section(".data"))) = 0;   /* ligne en cours (moitié) */
static u8 tall_half_y __attribute__((section(".data"))) = 0;
static u32 tall_line[0x32c / 4] __attribute__((section(".data")));  /* ligne interceptée */
static const u8 TALL_FIRST[90] = {21, 23, 24, 26, 27, 28, 30, 31, 32, 34, 35, 36, 38, 39, 40, 42, 43, 45, 46, 47, 49, 50, 51, 53, 54, 55, 57, 58, 59, 61, 62, 64, 65, 66, 68, 69, 70, 72, 73, 74, 76, 77, 78, 80, 81, 82, 84, 85, 87, 88, 89, 91, 92, 93, 95, 96, 97, 99, 100, 101, 103, 104, 106, 107, 108, 110, 111, 112, 114, 115, 116, 118, 119, 120, 122, 123, 125, 126, 127, 129, 130, 131, 133, 134, 135, 137, 138, 139, 141, 142};
static const u8 TALL_CNT[90] = {2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1};
static u32 ph_limit(void);
static void cue_line(u8 *dst, u32 n);
#define MODE_RX  (*(volatile u8 *)0x006003F8)
/* Copie des 23 premières lignes de l'image posée (lignes d'écran 21 à 52 de l'étirement), en
 * mémoire cachée (V32). La recopie V24 de ces lignes depuis l'autre tampon d'écran relisait
 * 6 500 mots d'écran à chaque image (écran non caché, plusieurs centaines de cycles par lecture
 * sur la platine) ; elle les prend désormais ici. Zone 0xFE0000 : libre (jamais écrite pendant une
 * session complète dans l'émulateur ; LNNRT y loge du code sur la platine). topc_m : lignes
 * à jour depuis la dernière pose (bit y) ; toute autre écriture de ces lignes à l'écran (redessin
 * d'après IMAGE) efface son bit, et la recopie reprend alors l'autre tampon. */
#define TOPC   ((u32 *)0x00FE0000)
#define TOPC_S 240                          /* mots par ligne de la copie (480 pixels au plus) ; 90 lignes */
/* lignes de l'image à jour (bit y % 30 de topc_m[y / 30]) */
static u32 topc_m[3] __attribute__((section(".data"))) = {0, 0, 0};
#define TOPC_SET(y) (topc_m[(y) / 30] |= 1u << ((y) % 30))
#define TOPC_CLR(y) (topc_m[(y) / 30] &= ~(1u << ((y) % 30)))
#define TOPC_TOP()  ((topc_m[0] & 0x7fffff) == 0x7fffff)          /* lignes 0 à 22 */
#define TOPC_ALL()  ((topc_m[0] & topc_m[1] & topc_m[2]) == 0x3fffffff)
/* HD_FLUIDE (octet 6 de l'en-tête, bits 2-3, valeur + 1) : 0 recopies depuis l'autre tampon
 * d'écran (v31) ; 1 (défaut) les 32 lignes du haut depuis la copie ; 2 toute la zone étirée (les
 * 90 lignes de l'image passent par la copie ; la recopie d'origine des lignes 53 à 142 aussi) */
static u8 copy_mode __attribute__((section(".data"))) = 1;
/* ligne de l'image affichée à chaque ligne d'écran 21 à 142 de l'étirement */
static const u8 TALL_ROW[122] = {0, 0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 8, 9, 10, 11, 11, 12, 13, 14, 14, 15, 16, 16, 17, 18, 19, 19, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 28, 28, 29, 30, 30, 31, 32, 33, 33, 34, 35, 36, 36, 37, 38, 39, 39, 40, 41, 42, 42, 43, 44, 45, 45, 46, 47, 47, 48, 49, 50, 50, 51, 52, 53, 53, 54, 55, 56, 56, 57, 58, 59, 59, 60, 61, 61, 62, 63, 64, 64, 65, 66, 67, 67, 68, 69, 70, 70, 71, 72, 73, 73, 74, 75, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 84, 85, 86, 87, 87, 88, 89};
static void cue_fix(u16 *line);
static u8 *buf_of(const u8 *p)
{
    if ((u32)(p - SCREEN) < 960 * 263) return SCREEN;
    if ((u32)(p - SCREEN2) < 960 * 272) return SCREEN2;
    return 0;
}
static void cp32(u32 *d, const u32 *s, u32 n) { for (u32 k = 0; k < n; k++) d[k] = s[k]; }
static u8 *rgb_dst_i(u8 *dst, const u8 *src, u32 n)
{
    if (!tall_live && !fw) return dst;
    u8 *b = buf_of(dst);
    if (!b) return dst;
    u32 nw = g_lw / 2, full = tall_x1 == g_xe;
    if (buf_of(src)) {                                      /* copie d'un tampon à l'autre : inchangée, */
        /* sauf la recopie des lignes 53 à 142 depuis l'autre tampon, faite à chaque image avant d'y
         * poser la waveform : les lignes 21 à 52 (haut de la waveform étirée) la suivent, sinon le
         * haut garde une image de retard sur le reste jusqu'à la pose (essai V23 : haut en retard
         * qui clignote) */
        if (!tall_live) return dst;
        u8 *sb = buf_of(src);
        u32 rd = (u32)(dst - b);
        if (((copy_mode == 2 && full) || fw) && n == 0x32c && sb != b && rd == (u32)(src - sb) &&
            rd >= 960 * 53 + 76 && rd < 960 * 143) {
            u32 r = 53;
            while (rd >= 960 * (r + 1)) r++;
            if (rd == 960 * r + 76) {
                /* recopie d'origine, ligne par ligne : servie depuis la copie (la ligne 53 entraîne
                 * aussi les lignes 21 à 52) ; la copie d'origine est sautée. En pleine largeur, les
                 * lignes entières (l'origine ne recopie que les colonnes 38 à 443) */
                int cache = full && TOPC_ALL();
                for (u32 y = r == 53 ? 21 : r; y <= r; y++)
                    cp32((u32 *)(b + y * 960 + g_c0 * 2),
                         cache ? TOPC + TALL_ROW[y - 21] * TOPC_S : (const u32 *)(sb + y * 960 + g_c0 * 2), nw);
                return 0;
            }
        }
        if (rd == 960 * 53 + 76 && src - sb == 960 * 53 + 76 && n == 0x32c && sb != b) {
            if (copy_mode && full && TOPC_TOP())
                for (u32 y = 0; y < 23; y++)
                    for (u32 j = 0; j < TALL_CNT[y]; j++)
                        cp32((u32 *)(b + (TALL_FIRST[y] + j) * 960 + g_c0 * 2), TOPC + y * TOPC_S, nw);
            else
                for (u32 y = 21; y < 53; y++)
                    cp32((u32 *)(b + y * 960 + g_c0 * 2), (const u32 *)(sb + y * 960 + g_c0 * 2), nw);
        }
        return dst;
    }
    u32 r = (u32)(dst - b);
    if (r < 960 * 53 || r >= 960 * 143) return dst;
    u32 y = 0;
    r -= 960 * 53;
    while (r >= 960) { r -= 960; y++; }
    if (n != 0x32c || r != 76) { tall_todo = 2; prof[12].n++; return dst; }   /* copie partielle : zone à refaire */
    if (!tall_live) {
        /* pleine largeur, fenêtre ouverte : ligne à sa place d'origine (colonnes 38 à 443), prise
         * dans l'image de 480 colonnes au même endroit (le calque n'en a qu'un morceau décalé) */
        cp32((u32 *)dst, (const u32 *)((const u8 *)IMAGE + y * 960 + 76), 0x32c / 4);
        if (y >= 3 && y < 8) cue_line(dst, 0x32c);
        return 0;
    }
    pose_seen = 1;
    if (tall_x1 < g_xe) {          /* moitié gauche étirée (fenêtre MENU à droite) */
        if (!fw) { tall_half_dst = b; tall_half_y = (u8)y; return (u8 *)tall_line; }
        const u16 *s = IMAGE + y * 480;
        for (u32 k = 0; k < TALL_CNT[y]; k++) {
            u16 *d = (u16 *)(b + (TALL_FIRST[y] + k) * 960);
            for (u32 x = 0; x < tall_x1; x++) d[x] = s[x];
            if (y >= 3 && y < 8) cue_line((u8 *)d, 2 * tall_x1);
        }
        u16 *d = (u16 *)(b + (53 + y) * 960);         /* moitié droite à sa ligne d'origine */
        for (u32 x = tall_x1; x < 444; x++) d[x] = s[x];
        return 0;
    }
    const u32 *s = fw ? (const u32 *)((const u8 *)IMAGE + y * 960) : (const u32 *)src;
    u8 *d = b + TALL_FIRST[y] * 960 + g_c0 * 2;
    if (!fw && ((u32)src & 3)) return d;
    if (fw || (copy_mode && (y < 23 || copy_mode == 2))) {
        /* la ligne passe par la copie en mémoire cachée (triangles des repères recolorés là, sans
         * relire l'écran), puis vers sa ou ses lignes d'écran (V32 ; toujours en pleine largeur) */
        u32 *c = TOPC + y * TOPC_S;
        cp32(c, s, nw);
        if (y >= 3 && y < 8) cue_fix((u16 *)c);
        for (u32 j = 0; j < TALL_CNT[y]; j++) cp32((u32 *)(d + j * 960), c, nw);
        TOPC_SET(y);
        return 0;
    }
    if (TALL_CNT[y] == 2) {
        /* ligne doublée (V31) : copiée ici vers ses deux lignes d'écran en une seule lecture de la
         * source (la copie d'origine puis notre recopie la lisaient deux fois) ; rgb_memcpy saute
         * alors la copie d'origine et rgb_after (rien d'autre à y faire pour ces lignes) */
        u32 *d1 = (u32 *)d, *d2 = (u32 *)(d + 960);
        for (u32 k = 0; k < nw; k++) { u32 v = s[k]; d1[k] = v; d2[k] = v; }
        if (y >= 3 && y < 8) { cue_line(d, 2 * g_lw); cue_line(d + 960, 2 * g_lw); }
        return 0;
    }
    return d;
}
/* moitié gauche étirée, moitié droite (sous la fenêtre) à sa ligne d'origine */
static void tall_half_put(void)
{
    u8 *b = tall_half_dst; u32 y = tall_half_y;
    tall_half_dst = 0;
    const u16 *s = (const u16 *)tall_line;
    u32 nl = tall_x1 - 38;
    for (u32 k = 0; k < TALL_CNT[y]; k++) {
        u16 *d = (u16 *)(b + (TALL_FIRST[y] + k) * 960 + 76);
        for (u32 x = 0; x < nl; x++) d[x] = s[x];
        if (y >= 3 && y < 8) cue_line((u8 *)d, 2 * nl);
    }
    u16 *d = (u16 *)(b + (53 + y) * 960 + 76);
    for (u32 x = nl; x < 406; x++) d[x] = s[x];
}
/* lignes d'image [y0, y1[ étirées dans un tampon (0 : les deux). V35 : chaque ligne de l'image
 * (mémoire non cachée) est lue une seule fois, par mots, dans tall_stage, puis recopiée vers ses
 * lignes d'écran des deux tampons ; avant, elle était relue pixel par pixel pour chaque ligne
 * d'écran et chaque tampon (117 000 lectures lentes pour un redessin complet, plus d'une image
 * de temps, plusieurs fois pendant la reconstruction de l'écran au chargement). */
static u32 tall_stage[TOPC_S] __attribute__((section(".data")));
static void tall_draw(u8 *buf, u32 y0, u32 y1)
{
    u16 *st = (u16 *)tall_stage;
    for (u32 y = y0; y < y1; y++) {
        TOPC_CLR(y);                               /* l'écran n'est plus la dernière pose */
        const u32 *s = (const u32 *)(IMAGE + y * g_imgw);
        u32 np, off;                               /* pixels recopiés, colonne de départ */
        if (fw) { np = tall_x1; off = 0; }         /* pleine largeur : colonne = colonne de l'image */
        else {
            u32 xe = tall_x1 - 38;                 /* 406, ou 189 sous la fenêtre MENU */
            st[0] = 0; st[1] = 0;
            u32 n = xe - 2 < IMG_W ? xe - 2 : IMG_W;
            cp32(tall_stage + 1, s, (n + 1) / 2);
            if (xe == 406) { st[402] = 0; st[403] = 0; st[404] = 0; st[405] = 0; }
            np = xe; off = 76;
        }
        if (fw) cp32(tall_stage, s, (np + 1) / 2);
        if (y >= 3 && y < 8) cue_fix(st);          /* triangles : couleurs Serato, sans relire l'écran */
        for (u32 k = 0; k < TALL_CNT[y]; k++) {
            u32 r = (TALL_FIRST[y] + k) * 960 + off;
            for (u32 b = 0; b < 2; b++) {
                u8 *bb = b ? SCREEN2 : SCREEN;
                if (buf && buf != bb) continue;
                u32 *d = (u32 *)(bb + r);
                cp32(d, tall_stage, np / 2);
                if (np & 1) ((u16 *)d)[np - 1] = st[np - 1];
            }
        }
    }
}
/* disposition d'origine, là où le calque est transparent : rien au-dessus de la ligne 53 ; en
 * dessous, selon la vue : 0 rien n'est touché (liste : l'interface refait tout), 1 du noir
 * (fenêtre par-dessus la vue morceau : INFO, UTILITY… — l'interface ne repose l'image que là où
 * elle la montre, et le fond noir de UTILITY est transparent dans le calque), 2 l'image (vue
 * morceau, ou fenêtre MENU qui n'en couvre que la moitié droite) */
static void flat_draw(u8 *buf, u32 below)
{
    for (u32 y = 21; y < (below ? 143u : 53u); y++) {
        const u8 *o = OVL + (y - OY) * 480;
        u16 *d = (u16 *)(buf + y * 960);
        const u16 *s = (y >= 53 && below == 2) ? IMAGE + (y - 53) * g_imgw : 0;
        for (u32 x = 38; x < 444; x++) {
            if (o[x]) continue;
            int ix = (int)x - g_x0;
            d[x] = (u16)((s && ix >= 0 && ix < (int)g_imgw) ? s[ix] : 0);
        }
    }
}
static void tall_apply_i(void)
{
    if (tall_live) {
        u32 xo = tall_x1 < 392 ? tall_x1 : 392;
        for (u32 y = 17; y < 50; y++) { u8 *o = OVL + y * 480; for (u32 x = 108; x < xo; x++) o[x] = 0; }
        tall_draw(0, 0, 90);
    } else {
        u32 below = !(MODE_RX & 0x80) ? 0 : ph_limit() ? 2 : 1;
        /* fenêtre ou liste en contexte Serato : l'interface refait elle-même ce qu'elle montre
         * (y toucher effaçait INFO et UTILITY à l'arrêt) ; morceau USB : zone remise à plat */
        if (below != 2 && ph_soft) return;
        flat_draw(SCREEN, below);
        flat_draw(SCREEN2, below);
    }
}
extern const u16 *blit_rect;
/* Fenêtre refermée : l'interface efface ses restes avec retard (vidéo V29 : bord droit et titre
 * « …RACK » de MENU, titre et bord gauche de UTILITY, une à plusieurs images). Le calque, lui,
 * n'a déjà plus la fenêtre : marges (colonnes 0 à 37 et 444 à 479, lignes 21 à 142) noircies là
 * où il est transparent. (Pas le bandeau : le calque n'y a pas encore le titre du morceau, le
 * noircir d'après lui effaçait le titre une image — essai dans l'émulateur.) */
static void win_clean(void)
{
    if (fw) return;                    /* pleine largeur : pas de marges, la pose suivante les refait */
    for (u32 y = 21; y < 143; y++) {
        const u8 *o = OVL + (y - OY) * 480;
        u16 *p = (u16 *)(SCREEN + y * 960), *q = (u16 *)(SCREEN2 + y * 960);
        for (u32 x = 0; x < 480; x++) {
            if (x == 38) x = 444;
            if (!o[x]) { p[x] &= 0x8000; q[x] &= 0x8000; }
        }
    }
}
static void tall_eval_f(int force)
{
    /* petites conversions (chiffres du temps, BPM… : plusieurs par image) : l'état des fenêtres
     * n'est réévalué qu'une fois par image (ph_limit lit 1 200 octets du calque) */
    static u32 last_fc = ~0u;
    const u16 *rc = blit_rect;
    if (!force && (u32)rc >= 0x1000 && (u32)rc[4] * rc[5] < 4000 && last_fc == fcount) return;
    last_fc = fcount;
    u32 lim = ph_limit();
    u8 live = (u8)(tall_opt && ph_hide && (lim == 392 || lim == 227));
    u32 x1 = lim == 227 ? 227 : g_xe;
    if (live == tall_live && (!live || x1 == tall_x1)) return;
    tall_live = live;
    if (live) tall_x1 = x1;
    topc_m[0] = topc_m[1] = topc_m[2] = 0;
    tall_todo = 3;
    pose_seen = 0;                 /* seules les images posées après le changement comptent */
    /* Fenêtre refermée (ou MENU → pleine largeur) : pas de redessin immédiat. L'interface n'a pas
     * encore effacé la fenêtre ; on voyait la waveform étirée avec les marges et le titre de la
     * fenêtre, puis une image noire (effacement), puis la waveform (vidéo V29 : UTILITY, MENU).
     * La pose suivante la redessine ; à l'arrêt, rgb_time s'en charge (tall_todo, pose_seen). */
    if (!live) tall_apply();
    else win_clean();
}

/* Appelée après CHAQUE copie de mémoire du programme (voir rgb_memcpy) : si la copie vient de
 * réécrire une ligne d'écran dans le champ, la tonalité y est redessinée aussitôt. */
static void cue_line(u8 *dst, u32 n);
static void ph_line(u8 *dst, u32 n);
static void hdr_line(u8 *dst, u32 n);
static void rgb_after_i(u8 *dst, u32 unused, u32 n)
{
    (void)unused;
    /* copie hors des deux tampons d'écran (la plupart des 350 copies par image) : rien à faire */
    if ((u32)(dst - SCREEN) >= 960 * (263 + 272) && !tall_half_dst) return;
    hdr_line(dst, n);
    if (tall_half_dst && dst == (u8 *)tall_line) { tall_half_put(); return; }
    cue_line(dst, n);
    ph_line(dst, n);
    if (!ov_n) return;          /* morceau de l'ordinateur : libellé KEY, même sans tonalité */
    /* deux tampons d'image de même format : l'écran (SCREEN) et le tampon de composition
     * (SCREEN2), d'où les lignes modifiées sont recopiées vers l'écran */
    u32 r = (u32)(dst - (SCREEN + 960 * 148));
    if (r >= 960 * (KEY_Y + KEY_H - 148)) {
        r = (u32)(dst - (SCREEN2 + 960 * 148));
        if (r >= 960 * (KEY_Y + KEY_H - 148)) return;    /* hors du champ (cas général) */
    }
    u32 y = 148;
    while (r >= 960) { r -= 960; y++; }
    u32 x0 = r >> 1, x1 = x0 + (n >> 1);             /* colonnes copiées : [x0, x1[ */
    if (x1 <= 47 || x0 >= KEY_X + KEY_W) return;
    key_row((u16 *)(dst - r), y, x0, x1);
}

/* La routine de copie d'origine (0xD49394, 139 appelants : fond, éléments, chiffres…) commence
 * par « CC = R2 == 0 ; IF CC JUMP fin » (4 octets), remplacés par un saut vers rgb_memcpy. Celle-ci
 * exécute la copie d'origine (ces deux instructions, puis la suite en 0xD49398), puis rgb_after.
 * Comme l'origine, elle ne touche pas aux registres de boucle LC1/LT1/LB1 et rend R0 = dst et R2 = n. */
__asm__(
    ".text\n"
    ".align 2\n"
    ".global _rgb_memcpy\n"
    "_rgb_memcpy:\n"
    "    [--SP] = RETS;\n"
    "    [--SP] = R0;\n"
    "    [--SP] = R2;\n"
    "    [--SP] = R1;\n"
    "    SP += -12;\n"
    "    CALL _rgb_dst;\n"                     /* R0 = destination à utiliser (étirement) */
    "    SP += 12;\n"
    "    R1 = [SP++];\n"
    "    R2 = [SP];\n"
    "    CC = R0 == 0;\n"                      /* 0 : copie déjà faite par rgb_dst */
    "    IF CC JUMP 2f;\n"
    "    [SP + 4] = R0;\n"
    "    CALL _rgb_copy0;\n"
    "    R2 = [SP];\n"
    "    R0 = [SP + 4];\n"
    "    SP += -12;\n"
    "    CALL _rgb_after;\n"
    "    SP += 12;\n"
    "2:  R2 = [SP++];\n"
    "    R0 = [SP++];\n"
    "    RETS = [SP++];\n"
    "    RTS;\n"
    "_rgb_copy0:\n"
    "    CC = R2 == 0;\n"
    "    IF CC JUMP 1f;\n"
    "    JUMP.L _orig_copy_body;\n"
    "1:  RTS;\n");

/* ---- Temps affiché : centièmes, sans le « F » des images -------------------------------------
 * Pour un morceau décrit comme un morceau USB, l'affichage montre la fraction de seconde en images
 * (1/75 s) suivies d'un « F ». Pour un morceau de l'ordinateur (waveform HD marquée, champ TRACK
 * vide), on rend les centièmes du mode Serato d'origine ; un vrai morceau USB n'est pas touché.
 *  - le « F » est un élément montré / caché par la routine d'origine 0xD17314 (R0 = 1 / 0),
 *    appelée quand le mode change (0xCF5D4E) ; nous l'appelons depuis rgb_time ;
 *  - les trois chiffres sont ceux de « demi-images × 5 » (0xD30A46 : images, dixièmes) ; on y
 *    substitue « centièmes × 10 ». La position de la waveform, elle, reste en demi-images. */
#define ORIG_F_SHOW ((void (*)(u32))0x00D17314)
#define TRACK_NO    (*(volatile u8 *)0x006003EA)     /* numéro de piste reçu (ff : champ vide) */
static u8 f_on __attribute__((section(".data"))) = 0xff;   /* état du « F » (ff : inconnu) */
u8 rgb_frac_hd __attribute__((section(".data"))) = 0;      /* dernier calcul : en centièmes */

void rgb_fshow(u32 on)                               /* remplace l'appel d'origine */
{
    ORIG_F_SHOW(on);
    f_on = (u8)on;
}

/* Remplace les trois appels périodiques de la mise à jour du temps (0xD306D4), exécutée à chaque image, même
 * à l'arrêt : c'est là que le « F » est mis d'accord avec le morceau courant (caché pour un morceau
 * de l'ordinateur, remontré pour un morceau USB). FMT_USB : format mémorisé par l'origine, 0x80
 * quand elle montre le « F ». */
/* ---- phase meter masqué pour un morceau en couleur ----
 * Le phase meter (deux rangées de 4 cases et « Bars », au-dessus de la grande waveform) est mis
 * à jour par 0xD2D80C à chaque statut, qui appelle 0xD2CDC8 (CALL en 0xD2D822, détourné ici)
 * pour savoir si le morceau courant est un morceau de l'ordinateur en couleur. */
#define ORIG_PH   ((void (*)(void *, u32))0x00D2CDC8)
/* Les cases, chiffres et « Bars » ne sont pas dessinés dans l'image : ils sont dans le calque
 * 8 bits du haut de l'écran (480 × 140 octets à 0x28, 0 = transparent), que l'interface
 * redessine à chaque changement. Masquer les objets n'a pas d'effet (essayé dans l'émulateur).
 * La zone est donc effacée à chaque image (rgb_time, appelée même à l'arrêt), tant que le
 * drapeau est posé ici. */
/* (ph_hide : défini plus haut, avant l'étirement) */
/* ph_soft : contexte Serato, y compris pendant le chargement d'un morceau (champ TRACK vide et
 * marque de la petite waveform pas encore arrivée) : sans lui le phase meter et le bandeau
 * bleu revenaient un instant à chaque chargement (essai V19). Il tombe dès qu'un morceau
 * numéroté (USB) est annoncé. */
/* (ph_soft : défini plus haut) */
static u8 pc_ctx __attribute__((section(".data"))) = 0;
#define MODE_RX_  (*(volatile u8 *)0x006003F8)
static void ph_ctx(void)
{
    u32 blank = TRACK_NO == 0xff;
    /* retour de la liste : la séquence de chargement rejouée par le principal porte un numéro de
     * piste ; tant que notre waveform et la marque de la petite waveform sont là, c'est toujours
     * le morceau de l'ordinateur (vidéo V29 : phase meter visible une ou deux images). Un morceau
     * USB efface les deux dès sa petite waveform reçue (ov_indexed, rgb_decode). */
    u32 ours = ov_n && rgb_points;
    /* V31 : pendant l'écran de chargement du retour, la marque n'est pas encore revenue (la vue
     * liste a remis l'overview d'origine) : notre waveform toujours décodée (rgb_points) et le
     * contexte Serato suffisent (émulateur : phase meter « Bars » visible sur l'écran de
     * chargement). Un morceau USB chargé ensuite l'efface dès son décodage (rgb_points = 0). */
    if (!ours && rgb_points && pc_ctx && (MODE_RX_ & 0x80)) ours = 1;
    if (!blank && !(MODE_RX_ & 0x01) && !ours) pc_ctx = 0;
    else if (ov_n || (MODE_RX_ & 0x01)) pc_ctx = 1;
    ph_hide = (u8)(ov_n && blank);
    ph_soft = (u8)((blank || ours) && pc_ctx);
}
/* écran de chargement (logo rekordbox, temps vide) : position reçue invalide. Le logo occupe le
 * calque sous le phase meter et passait pour une fenêtre ouverte (ph_limit), d'où le cadre du phase
 * meter et « Bars » visibles pendant tout le chargement, au retour de la liste notamment (V31) */
#define POS_LOADING() (*(volatile u16 *)0x006003D6 == 0xffff || *(volatile u16 *)0x006003D6 == 0xbbbb)
#define PH_X0  109
#define PH_X1  391
#define PH_Y0  20           /* lignes d'écran du phase meter : [PH_Y0, PH_Y1[ */
#define PH_Y1  53
void rgb_phase(void *page, u32 v)
{
    ORIG_PH(page, v);
    ph_ctx();
}
/* Le calque est recopié dans l'image d'écran (et son tampon de composition) quand l'interface
 * le change : la zone y est effacée aussi (fond noir à ces lignes). */
/* ---- bandeau du titre en gris (comme les CDJ-3000), en contexte Serato ----
 * Le bandeau (dégradé bleu, lignes 3 à 20) est dans le calque 8 bits et recopié dans l'image
 * d'écran : ses pixels bleus y sont remplacés par un gris de même intensité, à chaque image.
 * Contexte Serato : mode reçu 0x01 (ordinateur : liste, INFO) ou morceau de l'ordinateur en
 * couleur (décrit à l'écran comme un morceau USB). Un morceau USB n'est pas touché. */
/* Le bandeau est un dégradé horizontal fait des indices 0xC0 à 0xDF du calque 8 bits (lignes 0
 * à 16 du calque, affichées 3 lignes plus bas). Les indices sont convertis par la palette RGB555
 * du programme (deux exemplaires, 0x655124 et 0x655330) : en contexte Serato ces 32 entrées
 * sont mises à zéro, et l'interface dessine elle-même le bandeau en noir (pas de repeinture
 * après coup, donc pas de clignotement). Au changement d'état, les pixels déjà à l'écran sont
 * refaits d'après le calque pendant quelques passages. */
#define PAL1     ((volatile u16 *)0x00655124)
#define PAL2     ((volatile u16 *)0x00655330)
static const u16 HDR_PAL[32] = {
    0x02ba, 0x027a, 0x0259, 0x0238, 0x0217, 0x01d6, 0x01b5, 0x0193, 0x0194, 0x0172, 0x0151,
    0x012f, 0x0130, 0x010e, 0x00ec, 0x00cb, 0x00a9, 0x0087, 0x0066, 0x0044, 0x0023, 0x0001,
    0x08ca, 0x00aa, 0x08a7, 0x1edb, 0x1a79, 0x1a16, 0x15d4, 0x1591, 0x114f, 0x0d2d};
static u8 hdr_black __attribute__((section(".data"))) = 0;
static u8 hdr_todo __attribute__((section(".data"))) = 0;
/* L'interface recharge la palette en changeant de page (la Tag List a la sienne, à bandeau
 * gris) : les entrées sont donc remises à zéro avant chaque conversion du calque (hdr_pre,
 * appelée par les détournements rgb_blit), pas seulement au passage de rgb_time — sans quoi le
 * bandeau restait gris en Tag List et repassait un instant au bleu en la quittant (essai V20). */
static void hdr_pre_i(void)
{
    if (!hdr_black) return;
    /* palette rechargée : les deux bouts du dégradé le sont aussi (4 lectures au lieu de 64 à
     * chaque conversion, V31) */
    if (!(PAL1[0xc0] | PAL2[0xc0] | PAL1[0xdf] | PAL2[0xdf] | PAL1[0xd0] | PAL2[0xd0])) return;
    for (u32 i = 0xc0; i < 0xe0; i++) {
        if (PAL1[i]) PAL1[i] = 0;
        if (PAL2[i]) PAL2[i] = 0;
    }
}
/* Bandeau recopié vers l'écran sans passer par la palette (liste Serato : à chaque mouvement
 * de la molette, essai V25, vidéo : une image sur trente au dégradé bleu) : noirci dès la copie
 * de la ligne, sans attendre la conversion suivante. Palette rechargée entre-temps : remise à
 * zéro tout de suite aussi. */
/* Pixel du fond du bandeau à noircir : couleur du dégradé d'origine, ou gris (bandeau de la Tag
 * List, reste de la fenêtre MENU). Jamais le texte (blanc) : le calque ne correspond pas toujours
 * à l'image copiée, et noircir d'après lui seul effaçait un instant le texte du bandeau (essai
 * V26 : « CONTROL MODE » qui disparaît en parcourant la liste, titre au retour de MENU). */
static int hdr_bg(u32 v, u32 i)
{
    v &= 0x7fff;
    if (!v) return 0;
    if (v == HDR_PAL[i - 0xc0]) return 1;
    u32 r = v >> 10, g = (v >> 5) & 31, b = v & 31;
    u32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx <= 24 && mx - mn <= 3;
}
static void hdr_line(u8 *dst, u32 n)
{
    if (!hdr_black) return;
    u8 *b = SCREEN;
    u32 r = (u32)(dst - (b + 960 * 3));
    if (r >= 960 * 17) { b = SCREEN2; r = (u32)(dst - (b + 960 * 3)); if (r >= 960 * 17) return; }
    /* palette relue seulement pour les lignes du bandeau (V31 : quatre lectures lentes à chacune
     * des 350 copies de l'image auparavant) */
    if (PAL1[0xc0] | PAL2[0xc0] | PAL1[0xdf] | PAL2[0xdf]) hdr_pre();
    u32 y = 0;
    while (r >= 960) { r -= 960; y++; }
    u32 x0 = r >> 1, x1 = x0 + (n >> 1);
    if (x1 > 480) x1 = 480;
    const u8 *o = OVL + y * 480;
    u16 *p = (u16 *)(b + (y + 3) * 960);
    for (u32 x = x0; x < x1; x++)
        if (o[x] >= 0xc0 && o[x] <= 0xdf && hdr_bg(p[x], o[x])) p[x] &= 0x8000;
}
/* Passages de repeinture après un changement d'état (hdr_todo de 29 à 0 après décrément) :
 * 8 sur 30, resserrés au début. V35 : chaque passage relisait 8 160 octets du calque et 16 320
 * pixels d'écran, lectures lentes (300 à 800 cycles) : 7 à 8 M cycles, à chacune des 30 images
 * qui suivent un chargement, soit un tiers du temps d'une image. */
#define HDR_PASSES ((1u << 29) | (1u << 28) | (1u << 26) | (1u << 23) | (1u << 19) | (1u << 14) | \
                    (1u << 8) | (1u << 0))
static void hdr_set(u32 black)
{
    u32 chg = hdr_black != black;
    if (black) {
        /* palette rechargée par l'interface : ses deux bouts et son milieu le sont aussi
         * (V35 : 64 lectures lentes à chaque image auparavant, 6 maintenant) */
        if (chg || (PAL1[0xc0] | PAL2[0xc0] | PAL1[0xdf] | PAL2[0xdf] | PAL1[0xd0] | PAL2[0xd0]))
            for (u32 i = 0xc0; i < 0xe0; i++) { PAL1[i] = 0; PAL2[i] = 0; }
    } else if (chg || hdr_todo) {
        for (u32 i = 0; i < 32; i++) {           /* ne rétablit que ce que nous avons noirci */
            if (!PAL1[0xc0 + i]) PAL1[0xc0 + i] = HDR_PAL[i];
            if (!PAL2[0xc0 + i]) PAL2[0xc0 + i] = HDR_PAL[i];
        }
    }
    if (chg) { hdr_black = (u8)black; hdr_todo = 30; }
    if (!hdr_todo) return;
    hdr_todo--;
    if (!(HDR_PASSES >> hdr_todo & 1)) return;
    /* calque lu par mots (4 indices), pixels d'écran par paires, réécrits seulement s'ils changent */
    for (u32 y = 0; y < 17; y++) {
        const u32 *o = (const u32 *)(OVL + y * 480);
        u32 *p = (u32 *)(SCREEN + (y + 3) * 960), *q = (u32 *)(SCREEN2 + (y + 3) * 960);
        for (u32 w = 0; w < 120; w++) {
            u32 ow = o[w];
            /* aucun des 4 indices dans 0xC0..0xDF : (i & 0xE0) == 0xC0 */
            u32 m = (ow & 0xe0e0e0e0) ^ 0xc0c0c0c0;
            if ((m & 0xff) && (m & 0xff00) && (m & 0xff0000) && (m & 0xff000000)) continue;
            for (u32 h = 0; h < 2; h++, ow >>= 16) {
                u32 i0 = ow & 0xff, i1 = (ow >> 8) & 0xff;
                u32 in0 = (i0 & 0xe0) == 0xc0, in1 = (i1 & 0xe0) == 0xc0;
                if (!in0 && !in1) continue;
                u32 v0 = black || !in0 ? 0 : HDR_PAL[i0 - 0xc0];
                u32 v1 = black || !in1 ? 0 : HDR_PAL[i1 - 0xc0];
                /* masque des bits remplacés : 15 bits de chaque pixel du bandeau */
                u32 mk = (in0 ? 0x7fffu : 0) | (in1 ? 0x7fff0000u : 0), nv = v0 | v1 << 16;
                u32 k = w * 2 + h;
                u32 a = p[k], b = q[k];
                if ((a & mk) != nv) p[k] = (a & ~mk) | nv;
                if ((b & mk) != nv) q[k] = (b & ~mk) | nv;
            }
        }
    }
}

/* ---- couleurs des repères (hot cues Serato) ----
 * Les repères arrivent en cues mémoire (genre 0), que l'affichage dessine en triangles rouges
 * purs (0x7C00) : lignes 191 à 194 au-dessus de la petite waveform, lignes 56 à 60 en haut de la
 * grande (les traits de mesure rouges commencent ligne 61). Chaque triangle prend la couleur du
 * repère le plus proche. Table des repères reçue (paquet 0x05) : N en 0x600440, puis 10 octets
 * par repère en 0x600444 (+2 minutes, +4 secondes, +6 demi-images, +8 genre). */
#define CUE_N     (*(volatile u16 *)0x00600440)
#define CUE_TAB   ((volatile u8 *)0x00600444)
static void cue_rows(u32 y0, u32 y1, const int *cx, const u16 *col, u32 n)
{
    /* V31 : seulement autour des repères (les triangles font moins de 6 pixels de part et d'autre) :
     * toute la largeur, c'étaient 8 000 lectures de l'écran une image sur deux en lecture */
    for (u32 j = 0; j < n; j++) {
        int xa = cx[j] - 5, xb = cx[j] + 6;
        if (xa < (fw ? 0 : 36)) xa = fw ? 0 : 36;
        if (xb > (fw ? 480 : 448)) xb = fw ? 480 : 448;
        for (u32 y = y0; y < y1; y++) {
            u16 *l1 = (u16 *)(SCREEN + y * 960), *l2 = (u16 *)(SCREEN2 + y * 960);
            for (int x = xa; x < xb; x++) {
                u16 *pp[2] = {l1 + x, l2 + x};
                for (u32 b = 0; b < 2; b++) {
                    if ((*pp[b] & 0x7fff) != 0x7c00) continue;
                    int best = 6; u32 bj = 8;
                    for (u32 k = 0; k < n; k++) {
                        int d = x - cx[k]; if (d < 0) d = -d;
                        if (d < best) { best = d; bj = k; }
                    }
                    if (bj < 8 && col[bj]) *pp[b] = (u16)((*pp[b] & 0x8000) | col[bj]);
                }
            }
        }
    }
}
/* Grande waveform : l'image 400 × 90 (IMAGE) est recopiée à l'écran à chaque image, plus souvent
 * que rgb_time n'est appelée : les triangles (lignes 3 à 7 de l'image) sont recolorés dans
 * l'image elle-même, à l'arrêt depuis cue_paint, en lecture après le tracé de chaque colonne. */
static int cue_wp[8] __attribute__((section(".data")));   /* positions (points) des repères */
static u16 cue_wc[8] __attribute__((section(".data")));   /* et leurs couleurs */
static u32 cue_wn __attribute__((section(".data"))) = 0;
static int cue_cx[8] __attribute__((section(".data")));   /* colonnes des repères, image en cours */
static void cue_image_col(int x)
{
    if (!cue_wn || !wc_ppc || x < 0 || x >= (int)g_imgw) return;
    /* colonnes des repères : calculées une fois par image (une division par repère et par
     * colonne auparavant) */
    if (x == 0)
        for (u32 j = 0; j < cue_wn; j++) cue_cx[j] = wc_x + (cue_wp[j] - wc_i0) / wc_ppc;
    int best = 6; u32 bj = 8;
    for (u32 j = 0; j < cue_wn; j++) {
        int d = x - cue_cx[j];
        if (d < 0) d = -d;
        if (d < best) { best = d; bj = j; }
    }
    if (bj >= 8 || !cue_wc[bj]) return;
    for (u32 y = 3; y < 8; y++) {
        u16 *p = IMAGE + y * g_imgw + x;
        if ((*p & 0x7fff) == 0x7c00) *p = (u16)((*p & 0x8000) | cue_wc[bj]);
    }
}
/* Recoloration au moment où une ligne de la grande waveform est recopiée vers l'écran (même
 * routine de copie que pour la tonalité, rgb_memcpy) : lignes d'écran 56 à 60. */
static int cue_ox[8] __attribute__((section(".data")));   /* colonnes d'écran sur la petite waveform */
static void cue_line(u8 *dst, u32 n)
{
    if (!cue_wn || !ph_hide) return;
    u32 r = (u32)(dst - SCREEN);
    if (r >= 960 * 272) { r = (u32)(dst - SCREEN2); if (r >= 960 * 272) return; }
    int ov;
    u32 t0 = tall_live ? 26 : 56, t1 = tall_live ? 32 : 61;   /* lignes 3 à 7 de l'image */
    if (r >= 960 * t0 && r < 960 * t1) { ov = 0; r -= 960 * t0; if (!wc_ppc) return; }
    else if (r >= 960 * 190 && r < 960 * 196) { ov = 1; r -= 960 * 190; }
    else return;
    while (r >= 960) r -= 960;
    u16 *line = (u16 *)(dst - r);
    int x0 = (int)(r >> 1), x1 = x0 + (int)(n >> 1);
    if (x1 > 480) x1 = 480;
    for (int x = x0; x < x1; x++) {
        if ((line[x] & 0x7fff) != 0x7c00) continue;
        int best = 6; u32 bj = 8;
        for (u32 j = 0; j < cue_wn; j++) {
            int cx = ov ? cue_ox[j] : g_x0 + wc_x + (cue_wp[j] - wc_i0) / wc_ppc, d = x - cx;
            if (d < 0) d = -d;
            if (d < best) { best = d; bj = j; }
        }
        if (bj < 8 && cue_wc[bj]) line[x] = (u16)((line[x] & 0x8000) | cue_wc[bj]);
    }
}
static u32 cue_fc __attribute__((section(".data"))) = 0;      /* fcount au passage précédent */
static u8 cue_tick __attribute__((section(".data"))) = 0;
/* recoloration des triangles dans une ligne de la copie du haut (406 pixels depuis la colonne
 * d'écran 38), comme cue_line pour la grande waveform */
static void cue_fix(u16 *line)
{
    if (!cue_wn || !ph_hide || !wc_ppc) return;
    for (int x = 0; x < (int)g_lw; x++) {
        if ((line[x] & 0x7fff) != 0x7c00) continue;
        int best = 6; u32 bj = 8;
        for (u32 j = 0; j < cue_wn; j++) {
            int d = (int)g_c0 + x - (g_x0 + wc_x + (cue_wp[j] - wc_i0) / wc_ppc);
            if (d < 0) d = -d;
            if (d < best) { best = d; bj = j; }
        }
        if (bj < 8 && cue_wc[bj]) line[x] = (u16)((line[x] & 0x8000) | cue_wc[bj]);
    }
}
static void cue_paint(void)
{
    int ox[8], wx[8]; u16 col[8]; u32 n = 0, tn = CUE_N, changed = 0;
    if (tn > 40) tn = 40;
    for (u32 k = 0; k < tn && n < 8; k++) {
        volatile u8 *e = CUE_TAB + 10 * k;
        u32 kind = (e[8] | e[9] << 8) & 0x7fff;
        if (kind != 0) continue;
        int p = (int)(((u32)(e[2] | e[3] << 8) * 60 + (u32)(e[4] | e[5] << 8)) * 150 + (u32)(e[6] | e[7] << 8));
        ox[n] = ov_n ? 40 + (int)((u32)p * 400u / ov_n) : -100;
        wx[n] = wc_ppc ? g_x0 + wc_x + (p - wc_i0) / wc_ppc : -100;
        col[n] = cue_col[n];
        if (n >= cue_wn || cue_wp[n] != p || cue_wc[n] != col[n] || cue_ox[n] != ox[n]) changed = 1;
        cue_wp[n] = p; cue_wc[n] = cue_col[n]; cue_ox[n] = ox[n];
        n++;
    }
    if (n != cue_wn) changed = 1;
    cue_wn = n;
    if (!n) return;
    /* La grande waveform redessinée (lecture) passe déjà par la recoloration colonne par colonne
     * (rgb_column) et ligne par ligne (rgb_after) : la reprise complète n'est utile qu'à l'arrêt, ou
     * quand les repères changent ; en lecture, une fois toutes les deux secondes par sécurité
     * (V31 : elle coûtait plusieurs millions de cycles une image sur deux, d'où des à-coups). */
    u32 still = fcount == cue_fc;
    cue_fc = fcount;
    if (!changed && !still && ++cue_tick < 20) return;
    cue_tick = 0;
    cue_image_col(0);                          /* colonnes des repères de l'image en cours */
    for (u32 j = 0; j < n; j++)
        for (int x = cue_cx[j] - 5; x <= cue_cx[j] + 5; x++)
            if (x > 0 && x < (int)g_imgw) cue_image_col(x);
    cue_rows(191, 195, ox, col, n);
    if (tall_live) cue_rows(26, 32, wx, col, n);
    else cue_rows(56, 61, wx, col, n);  /* (au cas où l'écran a déjà été recomposé) */
}

static void ph_clear(u32 x1)
{
    for (u32 y = PH_Y0; y < PH_Y1; y++) {
        u8 *p = OVL + (y - OY) * 480 + 108, *e = OVL + (y - OY) * 480 + x1;
        while (p < e) *p++ = 0;
        u16 *q = (u16 *)(SCREEN + y * 960 + 2 * 108), *f = (u16 *)(SCREEN + y * 960 + 2 * x1);
        u16 *q2 = (u16 *)(SCREEN2 + y * 960 + 2 * 108);
        while (q < f) { *q++ = 0; *q2++ = 0; }
    }
}

/* Colonne de fin de l'effacement du phase meter, ou 0 s'il ne faut rien effacer : morceau USB,
 * vue liste, ou fenêtre ouverte par-dessus la vue morceau (INFO, UTILITY… : elles occupent le
 * calque sous le phase meter, vide en vue morceau ; la grande waveform est une image à part).
 * Seule la moitié droite occupée : fenêtre « LOAD PREVIOUS TRACK » de la touche MENU (colonnes
 * 227 à 479) ; le phase meter reste dessiné à sa gauche et y est effacé. */
u8 ph_notick __attribute__((section(".data"))) = 0;      /* essais : pas d'effacement périodique */
static u32 ph_lim_last __attribute__((section(".data"))) = 0;   /* dernier résultat de ph_limit */
static u32 ph_limit(void)
{
    if (!ph_soft || !(MODE_RX & 0x80)) { ph_lim_last = 0; return 0; }
    int right = 0;
    /* lu par mots de 32 bits, un mot sur deux : deux fois moins de lectures du calque (très lentes
     * sur la platine) que les octets de 4 en 4 d'avant, pour deux fois plus de pixels vus (V31) */
    for (u32 y = 54; y < 132; y += 6) {
        const u32 *o = (const u32 *)(OVL + y * 480);
        for (u32 k = 44 / 4; k < 224 / 4; k += 2) if (o[k]) { ph_lim_last = 0; return 0; }
        for (u32 k = 252 / 4; k < 436 / 4 && !right; k += 2) if (o[k]) right = 1;
    }
    ph_lim_last = right ? 227 : 392;
    return ph_lim_last;
}
/* Le même effacement au moment où une ligne du phase meter est recopiée vers l'écran : sans
 * lui, il apparaissait un instant à chaque fois que l'interface le redessine (fermeture de la
 * fenêtre MENU, fin du chargement — essai V19), jusqu'au passage suivant de rgb_time. */
static void ph_line(u8 *dst, u32 n)
{
    if (!ph_soft || tall_live) return;
    u32 r = (u32)(dst - SCREEN);
    if (r >= 960 * 263) { r = (u32)(dst - SCREEN2); if (r >= 960 * 263) return; }
    if (r < 960 * PH_Y0 || r >= 960 * PH_Y1) return;
    while (r >= 960) r -= 960;
    u32 x0 = r >> 1, xe = x0 + (n >> 1), x1 = ph_limit();
    if (!x1) return;
    if (x0 < 108) x0 = 108;
    if (xe > x1) xe = x1;
    u16 *line = (u16 *)(dst - r);
    for (u32 x = x0; x < xe; x++) line[x] = 0;
}

/* ---- phase meter : effacement à la source ----
 * Les éléments du calque sont convertis en RGB555 par la palette et écrits directement dans le
 * tampon affiché, par cinq routines selon le format (0xD0B58C, 0xD0ADEC, 0xD0AF2C, 0xD0B42C,
 * 0xD0A9E4), appelées du répartiteur 0xD0B6BC ; la routine de copie générale n'y passe pas.
 * C'est par là que le phase meter réapparaissait un instant à chaque redessin (ouverture de
 * MENU, allers-retours liste / INFO — essai V20). Les cinq appels sont détournés : après la
 * conversion, les pixels du rectangle dont l'indice de calque est propre au phase meter
 * (cases orange et bleues, dégradés : indices absents des fenêtres UTILITY, INFO et MENU,
 * relevés dans l'émulateur) sont remis à zéro dans le calque et les deux tampons. Les quelques
 * indices partagés (texte « Bars », traits) restent à l'effacement par zone de rgb_time. */
static const u32 PM_IDX[8] = {0x00000000, 0x00030001, 0x00000000, 0x67bd4004, 0xffff0300, 0x3f00ffff, 0x00000000, 0x00000000};
u32 blit_rets __attribute__((section(".data"))) = 0;
const u16 *blit_rect __attribute__((section(".data"))) = 0;   /* rectangle de la dernière conversion */
/* Destination de la conversion (relevé dans l'émulateur, V31) : le répartiteur 0xD0B6BC passe
 * R0 = calque source, R1 = rectangle source (x, y, largeur +8, hauteur +10), R2 = calque de
 * destination, 5e argument = position (x, y) dans ce calque. Calque (table 0x65414C, 44 octets) :
 * +0x12 format (8 : 16 bits), +0x1C pas en octets, +0x20 adresse des pixels. Les calques de
 * l'écran commencent à la ligne 3 (bandeau, phase meter, waveform) ou 143 (bas) de SCREEN ou de
 * SCREEN2 ; en lecture, toutes les petites conversions (chiffres, BPM, overview…) vont dans des
 * tampons intermédiaires ou dans le bas de l'écran, jamais dans le phase meter. */
const u16 *blit_dst __attribute__((section(".data"))) = 0;
const u16 *blit_pt __attribute__((section(".data"))) = 0;
/* Lignes [y0, y1[ et colonnes [x0, x1[ de l'écran écrites par la conversion en cours, dans le
 * tampon *buf. 0 : elle n'écrit pas dans l'écran ; 2 : destination inconnue (tout l'écran). */
static u32 blit_area(u32 *y0, u32 *y1, u32 *x0, u32 *x1, u8 **buf)
{
    const u16 *d = blit_dst, *pt = blit_pt, *rc = blit_rect;
    *y0 = 0; *y1 = 272; *x0 = 0; *x1 = 480; *buf = 0;
    if ((u32)d < 0x1000 || (u32)pt < 0x1000 || (u32)rc < 0x1000) return 2;
    u32 r = ((u32)d[16] | (u32)d[17] << 16) - (u32)SCREEN;
    if (r >= 960 * (263 + 272)) return 0;                 /* tampon intermédiaire, calque 8 bits */
    if (d[9] != 8 || d[14] != 960 || (pt[0] & 0x8000) || (pt[2] & 0x8000)) return 2;
    *buf = SCREEN;
    if (r >= 960 * 263) { r -= 960 * 263; *buf = SCREEN2; }
    r += (u32)pt[2] * 960 + (u32)pt[0] * 2;
    u32 y = r / 960;
    *y0 = y; *y1 = y + rc[5];
    *x0 = (r - y * 960) >> 1; *x1 = *x0 + rc[4];
    return 1;
}
/* La conversion 0xD0B42C pose, une fois par image, toute l'image de la waveform (rectangle
 * 406 × 90) dans le tampon d'écran, à la disposition d'origine ; les copies de lignes étirées ne la
 * recouvrent que plus loin dans l'image. Dans l'émulateur l'écran est relu après coup et c'est
 * invisible ; sur la platine l'écran la montrait une image sur deux (essai V22 : haut de la
 * waveform qui clignote en lecture et au zoom). Pendant l'étirement elle est donc sautée. */
u32 blit3_skip(const u16 *rect)
{
    return tall_live && rect[4] == 0x196 && rect[5] == 0x5a;
}
/* lignes d'écran [y0, y1[ de la waveform étirée refaites d'après IMAGE, dans buf (0 : les deux) */
static void tall_rows(u8 *buf, u32 y0, u32 y1)
{
    u32 a = 0, b = 90;
    while (a < 90 && (u32)TALL_FIRST[a] + TALL_CNT[a] <= y0) a++;
    while (b > a && TALL_FIRST[b - 1] >= y1) b--;
    if (a >= b) return;
    tall_draw(buf, a, b);                  /* buf : SCREEN, SCREEN2 ou 0 (les deux) */
}
static void ph_post_i(void)
{
    /* conversions de l'image de la waveform (400 ou 406 × 90, plusieurs par image) : ni le bandeau,
     * ni les fenêtres, ni le phase meter ne changent : rien à faire (V30) */
    const u16 *rc = blit_rect;
    if ((u32)rc >= 0x1000 && rc[5] == 90 && (rc[4] == 400 || rc[4] == 406 || rc[4] == 480)) return;
    /* V31 : les contrôles ne portent que sur ce que la conversion écrit à l'écran. Avant, chaque
     * petite conversion (5 à 10 par image en lecture) relisait tout le phase meter dans le calque
     * (2 500 mots, plusieurs centaines de cycles chacun sur la platine : calque et écran ne sont
     * pas en cache et partagent la mémoire avec l'affichage) */
    u32 y0, y1, x0, x1; u8 *buf;
    u32 scr = blit_area(&y0, &y1, &x0, &x1, &buf);
    unsigned t_h = prof_t0();
    /* bandeau dessiné sans passer par la palette (Tag List : dégradé gris) : noirci d'après
     * le calque (indices 0xC0 à 0xDF, lignes 0 à 16, affichées 3 lignes plus bas) */
    if (hdr_black && scr && y0 < 20 && y1 > 3) {
        int dirty = 0;
        /* points de contrôle sur toute la largeur (le reste gris de la fenêtre MENU, à droite,
         * échappait aux points d'avant, tous à gauche de la colonne 300 : essai V26) */
        for (u32 y = 2; y < 15 && !dirty; y += 6)
            for (u32 x = 4; x < 480 && !dirty; x += 34) {
                u32 i = OVL[y * 480 + x];
                if (i >= 0xc0 && i <= 0xdf &&
                    (hdr_bg(((u16 *)(SCREEN2 + (y + 3) * 960))[x], i) || hdr_bg(((u16 *)(SCREEN + (y + 3) * 960))[x], i))) dirty = 1;
            }
        if (dirty)
            for (u32 y = 0; y < 17; y++) {
                const u8 *o = OVL + y * 480;
                u16 *p = (u16 *)(SCREEN + (y + 3) * 960), *q = (u16 *)(SCREEN2 + (y + 3) * 960);
                for (u32 x = 0; x < 480; x++)
                    if (o[x] >= 0xc0 && o[x] <= 0xdf) {
                        if (hdr_bg(p[x], o[x])) p[x] &= 0x8000;
                        if (hdr_bg(q[x], o[x])) q[x] &= 0x8000;
                    }
            }
    }
    prof_add(18, t_h);
    ph_ctx();
    /* conversion de l'interface sur la zone de la waveform étirée : l'état des fenêtres est
     * réévalué tout de suite (une fenêtre qui s'ouvre par petits morceaux) */
    int over = scr && tall_live && y0 < 143 && y1 > 21 && x0 < tall_x1 && x1 > g_c0;
    tall_eval_f(over);
    if (!scr) return;                                   /* rien d'écrit à l'écran */
    u32 hit = 0;
    if (ph_soft && (MODE_RX & 0x80) && y0 < PH_Y1 && y1 > PH_Y0 - 3 && x0 < 392 && x1 > 108) {
        unsigned t_s = prof_t0();
        u32 sy0 = y0 > PH_Y0 - 3 ? y0 : PH_Y0 - 3, sy1 = y1 < PH_Y1 ? y1 : PH_Y1;
        u32 k0 = x0 > 108 ? (x0 - 108) / 4 : 0, k1 = x1 < 392 ? (x1 - 108 + 3) / 4 : (392 - 108) / 4;
        /* lu par mots de 32 bits : la zone est presque toujours vide (V29, profileur) */
        for (u32 y = sy0; y < sy1; y++) {
            u8 *o = OVL + (y - OY) * 480;
            const u32 *w = (const u32 *)(o + 108);
            u16 *p = (u16 *)(SCREEN + y * 960), *q = (u16 *)(SCREEN2 + y * 960);
            for (u32 k = k0; k < k1; k++) {
                if (!w[k]) continue;
                for (u32 x = 108 + 4 * k; x < 112 + 4 * k; x++) {
                    u32 i = o[x];
                    if (i && (PM_IDX[i >> 5] >> (i & 31) & 1)) { o[x] = 0; p[x] = 0; q[x] = 0; hit = 1; }
                }
            }
        }
        prof_add(19, t_s);
        if (hit) prof[16].n++;
    }
    if (ph_soft && (MODE_RX & 0x80) && POS_LOADING() && y0 < PH_Y1 && y1 > PH_Y0 - 3 && x0 < 392 && x1 > 108)
        ph_clear(392);                 /* chargement : tout le phase meter, cadre et « Bars » compris */
    if (!tall_live) return;
    /* étirement : le haut de la zone (lignes 21 à 52), que la conversion vient de recouvrir,
     * est refait sans attendre la prochaine image */
    if (hit) {
        u32 xo = tall_x1 < 392 ? tall_x1 : 392;
        for (u32 y = 17; y < 50; y++) { u8 *o = OVL + y * 480; for (u32 x = 108; x < xo; x++) o[x] = 0; }
        tall_draw(0, 0, 23);
    }
    /* V31 : toute conversion de l'interface sur la waveform étirée (fermeture d'une fenêtre : elle
     * remet le fond noir, vidéo V30 : une image noire en haut de l'écran en refermant MENU) est
     * recouverte aussitôt, à l'arrêt comme en lecture */
    if (y0 < 143 && y1 > 21 && x0 < tall_x1 && x1 > g_c0)
        tall_rows(buf, y0 > 21 ? y0 : 21, y1 < 143 ? y1 : 143);
}
__asm__(
    ".text\n"
    ".align 2\n"
    ".global _rgb_blit0\n"
    "_rgb_blit0:\n"
    "    [--SP] = RETS;\n"
    "    CALL _blit_head;\n"
    "    P1.L = 0xb58c; P1.H = 0xd0;\n"
    "    CALL (P1);\n"
    "    JUMP.S _blit_tail;\n"
    ".global _rgb_blit1\n"
    "_rgb_blit1:\n"
    "    [--SP] = RETS;\n"
    "    CALL _blit_head;\n"
    "    P1.L = 0xadec; P1.H = 0xd0;\n"
    "    CALL (P1);\n"
    "    JUMP.S _blit_tail;\n"
    ".global _rgb_blit2\n"
    "_rgb_blit2:\n"
    "    [--SP] = RETS;\n"
    "    CALL _blit_head;\n"
    "    P1.L = 0xaf2c; P1.H = 0xd0;\n"
    "    CALL (P1);\n"
    "    JUMP.S _blit_tail;\n"
    ".global _rgb_blit3\n"
    "_rgb_blit3:\n"
    "    [--SP] = RETS;\n"
    "    CALL _blit_head;\n"
    "    [--SP] = R0;\n"
    "    [--SP] = R1;\n"
    "    [--SP] = R2;\n"
    "    SP += -12;\n"
    "    R0 = R1;\n"
    "    CALL _blit3_skip;\n"
    "    SP += 12;\n"
    "    CC = R0 == 0;\n"
    "    R2 = [SP++];\n"
    "    R1 = [SP++];\n"
    "    R0 = [SP++];\n"
    "    IF !CC JUMP _blit3_none;\n"
    "    P1.L = 0xb42c; P1.H = 0xd0;\n"
    "    CALL (P1);\n"
    "    JUMP.S _blit_tail;\n"
    "_blit3_none:\n"
    "    R0 = 0;\n"
    "    JUMP.S _blit_tail;\n"
    ".global _rgb_blit4\n"
    "_rgb_blit4:\n"
    "    [--SP] = RETS;\n"
    "    CALL _blit_head;\n"
    "    P1.L = 0xa9e4; P1.H = 0xd0;\n"
    "    CALL (P1);\n"
    "    JUMP.S _blit_tail;\n"
    /* avant la conversion : palette du bandeau ; R0-R2 et les arguments de pile sont rendus
     * intacts, l'adresse de retour de l'appelant est rangée dans blit_rets */
    "_blit_head:\n"
    "    P0.L = _blit_rect; P0.H = _blit_rect;\n"
    "    [P0] = R1;\n"                         /* rectangle de la conversion (voir ph_post) */
    "    P0.L = _blit_dst; P0.H = _blit_dst;\n"
    "    [P0] = R2;\n"                         /* calque de destination (voir blit_area) */
    "    P1 = [SP + 16];\n"                    /* 5e argument (RETS de rgb_blitN déjà empilé) */
    "    P0.L = _blit_pt; P0.H = _blit_pt;\n"
    "    [P0] = P1;\n"
    "    [--SP] = RETS;\n"
    "    [--SP] = R0;\n"
    "    [--SP] = R1;\n"
    "    [--SP] = R2;\n"
    "    SP += -12;\n"
    "    CALL _hdr_pre;\n"
    "    SP += 12;\n"
    "    R2 = [SP++];\n"
    "    R1 = [SP++];\n"
    "    R0 = [SP++];\n"
    "    P2 = [SP++];\n"                       /* retour vers rgb_blitN */
    "    P1 = [SP++];\n"                       /* retour vers l'appelant d'origine */
    "    P0.L = _blit_rets; P0.H = _blit_rets;\n"
    "    [P0] = P1;\n"
    "    JUMP (P2);\n"
    "_blit_tail:\n"
    "    [--SP] = R0;\n"
    "    SP += -12;\n"
    "    CALL _ph_post;\n"
    "    SP += 12;\n"
    "    R0 = [SP++];\n"
    "    P0.L = _blit_rets; P0.H = _blit_rets;\n"
    "    P1 = [P0]; RETS = P1;\n"
    "    RTS;\n");

/* Fluidité (essai V27, vidéo au ralenti : pas de 3 puis 6 px, un dessin toutes les 25 à 42 ms).
 * La boucle principale de l'écran redessine la grande waveform (0xD2F0BE) d'après la position
 * de la dernière routine reçue (0x6003C0 : +0x16 minutes, +0x18 secondes, +0x1A demi-images),
 * qui change toutes les 20 ms environ ; le dessin, plus lent, tombe tantôt après une routine,
 * tantôt après deux. On dessine donc à la position interpolée à l'instant du dessin entre les
 * deux dernières positions reçues (une routine de retard, sans jamais dépasser la dernière),
 * d'après le compteur de cycles du processeur. La position reçue est remise après le dessin. */
typedef int s32;
#define POSF ((volatile u16 *)0x006003D6)
#define ORIG_WFRAME ((void (*)(void))0x00D2F0BE)
static inline u32 cyc(void)
{
    u32 c, f;
    __asm__ volatile("%0 = SYSCFG;" : "=d"(f));
    if (!(f & 2)) { f |= 2; __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(f)); }
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}
/* IN posé (ov_grow) : la boucle (octet +8, bit 0 de chaque point de la table 0x01011940, 12 octets
 * par point, 150 par seconde ; début : +3 bit 0x08, fin : +4 bit 0x40, relevé dans l'émulateur)
 * est étendue jusqu'à la tête de lecture avant chaque dessin. Le principal ne l'envoie plus. */
#define PTS ((volatile u8 *)0x01015180)
static u32 g_in, g_end __attribute__((section(".data"))) = 0;
static u8 g_on __attribute__((section(".data"))) = 0;
/* La boucle en cours de pose se reconnaît dans la table des repères de l'écran (0x600444, relue à
 * chaque dessin) : début (genre 3) et fin (genre 17) à moins de 3 demi-images, la fin que la partie
 * Mac envoie tant que OUT n'est pas appuyé. Plus rapide que la marque de la petite waveform (V28 :
 * 60 à 80 ms de retard au départ, et elle restait vraie un instant après OUT). */
static int cue_sliver(u32 *pin)
{
    u32 tn = CUE_N, in = 0, out = 0; int fi = 0, fo = 0;
    if (tn > 40) tn = 40;
    for (u32 k = 0; k < tn; k++) {
        volatile u8 *e = CUE_TAB + 10 * k;
        u32 kind = (e[8] | e[9] << 8) & 0x7fff;
        if (kind != 3 && kind != 17) continue;
        u32 p = ((u32)(e[2] | e[3] << 8) * 60 + (u32)(e[4] | e[5] << 8)) * 150 + (u32)(e[6] | e[7] << 8);
        if (kind == 3) { in = p; fi = 1; } else { out = p; fo = 1; }
    }
    if (fi && fo && out >= in && out - in <= 3) { *pin = in; return 1; }
    return 0;
}
static void grow_loop(u32 v)
{
    u32 in;
    if (!ov_n || !cue_sliver(&in)) { g_on = 0; return; }
    u32 p = v;                             /* en temps écoulé (V38) */
    if (p >= ov_n || in >= ov_n) return;
    if (!g_on || g_in != in) { g_in = in; g_end = in; g_on = 1; }
    /* liste des repères relue par l'écran entre-temps (autre repère posé) : boucle remise à la
     * fine bande d'origine, on repart du début */
    if (g_end > g_in && !(PTS[12 * g_end + 8] & 1)) g_end = g_in;
    if (p < g_in) return;
    if (p > g_end) for (u32 j = g_end + 1; j <= p; j++) PTS[12 * j + 8] |= 1;
    else for (u32 j = p + 1; j <= g_end; j++) PTS[12 * j + 8] &= (u8)~1;
    for (u32 j = g_in + 1; j < ov_n && j <= g_end + 3; j++) PTS[12 * j + 4] &= (u8)~0x40;
    PTS[12 * (p + 1) + 4] |= 0x40;
    g_end = p;
}
/* Estimateur de position en temps continu (V29). La V28 interpolait entre les deux dernières
 * positions vues au dessin : sur la platine l'écran dessine moins souvent que les routines
 * n'arrivent (33 à 67 ms contre 20 ms, vidéo au ralenti), chaque position n'était vue qu'au dessin
 * suivant et l'interpolation revenait à dessiner la position de l'image précédente. Ici :
 *  - vitesse mesurée sur des fenêtres d'au moins 150 ms (40 ms à la reprise), lissée ;
 *  - position avancée à chaque dessin du temps écoulé (compteur de cycles) × vitesse ;
 *  - rappel vers la position reçue (+10 ms : son retard moyen), 1/16 par dessin, 1/4 au-delà
 *    de 8 demi-images ; saut (boucle, needle search) : repositionnement immédiat ; position reçue
 *    inchangée depuis le dessin précédent : on n'avance pas ; figée plus de 80 ms : arrêt.
 * Unités (tout en 32 bits : l'émulateur ne sait pas exécuter les calculs 64 bits du compilateur) :
 * position en 1/256 de demi-image ; temps en blocs de 4096 cycles ; vitesse en demi-images × 2^20
 * par bloc. Seuils en millisecondes pour une horloge d'environ 400 MHz. Vérifié hors ligne sur
 * des dessins irréguliers (25 à 70 ms, arrêts de 200 à 380 ms) : écart 1,5 demi-image (V28 : 9). */
#define MS(x) ((u32)(x) * 98u)
static s32 e_pos __attribute__((section(".data"))) = 0;
static s32 e_v __attribute__((section(".data"))) = 0;
static u32 e_t __attribute__((section(".data"))) = 0;
static u32 a_t __attribute__((section(".data"))) = 0;
static u32 c_t __attribute__((section(".data"))) = 0;
static u32 a_raw __attribute__((section(".data"))) = 0;
static u32 l_raw __attribute__((section(".data"))) = 0;
static u8 e_on __attribute__((section(".data"))) = 0;
static u32 f_raw __attribute__((section(".data"))) = 0;      /* position reçue au dessin précédent */
static signed char e_dir __attribute__((section(".data"))) = 0;   /* sens du dernier mouvement */
static u8 e_jmp __attribute__((section(".data"))) = 0;   /* dernier changement reçu : un saut */
static u8 e_cnt __attribute__((section(".data"))) = 0;   /* images de suite à contresens de la vitesse */
static s32 e_adv(s32 v, u32 du) { return (v * (s32)du) >> 12; }   /* |v| ≤ 30000, du ≤ 39200 */
/* V31, pause (vidéo V30 : petits retours en arrière en appuyant sur pause). Vu en simulation, avec
 * le lissage du principal : à l'arrêt, la position reçue avance encore d'une demi-image à la fois
 * pendant quelques routines, et l'écran extrapolait à pleine vitesse (8 demi-images de trop), puis
 * revenait d'un coup sur la position reçue. Désormais :
 *  - décélération : si la position reçue a avancé de moins d'un tiers de ce que la vitesse prévoit
 *    depuis le dessin précédent, l'écran n'avance que de ce qu'elle a fait, sans avance de 10 ms ;
 *  - arrêt : un dépassement de moins de 6 demi-images (1 à 3 pixels) est gardé plutôt que repris
 *    en reculant ; la lecture suivante le résorbe en avançant ;
 *  - reprise : vitesse mesurée nettement plus forte que l'estimée, adoptée aux trois quarts.
 * Simulation : dépassement de 2 demi-images à la pause, sans recul ; à-coups en lecture inchangés. */
static u32 estimate(u32 raw)
{
    u32 now = cyc();
    if (!e_on) { e_on = 1; e_pos = (s32)(raw << 8); e_v = 0; e_t = a_t = c_t = now; a_raw = l_raw = f_raw = raw; return raw; }
    /* compteur de cycles arrêté (SYSCFG non modifiable ?) : position reçue telle quelle */
    if (now == e_t) return raw;
    u32 du = (now - e_t) >> 12;
    e_t = now;
    if (du > MS(400)) du = MS(400);
    /* position reçue inchangée depuis le dessin précédent, plus de 25 ms après : arrêt probable,
     * on n'avance pas */
    int moved = raw != l_raw;
    /* V39 : deux images de suite où la position reçue va à contresens de la vitesse : changement
     * de sens (lecture en arrière, reprise après un scratch), pas un retour de boucle (un seul
     * saut). La vitesse repart de zéro et se remesure. Avant, chaque image passait pour un retour
     * de boucle, la fenêtre de mesure était remise à zéro à chaque fois et la vitesse gardait son
     * ancien signe jusqu'au prochain arrêt : l'écran suivait la position brute, avec de petits
     * reculs (vidéos 5235 et 5236 ; simulation de ta session : vitesse négative pendant toute la
     * lecture qui suit les scratchs, une fois sur deux, jusqu'au CUE ou au rechargement). */
    if (moved && e_v && ((e_v > 0 && (s32)(raw - l_raw) < 0) || (e_v < 0 && (s32)(raw - l_raw) > 0))) {
        if (++e_cnt >= 2) { e_v = 0; e_cnt = 0; a_raw = raw; a_t = now; }
    } else if (moved) e_cnt = 0;
    int stopped = e_v == 0;            /* à l'arrêt (vitesse nulle depuis 80 ms) ou reprise pas encore mesurée */
    /* recul pendant la lecture (retour de boucle, même très courte) : saut net */
    int back = moved && ((e_v > 0 && (s32)(raw - l_raw) < -4) || (e_v < 0 && (s32)(raw - l_raw) > 4));
    s32 pred = e_pos;
    s32 exp = e_adv(e_v, du), got = (s32)(raw - f_raw) << 8;
    int slow = moved && !back && e_v && ((exp > 0 && got >= 0 && got < exp / 3) || (exp < 0 && got <= 0 && got > exp / 3));
    /* V35 : entre deux positions reçues, deux dessins à moins de 25 ms d'intervalle (à l'arrêt
     * surtout, le dessin y coûte moins) avançaient encore à l'ancienne vitesse : après un CUE en
     * lecture ou une pause, la waveform faisait +3, −5, +3 demi-images (émulateur, session de
     * 15 h 21) puis se calait. On n'avance plus ainsi que si la position reçue a changé il y a moins
     * de 25 ms (une routine), et pas après un saut (CUE, retour de boucle, hot cue) tant qu'elle n'a
     * pas bougé de nouveau. */
    s32 dl = (s32)(raw - l_raw);
    if (moved) e_jmp = (u8)(back || dl > 40 || dl < -40);
    if (moved || (du < MS(25) && !e_jmp && ((now - c_t) >> 12) < MS(25))) pred += slow ? got : exp;
    f_raw = raw;
    if (moved) { c_t = now; l_raw = raw; }
    s32 dr = (s32)(raw - a_raw);
    u32 da = (now - a_t) >> 12;
    if (dr > 300 || dr < -300 || (e_v > 0 && dr < -8) || (e_v < 0 && dr > 8)) { a_raw = raw; a_t = now; }
    else if (da >= MS(150) || (e_v == 0 && dr && da >= MS(80))) {   /* reprise : mesure rapide (80 ms, V39) */
        s32 vm = (dr * (1 << 20)) / (s32)da;
        if (vm > 30000) vm = 30000;
        if (vm < -30000) vm = -30000;
        if (e_v && (vm ^ e_v) >= 0 && (vm > 0 ? vm > e_v + e_v / 4 : vm < e_v + e_v / 4))
            e_v = e_v + (vm - e_v) * 3 / 4;
        else e_v = e_v ? e_v + (vm - e_v) / 4 : vm;
        a_raw = raw; a_t = now;
    }
    u32 idle = (now - c_t) >> 12;
    if (idle > MS(80)) { e_v = 0; a_raw = raw; a_t = now; }
    if (e_v > 0) e_dir = 1; else if (e_v < 0) e_dir = -1;
    int still = (!moved && idle >= MS(25)) || slow;
    s32 R = (s32)(raw << 8);
    /* la position reçue a en moyenne 10 ms de retard (routines toutes les 20 ms) */
    s32 target = (still || !e_v) ? R : R + e_adv(e_v, MS(10));
    s32 err = target - pred;
    /* V34 (vidéo V33 : petits mouvements au chargement, sur le CUE, à la pause) : à l'arrêt, la
     * position reçue qui change (CUE, molette, fin du lissage du principal) est prise telle quelle ;
     * arrêtée, la waveform se cale tout de suite sur elle, sauf un léger dépassement dans le sens
     * du mouvement (moins de 6 demi-images), gardé pour ne pas reculer. Avant : rattrapage au
     * 1/16 par image, une seconde de lente glissade, et un écart qui pouvait rester. */
    s32 ahead = e_dir >= 0 ? -err : err;
    if (stopped && moved) pred = R;
    else if (back || err > (40 << 8) || err < -(40 << 8)) { pred = target; a_raw = raw; a_t = now; }
    else if (still || !e_v) { if (!(ahead > 0 && ahead < (6 << 8))) pred = R; }
    else pred += (err > (8 << 8) || err < -(8 << 8)) ? err / 4 : err / 16;
    /* V39 : mouvement rapide (plus de 1,8 fois la vitesse normale environ : backspin, scratch) :
     * la vitesse mesurée suit mal un mouvement qui accélère ou ralentit sans cesse ; la position
     * reçue (avec 10 ms d'avance) est plus juste (simulation, backspin du 18/11 : irrégularité 13
     * au lieu de 19 avec la remise à zéro ci-dessus) */
    if (e_v > 3300 || e_v < -3300) pred = target;
    e_pos = pred;
    return pred < 0 ? 0 : (u32)((pred + 128) >> 8);
}
/* Réglage d'un point de la boucle à la molette (V31) : comme en mode USB, la grande waveform est
 * figée sur le point réglé (début pour IN, fin pour OUT), qui reste sous la tête de lecture ; le
 * masque grandit ou rétrécit avec la molette pendant que la lecture continue de boucler. Le point
 * est lu dans la table des repères, celle-là même d'où le masque est tracé : bord du masque et
 * tête restent ensemble. Rendu dans l'unité de la position reçue (temps restant en REMAIN). */
static int adj_point(u32 *pv)
{
    u32 tn = CUE_N, in = 0, out = 0; int fi = 0, fo = 0;
    if (tn > 40) tn = 40;
    for (u32 k = 0; k < tn; k++) {
        volatile u8 *e = CUE_TAB + 10 * k;
        u32 kind = (e[8] | e[9] << 8) & 0x7fff;
        if (kind != 3 && kind != 17) continue;
        u32 p = ((u32)(e[2] | e[3] << 8) * 60 + (u32)(e[4] | e[5] << 8)) * 150 + (u32)(e[6] | e[7] << 8);
        if (kind == 3) { in = p; fi = 1; } else { out = p; fo = 1; }
    }
    if (!fi || !fo || out <= in) return 0;
    *pv = ov_adj == 1 ? in : out;          /* en temps écoulé (V38 : conversion dans rgb_wframe) */
    return 1;
}
/* Constantes du dessin d'origine qui dépendent de la largeur de l'image (relevées au
 * désassembleur, V33) : pas de 800 octets (± 0x320), lignes 44 et 45 (traceur), 8 et 12 (traits),
 * centre (trait de la tête de lecture, colonne 200), bornes 399 et lignes 7 et 85 (triangles des
 * repères), taille effacée et descripteur de l'image (largeur, pas, taille). Valeur immédiate de
 * 16 bits au 3e octet de chaque instruction de 32 bits. Puis les étendues du dessin autour du centre
 * (constantes 64 bits lues par 0xD2F0BE en 0xCBC1C4 + …) : −199 / +200 colonnes, 400. */
typedef struct { u32 a; u16 o, f; } geo_t;
static const geo_t GEO_C[] = {
    {0xD2E242, 0x8980, 0xA500}, {0xD2E256, 0xFCE0, 0xFC40}, {0xD2E25C, 0x8CA0, 0xA8C0},
    {0xD2E264, 0x0320, 0x03C0}, {0xD2E2C0, 0xFCE0, 0xFC40}, {0xD2E35C, 0x2580, 0x2D00},
    {0xD2E386, 0x1900, 0x1E00}, {0xD2E39C, 0x0320, 0x03C0}, {0xD2E3DA, 0x0320, 0x03C0},
    {0xD2E406, 0x0320, 0x03C0}, {0xD2E42E, 0xFCE0, 0xFC40}, {0xD2E458, 0xFCE0, 0xFC40},
    {0xD2E4A8, 0x0190, 0x01E0}, {0xD2E4D4, 0x0320, 0x03C0}, {0xD2E4FA, 0x0320, 0x03C0},
    {0xD2E528, 0x0320, 0x03C0},
    {0xD2EC8A, 0x018F, 0x01DF}, {0xD2ECE4, 0x018F, 0x01DF}, {0xD2ED3E, 0x018F, 0x01DF},
    {0xD2ED98, 0x018F, 0x01DF}, {0xD2EDF2, 0x018F, 0x01DF}, {0xD2EE4C, 0x018F, 0x01DF},
    {0xD2EC9E, 0xFCDE, 0xFC3E}, {0xD2ED52, 0xFCDE, 0xFC3E}, {0xD2EDAC, 0xFCDE, 0xFC3E},
    {0xD2EE60, 0xFCDE, 0xFC3E}, {0xD2ECF8, 0xFCE2, 0xFC42},
    {0xD2ECA6, 0x15E0, 0x1A40}, {0xD2ED5A, 0x15E0, 0x1A40}, {0xD2EDB4, 0x15E0, 0x1A40},
    {0xD2EE68, 0x15E0, 0x1A40}, {0xD2ED00, 0x0CB8, 0x4278}, {0xD2EE0E, 0x0CB8, 0x4278},
    {0xD2F0F8, 0x1940, 0x5180}, {0xD2F4C2, 0x0320, 0x03C0}, {0xD2F4C6, 0x1940, 0x5180}};
/* (largeur du descripteur, 0xD2F4D2 : laissée à 400. Elle ne sert qu'à la conversion de l'image
 * vers le calque de la waveform, qui n'a que 406 colonnes ; en pleine largeur le contenu du calque
 * n'est pas montré, la pose prend ses lignes dans l'image.) */
static const geo_t GEO_D[] = {
    {0xCBC1E4, 0xFF39, 0xFF11}, {0xCBC1EC, 0x00C8, 0x00F0}, {0xCBC1FC, 0x00C8, 0x00F0},
    {0xCBC20C, 0x00C8, 0x00F0}, {0xCBC214, 0x00C8, 0x00F0}, {0xCBC22C, 0x0190, 0x01E0}};
#define GEO_NC (sizeof GEO_C / sizeof GEO_C[0])
#define GEO_ND (sizeof GEO_D / sizeof GEO_D[0])
static u8 geo_ok __attribute__((section(".data"))) = 0;      /* 0 à vérifier, 1 bon, 2 inattendu */
static void geo_set(u32 full)
{
    if (!geo_ok) {                     /* firmware attendu : chaque valeur d'origine à sa place */
        geo_ok = 1;
        for (u32 i = 0; i < GEO_NC; i++) if (*(volatile u16 *)(GEO_C[i].a + 2) != GEO_C[i].o) geo_ok = 2;
        for (u32 i = 0; i < GEO_ND; i++) if (*(volatile u16 *)GEO_D[i].a != GEO_D[i].o) geo_ok = 2;
    }
    if (geo_ok != 1) return;
    for (u32 i = 0; i < GEO_NC; i++) {
        volatile u16 *p = (volatile u16 *)(GEO_C[i].a + 2);
        *p = full ? GEO_C[i].f : GEO_C[i].o;
        /* code modifié : ligne du cache de données écrite en mémoire, ligne du cache
         * d'instructions invalidée */
        __asm__ volatile("FLUSH [%0]; SSYNC; IFLUSH [%0]; SSYNC;" : : "a"(p) : "memory");
    }
    for (u32 i = 0; i < GEO_ND; i++) *(volatile u16 *)GEO_D[i].a = full ? GEO_D[i].f : GEO_D[i].o;
    __asm__ volatile("CSYNC;" : : : "memory");
    fw = (u8)full;
    g_imgw = full ? 480 : 400; g_c0 = full ? 0 : 38; g_lw = full ? 480 : 406;
    g_x0 = full ? 0 : 40; g_xe = full ? 480 : 444;
    if (tall_x1 != 227) tall_x1 = g_xe;
    topc_m[0] = topc_m[1] = topc_m[2] = 0;
    tall_todo = 3;
}
/* V37 : état reçu vu par le dessin d'origine. 0xD2F0BE lit la position (0x6003C0 +0x16 minutes,
 * +0x18 secondes, +0x1A demi-images) APRÈS avoir effacé toute l'image de la waveform (86 400
 * octets, plusieurs millisecondes sur la platine) ; le gestionnaire de liaison recopie chaque
 * routine dans 0x6003C0 toutes les 20 ms (0xD0E346). Une routine arrivée pendant l'effacement
 * remplaçait notre position par la position brute : ce dessin-là partait de la position reçue.
 * Vidéos 5197 et 5199 : un dessin sur deux environ à la position de lecture pendant le réglage
 * IN, à 5 pixels de la position calée pendant la pause. Le dessin lit désormais une copie privée
 * de l'état (pointeur P5 de 0xD2F0BE redirigé, build_gui.py), remplie juste avant lui ; nous
 * n'écrivons plus dans 0x6003C0. Le dessin n'y lit que les champs 0x16 à 0x43. */
u32 rgb_posh[0x48 / 4] __attribute__((section(".data"), aligned(4)));
static void posh_draw(u32 set, u32 v)
{
    const volatile u32 *s = (const volatile u32 *)0x006003C0;
    for (u32 k = 0; k < 0x48 / 4; k++) rgb_posh[k] = s[k];
    if (set) {
        u16 *f = (u16 *)rgb_posh;
        f[0x16 / 2] = (u16)(v / 9000); f[0x18 / 2] = (u16)(v / 150 % 60); f[0x1a / 2] = (u16)(v % 150);
    }
    unsigned t = prof_t0(); ORIG_WFRAME(); prof_add(0, t);
}
void rgb_wframe(void)
{
    u32 m = POSF[0], sec = POSF[1], h = POSF[2];
    /* pleine largeur : morceau de l'ordinateur en couleur (marque de l'overview et waveform
     * décodée) ; un morceau USB efface les deux. Bascule entre deux dessins seulement. */
    /* notre morceau à l'écran : marque de l'overview égale à la waveform décodée, et champ TRACK
     * vide depuis plus de dix dessins (la séquence de chargement rejouée porte un instant le
     * numéro de Serato ; un morceau USB, le sien pour de bon). Sinon ni couleurs ni pleine
     * largeur : au chargement d'un morceau USB, l'ancienne waveform restait en couleur, à
     * plusieurs zooms, jusqu'à l'arrivée de la nouvelle (vidéo V33 : waveform qui clignote). */
    static u8 trk_n;
    if (TRACK_NO == 0xff) trk_n = 0; else if (trk_n < 255) trk_n++;
    col_ok = (u8)(ov_n && ov_n == rgb_points && trk_n < 10);
    u32 want = fw_opt && col_ok;
    if (want != fw) geo_set(want);
    /* morceau USB (mode d'origine) ou position invalide : dessin d'origine, sans rien toucher */
    if (!ov_n || TRACK_NO != 0xff || m == 0xffff || m == 0xbbbb || sec >= 60 || h >= 150) {
        e_on = 0; g_on = 0;
        posh_draw(0, 0); return;
    }
    u32 raw = (m * 60 + sec) * 150 + h;
    /* V38 : estimation toujours en temps écoulé. En REMAIN (bit 0x20 du champ 0x36), la position
     * reçue est le temps restant, sur la durée des champs 0x1C à 0x20 ; elle décroît en lecture.
     * Au passage de l'un à l'autre (touche TIME MODE), l'estimateur gardait sa vitesse de l'autre
     * sens : chaque image était prise pour un retour en arrière et il suivait la position brute,
     * sans interpoler, jusqu'au prochain arrêt (vidéo 5201, session du 21/11 : à-coups « dès la
     * marche arrière », avec ou sans boucle). Le réglage IN/OUT et la boucle qui s'étend
     * travaillent aussi en temps écoulé. */
    volatile u16 *rf = (volatile u16 *)0x006003C0;
    u32 rem = rf[0x36 / 2] & 0x20;
    u32 dur = ((u32)rf[0x1c / 2] * 60 + rf[0x1e / 2]) * 150 + rf[0x20 / 2];
    if (raw > dur) rem = 0;
    if (rem) raw = dur - raw;
    u32 v = estimate(raw);                  /* (suivi continué pendant le réglage : reprise sans à-coup) */
    /* Réglage : dernier point valable gardé. Pendant que la molette tourne, la liste des repères
     * est renvoyée à l'écran toutes les 40 ms et se trouve un instant incomplète ; on retombait
     * alors sur la position de lecture une image sur quatre environ (vidéo V33 : « une partie de
     * la waveform veut continuer de tourner »). Un point qui saute de plus d'une seconde n'est
     * pris que s'il se répète à l'image suivante (liste en cours de mise à jour). */
    static u32 adj_last, adj_cand; static u8 adj_have, adj_mode;
    u32 fz;
    if (ov_adj != adj_mode) { adj_mode = ov_adj; adj_have = 0; }
    if (ov_adj) {
        if (adj_point(&fz)) {
            u32 d = fz > adj_last ? fz - adj_last : adj_last - fz;
            if (!adj_have || d <= 150 || fz == adj_cand) { adj_last = fz; adj_have = 1; }
            adj_cand = fz;
        }
        if (adj_have) v = adj_last;
    }
    if (!ov_adj || !adj_have) { unsigned t = prof_t0(); grow_loop(v); prof_add(13, t); }
    if (rem) v = dur > v ? dur - v : 0;    /* retour à l'unité de l'affichage */
    if (prof_on) { static unsigned last; unsigned t = cyc_now(); if (last) { unsigned d = t - last; prof[1].acc += d; prof[1].n++; if (d > prof[1].max) prof[1].max = d; } last = t; }
    fcount++;
    posh_draw(1, v);
}

#define ORIG_TIME ((void (*)(u32, u32))0x00D306D4)
#define FMT_USB   (*(volatile u32 *)0x00600080)
/* ---- mesures affichées (HD_DEBUG=1) ----
 * Toutes les 10 passages de rgb_time (une seconde environ), dans le bandeau à droite, en
 * centaines de milliers de cycles : « images/s  intervalle moyen  intervalle max  dessin
 * d'origine  notre code  occupation % ». Police 3 × 5, blanc sur noir. */
static const u16 DIG3[12] = {0x7b6f, 0x2492, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf, 0x0000, 0x4a52};
static void dbg_text(u32 y0, const char *t)
{
    for (u32 y = 0; y < 7; y++) {
        u16 *p = (u16 *)(SCREEN + (y0 - 1 + y) * 960), *q = (u16 *)(SCREEN2 + (y0 - 1 + y) * 960);
        for (u32 x = 262; x < 478; x++) { p[x] &= 0x8000; q[x] &= 0x8000; }
    }
    u32 x = 264;
    for (const char *c = t; *c && x < 474; c++, x += 4) {
        u32 g = *c >= '0' && *c <= '9' ? (u32)(*c - '0') : *c == '%' ? 11 : 10;
        for (u32 r = 0; r < 5; r++)
            for (u32 b = 0; b < 3; b++)
                if (DIG3[g] >> (14 - 3 * r - b) & 1) {
                    ((u16 *)(SCREEN + (y0 + r) * 960))[x + b] |= 0x7fff;
                    ((u16 *)(SCREEN2 + (y0 + r) * 960))[x + b] |= 0x7fff;
                }
    }
}
static char *dbg_num(char *o, unsigned v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 9);
    while (n) *o++ = t[--n];
    *o++ = ' '; *o++ = ' ';
    return o;
}
/* banc d'essai mémoire : cycles pour 256 lectures de 32 bits (bus et cache de la platine) */
static unsigned mem_bench(const volatile u32 *p, u32 step)
{
    unsigned t = cyc_now(), s = 0;
    for (u32 i = 0; i < 256; i++) s += p[i * step];       /* pas de 36 octets : une ligne de cache par lecture */
    unsigned d = cyc_now() - t;
    return d + (s & 0);
}
static void dbg_show(void)
{
    static prof_t last[20]; static u8 cnt, alt; static char line[64], line2[64];
    if (++cnt < 10) { if (line[0]) { dbg_text(6, line); dbg_text(13, line2); } return; }
    cnt = 0;
    unsigned d[20], n[20];
    for (u32 i = 0; i < 20; i++) { d[i] = prof[i].acc - last[i].acc; n[i] = prof[i].n - last[i].n; last[i] = prof[i]; }
    unsigned fr = n[1] ? n[1] : 1;
    unsigned ours = (d[2] - d[3]) + d[4] + d[5] + d[6] + d[7] + (d[9] - d[8]) + (d[11] - d[10]);
    unsigned busy = d[0] + ours - (d[2] - d[3]);
    char *o = line;
    o = dbg_num(o, n[1]);
    o = dbg_num(o, d[1] / fr / 100000);
    o = dbg_num(o, prof[1].max / 100000); prof[1].max = 0;
    o = dbg_num(o, d[0] / fr / 10000);
    o = dbg_num(o, ours / fr / 10000);
    o = dbg_num(o, d[1] >= 100 ? busy / (d[1] / 100) : 0);   /* 32 bits seulement */
    *o++ = '%'; *o = 0;
    /* seconde ligne, en alternance : « 1 » détail de notre code par image (dizaines de milliers de
     * cycles) : copies avant, copies après, ph_post, colonnes, rgb_time, overview, dessin de la
     * zone, tonalité ; « 2 » banc mémoire (cycles / 10 pour 256 lectures) : nos variables, pile,
     * calque, écran, image de la waveform, table des points, tampon 0x20, palette */
    o = line2;
    if (!(alt ^= 1)) {
        o = dbg_num(o, 1);
        o = dbg_num(o, d[4] / fr / 10000);
        o = dbg_num(o, d[5] / fr / 10000);
        o = dbg_num(o, d[7] / fr / 10000);
        o = dbg_num(o, (d[2] - d[3]) / fr / 10000);
        o = dbg_num(o, (d[9] - d[8]) / fr / 10000);
        o = dbg_num(o, (d[11] - d[10]) / fr / 10000);
        o = dbg_num(o, d[14] / fr / 10000);
        o = dbg_num(o, d[15] / fr / 10000);
    } else {
        u32 stk[264];                                      /* pile : lecture suivie (pas 1) */
        for (u32 i = 0; i < 264; i++) stk[i] = i;
        o = dbg_num(o, 2);
        o = dbg_num(o, mem_bench((const volatile u32 *)0x00FD6000, 9) / 10);   /* notre code / données */
        o = dbg_num(o, mem_bench((const volatile u32 *)stk, 1) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)OVL, 9) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)SCREEN, 9) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)IMAGE, 9) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)PTS, 9) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)WAVE_BUF, 9) / 10);
        o = dbg_num(o, mem_bench((const volatile u32 *)PAL1, 1) / 10);    /* palette : 1 Ko */
    }
    *o = 0;
    dbg_text(6, line);
    dbg_text(13, line2);
}
static void rgb_time_i(u32 a, u32 b)
{
    if (FMT_USB == 0x80) {
        /* morceau de l'ordinateur : caché ; morceau numéroté (USB) : montré ; entre les deux
         * (chargement en cours, champ vide et pas encore de marque) : laissé tel quel, pour
         * qu'il ne réapparaisse pas une à trois secondes à chaque changement de morceau */
        u32 blank = TRACK_NO == 0xff;
        u32 want = (ov_n && blank) ? 0 : blank ? f_on : 1;
        if (f_on != want) { ORIG_F_SHOW(want); f_on = (u8)want; }
    }
    { unsigned t = prof_t0(); ORIG_TIME(a, b); prof_add(8, t); }
    /* phase meter (calque 8 bits), à chaque image, en vue morceau seulement (mode reçu 0x80 :
     * morceau décrit comme USB ; en vue liste ou INFO, 0x01, la zone est occupée par la liste) */
    /* panneau INFO (superposé à la vue morceau, mode toujours 0x80) : il occupe le calque à
     * gauche (colonnes 11 à 107), vide en vue morceau Serato (émulateur, 16/11) */
    /* fenêtre ouverte par-dessus la vue morceau (INFO, MENU, UTILITY…) : elle occupe le calque
     * sous le phase meter, vide en vue morceau (la grande waveform est une image à part ; seuls
     * le trait central, colonnes 240 à 242, et les bords y sont dessinés) */
    ph_ctx();
    tall_eval_f(1);
    u32 x1 = ph_lim_last;               /* calculé par tall_eval_f (une lecture du calque de moins) */
    if (!x1 && ph_soft && (MODE_RX & 0x80) && POS_LOADING()) x1 = 392;
    if (tall_todo) {
        if (pose_seen && tall_live) tall_todo = 0;      /* les images se succèdent : déjà refait */
        else { tall_todo--; tall_apply(); }
    }
    pose_seen = 0;
    if (x1 && !ph_notick && !tall_live) ph_clear(x1);
    hdr_set(ph_soft || (MODE_RX & 0x01));
    if (x1 == 392 && ph_hide) cue_paint();
    if (dbg_on && ph_hide) dbg_show();
}

u32 rgb_frac(u32 hf)                                 /* hf : demi-images, 0..149 */
{
    u32 hd = ov_n && TRACK_NO == 0xff;
    rgb_frac_hd = (u8)hd;
    return hd ? ((hf * 683) >> 10) * 10 : hf * 5;    /* 683 / 1024 = 2 / 3 */
}

/* Posé à la place de « R0 = R0.L * R1.L » (R0 = 5, R1 = demi-images) : rend R0, garde le reste. */
__asm__(
    ".text\n"
    ".align 2\n"
    ".global _rgb_frac_asm\n"
    "_rgb_frac_asm:\n"
    "    [--SP] = RETS;\n"
    "    [--SP] = ASTAT;\n"
    "    [--SP] = (R7:7, P5:5);\n"
    "    [--SP] = R1;\n"
    "    [--SP] = R2;\n"
    "    [--SP] = R3;\n"
    "    [--SP] = P0;\n"
    "    [--SP] = P1;\n"
    "    [--SP] = P2;\n"
    "    R0 = R1;\n"
    "    SP += -12;\n"
    "    CALL _rgb_frac;\n"
    "    SP += 12;\n"
    "    P2 = [SP++];\n"
    "    P1 = [SP++];\n"
    "    P0 = [SP++];\n"
    "    R3 = [SP++];\n"
    "    R2 = [SP++];\n"
    "    R1 = [SP++];\n"
    "    (R7:7, P5:5) = [SP++];\n"
    "    ASTAT = [SP++];\n"
    "    RETS = [SP++];\n"
    "    RTS;\n"
    /* Posé à la place de « B[P3 + 2] = R0 » (0xD30ABA), le rangement du troisième chiffre : en
     * lecture l'origine le fait alterner entre 0 et 5 ; en centièmes il reste à 0, comme dans le
     * mode ordinateur d'origine. */
    ".global _rgb_dec_asm\n"
    "_rgb_dec_asm:\n"
    "    [--SP] = ASTAT;\n"
    "    [--SP] = P0;\n"
    "    [--SP] = R1;\n"
    "    P0.L = _rgb_frac_hd;\n"
    "    P0.H = _rgb_frac_hd;\n"
    "    R1 = B[P0] (Z);\n"
    "    CC = R1 == 0;\n"
    "    IF CC JUMP 2f;\n"
    "    R0 = 0;\n"
    "2:  B[P3 + 2] = R0;\n"
    "    R1 = [SP++];\n"
    "    P0 = [SP++];\n"
    "    ASTAT = [SP++];\n"
    "    RTS;\n");

static u16 ov_cc[OV_W] __attribute__((section(".data")));     /* couleurs des colonnes */
static u32 ov_cc_n __attribute__((section(".data"))) = 0;     /* … pour ce nombre de points (0 : à refaire) */
static void rgb_overview_i(void)
{
    u16 *c = OV_COLS;
    if ((c[6] >> 8) == 'H' && (c[7] >> 8) == 'D') {      /* marque, écrite en dernier */
        ov_n = (u32)(c[3] >> 8) | (u32)(c[4] >> 8) << 8 | (u32)(c[5] >> 8) << 16;
        key_n = 0;                                       /* tonalité : colonnes 0..2 */
        for (u32 k = 0; k < 3; k++) {
            key_c[k] = (u8)(c[k] >> 8);
            if (key_c[k] && key_n == k) key_n = (u8)(k + 1);
        }
        /* couleurs des repères (hot cues Serato, dans l'ordre de leur position) : colonnes
         * 8..31, 3 octets RVB par repère */
        for (u32 j = 0; j < 8; j++) {
            u32 r = c[8 + 3 * j] >> 8, g = c[9 + 3 * j] >> 8, b = c[10 + 3 * j] >> 8;
            cue_col[j] = (u16)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        }
        ov_grow = (c[31] >> 8) == 'G';
        ov_adj = (c[31] >> 8) == 'I' ? 1 : (c[31] >> 8) == 'O' ? 2 : 0;
        for (u32 k = 0; k < 32; k++) c[k] &= 0xff;      /* indice de couleur d'origine (0) */
        key_paint();            /* libellé KEY même sans tonalité (sinon « TRACK » d'origine) */
        ov_sum = ov_checksum();
    } else if (ov_n && ov_indexed()) {
        ov_n = 0; ov_grow = 0; ov_adj = 0;               /* overview d'un morceau USB */
        key_n = 0;
    } else if ((c[0] | c[1] | c[2] | c[3] | c[4] | c[5] | c[6] | c[7]) >> 8) {
        /* marque en cours d'arrivée (les colonnes sont rangées pendant que l'on dessine) : on ne
         * conclut rien, la marque complète sera lue au prochain passage */
    } else if (ov_n && ov_checksum() != ov_sum) {
        ov_n = 0; ov_grow = 0; ov_adj = 0;               /* nouvelles colonnes, sans marque */
        key_n = 0;
    }
    { unsigned t = prof_t0(); ORIG_OV_DRAW(); prof_add(10, t); }
    u32 n = rgb_points;
    if (!ov_n || ov_n != n) return;
    /* couleurs des 400 colonnes calculées une fois par morceau (V31) : chaque tracé relisait tous
     * les points du morceau (deux lectures par point : 90 000 pour 5 minutes) */
    if (ov_cc_n != n) {
        for (u32 x = 0; x < OV_W; x++) {
            u32 lo = x * n / OV_W, hi = (x + 1) * n / OV_W;
            if (hi <= lo) hi = lo + 1;
            ov_cc[x] = hi > n ? 0 : ov_color(lo, hi);
        }
        ov_cc_n = n;
    }
    for (u32 x = 0; x < OV_W; x++) {
        if (!(c[x] & 0xff)) break;                       /* comme le tracé d'origine */
        u32 lo = x * n / OV_W, hi = (x + 1) * n / OV_W;
        if (hi <= lo) hi = lo + 1;
        if (hi > n) break;
        u16 col = ov_cc[x];
        u16 *p = OV_IMG + x;
        for (u32 y = 0; y < OV_ROWS; y++, p -= OV_W)
            if (*p) *p = (u16)(col | (*p & 0x8000));
    }
}

/* Retour de la liste (V29) : le principal n'envoie qu'une marque « HDCACHE », nombre de points
 * et empreinte, si l'écran a déjà reçu cette waveform. Elle est encore dans le tampon (réécrite en
 * PWV3, voir plus bas), sauf son début que le morceau de la marque vient d'écraser (896 octets
 * recopiés, zéros compris) : gardé ici,
 * avec une somme de contrôle du reste, vérifiée avant de la redécoder. */
static u32 c_id, c_n, c_end, c_sum __attribute__((section(".data"))) = 0;
#define C_HEAD 1024                        /* au-delà du premier morceau reçu (896 octets) */
static u8 c_head[C_HEAD] __attribute__((section(".data")));
u32 cache_hit, cache_miss __attribute__((section(".data"))) = 0;
static u32 buf_sum(const u8 *b, u32 i, u32 n)
{
    u32 s = 0;
    for (; i < n; i++) s = (s << 1 | s >> 31) + b[i];
    return s;
}
void rgb_decode(u32 len)
{
    u8 *b = WAVE_BUF;
    u32 k, n;
    if (len == 16 && b[0] == 'H' && b[1] == 'D' && b[2] == 'C' && b[3] == 'A' && b[4] == 'C' &&
        b[5] == 'H' && b[6] == 'E' && b[7] == 0) {
        n = b[8] | b[9] << 8 | b[10] << 16 | (u32)b[11] << 24;
        u32 id = b[12] | b[13] << 8 | b[14] << 16 | (u32)b[15] << 24;
        if (c_n > C_HEAD && n == c_n && id == c_id && buf_sum(b, C_HEAD, c_end) == c_sum) {
            for (k = 0; k < C_HEAD; k++) b[k] = c_head[k];
            WAVE_LEN = n;
            ORIG_DECODE(n);
            rgb_points = n;
            ov_redraw = ov_n == n;
            cache_hit++;
            return;
        }
        cache_miss++;                      /* (ne devrait pas arriver) : pas de waveform */
        rgb_points = 0;
        WAVE_LEN = 0;
        ORIG_DECODE(0);
        return;
    }
    /* marque : « HDRGB5 », un octet d'options (bit 0 : hauteur d'origine), version 1 */
    for (k = 0; k < 6 && b[k] == MAGIC[k]; k++) ;
    if (k < 6 || b[7] != MAGIC[7] || len < 8 || len > BUF_MAX) {
        /* données d'origine (PWV3) : décodage d'origine, sans couleur */
        rgb_points = 0;
        key_n = 0;
        if (ov_n) { ov_n = 0; ov_redraw = 1; }           /* overview encore en couleur : à refaire */
        ORIG_DECODE(len);
        return;
    }
    tall_opt = (u8)!(b[6] & 1);
    dbg_on = (u8)(b[6] & 2);
    { u32 m = (u32)(b[6] >> 2) & 3; copy_mode = (u8)(m ? m - 1 : 1); }
    fw_opt = (u8)!(b[6] & 0x10);           /* HD_LARGE=0 : 400 colonnes */
    prof_on = dbg_on;
    n = (len - 8) / 2;
    if (n > RGB_MAX) n = RGB_MAX;
    {   /* empreinte, comme le principal la calcule pour la marque « HDCACHE » */
        u32 id = n, m = 2 * n < 4096 ? 2 * n : 4096;
        for (k = 0; k < m; k++) id = id * 31 + b[8 + k];
        c_id = id;
    }
    colors_p = 8 + 4 * n <= BUF_MAX ? (u16 *)(b + ((8 + 2 * n + 1) & ~1u)) : COLORS_FAR;
    /* 1) couleurs dans la table des points, d'après les données PWV5 */
    for (k = 0; k < n; k++) {
        u32 v = (u32)b[8 + 2 * k] << 8 | b[9 + 2 * k];
        COLORS[k] = rgb555(v >> 13 & 7, v >> 10 & 7, v >> 7 & 7);
    }
    /* 2) le tampon est réécrit en PWV3 (hauteur + indice de couleur fixe), sur place (k ≤ 8 + 2k) */
    for (k = 0; k < n; k++) {
        u32 v = (u32)b[8 + 2 * k] << 8 | b[9 + 2 * k];
        b[k] = (u8)((v >> 2 & 0x1f) | RGB_INDEX << 5);
    }
    WAVE_LEN = n;                          /* longueur en points pour le reste du programme */
    ov_cc_n = 0;                           /* couleurs de l'overview à recalculer */
    ORIG_DECODE(n);
    for (k = 0; k < C_HEAD && k < n; k++) c_head[k] = b[k];     /* après le décodage d'origine */
    c_end = (u8 *)colors_p == (u8 *)COLORS_FAR ? n : (u32)((u8 *)(colors_p + n) - b);
    c_sum = buf_sum(b, C_HEAD, c_end); c_n = colors_p == COLORS_FAR ? 0 : n;   /* pas de cache au-delà de 25 min */
    rgb_points = n;
    /* l'overview de ce morceau est arrivée avant ses couleurs : à redessiner. Pas d'ici (réception
     * de la liaison) : rgb_column le fait depuis la boucle de tracé de l'interface. */
    ov_redraw = ov_n == n;
}

static void rgb_column_i(int x, int i0, int i1, int a4, int mode)
{
    if (mode == 0 && i1 > i0) { wc_x = x; wc_i0 = i0; wc_ppc = i1 - i0; }   /* repères (cue_paint) */
    if (ov_redraw) { ov_redraw = 0; ORIG_OV_UPDATE(); }
    /* tonalité : repeinte une fois par image tant que la grande waveform est tracée (le
     * bandeau n'est pas recomposé à l'arrêt, et l'est par d'autres chemins en vue liste) */
    static u8 kp;
    if (x == 0 && mode == 0 && ov_n && !(kp++ & 15)) key_paint();   /* rgb_after la refait aux copies */
    /* Couleur de la colonne : posée dans l'entrée de la table des 8 couleurs que lit le traceur
     * d'origine, le temps de son appel. Avant la V29, la colonne était tracée en bleu puis relue
     * et recolorée pixel par pixel dans l'image (400 colonnes × 45 lignes à 800 octets d'écart :
     * un défaut de cache par pixel, la moitié du coût du dessin au profileur). */
    if (mode == 0 && col_ok && x >= 0 && x < (int)g_imgw && i0 >= 0 && (u32)i0 < rgb_points) {
        volatile u32 *tab = (volatile u32 *)COLOR_TAB;
        u32 save = tab[RGB_INDEX];
        tab[RGB_INDEX] = (save & 0xffff8000u) | (COLORS[i0] & 0x7fff);
        unsigned t = prof_t0(); ORIG_COLUMN(x, i0, i1, a4, mode); prof_add(3, t);
        tab[RGB_INDEX] = save;
    } else { unsigned t = prof_t0(); ORIG_COLUMN(x, i0, i1, a4, mode); prof_add(3, t); }
    if (mode == 0 && ph_hide) { unsigned t = prof_t0(); cue_image_col(x); prof_add(17, t); }
}

/* ---- enveloppes mesurées (profileur) ---- */
u8 *rgb_dst(u8 *dst, const u8 *src, u32 n) { unsigned t = prof_t0(); u8 *r = rgb_dst_i(dst, src, n); prof_add(4, t); return r; }
void rgb_after(u8 *dst, u32 unused, u32 n) { unsigned t = prof_t0(); rgb_after_i(dst, unused, n); prof_add(5, t); }
void ph_post(void) { unsigned t = prof_t0(); ph_post_i(); prof_add(7, t); }
void rgb_column(int x, int i0, int i1, int a4, int mode) { unsigned t = prof_t0(); rgb_column_i(x, i0, i1, a4, mode); prof_add(2, t); }
void rgb_time(u32 a, u32 b) { unsigned t = prof_t0(); rgb_time_i(a, b); prof_add(9, t); }
void rgb_overview(void) { unsigned t = prof_t0(); rgb_overview_i(); prof_add(11, t); }
static void tall_apply(void) { unsigned t = prof_t0(); tall_apply_i(); prof_add(14, t); }
static void key_paint(void) { unsigned t = prof_t0(); key_paint_i(); prof_add(15, t); }
