#ifndef LIGHTNING_H
#define LIGHTNING_H

/*
 * lightning.h — SPC Day-1 Convective Outlook risk checker
 *
 * Polls https://www.spc.noaa.gov/products/outlook/day1otlk_cat.nolyr.geojson
 * (free, no API key) and performs a point-in-polygon test to determine whether
 * the camera's location falls within any convective risk area.
 *
 * Risk levels (matching SPC LABEL strings):
 *   1 = TSTM  — General thunderstorm area
 *   2 = MRGL  — Marginal risk
 *   3 = SLGT  — Slight risk
 *   4 = ENH   — Enhanced risk
 *   5 = MDT   — Moderate risk
 *   6 = HIGH  — High risk
 */

typedef struct {
    char label[8];    /* "TSTM", "MRGL", "SLGT", "ENH", "MDT", "HIGH" */
    int  risk_level;  /* 1–6 per above scale; 0 = no risk / no data   */
} LightningRisk;

/*
 * Fetch current SPC Day-1 outlook and test whether (lat, lon) is at risk.
 * Returns:
 *   1  — location is at risk; *out filled with highest matching risk level
 *   0  — no risk (point outside all polygons)
 *  -1  — fetch or parse error
 */
int lightning_check(double lat, double lon, LightningRisk *out);

/* Human-readable description for a risk level integer (1–6). */
const char *lightning_risk_label(int risk_level);

#endif /* LIGHTNING_H */
