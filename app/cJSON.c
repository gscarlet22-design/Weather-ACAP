/*
 * cJSON - Ultralightweight JSON parser (MIT License)
 */
#include "cJSON.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *parse_string_content(parse_buf *b) {
    /* Caller has already consumed the opening '"' */
    size_t start = b->offset;
    size_t len   = 0;

    /* First pass: measure */
    while (b->offset < b->length && b->content[b->offset] != '"') {
        if (b->content[b->offset] == '\\') b->offset++; /* skip escape */
        b->offset++;
        len++;
    }
    if (b->offset >= b->length) return NULL; /* unterminated */
    b->offset++; /* consume closing '"' */

    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;

    const unsigned char *src = b->content + start;
    char *dst = out;

    while (*src && *src != '"') {
        if (*src == '\\') {
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
                    /* Minimal: emit '?' for non-ASCII */
                    src += 4; /* skip 4 hex digits */
                    *dst++ = '?';
                    break;
                }
                default:   *dst++ = *src; break;
            }
        } else {
            *dst++ = (char)*src;
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
    if (peek(b) == '}') { b->offset++; b->depth--; return cjson_new(); } /* empty */

    while (b->offset < b->length) {
        skip_ws(b);
        if (peek(b) != '"') break;
        b->offset++; /* consume '"' */
        char *key = parse_string_content(b);
        if (!key) break;

        skip_ws(b);
        if (peek(b) != ':') { free(key); break; }
        b->offset++; /* consume ':' */
        skip_ws(b);

        cJSON *val = parse_value(b);
        if (!val) { free(key); break; }
        val->string = key;

        if (!head) { head = tail = val; }
        else       { tail->next = val; val->prev = tail; tail = val; }

        skip_ws(b);
        if (peek(b) == ',') b->offset++;
        else if (peek(b) == '}') { b->offset++; break; }
        else break;
    }

    cJSON *obj = cjson_new();
    if (!obj) { /* leak on OOM; acceptable for embedded */ return NULL; }
    obj->type  = cJSON_Object;
    obj->child = head;
    b->depth--;
    return obj;
}

/* ── array ───────────────────────────────────────────────────────────────── */

static cJSON *parse_array(parse_buf *b) {
    if (b->depth > MAX_DEPTH) return NULL;
    b->depth++;

    b->offset++; /* consume '[' */
    cJSON *head = NULL, *tail = NULL;

    skip_ws(b);
    if (peek(b) == ']') { b->offset++; b->depth--; cJSON *a = cjson_new(); if(a) a->type=cJSON_Array; return a; }

    while (b->offset < b->length) {
        skip_ws(b);
        cJSON *val = parse_value(b);
        if (!val) break;

        if (!head) { head = tail = val; }
        else       { tail->next = val; val->prev = tail; tail = val; }

        skip_ws(b);
        if (peek(b) == ',') b->offset++;
        else if (peek(b) == ']') { b->offset++; break; }
        else break;
    }

    cJSON *arr = cjson_new();
    if (!arr) return NULL;
    arr->type  = cJSON_Array;
    arr->child = head;
    b->depth--;
    return arr;
}

/* ── number ──────────────────────────────────────────────────────────────── */

static cJSON *parse_number(parse_buf *b) {
    char tmp[64];
    size_t i = 0;
    while (i < sizeof(tmp) - 1 && b->offset < b->length) {
        unsigned char c = b->content[b->offset];
        if (isdigit(c) || c == '-' || c == '+' || c == 'e' || c == 'E' || c == '.') {
            tmp[i++] = (char)c;
            b->offset++;
        } else break;
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
    return parse_value(&b);
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
