/*
 * cJSON - Ultralightweight JSON parser (MIT License)
 * Subset sufficient for NWS / Open-Meteo / Census Geocoder responses.
 */
#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>

#define cJSON_Invalid (0)
#define cJSON_False   (1 << 0)
#define cJSON_True    (1 << 1)
#define cJSON_NULL    (1 << 2)
#define cJSON_Number  (1 << 3)
#define cJSON_String  (1 << 4)
#define cJSON_Array   (1 << 5)
#define cJSON_Object  (1 << 6)

typedef struct cJSON {
    struct cJSON *next;       /* siblings in array/object */
    struct cJSON *prev;
    struct cJSON *child;      /* first child for array/object */
    int    type;
    char  *valuestring;       /* cJSON_String */
    double valuedouble;       /* cJSON_Number */
    char  *string;            /* key name when inside an object */
} cJSON;

/* Parse a null-terminated JSON string.  Returns NULL on error.
 * Caller must cJSON_Delete() the result. */
cJSON *cJSON_Parse(const char *value);

/* Free a parsed cJSON tree. */
void cJSON_Delete(cJSON *item);

/* Object accessors — case-insensitive. */
cJSON *cJSON_GetObjectItem(const cJSON *obj, const char *key);

/* Array accessors. */
int    cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);

/* Type predicates. */
static inline int cJSON_IsNull(const cJSON *item)   { return item && (item->type == cJSON_NULL);   }
static inline int cJSON_IsTrue(const cJSON *item)   { return item && (item->type == cJSON_True);   }
static inline int cJSON_IsFalse(const cJSON *item)  { return item && (item->type == cJSON_False);  }
static inline int cJSON_IsNumber(const cJSON *item) { return item && (item->type == cJSON_Number); }
static inline int cJSON_IsString(const cJSON *item) { return item && (item->type == cJSON_String); }
static inline int cJSON_IsArray(const cJSON *item)  { return item && (item->type == cJSON_Array);  }
static inline int cJSON_IsObject(const cJSON *item) { return item && (item->type == cJSON_Object); }

/* Convenience: get a string value from an object field, or NULL. */
static inline const char *cJSON_GetStringValue(const cJSON *item) {
    return (item && item->type == cJSON_String) ? item->valuestring : NULL;
}

#endif /* CJSON_H */
