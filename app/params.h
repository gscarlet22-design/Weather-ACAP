#ifndef PARAMS_H
#define PARAMS_H

#include <glib.h>

/* Initialize axparameter handle (daemon).  Creates missing parameters. */
gboolean params_init(GError **error);

/* Initialize axparameter handle (CGI).  Read/write only — does NOT create
 * parameters.  Falls back to compiled defaults if axparameter is unavailable. */
gboolean params_init_readonly(void);

/* Release axparameter handle. */
void params_cleanup(void);

/* Get a parameter value.  Returns heap-allocated string; caller must free().
 * Falls back to the compiled-in default if the parameter is unset. */
char *params_get(const char *name);

/* Set a parameter value and persist immediately.  Returns FALSE and sets
 * *error on failure. */
gboolean params_set(const char *name, const char *value, GError **error);

/* Set a parameter value in memory only.  Call params_flush() afterwards to
 * write the whole store once — use this when applying many keys at a time
 * so the flash sees one fsync instead of one per key. */
void     params_set_deferred(const char *name, const char *value);

/* Persist the in-memory store.  Returns FALSE and sets *error on failure. */
gboolean params_flush(GError **error);

/* Convenience typed getters — caller does NOT free. */
int  params_get_int(const char *name, int default_val);

#endif /* PARAMS_H */
