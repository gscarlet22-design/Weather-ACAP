#ifndef HISTORY_H
#define HISTORY_H

/*
 * Alert history — ring buffer written to /tmp/weather_acap_history.jsonl.
 * Each line is a JSON object with {ts, event, headline, action}.
 *
 * action is one of: "activated", "cleared", "firedrill".
 * Reads are done by the CGI via plain file read.
 */

void history_append(const char *event,
                    const char *headline,
                    const char *action);

#endif /* HISTORY_H */
