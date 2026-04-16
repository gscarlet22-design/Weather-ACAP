# ─────────────────────────────────────────────────────────────────────────────
# Weather ACAP — ACAP Native SDK Makefile
#
# The SDK environment-setup-* script sets CC, CFLAGS, LDFLAGS, PKG_CONFIG_PATH.
# acap-build calls 'make', then packages:
#   - the binary named <appName> (weather_acap)
#   - the html/ directory (static files + compiled CGI binary)
# ─────────────────────────────────────────────────────────────────────────────

MAIN_PROG = weather_acap
CGI_PROG  = html/weather_acap.cgi  # .cgi extension required by camera's lighttpd

PKGS    = glib-2.0 axparameter libcurl
CFLAGS += $(shell pkg-config --cflags $(PKGS)) \
          -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lm

# Main daemon sources
MAIN_SRCS = weather_acap.c nws.c openmeteo.c weather_api.c \
            alerts.c overlay.c params.c cJSON.c \
            vapix.c history.c webhook.c
MAIN_OBJS = $(MAIN_SRCS:.c=.o)

# CGI sources (shares params.o, cJSON.o, vapix.o, weather_api.o, overlay.o, alerts.o, webhook.o,
# nws.o, openmeteo.o — many because the CGI exposes the full engine for diagnostics.)
CGI_SRCS = config_cgi.c

.PHONY: all clean

all: $(MAIN_PROG) $(CGI_PROG)

# Main daemon — CFLAGS carries --sysroot and -L needed for cross-linking
$(MAIN_PROG): $(MAIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Config CGI — built into html/ and shares prebuilt object files from the main daemon
$(CGI_PROG): config_cgi.o params.o cJSON.o vapix.o \
             weather_api.o nws.o openmeteo.o overlay.o alerts.o webhook.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(CGI_PROG): | html

html:
	mkdir -p html

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) *.o $(MAIN_PROG) $(CGI_PROG)
