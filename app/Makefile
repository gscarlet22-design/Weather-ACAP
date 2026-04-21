# ─────────────────────────────────────────────────────────────────────────────
# Weather ACAP — ACAP Native SDK Makefile
#
# The SDK environment-setup-* script sets CC, CFLAGS, LDFLAGS, PKG_CONFIG_PATH.
# acap-build calls 'make', then packages:
#   - the binary named <appName> (weather_acap)         → package root
#   - the binary declared by manifest httpConfig.name   → package root
#   - the html/ directory (static UI assets only)       → html/
# ─────────────────────────────────────────────────────────────────────────────

MAIN_PROG = weather_acap
# CGI is a FastCGI daemon (manifest httpConfig type="fastCgi").
# It MUST live at the package root — the ACAP runtime spawns it from there
# and sets FCGI_SOCKET_NAME to the Unix-domain socket Apache forwards to.
# Previous transferCgi path is deprecated in AXIS OS 12+ and silently 500s.
CGI_PROG  = weather_acap.cgi

# Daemon uses all three libraries.
# CGI has NO libcurl dependency — CGI_NO_CURL guards all curl code at compile
# time so the binary has no dynamic dependency on libcurl.  This keeps the CGI
# binary minimal under Apache's per-ACAP suexec context.
DAEMON_PKGS = glib-2.0 axparameter libcurl

CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

DAEMON_CFLAGS = $(shell pkg-config --cflags $(DAEMON_PKGS))
DAEMON_LDLIBS = $(shell pkg-config --libs $(DAEMON_PKGS)) -lm

# CGI links libfcgi for the FastCGI protocol, and libm for overlay math.
# It does NOT link libcurl — all curl code is guarded by -DCGI_NO_CURL so
# the binary stays minimal in Apache's per-ACAP sandbox context.
CGI_PKGS      = fcgi
CGI_CFLAGS    = -DCGI_NO_CURL $(shell pkg-config --cflags $(CGI_PKGS))
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

# ── CGI objects (compiled with -DCGI_NO_CURL, linked against libfcgi) ───
cgi_config_cgi.o: config_cgi.c
	$(CC) $(CFLAGS) $(CGI_CFLAGS) -c -o $@ $<

cgi_%.o: %.c
	$(CC) $(CFLAGS) $(CGI_CFLAGS) -c -o $@ $<

$(CGI_PROG): cgi_config_cgi.o $(CGI_SHARED_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(CGI_LDLIBS)

clean:
	$(RM) *.o cgi_*.o $(MAIN_PROG) $(CGI_PROG)
