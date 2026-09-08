/*
 * cJSON - Ultralightweight JSON parser (MIT License)
 *
 * Trimmed, parse-only implementation.  Differences from upstream cJSON:
 *   - no Create*/Print*/Add* API (see cJSON.h for what exists)
 *   - malformed input returns NULL from cJSON_Parse rather than a partial
 *     tree — a truncated NWS body must not look like "fewer alerts"
 *   - \uXXXX escapes (incl. surrogate pairs) are decoded to UTF-8
 */
#include "cJSON.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── allocator ───────────────────────────────────────────────────────────── */

static cJSON *cjson_new(void) {
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    return item;
}

/* ── parse buffer ────────────────────────────────────────────────────────── */

typedef struct {
    const unsigned char *content;
    size_t length;
    size_t offset;
    int    depth;
} parse_buf;

#define MAX_DEPTH 512

static unsigned char peek(parse_buf *b) {
    if (b->offset >= b->length) return 0;
    return b->content[b->offset];
}

static void skip_ws(parse_buf *b) {
    while (b->offset < b->length && isspace((unsigned char)b->content[b->offset]))
        b->offset++;
}

/* ── string unescaping ───────────────────────────────────────────────────── */

static int hex4(const unsigned char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *utf8_put(char *dst, unsigned cp) {
    if (cp < 0x80) {
        *dst++ = (char)cp;
    } else if (cp < 0x800) {
        *dst++ = (char)(0xC0 | (cp >> 6));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *dst++ = (char)(0xE0 | (cp >> 12));
        *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *dst++ = (char)(0xF0 | (cp >> 18));
        *dst++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    }
    return dst;
}

/* Caller has consumed the opening '"'.  Returns a heap string, or NULL on
 * an unterminated string or malformed escape.  The decoded form is never
 * longer than the raw span, so one allocation of the raw length suffices. */
static char *parse_string_content(parse_buf *b) {
    size_t start = b->offset;

    /* First pass: find the closing quote and validate escapes. */
    while (b->offset < b->length && b->content[b->offset] != '"') {
        if (b->content[b->offset] == '\\') {
            b->offset++;
            if (b->offset >= b->length) return NULL;
            if (b->content[b->offset] == 'u') {
                unsigned tmp;
                if (b->offset + 4 >= b->length) return NULL;
                if (!hex4(b->content + b->offset + 1, &tmp)) return NULL;
                b->offset += 4;
            }
        }
        b->offset++;
    }
    if (b->offset >= b->length) return NULL;   /* unterminated */
    size_t end = b->offset;
    b->offset++;                               /* consume closing '"' */

    char *out = (char *)malloc(end - start + 1);
    if (!out) return NULL;

    const unsigned char *src = b->content + start;
    const unsigned char *lim = b->content + end;
    char *dst = out;

    while (src < lim) {
        if (*src != '\\') { *dst++ = (char)*src++; continue; }
        src++;
        switch (*src) {
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case '/':  *dst++ = '/';  break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'n':  *dst++ = '\n'; break;
            case 'r':  *dst++ = '\r'; break;
            case 't':  *dst++ = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                hex4(src + 1, &cp);          /* validated in pass 1 */
                src += 4;
                /* Surrogate pair → single code point */
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    src + 6 < lim && src[1] == '\\' && src[2] == 'u') {
                    unsigned lo = 0;
                    if (hex4(src + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        src += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
                dst = utf8_put(dst, cp);
                break;
            }
            default:   *dst++ = (char)*src; break;
        }
        src++;
    }
    *dst = '\0';
    return out;
}

/* ── forward declaration ─────────────────────────────────────────────────── */
static cJSON *parse_value(parse_buf *b);

/* ── object ──────────────────────────────────────────────────────────────── */

static cJSON *parse_object(parse_buf *b) {
    if (b->depth > MAX_DEPTH) return NULL;
    b->depth++;

    b->offset++; /* consume '{' */
    cJSON *head = NULL, *tail = NULL;

    skip_ws(b);
    if (peek(b) == '}') {
        b->offset++;
        b->depth--;
        cJSON *o = cjson_new();
        if (o) o->type = cJSON_Object;
        return o;
    }

    for (;;) {
        skip_ws(b);
        if (peek(b) != '"') goto fail;
        b->offset++; /* consume '"' */
        char *key = parse_string_content(b);
        if (!key) goto fail;

        skip_ws(b);
        if (peek(b) != ':') { free(key); goto fail; }
        b->offset++; /* consume ':' */

        cJSON *val = parse_value(b);
        if (!val) { free(key); goto fail; }
        val->string = key;

        if (!head) { head = tail = val; }
        else       { tail->next = val; val->prev = tail; tail = val; }

        skip_ws(b);
        if (peek(b) == ',') { b->offset++; continue; }
        if (peek(b) == '}') { b->offset++; break; }
        goto fail;
    }

    cJSON *obj = cjson_new();
    if (!obj) goto fail;
    obj->type  = cJSON_Object;
    obj->child = head;
    b->depth--;
    return obj;

fail:
    cJSON_Delete(head);
    b->depth--;
    return NULL;
}

/* ── array ───────────────────────────────────────────────────────────────── */

static cJSON *parse_array(parse_buf *b) {
    if (b->depth > MAX_DEPTH) return NULL;
    b->depth++;

    b->offset++; /* consume '[' */
    cJSON *head = NULL, *tail = NULL;

    skip_ws(b);
    if (peek(b) == ']') {
        b->offset++;
        b->depth--;
        cJSON *a = cjson_new();
        if (a) a->type = cJSON_Array;
        return a;
    }

    for (;;) {
        skip_ws(b);
        cJSON *val = parse_value(b);
        if (!val) goto fail;

        if (!head) { head = tail = val; }
        else       { tail->next = val; val->prev = tail; tail = val; }

        skip_ws(b);
        if (peek(b) == ',') { b->offset++; continue; }
        if (peek(b) == ']') { b->offset++; break; }
        goto fail;
    }

    cJSON *arr = cjson_new();
    if (!arr) goto fail;
    arr->type  = cJSON_Array;
    arr->child = head;
    b->depth--;
    return arr;

fail:
    cJSON_Delete(head);
    b->depth--;
    return NULL;
}

/* ── number ──────────────────────────────────────────────────────────────── */

static int is_num_char(unsigned char c) {
    return isdigit(c) || c == '-' || c == '+' || c == 'e' || c == 'E' || c == '.';
}

static cJSON *parse_number(parse_buf *b) {
    char tmp[64];
    size_t i = 0;
    while (b->offset < b->length && is_num_char(b->content[b->offset])) {
        if (i < sizeof(tmp) - 1) tmp[i++] = (char)b->content[b->offset];
        b->offset++;      /* always consume — never leave digits behind */
    }
    tmp[i] = '\0';
    cJSON *item = cjson_new();
    if (!item) return NULL;
    item->type        = cJSON_Number;
    item->valuedouble = atof(tmp);
    return item;
}

/* ── top-level value dispatcher ──────────────────────────────────────────── */

static cJSON *parse_value(parse_buf *b) {
    skip_ws(b);
    if (b->offset >= b->length) return NULL;

    unsigned char c = peek(b);

    if (c == '{') return parse_object(b);
    if (c == '[') return parse_array(b);

    if (c == '"') {
        b->offset++; /* consume '"' */
        char *s = parse_string_content(b);
        if (!s) return NULL;
        cJSON *item = cjson_new();
        if (!item) { free(s); return NULL; }
        item->type        = cJSON_String;
        item->valuestring = s;
        return item;
    }

    if (c == '-' || isdigit(c)) return parse_number(b);

    if (b->offset + 4 <= b->length && memcmp(b->content + b->offset, "true", 4) == 0) {
        b->offset += 4;
        cJSON *item = cjson_new(); if(item) item->type = cJSON_True; return item;
    }
    if (b->offset + 5 <= b->length && memcmp(b->content + b->offset, "false", 5) == 0) {
        b->offset += 5;
        cJSON *item = cjson_new(); if(item) item->type = cJSON_False; return item;
    }
    if (b->offset + 4 <= b->length && memcmp(b->content + b->offset, "null", 4) == 0) {
        b->offset += 4;
        cJSON *item = cjson_new(); if(item) item->type = cJSON_NULL; return item;
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────────── */

cJSON *cJSON_Parse(const char *value) {
    if (!value) return NULL;
    parse_buf b;
    b.content = (const unsigned char *)value;
    b.length  = strlen(value);
    b.offset  = 0;
    b.depth   = 0;
    return parse_value(&b);   /* trailing bytes after the root are ignored */
}

void cJSON_Delete(cJSON *item) {
    while (item) {
        cJSON *next = item->next;
        if (item->child)       cJSON_Delete(item->child);
        if (item->valuestring) free(item->valuestring);
        if (item->string)      free(item->string);
        free(item);
        item = next;
    }
}

cJSON *cJSON_GetObjectItem(const cJSON *obj, const char *key) {
    if (!obj || !key || obj->type != cJSON_Object) return NULL;
    cJSON *c = obj->child;
    while (c) {
        if (c->string && strcasecmp(c->string, key) == 0) return c;
        c = c->next;
    }
    return NULL;
}

int cJSON_GetArraySize(const cJSON *array) {
    if (!array || array->type != cJSON_Array) return 0;
    int n = 0;
    cJSON *c = array->child;
    while (c) { n++; c = c->next; }
    return n;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    if (!array || array->type != cJSON_Array || index < 0) return NULL;
    cJSON *c = array->child;
    while (c && index-- > 0) c = c->next;
    return c;
}
