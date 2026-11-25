/* Waveform compressée (V36), commune au codeur du Mac (mac/hdcore.c) et au décodeur du principal
 * (hdwave.c). Le lien USB vers la platine ne passe qu'un paquet de 64 octets par milliseconde :
 * 1,6 s pour les 90 Ko d'un morceau de 5 min, waveform figée à l'écran pendant ce temps.
 *
 * Points de 16 bits (gros-boutiste dans le tampon) : couleur sur 9 bits (R, V, B sur 3 bits,
 * bits 15-7), hauteur sur 5 bits (bits 6-2), bits 1-0 à zéro.
 * Codage arithmétique binaire adaptatif, comme celui de LZMA (probabilités sur 11 bits, adaptation
 * d'un 32e, renormalisation par octet). Modèles :
 *  - hauteur : arbre de 5 bits, selon la hauteur du point précédent (32 contextes) ;
 *  - « même couleur que le point précédent » : selon (même couleur au point précédent, hauteur
 *    bornée à 3) ;
 *  - sinon couleur : arbre de 9 bits, selon les 3 bits hauts de la couleur précédente.
 * Mesuré sur trois morceaux : 7,4 à 7,5 bits par point au lieu de 16 (÷ 2,15), sans perte.
 * Un point consomme au plus 15 décisions de 12 bits chacune au pire : moins de 32 octets. */
#ifndef HDCZ_H
#define HDCZ_H

#define CZ_FN static __attribute__((unused))
#define CZ_NH (32 * 32)
#define CZ_NF 8
#define CZ_NC (8 * 512)

typedef struct {
    unsigned short mh[CZ_NH], mf[CZ_NF], mc[CZ_NC];
    unsigned char h, same;
    unsigned short c;
} cz_model_t;

CZ_FN void cz_model_init(cz_model_t *m)
{
    for (unsigned i = 0; i < CZ_NH; i++) m->mh[i] = 1024;
    for (unsigned i = 0; i < CZ_NF; i++) m->mf[i] = 1024;
    for (unsigned i = 0; i < CZ_NC; i++) m->mc[i] = 1024;
    m->h = 0; m->same = 0; m->c = 0;
}

/* ---- décodeur : octets lus dans un anneau (ring, masque mask), pos = octets déjà consommés */
typedef struct {
    cz_model_t m;
    unsigned range, code, pos, mask;
    const unsigned char *ring;
} cz_dec_t;

CZ_FN unsigned cz_byte(cz_dec_t *d) { return d->ring[d->pos++ & d->mask]; }

CZ_FN void cz_dec_init(cz_dec_t *d, const unsigned char *ring, unsigned mask)
{
    cz_model_init(&d->m);
    d->ring = ring; d->mask = mask; d->pos = 0;
    d->range = 0xFFFFFFFFu; d->code = 0;
}
/* à appeler quand 5 octets au moins sont arrivés */
CZ_FN void cz_dec_start(cz_dec_t *d)
{
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | cz_byte(d);
}

CZ_FN unsigned cz_bit(cz_dec_t *d, unsigned short *p)
{
    unsigned bound = (d->range >> 11) * *p, b;
    if (d->code < bound) { d->range = bound; *p = (unsigned short)(*p + ((2048 - *p) >> 5)); b = 0; }
    else { d->range -= bound; d->code -= bound; *p = (unsigned short)(*p - (*p >> 5)); b = 1; }
    if (d->range < (1u << 24)) { d->range <<= 8; d->code = (d->code << 8) | cz_byte(d); }
    return b;
}

CZ_FN unsigned cz_tree(cz_dec_t *d, unsigned short *p, int bits)
{
    unsigned m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) | cz_bit(d, p + m);
    return m - (1u << bits);
}

/* point suivant (16 bits) */
CZ_FN unsigned cz_point(cz_dec_t *d)
{
    cz_model_t *m = &d->m;
    unsigned h = cz_tree(d, m->mh + 32 * m->h, 5);
    unsigned same = cz_bit(d, m->mf + 4 * m->same + (h < 3 ? h : 3));
    unsigned c = same ? m->c : cz_tree(d, m->mc + 512 * (m->c >> 6), 9);
    m->h = (unsigned char)h; m->same = (unsigned char)same; m->c = (unsigned short)c;
    return c << 7 | h << 2;
}

#ifdef CZ_ENCODER
/* ---- codeur (Mac) */
typedef struct {
    cz_model_t m;
    unsigned long long low;
    unsigned range, cache_size;
    unsigned char cache;
    unsigned char *out;
    unsigned n, cap;
} cz_enc_t;

CZ_FN void cz_out(cz_enc_t *e, unsigned char b) { if (e->n < e->cap) e->out[e->n] = b; e->n++; }

CZ_FN void cz_shift(cz_enc_t *e)
{
    if ((unsigned)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        unsigned char t = e->cache;
        do { cz_out(e, (unsigned char)(t + (unsigned char)(e->low >> 32))); t = 0xFF; } while (--e->cache_size != 0);
        e->cache = (unsigned char)((unsigned)e->low >> 24);
    }
    e->cache_size++;
    e->low = (unsigned long long)((unsigned)e->low << 8);
}

CZ_FN void cz_enc_init(cz_enc_t *e, unsigned char *out, unsigned cap)
{
    cz_model_init(&e->m);
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache_size = 1; e->cache = 0;
    e->out = out; e->n = 0; e->cap = cap;
}

CZ_FN void cz_ebit(cz_enc_t *e, unsigned short *p, unsigned b)
{
    unsigned bound = (e->range >> 11) * *p;
    if (!b) { e->range = bound; *p = (unsigned short)(*p + ((2048 - *p) >> 5)); }
    else { e->low += bound; e->range -= bound; *p = (unsigned short)(*p - (*p >> 5)); }
    while (e->range < (1u << 24)) { e->range <<= 8; cz_shift(e); }
}

CZ_FN void cz_etree(cz_enc_t *e, unsigned short *p, int bits, unsigned v)
{
    unsigned m = 1;
    for (int i = bits - 1; i >= 0; i--) { unsigned b = (v >> i) & 1; cz_ebit(e, p + m, b); m = (m << 1) | b; }
}

CZ_FN void cz_epoint(cz_enc_t *e, unsigned w)
{
    cz_model_t *m = &e->m;
    unsigned h = (w >> 2) & 31, c = w >> 7;
    cz_etree(e, m->mh + 32 * m->h, 5, h);
    unsigned same = c == m->c;
    cz_ebit(e, m->mf + 4 * m->same + (h < 3 ? h : 3), same);
    if (!same) cz_etree(e, m->mc + 512 * (m->c >> 6), 9, c);
    m->h = (unsigned char)h; m->same = (unsigned char)same; m->c = (unsigned short)c;
}

CZ_FN void cz_enc_end(cz_enc_t *e) { for (int i = 0; i < 5; i++) cz_shift(e); }
#endif
#endif
