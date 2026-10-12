/* ==================================================================
 * json.c - 読むだけの JSON パーサー
 *
 *  YouTube の応答は 1 回で数百 KB〜数 MB、節が数万個ある。節ごとに malloc すると
 *  確保の回数と断片化がかさむので、64KB の塊(アリーナ)から切り出し、
 *  使い終わったら塊ごと捨てる。文字列もアリーナの中でエスケープを解く。
 * ================================================================== */

#include "ytm.h"

#define BLK_SIZE (64 * 1024)

typedef struct Blk {
    struct Blk *next;
    size_t      used, cap;
    /* この後ろに中身 */
} Blk;

struct JDoc {
    Blk *blk;
    J   *root;
};

typedef struct {
    JDoc       *d;
    const char *p, *end;
    int         failed, depth;
} JP;

static void *arena(JDoc *d, size_t n)
{
    Blk *b = d->blk;
    n = (n + 7) & ~(size_t)7;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > BLK_SIZE - sizeof(Blk) ? n : BLK_SIZE - sizeof(Blk);
        b = (Blk *)malloc(sizeof(Blk) + cap);
        if (!b) return NULL;
        b->next = d->blk;
        b->used = 0;
        b->cap = cap;
        d->blk = b;
    }
    {
        char *p = (char *)(b + 1) + b->used;
        b->used += n;
        return p;
    }
}

static J *new_j(JP *jp, JType t)
{
    J *j = (J *)arena(jp->d, sizeof(J));
    if (!j) { jp->failed = 1; return NULL; }
    memset(j, 0, sizeof(*j));
    j->t = t;
    return j;
}

static void skip_ws(JP *jp)
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static int hex4(const char *p, const char *end, unsigned *out)
{
    unsigned v = 0;
    int      i;
    if (end - p < 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *put_u8(char *o, unsigned cp)
{
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* jp->p は '"' を指している。解いた文字列はアリーナに置く(元の長さを超えない) */
static const char *parse_string(JP *jp)
{
    const char *s = ++jp->p, *q = s;
    char       *out, *o;
    /* 終わりを探す */
    while (q < jp->end && *q != '"') {
        if (*q == '\\') q++;
        q++;
    }
    if (q >= jp->end) { jp->failed = 1; return NULL; }
    out = o = (char *)arena(jp->d, (size_t)(q - s) + 1);
    if (!out) { jp->failed = 1; return NULL; }
    while (jp->p < q) {
        char c = *jp->p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *jp->p++;
        switch (c) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (!hex4(jp->p, jp->end, &cp)) { jp->failed = 1; return NULL; }
            jp->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && jp->p + 6 <= q && jp->p[0] == '\\' && jp->p[1] == 'u' &&
                hex4(jp->p + 2, jp->end, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                jp->p += 6;
            }
            o = put_u8(o, cp);   /* \uXXXX(6 バイト)は UTF-8 で 3 バイト以下、対なら 12→4 */
            break;
        }
        default: jp->failed = 1; return NULL;
        }
    }
    *o = 0;
    jp->p = q + 1;
    return out;
}

static J *parse_value(JP *jp)
{
    J   *j = NULL;
    char c;
    skip_ws(jp);
    if (jp->p >= jp->end) { jp->failed = 1; return NULL; }
    if (++jp->depth > 512) { jp->failed = 1; return NULL; }
    c = *jp->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        J **tail;
        j = new_j(jp, obj ? J_OBJ : J_ARR);
        if (!j) return NULL;
        tail = &j->child;
        jp->p++;
        skip_ws(jp);
        if (jp->p < jp->end && *jp->p == (obj ? '}' : ']')) {
            jp->p++;
        } else {
            for (;;) {
                const char *key = NULL;
                J *v;
                if (obj) {
                    skip_ws(jp);
                    if (jp->p >= jp->end || *jp->p != '"') { jp->failed = 1; break; }
                    key = parse_string(jp);
                    if (!key) break;
                    skip_ws(jp);
                    if (jp->p >= jp->end || *jp->p != ':') { jp->failed = 1; break; }
                    jp->p++;
                }
                v = parse_value(jp);
                if (!v) { jp->failed = 1; break; }
                v->key = key;
                *tail = v;
                tail = &v->next;
                skip_ws(jp);
                if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
                if (jp->p < jp->end && *jp->p == (obj ? '}' : ']')) { jp->p++; break; }
                jp->failed = 1;
                break;
            }
        }
    } else if (c == '"') {
        j = new_j(jp, J_STR);
        if (j) j->s = parse_string(jp);
    } else if (jp->end - jp->p >= 4 && !strncmp(jp->p, "true", 4)) {
        j = new_j(jp, J_BOOL);
        if (j) j->n = 1;
        jp->p += 4;
    } else if (jp->end - jp->p >= 5 && !strncmp(jp->p, "false", 5)) {
        j = new_j(jp, J_BOOL);
        jp->p += 5;
    } else if (jp->end - jp->p >= 4 && !strncmp(jp->p, "null", 4)) {
        j = new_j(jp, J_NULL);
        jp->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        j = new_j(jp, J_NUM);
        if (j) j->n = strtod(jp->p, &e);   /* 本文は NUL で終わっている(net.c が足す) */
        else e = (char *)jp->p + 1;
        jp->p = e;
    } else {
        jp->failed = 1;
    }
    jp->depth--;
    return jp->failed ? NULL : j;
}

JDoc *JsonParse(const char *text, size_t len)
{
    JDoc *d;
    JP    jp;
    if (!text) return NULL;
    d = (JDoc *)calloc(1, sizeof(JDoc));
    if (!d) return NULL;
    jp.d = d;
    jp.p = text;
    jp.end = text + len;
    jp.failed = 0;
    jp.depth = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) jp.p += 3;
    d->root = parse_value(&jp);
    if (jp.failed || !d->root) { JsonFree(d); return NULL; }
    return d;
}

J *JsonRoot(JDoc *d) { return d ? d->root : NULL; }

void JsonFree(JDoc *d)
{
    Blk *b, *n;
    if (!d) return;
    for (b = d->blk; b; b = n) { n = b->next; free(b); }
    free(d);
}

J *JGet(const J *obj, const char *key)
{
    J *c;
    if (!obj || obj->t != J_OBJ) return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

J *JAt(const J *arr, int i)
{
    J *c;
    if (!arr || arr->t != J_ARR) return NULL;
    for (c = arr->child; c && i > 0; c = c->next) i--;
    return c;
}

J *JPath(const J *j, const char *path)
{
    char        seg[128];
    const char *p = path;
    while (j && *p) {
        size_t n = strcspn(p, ".");
        if (n >= sizeof(seg)) return NULL;
        memcpy(seg, p, n);
        seg[n] = 0;
        p += n;
        if (*p == '.') p++;
        if (j->t == J_ARR && seg[0] >= '0' && seg[0] <= '9') j = JAt(j, atoi(seg));
        else j = JGet(j, seg);
    }
    return (J *)j;
}

const char *JStr(const J *j, const char *path)
{
    J *v = JPath(j, path);
    return v && v->t == J_STR ? v->s : NULL;
}

double JNum(const J *j, const char *path, double def)
{
    J *v = JPath(j, path);
    if (!v) return def;
    if (v->t == J_NUM || v->t == J_BOOL) return v->n;
    if (v->t == J_STR && v->s[0]) return atof(v->s);
    return def;
}

J *JFind(const J *j, const char *key)
{
    J *c, *r;
    if (!j || (j->t != J_OBJ && j->t != J_ARR)) return NULL;
    for (c = j->child; c; c = c->next) {
        if (j->t == J_OBJ && c->key && !strcmp(c->key, key)) return c;
        if ((r = JFind(c, key)) != NULL) return r;
    }
    return NULL;
}

WCHAR *JText(const J *t)
{
    const char *s;
    J          *runs, *r;
    Buf         b = {0};
    WCHAR      *w;
    if (!t) return NULL;
    if ((s = JStr(t, "simpleText")) != NULL) return U2W(s);
    runs = JGet(t, "runs");
    if (!runs || runs->t != J_ARR) return NULL;
    for (r = runs->child; r; r = r->next)
        if ((s = JStr(r, "text")) != NULL) BufAddStr(&b, s);
    if (!b.p) return NULL;
    w = U2W(b.p);
    BufFree(&b);
    return w;
}
