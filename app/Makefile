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

# Daemon uses all three libraries; CGI only uses libcurl (no glib/axparameter)
DAEMON_PKGS = glib-2.0 axparameter libcurl
CGI_PKGS    = libcurl

CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

DAEMON_CFLAGS = $(shell pkg-config --cflags $(DAEMON_PKGS))
DAEMON_LDLIBS = $(shell pkg-config --libs $(DAEMON_PKGS)) -lm

CGI_CFLAGS    = $(shell pkg-config --cflags $(CGI_PKGS))
CGI_LDLIBS    = $(shell pkg-config --libs $(CGI_PKGS)) -lm

# Main daemon sources
MAIN_SRCS = weather_acap.c nws.c openmeteo.c weather_api.c \
            alerts.c overlay.c params.c cJSON.c \
            vapix.c history.c webhook.c
MAIN_OBJS = $(MAIN_SRCS:.c=.o)

# CGI sources — no params.o (no axparameter), uses file-based config instead
CGI_SRCS = config_cgi.c
CGI_SHARED_OBJS = cgi_cJSON.o cgi_vapix.o cgi_weather_api.o cgi_nws.o \
                  cgi_openmeteo.o cgi_overlay.o cgi_alerts.o cgi_webhook.o

.PHONY: all clean

all: $(MAIN_PROG) $(CGI_PROG)

# ── Daemon objects (compiled with glib/axparameter flags) ────────────────
$(MAIN_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(DAEMON_CFLAGS) -c -o $@ $<

$(MAIN_PROG): $(MAIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(DAEMON_LDLIBS)

# ── CGI objects (compiled with libcurl flags only, no glib) ──────────────
cgi_config_cgi.o: config_cgi.c
	$(CC) $(CFLAGS) $(CGI_CFLAGS) -c -o $@ $<

cgi_%.o: %.c
	$(CC) $(CFLAGS) $(CGI_CFLAGS) -c -o $@ $<

$(CGI_PROG): cgi_config_cgi.o $(CGI_SHARED_OBJS) | html
	$(CC) $(CFLAGS) -o $@ $^ $(CGI_LDLIBS)

html:
	mkdir -p html

clean:
	$(RM) *.o cgi_*.o $(MAIN_PROG) $(CGI_PROG)
