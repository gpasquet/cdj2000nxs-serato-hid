// hdcore — voir hdcore.h.
#include "hdcore.h"

#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

// Valeur au rang q (0..1) d'un tableau, sans le modifier.
static float quantile(const float *v, uint32_t n, double q) {
    if (!n) return 0;
    float *c = malloc(n * sizeof *c);
    memcpy(c, v, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_float);
    float r = c[(uint32_t)fmin(n - 1, q * (n - 1))];
    free(c);
    return r;
}

// MARK: - waveform couleur
//
// 150 points par seconde. Pour chaque point : pic (hauteur) et énergie (RMS) de trois bandes,
// séparées par des filtres 12 dB/octave (limites FC_BAS et FC_HAUT) ; modèle de couleur et de
// hauteur décrit à make_wave.
// Format PWV5 de rekordbox : rouge bits 15-13, vert 12-10, bleu 9-7, hauteur 6-2.

// Filtres du premier ordre en cascade (deux étages = 12 dB/octave). Le passe-haut d'un étage est
// le complément exact de son passe-bas (x − y), sans fuite des graves.
typedef struct { float a, y[2]; } flt_t;
#define FC_BAS 225.0     // séparation graves / médiums (Hz), calée sur rekordbox ; HD_FC_BAS
#define FC_HAUT 3500.0   // séparation médiums / aigus (Hz), calée sur rekordbox ; HD_FC_HAUT

static void flt_init(flt_t *f, double fc, double fs) {
    f->a = (float)(1 - exp(-2 * M_PI * fc / fs));
    f->y[0] = f->y[1] = 0;
}
static inline float lowpass(flt_t *f, float x) {
    for (int i = 0; i < 2; i++) { f->y[i] += f->a * (x - f->y[i]); x = f->y[i]; }
    return x;
}
static inline float highpass(flt_t *f, float x) {
    for (int i = 0; i < 2; i++) { f->y[i] += f->a * (x - f->y[i]); x = x - f->y[i]; }
    return x;
}

static int decode_bands(const char *path, hd_track_t *t, float **peak, float **lo, float **mid, float **hi) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    ExtAudioFileRef f = NULL;
    int ok = 0;
    if (!url || ExtAudioFileOpenURL(url, &f) != noErr) goto done;

    AudioStreamBasicDescription in = {0};
    UInt32 sz = sizeof in;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &in);
    SInt64 frames = 0;
    sz = sizeof frames;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &sz, &frames);
    double rate = in.mSampleRate > 0 ? in.mSampleRate : 44100;
    UInt32 ch = in.mChannelsPerFrame >= 2 ? 2 : 1;
    AudioStreamBasicDescription out = {
        .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBitsPerChannel = 32, .mChannelsPerFrame = ch, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4 * ch, .mBytesPerPacket = 4 * ch};
    if (ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out) != noErr) goto done;

    double spp = rate / HD_RATE;     // échantillons par point
    uint32_t cap = (uint32_t)(frames / spp) + 16;
    *peak = calloc(cap, sizeof(float));
    *lo = calloc(cap, sizeof(float));
    *mid = calloc(cap, sizeof(float));
    *hi = calloc(cap, sizeof(float));
    uint32_t *cnt = calloc(cap, sizeof(uint32_t));
    flt_t fl, fh, fm1, fm2;                       // graves < 200 Hz, aigus > 2,5 kHz, médiums entre
    double f1 = getenv("HD_FC_BAS") ? atof(getenv("HD_FC_BAS")) : FC_BAS;
    double f2 = getenv("HD_FC_HAUT") ? atof(getenv("HD_FC_HAUT")) : FC_HAUT;
    flt_init(&fl, f1, rate);
    flt_init(&fh, f2, rate);
    flt_init(&fm1, f2, rate);
    flt_init(&fm2, f1, rate);

    enum { CHUNK = 16384 };
    float *buf = malloc(CHUNK * ch * sizeof(float));
    SInt64 pos = 0;
    for (;;) {
        AudioBufferList bl = {.mNumberBuffers = 1};
        bl.mBuffers[0].mNumberChannels = ch;
        bl.mBuffers[0].mDataByteSize = CHUNK * ch * sizeof(float);
        bl.mBuffers[0].mData = buf;
        UInt32 n = CHUNK;
        if (ExtAudioFileRead(f, &n, &bl) != noErr || n == 0) break;
        for (UInt32 i = 0; i < n; i++, pos++) {
            float x = ch == 2 ? 0.5f * (buf[2 * i] + buf[2 * i + 1]) : buf[i];
            uint32_t k = (uint32_t)(pos / spp);
            if (k >= cap) k = cap - 1;
            float a = fabsf(x), l = lowpass(&fl, x), h = highpass(&fh, x),
                  m = highpass(&fm2, lowpass(&fm1, x));
            if (a > (*peak)[k]) (*peak)[k] = a;
            (*lo)[k] += l * l;
            (*mid)[k] += m * m;
            (*hi)[k] += h * h;
            cnt[k]++;
        }
    }
    uint32_t np = (uint32_t)ceil(pos / spp);
    if (np > cap) np = cap;
    for (uint32_t k = 0; k < np; k++) {
        float c = cnt[k] ? (float)cnt[k] : 1;
        (*lo)[k] = sqrtf((*lo)[k] / c);
        (*mid)[k] = sqrtf((*mid)[k] / c);
        (*hi)[k] = sqrtf((*hi)[k] / c);
    }
    free(cnt);
    free(buf);
    t->n = np;
    t->duration = pos / rate;
    ok = np > 0;
done:
    if (f) ExtAudioFileDispose(f);
    if (url) CFRelease(url);
    return ok;
}

// Modèle calé sur les données RGB de rekordbox (PWV5), la référence des platines Pioneer et du
// mod LNNRT : 54 000 points de « Aye Gyl » et « Bad Boys » analysés par rekordbox, comparés point
// par point (EXPERIENCE.md, 14/11/2026). Règles constatées dans ces données :
//  - la couleur est toujours à pleine luminosité (la composante la plus forte vaut 7) ;
//  - rouge = graves (< 225 Hz), vert = médiums, bleu = aigus (> 3,5 kHz), sans lissage dans le
//    temps ; composantes = énergie^0,4, aigus × 1,5, rapportées à la plus forte ;
//  - hauteur = 31 × (pic / pic maximal du morceau)^2,1 : proche de l'énergie, donc très creusée.
// Écart moyen obtenu : 0,5 niveau (sur 7) par composante, 1 niveau (sur 31) en hauteur.
// Réglages pour les essais (variables d'environnement) : HD_FC_BAS, HD_FC_HAUT (Hz),
// HD_GAINS="g,m,a", HD_EXP="g,m,a" (compression), HD_LISSAGE (points), HD_GAMMA et HD_HMAX (hauteur),
// HD_LUMI (1 : luminosité selon le pic, ancien modèle « Serato »), HD_DECALAGE_MP3 (ms).
static float env_f(const char *k, float d) { const char *v = getenv(k); return v ? (float)atof(v) : d; }

static void make_wave(hd_track_t *t, const float *peak, const float *lo, const float *mid, const float *hi,
                      uint32_t shift) {
    uint32_t n = t->n + shift > HD_POINTS_MAX ? HD_POINTS_MAX : t->n + shift;
    float g[3] = {1.0f, 1.0f, 1.5f}, ex[3] = {0.4f, 0.4f, 0.4f};
    const char *gs = getenv("HD_GAINS"), *es = getenv("HD_EXP");
    if (gs) sscanf(gs, "%f,%f,%f", &g[0], &g[1], &g[2]);
    if (es) sscanf(es, "%f,%f,%f", &ex[0], &ex[1], &ex[2]);
    float gamma = env_f("HD_GAMMA", 2.1f), hmax = env_f("HD_HMAX", 31.0f);
    int lumi = (int)env_f("HD_LUMI", 0);
    float pmax = fmaxf(quantile(peak, t->n, 0.999), 1e-6f);
    t->wave_len = 8 + 2 * n;
    t->wave = calloc(1, t->wave_len);
    memcpy(t->wave, "HDRGB5\0\1", 8);
    // octet 6 de la marque = options lues par l'écran (firmware ≥ v22 ; avant, la marque doit
    // rester intacte). Bit 0 (HD_TALL=0) : grande waveform à sa hauteur d'origine.
    const char *tall = getenv("HD_TALL");
    if (tall && tall[0] == '0') t->wave[6] = 1;
    // HD_DEBUG=1 : l'écran affiche ses mesures de fluidité dans le bandeau (firmware ≥ v29)
    const char *dbg = getenv("HD_DEBUG");
    if (dbg && dbg[0] == '1') t->wave[6] |= 2;
    // HD_FLUIDE=0/1/2 (firmware ≥ v32) : recopies de la waveform étirée dans l'écran. 0 : comme la
    // v31 (relues dans l'autre tampon d'écran) ; 1 (défaut) : les 32 lignes du haut depuis une
    // copie en mémoire cachée ; 2 : toute la zone (les 122 lignes) depuis cette copie. Essai sur
    // platine : comparer les images/s de HD_DEBUG.
    // HD_LARGE=0 (firmware ≥ v33) : grande waveform sur 400 colonnes, comme à l'origine (sinon
    // toute la largeur de l'écran, sans marge ni molette de zoom)
    const char *lg = getenv("HD_LARGE");
    if (lg && lg[0] == '0') t->wave[6] |= 0x10;
    const char *fl = getenv("HD_FLUIDE");
    if (fl && fl[0] >= '0' && fl[0] <= '2') t->wave[6] |= (uint8_t)((fl[0] - '0' + 1) << 2);
    for (uint32_t j = shift; j < n; j++) {
        uint32_t k = j - shift;
        float p = fminf(1, peak[k] / pmax);
        int h = (int)lrintf(fmaxf(0, fminf(31, hmax * powf(p, gamma) - 0.5f)));
        float c[3] = {g[0] * powf(lo[k], ex[0]), g[1] * powf(mid[k], ex[1]), g[2] * powf(hi[k], ex[2])};
        float m = fmaxf(c[0], fmaxf(c[1], c[2]));
        float v = lumi ? fminf(1, 0.33f + 0.67f * powf(p, 0.75f)) : 1.0f;
        int rgb[3] = {0, 0, 0};
        if (m > 1e-9f)
            for (int i = 0; i < 3; i++) rgb[i] = (int)lrintf(7 * v * c[i] / m);
        if (getenv("HD_MIRE")) {        // mire d'essai : R, V, B, jaune, orange, violet, blanc, gris (0,3 s chacun)
            static const int mire[8][3] = {{7,0,0},{0,7,0},{0,0,7},{7,7,0},{7,3,0},{4,0,7},{7,7,7},{3,3,3}};
            memcpy(rgb, mire[(j / 45) % 8], sizeof rgb);
            h = 12 + 2 * (int)((j / 45) % 8);
        }
        uint16_t w = (uint16_t)(rgb[0] << 13 | rgb[1] << 10 | rgb[2] << 7 | h << 2);
        t->wave[8 + 2 * j] = w >> 8;
        t->wave[9 + 2 * j] = w & 0xFF;
    }
    t->n = n;
}

// Type de MP3 pour le recalage sur la timeline de Serato : 70 (trame Xing/Info), 35 (amorçage
// connu d'AudioToolbox sans cette trame), 0 (rien).
static double mp3_shift_ms(const char *path) {
    uint8_t h[10], b[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long off = 0;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3))
        off = 10 + ((long)(h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f));
    fseek(f, off, SEEK_SET);
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    size_t i = 0;
    while (i + 4 < n && !(b[i] == 0xff && (b[i + 1] & 0xe0) == 0xe0)) i++;
    for (size_t k = i + 4; k + 4 <= n && k < i + 48; k++)
        if (!memcmp(b + k, "Xing", 4) || !memcmp(b + k, "Info", 4)) return 70;
    double ms = 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    AudioFileID af;
    if (url && AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) == noErr) {
        AudioFilePacketTableInfo pt;
        UInt32 sz = sizeof pt;
        if (AudioFileGetProperty(af, kAudioFilePropertyPacketTableInfo, &sz, &pt) == noErr && pt.mPrimingFrames > 0) ms = 35;
        AudioFileClose(af);
    }
    if (url) CFRelease(url);
    return ms;
}

// MARK: - grille Serato
//
// Tag « Serato BeatGrid » : en clair dans une trame ID3 GEOB (MP3, AIFF, WAV), ou en base64
// précédé de « application/octet-stream\0\0Serato BeatGrid\0 » (FLAC, M4A).
// Contenu : 01 00, nombre de repères (u32 BE), puis par repère : position en s (f32 BE) et
// nombre de temps jusqu'au repère suivant (u32 BE) ; le dernier : position et BPM (f32 BE).
// Le premier repère est le premier temps d'une mesure.

static const char GEOB_DESC[] = "Serato BeatGrid";
static const char B64_HEAD[] = "YXBwbGljYXRpb24vb2N0ZXQtc3RyZWFtAABTZXJhdG8gQmVhdEdyaWQA";
static const char MIME_HEAD[] = "application/octet-stream\0\0Serato BeatGrid";  // + '\0'

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static float bef(const uint8_t *p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const uint8_t *s, size_t n, uint8_t *out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r') continue;
        int v = b64v(s[i]);
        if (v < 0) break;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
    }
    return o;
}

static const uint8_t *memfind(const uint8_t *h, size_t hn, const void *nd, size_t nn) {
    if (nn > hn) return NULL;
    for (const uint8_t *p = h; (p = memchr(p, ((const uint8_t *)nd)[0], hn - nn + 1 - (p - h))); p++)
        if (!memcmp(p, nd, nn)) return p;
    return NULL;
}

static uint32_t grid_from_markers(const uint8_t *d, size_t len, double dur, uint8_t *grid, uint32_t max, double *bpm) {
    if (len < 6 || d[0] != 1 || d[1] != 0) return 0;
    uint32_t m = be32(d + 2);
    if (m == 0 || m > 10000 || len < 6 + 8 * (size_t)m) return 0;
    const uint8_t *r = d + 6;
    double first = bef(r), step0 = 0;
    // temps (s) et numéro relatif au premier repère
    enum { CAP = 1 << 16 };
    double *tt = malloc(CAP * sizeof *tt);
    uint32_t nt = 0;
    for (uint32_t i = 0; i + 1 < m && nt < CAP; i++) {
        double p = bef(r + 8 * i), q = bef(r + 8 * (i + 1));
        uint32_t nb = be32(r + 8 * i + 4);
        if (nb == 0 || nb > 100000 || q <= p) continue;
        double st = (q - p) / nb;
        if (!step0) step0 = st;
        for (uint32_t k = 0; k < nb && nt < CAP; k++) tt[nt++] = p + k * st;
    }
    double pl = bef(r + 8 * (m - 1)), b = bef(r + 8 * (m - 1) + 4);
    if (!(b > 20 && b < 400)) { free(tt); return 0; }
    *bpm = b;
    double st = 60.0 / b;
    if (!step0) step0 = st;
    for (double x = pl; x < dur && nt < CAP; x += st) tt[nt++] = x;
    // temps avant le premier repère
    int back = 0;
    while (first - (back + 1) * step0 >= 0) back++;
    uint32_t out = 0;
    for (int i = back; i >= 1 && out < max; i--, out++) {
        double x = first - i * step0;
        int num = ((-i) % 4 + 4) % 4 + 1;
        uint32_t ms = (uint32_t)lrint(x * 1000);
        grid[4 * out] = (uint8_t)num; grid[4 * out + 1] = ms >> 16 & 0xFF;
        grid[4 * out + 2] = ms & 0xFF; grid[4 * out + 3] = ms >> 8 & 0xFF;
    }
    for (uint32_t i = 0; i < nt && out < max; i++, out++) {
        uint32_t ms = (uint32_t)lrint(fmax(0, tt[i]) * 1000);
        grid[4 * out] = (uint8_t)(i % 4 + 1); grid[4 * out + 1] = ms >> 16 & 0xFF;
        grid[4 * out + 2] = ms & 0xFF; grid[4 * out + 3] = ms >> 8 & 0xFF;
    }
    free(tt);
    return out;
}

uint32_t hd_serato_grid(const char *path, double duration, uint8_t *grid, uint32_t max, double *bpm) {
    *bpm = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 400L * 1024 * 1024) { fclose(f); return 0; }
    uint8_t *d = malloc(sz);
    size_t got = fread(d, 1, sz, f);
    fclose(f);
    uint32_t n = 0;
    const uint8_t *p = memfind(d, got, GEOB_DESC, sizeof GEOB_DESC);   // avec le '\0' final
    if (p) n = grid_from_markers(p + sizeof GEOB_DESC, got - (p + sizeof GEOB_DESC - d), duration, grid, max, bpm);
    if (!n && (p = memfind(d, got, B64_HEAD, sizeof B64_HEAD - 1))) {
        size_t rest = got - (p - d), e = 0;
        while (e < rest && (b64v(p[e]) >= 0 || p[e] == '\n' || p[e] == '\r')) e++;
        uint8_t *dec = malloc(e + 4);
        size_t dn = b64_decode(p, e, dec);
        size_t hl = sizeof MIME_HEAD;                                  // avec le '\0' final
        if (dn > hl && !memcmp(dec, MIME_HEAD, hl)) n = grid_from_markers(dec + hl, dn - hl, duration, grid, max, bpm);
        free(dec);
    }
    free(d);
    return n;
}

// MARK: - repères Serato (hot cues, boucles)

// Tag « Serato Markers2 » : dans une trame ID3 GEOB (MP3, AIFF, WAV), « 01 01 » puis le contenu
// en base64 ; en FLAC / M4A, le tout est encore en base64 derrière « application/octet-stream
// \0\0Serato Markers2\0 ». Contenu décodé : 01 01, puis des entrées « nom\0, longueur u32 BE,
// corps ». CUE : 00, indice, position ms u32 BE, 00, RVB, … ; LOOP : 00, indice, début, fin (ms).
static const char M2_DESC[] = "Serato Markers2";
static const char M2_B64[] = "YXBwbGljYXRpb24vb2N0ZXQtc3RyZWFtAABTZXJhdG8gTWFya2VyczIA";

static size_t b64_span(const uint8_t *p, size_t rest) {
    size_t e = 0;
    while (e < rest && (b64v(p[e]) >= 0 || p[e] == '\n' || p[e] == '\r' || p[e] == '=')) e++;
    return e;
}

// Entrées du contenu décodé (après 01 01) → repères.
static void m2_entries(const uint8_t *x, size_t n, hd_track_t *t) {
    size_t i = 2;
    while (i < n) {
        const uint8_t *z = memchr(x + i, 0, n - i);
        if (!z || z == x + i || (size_t)(z - x) + 5 > n) break;
        size_t nl = z - (x + i), j = (z - x) + 1;
        uint32_t L = be32(x + j);
        const uint8_t *b = x + j + 4;
        if (j + 4 + L > n) break;
        if (nl == 3 && !memcmp(x + i, "CUE", 3) && L >= 6 && b[1] < 8 && t->cue_n < HD_CUE_MAX) {
            t->cue_idx[t->cue_n] = b[1]; t->cue_ms[t->cue_n] = be32(b + 2); t->cue_type[t->cue_n] = 0;
            if (L >= 10) memcpy(t->cue_rgb[t->cue_n], b + 7, 3);       // couleur Serato
            t->cue_n++;
        }
        // (Boucles enregistrées « LOOP » non envoyées : l'affichage ne trace un début de boucle
        // (genre 3) qu'avec sa fin (17), et dessine alors la zone comme une boucle ACTIVE.)
        i = j + 4 + L;
    }
}

// Contenu « 01 01 base64 » (celui de la trame GEOB) → repères.
static void m2_payload(const uint8_t *p, size_t rest, hd_track_t *t) {
    if (rest < 2 || p[0] != 1 || p[1] != 1) return;
    size_t e = b64_span(p + 2, rest - 2);
    uint8_t *dec = malloc(e + 8);
    size_t dn = b64_decode(p + 2, e, dec);
    if (dn >= 2 && dec[0] == 1 && dec[1] == 1) m2_entries(dec, dn, t);
    free(dec);
}

static void hd_serato_cues(const char *path, hd_track_t *t) {
    t->cue_n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 400L * 1024 * 1024) { fclose(f); return; }
    uint8_t *d = malloc(sz);
    size_t got = fread(d, 1, sz, f);
    fclose(f);
    const uint8_t *p = memfind(d, got, M2_DESC, sizeof M2_DESC);        // avec le '\0' final
    if (p) m2_payload(p + sizeof M2_DESC, got - (p + sizeof M2_DESC - d), t);
    if (!t->cue_n && (p = memfind(d, got, M2_B64, sizeof M2_B64 - 1))) {
        size_t e = b64_span(p, got - (p - d));
        uint8_t *dec = malloc(e + 8);
        size_t dn = b64_decode(p, e, dec);
        const uint8_t *q = memfind(dec, dn, M2_DESC, sizeof M2_DESC);
        if (q) m2_payload(q + sizeof M2_DESC, dn - (q + sizeof M2_DESC - dec), t);
        free(dec);
    }
    free(d);
}

// Genre de repère pour l'affichage de la platine (paquet 0x05, notes-cue.md). HD_CUES_TEST =
// « genre:secondes,… » remplace les repères du fichier (essais de genres dans l'émulateur).
static void cue_kinds(hd_track_t *t) {
    const char *e = getenv("HD_CUES_TEST");
    if (e && *e) {
        t->cue_n = 0;
        while (*e && t->cue_n < HD_CUE_MAX) {
            unsigned k; double sec; int used = 0;
            if (sscanf(e, "%i:%lf%n", &k, &sec, &used) != 2) break;
            t->cue_kind[t->cue_n] = (uint16_t)k; t->cue_ms[t->cue_n] = (uint32_t)lrint(sec * 1000);
            t->cue_n++;
            e += used;
            if (*e == ',') e++;
        }
        return;
    }
    // Genres vérifiés dans l'émulateur (16/11) : 4, 5, 6 = hot cues A, B, C (triangles verts
    // avec lettre sous la petite waveform) ; 0 = cue mémoire (petit triangle en haut de la petite
    // et de la grande waveform, sans lettre, plusieurs possibles). Choix de l'utilisateur (16/11) :
    // tous les hot cues en triangles du haut, sans lettre, à la couleur de Serato (recoloriés
    // par le programme d'affichage). Rangés par position : l'écran retrouve les couleurs dans
    // cet ordre.
    for (uint32_t i = 1; i < t->cue_n; i++)
        for (uint32_t j = i; j > 0 && t->cue_ms[j - 1] > t->cue_ms[j]; j--) {
            uint32_t m = t->cue_ms[j]; t->cue_ms[j] = t->cue_ms[j - 1]; t->cue_ms[j - 1] = m;
            uint8_t a = t->cue_idx[j]; t->cue_idx[j] = t->cue_idx[j - 1]; t->cue_idx[j - 1] = a;
            uint8_t c[3]; memcpy(c, t->cue_rgb[j], 3); memcpy(t->cue_rgb[j], t->cue_rgb[j - 1], 3);
            memcpy(t->cue_rgb[j - 1], c, 3);
        }
    for (uint32_t i = 0; i < t->cue_n; i++) t->cue_kind[i] = HD_KIND_MEMORY;
}

// MARK: - analyse complète

// Tonalité inscrite dans les tags du fichier (Serato l'y écrit à l'analyse : ID3 TKEY, etc.).
// Tonalité écrite par Serato hors du dictionnaire d'AudioToolbox (essai V28 : FLAC sans tonalité à
// l'écran) : commentaire Vorbis « INITIALKEY=… » (FLAC, OGG), atome « ----:…:initialkey » (M4A).
static void key_from_tags(const char *path, char *key, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t max = 4 << 20, got;
    uint8_t *d = malloc(max);
    if (!d) { fclose(f); return; }
    got = fread(d, 1, max, f);
    fclose(f);
    const char *v = NULL; size_t vl = 0;
    for (size_t i = 4; i + 11 < got && !v; i++) {
        if (strncasecmp((const char *)d + i, "INITIALKEY=", 11)) continue;
        uint32_t l = d[i - 4] | d[i - 3] << 8 | d[i - 2] << 16 | (uint32_t)d[i - 1] << 24;   // longueur Vorbis
        if (l > 11 && l < 64 && i + l <= got) { v = (const char *)d + i + 11; vl = l - 11; }
    }
    for (size_t i = 0; i + 10 < got && !v; i++) {
        if (memcmp(d + i, "initialkey", 10)) continue;
        const uint8_t *q = d + i + 10;                 // puis l'atome « data » : taille, « data », 8 octets
        if (q + 16 <= d + got && !memcmp(q + 4, "data", 4)) {
            uint32_t l = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
            if (l > 16 && l < 64 && q + l <= d + got) { v = (const char *)q + 16; vl = l - 16; }
        }
    }
    size_t n = 0;
    for (size_t i = 0; v && i < vl && n + 1 < cap && n < 6; i++)
        if (v[i] > ' ' && v[i] < 127) key[n++] = v[i];
    key[n] = 0;
    free(d);
}

static void read_key(const char *path, char *key, size_t cap) {
    key[0] = 0;
    if (getenv("HD_CLE")) { snprintf(key, cap, "%s", getenv("HD_CLE")); return; }   // essais
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    AudioFileID af = NULL;
    if (url && AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) == noErr) {
        CFDictionaryRef info = NULL;
        UInt32 sz = sizeof info;
        if (AudioFileGetProperty(af, kAudioFilePropertyInfoDictionary, &sz, &info) == noErr && info) {
            CFStringRef k = CFDictionaryGetValue(info, CFSTR(kAFInfoDictionary_KeySignature));
            if (k && CFGetTypeID(k) == CFStringGetTypeID()) {
                char tmp[32] = "";
                CFStringGetCString(k, tmp, sizeof tmp, kCFStringEncodingASCII);
                size_t n = 0;
                for (char *c = tmp; *c && n + 1 < cap && n < 6; c++)
                    if (*c > ' ' && *c < 127) key[n++] = *c;
                key[n] = 0;
            }
            CFRelease(info);
        }
        AudioFileClose(af);
    }
    if (url) CFRelease(url);
    if (!key[0]) key_from_tags(path, key, cap);
}

hd_track_t *hd_analyse(const char *path) {
    double t0 = now();
    hd_track_t *t = calloc(1, sizeof *t);
    float *peak = NULL, *lo = NULL, *mid = NULL, *hi = NULL;
    read_key(path, t->key, sizeof t->key);
    if (!decode_bands(path, t, &peak, &lo, &mid, &hi)) {
        free(peak); free(lo); free(mid); free(hi); free(t);
        return NULL;
    }
    // lissage des énergies sur ±2 points (≈ 33 ms) : calé sur Serato, dont la couleur ne varie pas
    // d'un point à l'autre à l'intérieur d'un même coup (HD_LISSAGE pour les essais)
    int r = (int)env_f("HD_LISSAGE", 0);
    if (r > 0) {
        float *tmp = malloc(t->n * sizeof(float));
        float *band[3] = {lo, mid, hi};
        for (int bi = 0; bi < 3; bi++) {
            double acc = 0;
            int a0 = 0, a1 = 0;          // fenêtre [a0, a1[
            for (uint32_t k = 0; k < t->n; k++) {
                int lo_i = (int)k - r < 0 ? 0 : (int)k - r, hi_i = (int)k + r + 1 > (int)t->n ? (int)t->n : (int)k + r + 1;
                while (a1 < hi_i) { acc += (double)band[bi][a1] * band[bi][a1]; a1++; }
                while (a0 < lo_i) { acc -= (double)band[bi][a0] * band[bi][a0]; a0++; }
                tmp[k] = (float)sqrt(fmax(0, acc) / (a1 - a0));
            }
            memcpy(band[bi], tmp, t->n * sizeof(float));
        }
        free(tmp);
    }
    if (getenv("HD_DUMP")) {          // calage : pic et énergie des trois bandes, en float32
        FILE *f = fopen(getenv("HD_DUMP"), "wb");
        if (f) {
            fwrite(peak, 4, t->n, f); fwrite(lo, 4, t->n, f); fwrite(mid, 4, t->n, f); fwrite(hi, 4, t->n, f);
            fclose(f);
        }
    }
    // Serato place les MP3 70 ms plus loin sur sa timeline que le décodage d'AudioToolbox (et
    // que rekordbox) : mesuré sur « Aye Gyl » (LAME 3.100). La grille et la position de lecture
    // venant de Serato, la waveform est décalée d'autant pour rester alignée.
    // Ce décalage dépend de ce qu'AudioToolbox retire au début du fichier (essai V11, 15/11 :
    // grille décalée sur les MP3 sans en-tête LAME). Mesuré, à ±10 ms, entre la grille Serato
    // et les attaques : trame Xing/Info (LAME) 70 ms ; amorçage iTunes seul (iTunSMPB) 35 ms ;
    // aucune information de lecture sans blanc 0 ms. HD_DECALAGE_MP3 impose une valeur.
    uint32_t shift = 0;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".mp3")) {
        double ms = getenv("HD_DECALAGE_MP3") ? atof(getenv("HD_DECALAGE_MP3")) : mp3_shift_ms(path);
        t->shift_ms = ms;
        shift = (uint32_t)lrint(ms * HD_RATE / 1000.0);
    }
    make_wave(t, peak, lo, mid, hi, shift);
    free(peak); free(lo); free(mid); free(hi);
    t->grid = calloc(HD_GRID_MAX, 4);
    t->grid_n = hd_serato_grid(path, t->duration, t->grid, HD_GRID_MAX, &t->bpm);
    hd_serato_cues(path, t);
    cue_kinds(t);
    t->analyse_s = now() - t0;
    return t;
}

void hd_free(hd_track_t *t) {
    if (!t) return;
    free(t->cz);
    free(t->wave);
    free(t->grid);
    free(t);
}

// MARK: - paquets HID 0x30

void hd_clear_packet(uint8_t pkt[64]) {
    memset(pkt, 0, 64);
    pkt[0] = 0x30;
}

// Waveform compressée (firmware ≥ v36, voir firmware/patch/hdcz.h) : environ deux fois moins de
// paquets, donc deux fois moins d'attente au chargement. HD_COMPRESSE=0 : envoi d'origine (30 02).
#define CZ_ENCODER
#include "../firmware/patch/hdcz.h"
static void compress(hd_track_t *t) {
    const char *e = getenv("HD_COMPRESSE");
    if (t->cz_len || (e && e[0] == '0') || t->wave_len < 8) return;
    uint32_t n = (t->wave_len - 8) / 2;
    for (uint32_t j = 0; j < n; j++) if (t->wave[9 + 2 * j] & 3) return;   // bits 1-0 à zéro
    uint32_t cap = 2 * n + 64;
    uint8_t *out = malloc(cap);
    cz_enc_t *z = malloc(sizeof *z);
    if (!out || !z) { free(out); free(z); return; }
    cz_enc_init(z, out, cap);
    for (uint32_t j = 0; j < n; j++) cz_epoint(z, (unsigned)t->wave[8 + 2 * j] << 8 | t->wave[9 + 2 * j]);
    cz_enc_end(z);
    if (z->n <= cap && z->n < 2 * n) { t->cz = out; t->cz_len = z->n; } else free(out);
    free(z);
}

uint32_t hd_packet_count(const hd_track_t *t) {
    compress((hd_track_t *)t);
    uint32_t w = t->cz_len ? t->cz_len : t->wave_len;
    return 4 + (t->key[0] != 0) + 2 * (t->cue_n != 0) + (w + 59) / 60 + (t->grid_n * 4 + 59) / 60;
}

int hd_packets(const hd_track_t *t, int (*send)(void *ctx, const uint8_t pkt[64]), void *ctx) {
    uint8_t p[64];
#define SEND() do { if (send(ctx, p)) return -1; } while (0)
    hd_clear_packet(p);
    SEND();
    // 30 07 : tonalité (ASCII), affichée devant le titre par le patch
    if (t->key[0]) {
        memset(p, 0, 64);
        p[0] = 0x30; p[1] = 0x07; p[2] = (uint8_t)strlen(t->key);
        memcpy(p + 3, t->key, p[2]);
        SEND();
    }
    // 30 01 : longueur, durée (minutes, secondes × 256)
    memset(p, 0, 64);
    p[0] = 0x30; p[1] = 0x01;
    uint32_t L = t->wave_len;
    memcpy(p + 2, &L, 4);                               // petit-boutiste (SH-4 et Mac)
    compress((hd_track_t *)t);
    if (t->cz_len) {                                    // octet 10 = 1 : suivent des 30 0a
        p[10] = 1;
        memcpy(p + 12, &t->cz_len, 4);
        memcpy(p + 16, t->wave, 8);
    }
    uint16_t mn = (uint16_t)(t->duration / 60);
    uint16_t s256 = (uint16_t)lrint((t->duration - mn * 60) * 256);
    if (s256 >= 60 * 256) s256 = 60 * 256 - 1;
    memcpy(p + 6, &mn, 2);
    memcpy(p + 8, &s256, 2);
    SEND();
    memset(p, 0, 64);
    p[0] = 0x30; p[1] = 0x03;
    memcpy(p + 2, &t->grid_n, 4);
    SEND();
    uint32_t gl = t->grid_n * 4;
    for (uint32_t off = 0, k = 0; off < gl; off += 60, k++) {
        memset(p, 0, 64);
        p[0] = 0x30; p[1] = 0x04; p[2] = k & 0xFF; p[3] = k >> 8;
        memcpy(p + 4, t->grid + off, gl - off < 60 ? gl - off : 60);
        SEND();
    }
    // 30 10 n (genre u16, ms u32)×n : repères, ajoutés par le patch à la liste de l'affichage
    if (t->cue_n) {
        memset(p, 0, 64);
        uint32_t n = t->cue_n > 8 ? 8 : t->cue_n;
        p[0] = 0x30; p[1] = 0x10; p[2] = (uint8_t)n;
        for (uint32_t i = 0; i < n; i++) {
            memcpy(p + 3 + 6 * i, &t->cue_kind[i], 2);
            memcpy(p + 5 + 6 * i, &t->cue_ms[i], 4);
        }
        SEND();
        // 30 11 n (R V B)×n : couleurs, dans le même ordre
        memset(p, 0, 64);
        p[0] = 0x30; p[1] = 0x11; p[2] = (uint8_t)n;
        for (uint32_t i = 0; i < n; i++) memcpy(p + 3 + 3 * i, t->cue_rgb[i], 3);
        SEND();
    }
    // 30 14 : tout le reste est arrivé (tonalité, durée, grille, repères) ; la platine peut
    // lancer le chargement à l'écran dès le premier paquet de waveform et la lui transmettre au
    // fur et à mesure (firmware ≥ v20 ; ignoré avant). HD_EARLY=0 : attendre la fin (30 05).
    const char *early = getenv("HD_EARLY");
    if (!(early && *early == '0')) {
        memset(p, 0, 64);
        p[0] = 0x30; p[1] = 0x14;
        SEND();
    }
    const uint8_t *wd = t->cz_len ? t->cz : t->wave;
    uint32_t wl = t->cz_len ? t->cz_len : t->wave_len;
    for (uint32_t off = 0, k = 0; off < wl; off += 60, k++) {
        memset(p, 0, 64);
        p[0] = 0x30; p[1] = t->cz_len ? 0x0a : 0x02; p[2] = k & 0xFF; p[3] = k >> 8;
        memcpy(p + 4, wd + off, wl - off < 60 ? wl - off : 60);
        SEND();
    }
    memset(p, 0, 64);
    p[0] = 0x30; p[1] = 0x05;
    SEND();
#undef SEND
    return 0;
}
