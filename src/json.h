#ifndef IGOR_JSON_H
#define IGOR_JSON_H

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_type_t;

typedef struct json_value {
    json_type_t type;
    char *str;      /* JSON_STRING: decoded text; JSON_NUMBER: raw text */
    double num;     /* JSON_NUMBER */
    int boolean;    /* JSON_BOOL */
    struct json_value **items; /* JSON_ARRAY / JSON_OBJECT */
    char **keys;                /* JSON_OBJECT (parallel to items) */
    int count;
} json_value_t;

/* Parse a complete JSON document. Returns NULL on malformed input. */
json_value_t *json_parse(const char *text);
void json_free(json_value_t *v);

/* Object/array accessors. Return NULL when missing or wrong type. */
json_value_t *json_get(const json_value_t *obj, const char *key);
json_value_t *json_at(const json_value_t *arr, int idx);
const char *json_str(const json_value_t *v);

/* Navigate a dotted path such as "choices.0.message.content". */
json_value_t *json_path(const json_value_t *root, const char *path);

/* Numeric value of a JSON_NUMBER, or def when missing / not a number. */
double json_num(const json_value_t *v, double def);

/* Return a malloc'd JSON string literal (quoted + escaped). Caller frees. */
char *json_quote_alloc(const char *s);

#endif
