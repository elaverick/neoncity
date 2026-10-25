#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Parser {
    const char *p;
    int         line;
    char       *err;
    size_t      errSize;
    int         failed;
} Parser;

static void fail(Parser *ps, const char *fmt, ...) {
    if (ps->failed) return;
    ps->failed = 1;
    int n = snprintf(ps->err, ps->errSize, "line %d: ", ps->line);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < ps->errSize) vsnprintf(ps->err + n, ps->errSize - (size_t)n, fmt, ap);
    va_end(ap);
}

/* Skips whitespace and comments. */
static void skip(Parser *ps) {
    for (;;) {
        char c = *ps->p;
        if (c == '\n') { ps->line++; ps->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') ps->p++;
        else if (c == '/' && ps->p[1] == '/') { while (*ps->p && *ps->p != '\n') ps->p++; }
        else if (c == '/' && ps->p[1] == '*') {
            ps->p += 2;
            while (*ps->p && !(ps->p[0] == '*' && ps->p[1] == '/')) { if (*ps->p == '\n') ps->line++; ps->p++; }
            if (!*ps->p) { fail(ps, "unterminated comment"); return; }
            ps->p += 2;
        } else return;
    }
}

static Json *node(Parser *ps, JsonType t) {
    Json *j = (Json *)calloc(1, sizeof(Json));
    j->type = t;
    j->line = ps->line;
    return j;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
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

/* A string literal at ps->p (its opening quote), unescaped into a new buffer. */
static char *string_lit(Parser *ps) {
    const char *s = ++ps->p;
    size_t cap = 16, n = 0;
    char *out = (char *)malloc(cap);
    for (;;) {
        char c = *s;
        if (c == '"') break;
        if (c == 0 || c == '\n') { fail(ps, "unterminated string"); free(out); return NULL; }
        char buf[4];
        int len = 1;
        if (c == '\\') {
            char e = *++s;
            switch (e) {
            case '"': buf[0] = '"'; break;
            case '\\': buf[0] = '\\'; break;
            case '/': buf[0] = '/'; break;
            case 'b': buf[0] = '\b'; break;
            case 'f': buf[0] = '\f'; break;
            case 'n': buf[0] = '\n'; break;
            case 'r': buf[0] = '\r'; break;
            case 't': buf[0] = '\t'; break;
            case 'u': {
                unsigned u;
                if (!hex4(s + 1, &u)) { fail(ps, "bad \\u escape"); free(out); return NULL; }
                s += 4;
                if (u < 0x80) buf[0] = (char)u;
                else if (u < 0x800) { buf[0] = (char)(0xC0 | (u >> 6)); buf[1] = (char)(0x80 | (u & 63)); len = 2; }
                else { buf[0] = (char)(0xE0 | (u >> 12)); buf[1] = (char)(0x80 | ((u >> 6) & 63)); buf[2] = (char)(0x80 | (u & 63)); len = 3; }
                break;
            }
            default: fail(ps, "bad escape \\%c", e); free(out); return NULL;
            }
        } else buf[0] = c;
        s++;
        if (n + (size_t)len + 1 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        memcpy(out + n, buf, (size_t)len);
        n += (size_t)len;
    }
    out[n] = 0;
    ps->p = s + 1;
    return out;
}

static Json *value(Parser *ps, int depth);

/* The elements of an array or members of an object, up to `close`. */
static Json *items(Parser *ps, Json *j, char close, int depth) {
    Json **tail = &j->child;
    ps->p++;
    skip(ps);
    if (*ps->p == close) { ps->p++; return j; }
    for (;;) {
        char *key = NULL;
        if (j->type == JSON_OBJECT) {
            skip(ps);
            if (*ps->p != '"') { fail(ps, "expected a member name"); break; }
            key = string_lit(ps);
            if (!key) break;
            skip(ps);
            if (*ps->p != ':') { fail(ps, "expected ':' after \"%s\"", key); free(key); break; }
            ps->p++;
        }
        Json *v = value(ps, depth + 1);
        if (!v) { free(key); break; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return j; }
        fail(ps, "expected ',' or '%c'", close);
        break;
    }
    json_free(j);
    return NULL;
}

static Json *value(Parser *ps, int depth) {
    skip(ps);
    if (ps->failed) return NULL;
    if (depth > 64) { fail(ps, "nested too deep"); return NULL; }
    char c = *ps->p;
    if (c == '{') return items(ps, node(ps, JSON_OBJECT), '}', depth);
    if (c == '[') return items(ps, node(ps, JSON_ARRAY), ']', depth);
    if (c == '"') {
        Json *j = node(ps, JSON_STRING);
        j->string = string_lit(ps);
        if (!j->string) { free(j); return NULL; }
        return j;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double v = strtod(ps->p, &end);
        if (end == ps->p) { fail(ps, "bad number"); return NULL; }
        Json *j = node(ps, JSON_NUMBER);
        j->number = v;
        ps->p = end;
        return j;
    }
    static const struct { const char *word; JsonType t; double v; } WORDS[] = {
        { "true", JSON_BOOL, 1.0 }, { "false", JSON_BOOL, 0.0 }, { "null", JSON_NULL, 0.0 },
    };
    for (int i = 0; i < 3; ++i) {
        size_t n = strlen(WORDS[i].word);
        if (strncmp(ps->p, WORDS[i].word, n) == 0) {
            Json *j = node(ps, WORDS[i].t);
            j->number = WORDS[i].v;
            ps->p += n;
            return j;
        }
    }
    fail(ps, c ? "unexpected '%c'" : "unexpected end of text", c);
    return NULL;
}

Json *json_parse(const char *text, char *err, size_t errSize) {
    Parser ps = { text, 1, err, errSize, 0 };
    if (errSize) err[0] = 0;
    Json *j = value(&ps, 0);
    if (!j) return NULL;
    skip(&ps);
    if (*ps.p) { fail(&ps, "text after the value"); json_free(j); return NULL; }
    return j;
}

Json *json_load(const char *path, char *err, size_t errSize) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errSize, "can't open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)(size > 0 ? size : 0) + 1);
    size_t got = size > 0 ? fread(text, 1, (size_t)size, f) : 0;
    fclose(f);
    text[got] = 0;
    Json *j = json_parse(text, err, errSize);
    free(text);
    return j;
}

void json_free(Json *j) {
    while (j) {
        Json *next = j->next;
        json_free(j->child);
        free(j->string);
        free(j->key);
        free(j);
        j = next;
    }
}

const Json *json_get(const Json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (const Json *m = obj->child; m; m = m->next)
        if (strcmp(m->key, key) == 0) return m;
    return NULL;
}

int json_count(const Json *j) {
    int n = 0;
    if (j && (j->type == JSON_ARRAY || j->type == JSON_OBJECT))
        for (const Json *e = j->child; e; e = e->next) n++;
    return n;
}

const Json *json_at(const Json *arr, int i) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    const Json *e = arr->child;
    while (e && i-- > 0) e = e->next;
    return e;
}

const char *json_type_name(JsonType t) {
    static const char *const NAMES[] = { "null", "a bool", "a number", "a string", "an array", "an object" };
    return NAMES[t];
}
