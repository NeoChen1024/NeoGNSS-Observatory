/* SPDX-License-Identifier: GPL-3.0-only */
/* Internal worker. The public Click CLI owns options and provenance. */
#include "rtklib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int showmsg(const char *format, ...) {
  (void)format;
  return 0;
}
void settspan(gtime_t start, gtime_t end) {
  (void)start;
  (void)end;
}
void settime(gtime_t time) { (void)time; }

static int slot(const obsd_t *obs, const char *code) {
  for (int i = 0; i < NFREQ + NEXOBS; i++) {
    if (obs->code[i] && !strcmp(code2obs(obs->code[i]), code))
      return i;
  }
  return -1;
}

int main(int argc, char **argv) {
  if (argc != 8) {
    fprintf(
        stderr,
        "Usage: neognss_rinex_geometry OBS NAV X Y Z SHELL_KM OUTPUT.csv\n");
    return 2;
  }
  obs_t obs = {0};
  nav_t nav = {0};
  sta_t station = {0};
  double rr[3], pos[3], height = atof(argv[6]);
  for (int i = 0; i < 3; i++)
    rr[i] = atof(argv[3 + i]);
  if (!isfinite(height) || height <= 0 || norm(rr, 3) < 6E6 ||
      norm(rr, 3) > 7E6)
    return 2;
  ecef2pos(rr, pos);
  if (readrnx(argv[2], 1, "", &obs, &nav, &station) <= 0 ||
      readrnx(argv[1], 1, "", &obs, &nav, &station) <= 0 || obs.n == 0) {
    fprintf(stderr, "Cannot read nonempty RINEX OBS/NAV\n");
    return 1;
  }
  sortobs(&obs);
  uniqnav(&nav);
  FILE *out = fopen(argv[7], "wx");
  if (!out) {
    perror("open output");
    return 1;
  }
  fprintf(out,
          "gpst,gps_week,gps_tow,satellite,code1,code2,frequency1,frequency2,"
          "phase1,phase2,lli1,lli2,code_range1,code_range2,cn01,cn02,geometry_"
          "ok,health,azimuth,elevation,ipp_latitude,ipp_longitude,mapping\n");
  for (int k = 0; k < obs.n; k++) {
    const obsd_t *o = obs.data + k;
    int sys = satsys(o->sat, NULL), a = -1, b = -1;
    const char *c1 = NULL, *c2 = NULL;
    switch (sys) {
    case SYS_GPS:
    case SYS_QZS:
      c1 = "1C";
      c2 = "2X";
      break;
    case SYS_GAL:
      c1 = "1X";
      c2 = "7X";
      break;
    case SYS_CMP:
      c1 = "2I";
      c2 = "7I";
      break;
    case SYS_GLO:
      c1 = "1C";
      c2 = "2C";
      break;
    default:
      continue;
    }
    a = slot(o, c1);
    b = slot(o, c2);
    /* Missing signals still emit a row, so the arc tracker sees outages. */
    double rs[6], dts[2], var, e[3], azel[2] = {0}, ipp[3] = {0};
    int health = 0;
    satposs(o->time, o, 1, &nav, EPHOPT_BRDC, rs, dts, &var, &health);
    int ok = norm(rs, 3) > 1E6 && health == 0;
    double mapping = 0;
    if (ok) {
      /* Rotate transmit-time ECEF into the reception-time ECEF frame. */
      double delta[3];
      for (int i = 0; i < 3; i++)
        delta[i] = rs[i] - rr[i];
      double angle = OMGE * norm(delta, 3) / CLIGHT;
      double x = rs[0], y = rs[1];
      rs[0] = cos(angle) * x + sin(angle) * y;
      rs[1] = -sin(angle) * x + cos(angle) * y;
      for (int i = 0; i < 3; i++)
        e[i] = rs[i] - rr[i];
      double distance = norm(e, 3);
      for (int i = 0; i < 3; i++)
        e[i] /= distance;
      satazel(pos, e, azel);
      if (azel[1] > 0)
        mapping = ionppp(pos, azel, 6371.0, height, ipp);
      else
        ok = 0;
    }
    int week;
    double tow = time2gpst(o->time, &week);
    char satellite[16];
    satno2id(o->sat, satellite);
    fprintf(out,
            "%.7f,%d,%.7f,%s,%s,%s,%.3f,%.3f,%.6f,%.6f,%u,%u,%.4f,%.4f,%.3f,%."
            "3f,%d,%d,%.7f,%.7f,%.7f,%.7f,%.9f\n",
            week * 604800.0 + tow, week, tow, satellite, c1, c2,
            a < 0 ? 0 : sat2freq(o->sat, o->code[a], &nav),
            b < 0 ? 0 : sat2freq(o->sat, o->code[b], &nav), a < 0 ? 0 : o->L[a],
            b < 0 ? 0 : o->L[b], a < 0 ? 0 : o->LLI[a], b < 0 ? 0 : o->LLI[b],
            a < 0 ? 0 : o->P[a], b < 0 ? 0 : o->P[b], a < 0 ? 0 : o->SNR[a],
            b < 0 ? 0 : o->SNR[b], ok, health, azel[0] * R2D, azel[1] * R2D,
            ipp[0] * R2D, ipp[1] * R2D, mapping);
  }
  int failed = ferror(out);
  if (fclose(out))
    failed = 1;
  freeobs(&obs);
  freenav(&nav, 0x7F);
  return failed ? 1 : 0;
}
