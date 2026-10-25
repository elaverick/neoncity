#pragma once
#include <stddef.h>

/* A small JSON reader for the engine's data files (assets/materials/). It
 * reads standard JSON plus comments (// to the end of the line, and block
 * comments), so a file can say where its numbers come from. The document is
 * a tree of values; an object's members keep their order. */
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct Json Json;
struct Json {
    JsonType type;
    double   number;   /* a number's value; a bool's 0 or 1 */
    char    *string;   /* a string's value (UTF-8) */
    char    *key;      /* a member's name, in an object */
    int      line;     /* where the value starts in the text */
    Json    *child;    /* an array's first element, an object's first member */
    Json    *next;     /* the next element or member */
};

/* Parses text. On error returns NULL and writes "line N: what" to err. */
Json       *json_parse(const char *text, char *err, size_t errSize);
/* Reads and parses a file; err as json_parse, or why the file couldn't be read. */
Json       *json_load(const char *path, char *err, size_t errSize);
void        json_free(Json *j);
/* An object's member called key, or NULL (and NULL if obj isn't an object). */
const Json *json_get(const Json *obj, const char *key);
/* How many elements an array (or members an object) has. */
int         json_count(const Json *j);
const Json *json_at(const Json *arr, int i);
const char *json_type_name(JsonType t);
