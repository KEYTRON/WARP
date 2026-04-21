#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "warp.h"

/* ── minimal recursive descent JSON parser ───────────────────── */

typedef struct { const char *p; } parser_t;

static void skip_ws(parser_t *p) {
    while (*p->p && isspace((unsigned char)*p->p)) p->p++;
}

static json_t *new_node(json_type_t t) {
    json_t *n = calloc(1, sizeof(json_t));
    if (n) n->type = t;
    return n;
}

static char *parse_string_raw(parser_t *p) {
    if (*p->p != '"') return NULL;
    p->p++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (*p->p && *p->p != '"') {
        if (*p->p == '\\') {
            p->p++;
            char c;
            switch (*p->p) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                default:   c = *p->p; break;
            }
            buf[len++] = c;
        } else {
            buf[len++] = *p->p;
        }
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        p->p++;
    }
    if (*p->p == '"') p->p++;
    buf[len] = '\0';
    return buf;
}

static json_t *parse_value(parser_t *p);

static json_t *parse_array(parser_t *p) {
    p->p++; /* skip '[' */
    json_t *node = new_node(JSON_ARRAY);
    if (!node) return NULL;
    int cap = 8;
    node->v.arr.items = malloc(cap * sizeof(json_t*));
    skip_ws(p);
    if (*p->p == ']') { p->p++; return node; }
    while (*p->p) {
        json_t *item = parse_value(p);
        if (!item) break;
        if (node->v.arr.count >= cap) {
            cap *= 2;
            node->v.arr.items = realloc(node->v.arr.items, cap * sizeof(json_t*));
        }
        node->v.arr.items[node->v.arr.count++] = item;
        skip_ws(p);
        if (*p->p == ',') { p->p++; skip_ws(p); }
        else break;
    }
    if (*p->p == ']') p->p++;
    return node;
}

static json_t *parse_object(parser_t *p) {
    p->p++; /* skip '{' */
    json_t *node = new_node(JSON_OBJECT);
    if (!node) return NULL;
    int cap = 8;
    node->v.arr.items = malloc(cap * sizeof(json_t*));
    skip_ws(p);
    if (*p->p == '}') { p->p++; return node; }
    while (*p->p) {
        skip_ws(p);
        if (*p->p != '"') break;
        char *key = parse_string_raw(p);
        skip_ws(p);
        if (*p->p != ':') { free(key); break; }
        p->p++;
        skip_ws(p);
        json_t *val = parse_value(p);
        if (!val) { free(key); break; }
        val->key = key;
        if (node->v.arr.count >= cap) {
            cap *= 2;
            node->v.arr.items = realloc(node->v.arr.items, cap * sizeof(json_t*));
        }
        node->v.arr.items[node->v.arr.count++] = val;
        skip_ws(p);
        if (*p->p == ',') { p->p++; }
        else break;
    }
    skip_ws(p);
    if (*p->p == '}') p->p++;
    return node;
}

static json_t *parse_value(parser_t *p) {
    skip_ws(p);
    if (!*p->p) return NULL;

    if (*p->p == '"') {
        json_t *n = new_node(JSON_STRING);
        if (!n) return NULL;
        n->v.s = parse_string_raw(p);
        return n;
    }
    if (*p->p == '{') return parse_object(p);
    if (*p->p == '[') return parse_array(p);
    if (strncmp(p->p, "true", 4) == 0)  { p->p += 4; json_t *n = new_node(JSON_BOOL); n->v.b = 1; return n; }
    if (strncmp(p->p, "false", 5) == 0) { p->p += 5; json_t *n = new_node(JSON_BOOL); n->v.b = 0; return n; }
    if (strncmp(p->p, "null", 4) == 0)  { p->p += 4; return new_node(JSON_NULL); }
    if (*p->p == '-' || isdigit((unsigned char)*p->p)) {
        json_t *n = new_node(JSON_NUMBER);
        if (!n) return NULL;
        char *end;
        n->v.n = strtod(p->p, &end);
        p->p = end;
        return n;
    }
    return NULL;
}

json_t *json_parse(const char *src) {
    if (!src) return NULL;
    parser_t p = { .p = src };
    return parse_value(&p);
}

json_t *json_get(json_t *obj, const char *key) {
    if (!obj || (obj->type != JSON_OBJECT && obj->type != JSON_ARRAY)) return NULL;
    for (int i = 0; i < obj->v.arr.count; i++) {
        json_t *item = obj->v.arr.items[i];
        if (item && item->key && strcmp(item->key, key) == 0) return item;
    }
    return NULL;
}

const char *json_str(json_t *node, const char *key, const char *def) {
    if (!node) return def;
    json_t *v = key ? json_get(node, key) : node;
    if (!v || v->type != JSON_STRING) return def;
    return v->v.s;
}

double json_num(json_t *node, const char *key, double def) {
    if (!node) return def;
    json_t *v = key ? json_get(node, key) : node;
    if (!v || v->type != JSON_NUMBER) return def;
    return v->v.n;
}

void json_free(json_t *node) {
    if (!node) return;
    free(node->key);
    switch (node->type) {
        case JSON_STRING: free(node->v.s); break;
        case JSON_ARRAY:
        case JSON_OBJECT:
            for (int i = 0; i < node->v.arr.count; i++)
                json_free(node->v.arr.items[i]);
            free(node->v.arr.items);
            break;
        default: break;
    }
    free(node);
}
