#include "moon.h"
#include <math.h>

// Elongation of the moon from the sun from the low-precision series in Meeus, "Astronomical Algorithms" (mean
// elongation plus the dominant periodic terms of the lunar and solar longitudes). Checked against the almanac for
// 2026: new, full and quarter moons land within 20 minutes.
namespace moon {
  namespace {
    constexpr double SYNODIC = 29.530588853;
    constexpr double J2000 = 946728000.0;           // 2000-01-01 12:00 UTC
    double rad(double deg) { return fmod(deg, 360.0) * M_PI / 180.0; }
  }

  Info at(time_t utc) {
    Info m;
    const double T = ((double)utc - J2000) / 86400.0 / 36525.0;
    const double Dm = 297.8501921 + 445267.1114034 * T;     // mean elongation
    const double D = rad(Dm);
    const double Mp = rad(134.9633964 + 477198.8675055 * T);   // moon's mean anomaly
    const double M = rad(357.5291092 + 35999.0502909 * T);     // sun's mean anomaly
    const double F = rad(93.2720950 + 483202.0175233 * T);     // argument of latitude
    const double moonCorr = 6.289 * sin(Mp) + 1.274 * sin(2 * D - Mp) + 0.658 * sin(2 * D) + 0.214 * sin(2 * Mp) - 0.186 * sin(M)
                          - 0.114 * sin(2 * F) - 0.059 * sin(2 * D - 2 * Mp) - 0.057 * sin(2 * D - M - Mp) + 0.053 * sin(2 * D + Mp)
                          + 0.046 * sin(2 * D - M) + 0.041 * sin(Mp - M) - 0.035 * sin(D) - 0.031 * sin(Mp + M);
    const double sunCorr = 1.915 * sin(M) + 0.020 * sin(2 * M);
    double e = fmod(Dm + moonCorr - sunCorr, 360.0);
    if (e < 0) e += 360.0;
    const double phase = e / 360.0;
    m.fraction = (float)phase;
    m.age_days = (float)(phase * SYNODIC);
    m.illumination = (float)((1.0 - cos(2.0 * M_PI * phase)) / 2.0);
    m.waxing = phase < 0.5;
    const double w = 0.75 / SYNODIC;   // new, quarters and full carry their name for about 1.5 days
    if (phase < w || phase > 1.0 - w) m.phase = Phase::New;
    else if (fabs(phase - 0.25) < w) m.phase = Phase::FirstQuarter;
    else if (fabs(phase - 0.5) < w) m.phase = Phase::Full;
    else if (fabs(phase - 0.75) < w) m.phase = Phase::LastQuarter;
    else if (phase < 0.25) m.phase = Phase::WaxingCrescent;
    else if (phase < 0.5) m.phase = Phase::WaxingGibbous;
    else if (phase < 0.75) m.phase = Phase::WaningGibbous;
    else m.phase = Phase::WaningCrescent;
    double toFull = 0.5 - phase; if (toFull < 0) toFull += 1.0;
    double toNew = 1.0 - phase; if (toNew >= 1.0) toNew -= 1.0;
    m.days_to_full = (float)(toFull * SYNODIC);
    m.days_to_new = (float)(toNew * SYNODIC);
    return m;
  }

  const char* name(Phase p) {
    switch (p) {
      case Phase::New: return "New moon";
      case Phase::WaxingCrescent: return "Waxing crescent";
      case Phase::FirstQuarter: return "First quarter";
      case Phase::WaxingGibbous: return "Waxing gibbous";
      case Phase::Full: return "Full moon";
      case Phase::WaningGibbous: return "Waning gibbous";
      case Phase::LastQuarter: return "Last quarter";
      default: return "Waning crescent";
    }
  }
  const char* shortName(Phase p) {
    switch (p) {
      case Phase::New: return "NEW MOON";
      case Phase::WaxingCrescent: return "WAX CRESCENT";
      case Phase::FirstQuarter: return "FIRST QTR";
      case Phase::WaxingGibbous: return "WAX GIBBOUS";
      case Phase::Full: return "FULL MOON";
      case Phase::WaningGibbous: return "WAN GIBBOUS";
      case Phase::LastQuarter: return "LAST QTR";
      default: return "WAN CRESCENT";
    }
  }
}
