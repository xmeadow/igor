#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Tiny growable byte buffer used during parsing/string building. */
typedef struct {
    char *d;
    size_t len, cap;
} sbuf_t;

static int sb_init(sbuf_t *b) {
    b->cap = 64;
    b->len = 0;
    b->d = (char *)malloc(b->cap);
    return b->d != NULL;
}

static void sb_free(sbuf_t *b) {
    free(b->d);
    b->d = NULL;
    b->len = b->cap = 0;
}

static int sb_reserve(sbuf_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t ncap = b->cap * 2;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char *nd = (char *)realloc(b->d, ncap);
    if (!nd) return 0;
    b->d = nd;
    b->cap = ncap;
    return 1;
}

static int sb_putc(sbuf_t *b, char c) {
    if (!sb_reserve(b, 1)) return 0;
    b->d[b->len++] = c;
    return 1;
}

static int sb_putz(sbuf_t *b, const char *s) {
    size_t n = strlen(s);
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->d + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_pututf8(sbuf_t *b, unsigned cp) {
    if (cp < 0x80) return sb_putc(b, (char)cp);
    if (cp < 0x800) {
        return sb_putc(b, (char)(0xC0 | (cp >> 6))) &&
               sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp < 0x10000) {
        return sb_putc(b, (char)(0xE0 | (cp >> 12))) &&
               sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
    return sb_putc(b, (char)(0xF0 | (cp >> 18))) &&
           sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           sb_putc(b, (char)(0x80 | (cp & 0x3F)));
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static json_value_t *parse_value(const char **p);

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static json_value_t *new_value(json_type_t t) {
    json_value_t *v = (json_value_t *)calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static json_value_t *parse_string(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    sbuf_t b;
    if (!sb_init(&b)) return NULL;

    while (**p && **p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c == '\\') {
            (*p)++;
            char e = *(*p)++;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(*p, &cp)) { sb_free(&b); return NULL; }
                    *p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && (*p)[0] == '\\' && (*p)[1] == 'u') {
                        unsigned lo;
                        if (!hex4(*p + 2, &lo)) { sb_free(&b); return NULL; }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            *p += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    if (!sb_pututf8(&b, cp)) { sb_free(&b); return NULL; }
                    continue;
                }
                default:
                    sb_free(&b);
                    return NULL;
            }
        } else {
            (*p)++;
        }
        if (!sb_putc(&b, (char)c)) { sb_free(&b); return NULL; }
    }

    if (**p != '"') { sb_free(&b); return NULL; }
    (*p)++;
    if (!sb_putc(&b, 0)) { sb_free(&b); return NULL; }

    json_value_t *v = new_value(JSON_STRING);
    if (!v) { sb_free(&b); return NULL; }
    v->str = b.d; /* transfer ownership */
    return v;
}

static json_value_t *parse_number(const char **p) {
    const char *start = *p;
    if (**p == '-') (*p)++;
    while (**p >= '0' && **p <= '9') (*p)++;
    if (**p == '.') {
        (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }
    if (**p == 'e' || **p == 'E') {
        (*p)++;
        if (**p == '+' || **p == '-') (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }
    size_t n = (size_t)(*p - start);
    json_value_t *v = new_value(JSON_NUMBER);
    if (!v) return NULL;
    v->str = (char *)malloc(n + 1);
    if (!v->str) { free(v); return NULL; }
    memcpy(v->str, start, n);
    v->str[n] = 0;
    v->num = strtod(v->str, NULL);
    return v;
}

static json_value_t *parse_literal(const char **p) {
    json_value_t *v;
    if (strncmp(*p, "true", 4) == 0) {
        v = new_value(JSON_BOOL);
        if (!v) return NULL;
        v->boolean = 1;
        *p += 4;
    } else if (strncmp(*p, "false", 5) == 0) {
        v = new_value(JSON_BOOL);
        if (!v) return NULL;
        v->boolean = 0;
        *p += 5;
    } else if (strncmp(*p, "null", 4) == 0) {
        v = new_value(JSON_NULL);
        if (!v) return NULL;
        *p += 4;
    } else {
        return NULL;
    }
    return v;
}

static json_value_t *parse_array(const char **p) {
    (*p)++; /* '[' */
    json_value_t *v = new_value(JSON_ARRAY);
    if (!v) return NULL;
    skip_ws(p);
    if (**p == ']') { (*p)++; return v; }
    for (;;) {
        json_value_t *item = parse_value(p);
        if (!item) { json_free(v); return NULL; }
        json_value_t **ni = (json_value_t **)realloc(v->items, (size_t)(v->count + 1) * sizeof(*ni));
        if (!ni) { json_free(item); json_free(v); return NULL; }
        v->items = ni;
        v->items[v->count++] = item;
        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; return v; }
        json_free(v);
        return NULL;
    }
}

static json_value_t *parse_object(const char **p) {
    (*p)++; /* '{' */
    json_value_t *v = new_value(JSON_OBJECT);
    if (!v) return NULL;
    skip_ws(p);
    if (**p == '}') { (*p)++; return v; }
    for (;;) {
        skip_ws(p);
        if (**p != '"') { json_free(v); return NULL; }
        json_value_t *k = parse_string(p);
        if (!k) { json_free(v); return NULL; }
        skip_ws(p);
        if (**p != ':') { json_free(k); json_free(v); return NULL; }
        (*p)++;
        json_value_t *val = parse_value(p);
        if (!val) { json_free(k); json_free(v); return NULL; }

        char **nk = (char **)realloc(v->keys, (size_t)(v->count + 1) * sizeof(*nk));
        json_value_t **ni = (json_value_t **)realloc(v->items, (size_t)(v->count + 1) * sizeof(*ni));
        if (!nk || !ni) {
            free(nk);
            free(ni);
            json_free(k);
            json_free(val);
            json_free(v);
            return NULL;
        }
        v->keys = nk;
        v->items = ni;
        v->keys[v->count] = k->str; /* transfer the string */
        k->str = NULL;
        json_free(k);
        v->items[v->count] = val;
        v->count++;

        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; return v; }
        json_free(v);
        return NULL;
    }
}

static json_value_t *parse_value(const char **p) {
    skip_ws(p);
    switch (**p) {
        case '{': return parse_object(p);
        case '[': return parse_array(p);
        case '"': return parse_string(p);
        case 't':
        case 'f':
        case 'n': return parse_literal(p);
        default:
            if (**p == '-' || (**p >= '0' && **p <= '9')) return parse_number(p);
            return NULL;
    }
}

json_value_t *json_parse(const char *text) {
    const char *p = text;
    json_value_t *v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (*p != 0) { json_free(v); return NULL; }
    return v;
}

void json_free(json_value_t *v) {
    if (!v) return;
    for (int i = 0; i < v->count; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

json_value_t *json_get(const json_value_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

json_value_t *json_at(const json_value_t *arr, int idx) {
    if (!arr || arr->type != JSON_ARRAY || idx < 0 || idx >= arr->count) return NULL;
    return arr->items[idx];
}

const char *json_str(const json_value_t *v) {
    if (!v || v->type != JSON_STRING) return NULL;
    return v->str;
}

json_value_t *json_path(const json_value_t *root, const char *path) {
    const json_value_t *cur = root;
    const char *p = path;
    char buf[256];

    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '.') n++;
        if (n == 0) { p++; continue; }
        if (n >= sizeof(buf)) return NULL;
        memcpy(buf, p, n);
        buf[n] = 0;

        int is_num = 1;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] < '0' || buf[i] > '9') { is_num = 0; break; }
        }

        if (is_num) {
            cur = json_at(cur, atoi(buf));
        } else {
            cur = json_get(cur, buf);
        }
        if (!cur) return NULL;

        p += n;
        if (*p == '.') p++;
    }
    return (json_value_t *)cur;
}

char *json_quote_alloc(const char *s) {
    sbuf_t b;
    if (!sb_init(&b)) return NULL;
    if (!sb_putc(&b, '"')) { sb_free(&b); return NULL; }
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        switch (c) {
            case '"': if (!sb_putz(&b, "\\\"")) { sb_free(&b); return NULL; } break;
            case '\\': if (!sb_putz(&b, "\\\\")) { sb_free(&b); return NULL; } break;
            case '\n': if (!sb_putz(&b, "\\n")) { sb_free(&b); return NULL; } break;
            case '\r': if (!sb_putz(&b, "\\r")) { sb_free(&b); return NULL; } break;
            case '\t': if (!sb_putz(&b, "\\t")) { sb_free(&b); return NULL; } break;
            case '\b': if (!sb_putz(&b, "\\b")) { sb_free(&b); return NULL; } break;
            case '\f': if (!sb_putz(&b, "\\f")) { sb_free(&b); return NULL; } break;
            default:
                if (c < 0x20) {
                    char hb[8];
                    snprintf(hb, sizeof(hb), "\\u%04x", (unsigned)c);
                    if (!sb_putz(&b, hb)) { sb_free(&b); return NULL; }
                } else {
                    if (!sb_putc(&b, (char)c)) { sb_free(&b); return NULL; }
                }
        }
    }
    if (!sb_putc(&b, '"')) { sb_free(&b); return NULL; }
    if (!sb_putc(&b, 0)) { sb_free(&b); return NULL; }
    return b.d;
}
