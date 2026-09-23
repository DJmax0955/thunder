#include "texset_scan_v2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

typedef enum { EXT_NONE = -1, EXT_THB = 0, EXT_TBB = 1, EXT_TSZIP = 2 } Ext;
static const char *EXT_NAME[3] = { ".thb", ".tbb", ".tszip" };

typedef struct {
    char name[TEXSET_NAME_MAX];
    char path[3][TEXSET_PATH_MAX];   /* by Ext; "" = absent */
    int  dup[3];                     /* extra copies seen */
    char dup_path[3][TEXSET_PATH_MAX];
} Group;

static char *dup_str(const char *s) {   /* strdup isn't in strict C99 / is _strdup on MSVC */
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* stem + extension id; EXT_NONE for anything we don't handle */
static Ext split_name(const char *path, char *stem, size_t stem_sz) {
    const char *b = base_name(path);
    const char *dot = strrchr(b, '.');
    if (!dot || dot == b) return EXT_NONE;
    Ext e = EXT_NONE;
    for (int i = 0; i < 3; i++) if (ieq(dot, EXT_NAME[i])) { e = (Ext)i; break; }
    if (e == EXT_NONE) return EXT_NONE;
    size_t n = (size_t)(dot - b);
    if (n == 0 || n >= stem_sz) return EXT_NONE;
    memcpy(stem, b, n); stem[n] = 0;
    return e;
}

/* growing error text */
typedef struct { char *buf; size_t len, cap; } Msg;
static void msg_add(Msg *m, const char *fmt, ...) {
    va_list ap; char line[TEXSET_PATH_MAX + 256];
    va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    size_t n = strlen(line);
    if (m->len + n + 2 > m->cap) {
        size_t cap = (m->cap ? m->cap * 2 : 512);
        while (cap < m->len + n + 2) cap *= 2;
        char *nb = (char *)realloc(m->buf, cap);
        if (!nb) return;
        m->buf = nb; m->cap = cap;
    }
    memcpy(m->buf + m->len, line, n); m->len += n;
    m->buf[m->len++] = '\n'; m->buf[m->len] = 0;
}

int texset_scan(const char *const *paths, int path_count, int require_tszip,
                TexSet **out_sets, char **out_error) {
    *out_sets = NULL; *out_error = NULL;
    if (path_count < 0) path_count = 0;

    Group *g = (Group *)calloc((size_t)path_count + 1, sizeof(Group));
    if (!g) { *out_error = dup_str("out of memory"); return -1; }
    int gc = 0, ignored = 0;

    for (int i = 0; i < path_count; i++) {
        char stem[TEXSET_NAME_MAX];
        Ext e = split_name(paths[i], stem, sizeof(stem));
        if (e == EXT_NONE) { ignored++; continue; }
        int k = -1;
        for (int j = 0; j < gc; j++) if (ieq(g[j].name, stem)) { k = j; break; }
        if (k < 0) { k = gc++; snprintf(g[k].name, sizeof(g[k].name), "%s", stem); }
        if (g[k].path[e][0]) {
            g[k].dup[e]++;
            snprintf(g[k].dup_path[e], TEXSET_PATH_MAX, "%s", paths[i]);
        } else {
            snprintf(g[k].path[e], TEXSET_PATH_MAX, "%s", paths[i]);
        }
    }

    Msg m = { NULL, 0, 0 };
    int complete = 0;
    for (int j = 0; j < gc; j++) {
        Group *q = &g[j];
        int need[3] = { 1, 1, require_tszip ? 1 : 0 };
        char have[128] = "", miss[128] = "";
        for (int e = 0; e < 3; e++) {
            if (q->path[e][0]) {
                if (have[0]) strncat(have, ", ", sizeof(have) - strlen(have) - 1);
                strncat(have, EXT_NAME[e], sizeof(have) - strlen(have) - 1);
            } else if (need[e]) {
                if (miss[0]) strncat(miss, ", ", sizeof(miss) - strlen(miss) - 1);
                strncat(miss, EXT_NAME[e], sizeof(miss) - strlen(miss) - 1);
            }
        }
        if (!miss[0]) complete++;
        if (miss[0])
            msg_add(&m, "%s: missing %s%s", q->name, miss,
                    have[0] ? "" : " (nothing usable dropped for this name)");
        if (miss[0] && have[0])
            msg_add(&m, "    has %s", have);
        for (int e = 0; e < 3; e++)
            if (q->dup[e])
                msg_add(&m, "%s: more than one %s dropped (%s) - add one set at a time",
                        q->name, EXT_NAME[e], q->dup_path[e]);
    }
    if (m.buf) { free(g); *out_error = m.buf; return -1; }

    TexSet *sets = (TexSet *)calloc((size_t)gc, sizeof(TexSet));
    if (!sets) { free(g); *out_error = dup_str("out of memory"); return -1; }
    for (int j = 0; j < gc; j++) {
        snprintf(sets[j].name, TEXSET_NAME_MAX, "%s", g[j].name);
        snprintf(sets[j].thb, TEXSET_PATH_MAX, "%s", g[j].path[EXT_THB]);
        snprintf(sets[j].tbb, TEXSET_PATH_MAX, "%s", g[j].path[EXT_TBB]);
        snprintf(sets[j].tszip, TEXSET_PATH_MAX, "%s", g[j].path[EXT_TSZIP]);
    }
    free(g);
    *out_sets = sets;
    return gc;
}

void texset_free(TexSet *sets) { free(sets); }
