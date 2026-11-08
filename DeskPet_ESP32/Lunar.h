#pragma once
#include "LunarData.h"
// Gregorian civil date -> days since 1970-01-01 (Howard Hinnant's civil-date formula).
inline int32_t civilDay(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = y - era * 400;
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
inline bool lunarDate(int y, int m, int d, int &month, int &day, bool &leap) {
  if (y < 2000 || y > 2099) return false;
  const int32_t target = civilDay(y, m, d);
  int lo = 0, hi = sizeof(LUNAR_MONTHS) / sizeof(LUNAR_MONTHS[0]);
  while (lo < hi) { int mid = (lo + hi) / 2; if (LUNAR_MONTHS[mid].epochDay <= target) lo = mid + 1; else hi = mid; }
  if (!lo) return false;
  const auto &r = LUNAR_MONTHS[lo - 1];
  month = r.month; day = target - r.epochDay + 1; leap = r.leap;
  return day >= 1 && day <= 30;
}
inline String lunarText(int month, int day, bool leap) {
  const char *months[] = {"正","二","三","四","五","六","七","八","九","十","冬","腊"};
  const char *digits[] = {"","一","二","三","四","五","六","七","八","九"};
  String s = leap ? "闰" : "";
  s += months[month - 1]; s += "月";
  if (day < 10) { s += "初"; s += digits[day]; }
  else if (day == 10) s += "初十";
  else if (day < 20) { s += "十"; s += digits[day - 10]; }
  else if (day == 20) s += "二十";
  else if (day < 30) { s += "廿"; s += digits[day - 20]; }
  else s += "三十";
  return s;
}
