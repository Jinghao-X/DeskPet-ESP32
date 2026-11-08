// 3D night ocean for a 128x64 one-bit OLED.
// Perspective height-field waves (front-to-back column ray march), moonlight glitter
// from the real moon phase, random big waves from four directions, and whales.
#include "Ocean.h"
#include <math.h>
#include <string.h>
#include <time.h>
#ifdef ESP32
#include <esp_random.h>
#endif

namespace {

constexpr int W = 128, H = 64;
constexpr float HZ = 24, F = 40, CAMH = 3, DT = 1.0f / 30;
constexpr int MOON_X = 100, MOON_Y = 8, MOON_R = 6;
constexpr float FAR_A = 5;                       // horizon swell height in pixels
constexpr float WHALE_MIN_S = 60, WHALE_MAX_S = 180;
constexpr float TWO_PI_F = 6.28318531f;
constexpr int MAXP = 360;

uint8_t fb[W * H];
uint8_t mask[W * H];
int16_t occl[W];
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// ---------- small math helpers ----------
float SIN_LUT[1025];
inline float fsin(float x) {
  float idx = x * (1024.0f / TWO_PI_F);
  int i = (int)idx;
  if (idx < 0 && idx != (float)i) i -= 1;
  float f = idx - (float)i;
  i &= 1023;
  return SIN_LUT[i] + (SIN_LUT[i + 1] - SIN_LUT[i]) * f;
}
inline float fcos(float x) { return fsin(x + 1.57079633f); }
inline float smooth(float a, float b, float x) {
  float u = (x - a) / (b - a);
  u = u < 0 ? 0 : (u > 1 ? 1 : u);
  return u * u * (3 - 2 * u);
}
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float hashf(uint32_t n) {
  n = (n ^ (n >> 16)) * 0x45d9f3bu;
  n = (n ^ (n >> 16)) * 0x45d9f3bu;
  n ^= n >> 16;
  return (float)n * (1.0f / 4294967296.0f);
}
uint32_t rngState = 0x1234567;
inline float frand() {
  rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
  return (float)(rngState >> 8) * (1.0f / 16777216.0f);
}
inline float wrap2pi(double v) { return (float)fmod(v, (double)TWO_PI_F); }
inline void P(float x, float y) {
  int xi = (int)lroundf(x), yi = (int)lroundf(y);
  if (xi >= 0 && xi < W && yi >= 0 && yi < H) fb[yi * W + xi] = 1;
}

// ---------- waves ----------
struct WaveDef { float A, k, dx, dz, w, p; };
const WaveDef WAVES[5] = {
  {0.32f, 0.55f, 0.15f, -0.99f, 1.25f, 0.0f},
  {0.20f, 0.80f, -0.80f, -0.60f, 1.60f, 1.7f},
  {0.14f, 1.20f, 0.90f, 0.44f, 2.00f, 4.1f},
  {0.08f, 2.00f, -0.30f, 0.95f, 2.60f, 2.2f},
  {0.05f, 3.10f, 0.60f, -0.80f, 3.30f, 5.3f},
};
const float LN = sqrtf(0.9f * 0.9f + 0.3f * 0.3f + 1.0f);
const float LX = 0.9f / LN, LY = 0.3f / LN, LZ = 1.0f / LN;

double tD = 0;
uint32_t frame = 0;
float sets = 1, waveOff[5], far1, far2, far3, bigOff, wakeOff;
float gust = 0, nextGust = 3, splash = 0, shake = 0, gustK = 0;
float moonPhase = 0.35f;

struct BigWave { bool on; uint8_t ty; float dx, dz, pos, start, end, spd, amp, breakAt, fade; bool breaking, splashed; };
BigWave bigs[2];

struct Whale {
  bool on; uint8_t kind; float Z, X, Xn, dir, age, T, yc, th, wake, ringT;
  bool fUp, fDown;
};
Whale wh;
float nextWhale = 25;
// After a big wave: one chance (not stacking) that a whale shows up within a few seconds.
constexpr float WAVE_WHALE_CHANCE = 0.45f, WAVE_BOOST_WINDOW = 10;
float waveBoost = 0;

struct Particle { float X, Y, Z, vx, vy, vz, s; uint8_t flags; };
constexpr uint8_t PF_MIST = 1, PF_BIG = 2;
Particle parts[MAXP];
int np = 0;

struct Star { uint8_t x, y; float ph, sp; bool big; };
Star stars[30];
struct Shoot { bool on; float x, y; int life; };
Shoot shoot = {false, 0, 0, 0};
float nextShoot = 4;
uint32_t frameMs = 0;

struct FOut { float h, bf, base, fc, gx, gz; };

// Distance-only terms for the fixed ray-march depths, computed once (saves divisions per sample).
struct ZPre { float z, invZ, hsc, wf, att[5]; };
constexpr int MAXZ = 96;
ZPre zpre[MAXZ];
int nz = 0;
void fillZPre(ZPre &p, float Z) {
  const float zz = Z > 0 ? Z : 0;
  p.z = Z; p.invZ = Z > 0.01f ? 1.0f / Z : 100; p.hsc = 1 + zz / 18; p.wf = smooth(6, 40, zz);
  for (int i = 0; i < 5; ++i) { const float q = WAVES[i].k * zz * 0.015f; p.att[i] = 1.0f / (1.0f + q * q); }
}

void field(float X, float Z, FOut &o, const ZPre *zp = nullptr) {
  ZPre local;
  if (!zp) { fillZPre(local, Z); zp = &local; }
  float h = 0, gx = 0, gz = 0;
  for (int i = 0; i < 5; ++i) {
    const WaveDef &wv = WAVES[i];
    const float att = zp->att[i];
    const float th = wv.k * (wv.dx * X + wv.dz * Z) - waveOff[i] + wv.p;
    const float sn = fsin(th), cs = fcos(th), g = (sn + 1) * 0.5f;
    const float A = wv.A * sets * att;
    h += A * (2 * g * g - 1);
    const float dth = 2 * A * g * cs * wv.k;
    gx += dth * wv.dx; gz += dth * wv.dz;
  }
  const float hsc = zp->hsc;
  o.base = h;
  h *= hsc; gx *= hsc; gz *= hsc;
  o.fc = 0;
  const float wf = zp->wf;
  if (wf > 0 && Z > 0.5f) {
    const float xs = X * F * zp->invZ;
    const float t1 = 0.10f * xs + 0.22f * Z - far1, t2 = -0.17f * xs + 0.31f * Z - far2 + 2.0f, t3 = 0.05f * xs - 0.13f * Z - far3 + 4.0f;
    const float g1 = (fsin(t1) + 1) * 0.5f, g2 = (fsin(t2) + 1) * 0.5f, g3 = (fsin(t3) + 1) * 0.5f;
    const float v1 = 2 * g1 * g1 - 1, v2 = 2 * g2 * g2 - 1, v3 = 2 * g3 * g3 - 1;
    const float d1 = 2 * g1 * fcos(t1), d2 = 2 * g2 * fcos(t2), d3 = 2 * g3 * fcos(t3);
    const float fv = 0.6f * v1 + 0.35f * v2 + 0.45f * v3;
    const float amp = wf * FAR_A * sets * Z * (1.0f / F);
    h += amp * fv;
    gx += wf * FAR_A * sets * (0.6f * d1 * 0.10f - 0.35f * d2 * 0.17f + 0.45f * d3 * 0.05f);
    gz += amp * (0.6f * d1 * 0.22f + 0.35f * d2 * 0.31f - 0.45f * d3 * 0.13f) + wf * FAR_A * fv * (1.0f / F);
    o.fc = wf * fmaxf(0, (fv - 0.55f) / 0.45f);
  }
  float bf = 0;
  for (const BigWave &b : bigs) {
    if (!b.on) continue;
    const float d = b.dx * X + b.dz * Z - b.pos;
    const float w = d > 0 ? (b.breaking ? 0.9f : 1.3f) : 4.0f;
    const float e2 = (d * d) / (w * w);
    if (e2 > 7) continue;
    const float e = expf(-e2);
    const float along = b.dz * X - b.dx * Z;
    const float m = b.amp * b.fade * (1 + 0.15f * fsin(0.4f * along + bigOff)) * hsc;
    h += m * e;
    const float dd = m * e * (-2 * d / (w * w));
    gx += dd * b.dx; gz += dd * b.dz;
    if (b.breaking && d > -0.8f && d < 1.6f) bf = fmaxf(bf, e);
  }
  if (wh.on) {
    const float dx = X - wh.Xn, dz = Z - wh.Z, r2 = dx * dx + dz * dz;
    if (wh.wake > 0 && r2 < 81) {
      const float r = sqrtf(r2);
      h += wh.wake * 0.35f * expf(-r2 / 10) * fcos(r * 2.2f - wakeOff);
    }
    if (wh.ringT >= 0) {
      const float r = sqrtf(r2);
      const float c = 1.2f + wh.ringT * 5, dec = expf(-wh.ringT * 0.6f), dr = r - c;
      const float e = expf(-dr * dr / 2);
      h += 1.5f * dec * e - 0.45f * dec * expf(-(dr + 1.8f) * (dr + 1.8f) / 1.4f);
      if (r > 0.1f) { const float k = 1.5f * dec * e * (-dr) / r; gx += k * dx; gz += k * dz; }
      bf = fmaxf(bf, e * dec * 1.2f);
      if (r < c - 0.5f) bf = fmaxf(bf, dec * 0.85f * expf(-r2 / (c * c)));
    }
  }
  o.h = h; o.bf = bf; o.gx = gx; o.gz = gz;
}

void spawn(float X, float Y, float Z, float vx, float vy, float vz, float s, uint8_t flags = 0) {
  if (np >= MAXP) return;
  parts[np++] = {X, Y, Z, vx, vy, vz, s, flags};
}
void burst(float X, float Z, int n, float pw, float spread) {
  FOut o; field(X, Z, o);
  for (int k = 0; k < n; ++k) {
    const float a = frand() * TWO_PI_F, r = frand() * spread;
    const float ca = cosf(a), sa = sinf(a);
    spawn(X + ca * r * 0.6f, o.h, Z + sa * r * 0.3f, ca * (0.03f + frand() * 0.1f) * pw,
          (0.08f + frand() * 0.2f) * pw, sa * 0.04f * pw, 1, (pw >= 2 && frand() < 0.55f) ? PF_BIG : 0);
  }
}

// ---------- whale geometry ----------
struct V2 { float x, y; };
enum Kind : uint8_t { K_DARK, K_LIGHT, K_BELLY, K_SIDE, K_FLUKE };
const float BS = 1.6f, FS = 0.9f;

const V2 BODY_TOP[] = {{4.6f,0.3f},{4.35f,0.72f},{3.7f,0.98f},{2.8f,1.08f},{1.6f,1.05f},{0.3f,0.9f},{-0.9f,0.7f},{-1.35f,0.62f},{-1.85f,1.02f},{-2.05f,0.98f},{-2.05f,0.52f},{-2.8f,0.36f},{-3.5f,0.22f},{-3.85f,0.18f}};
const V2 BODY_BOT[] = {{-3.85f,-0.16f},{-3.0f,-0.3f},{-2.2f,-0.42f},{-1.2f,-0.6f},{-0.6f,-0.7f},{0,-0.8f},{1.0f,-0.95f},{2.0f,-1.05f},{3.0f,-1.0f},{3.8f,-0.75f},{4.4f,-0.05f}};
const V2 FLUKE_PTS[] = {{-3.6f,0.22f},{-4.25f,0.45f},{-4.75f,0.95f},{-5.15f,1.15f},{-5.0f,0.6f},{-4.6f,0.05f},{-5.0f,-0.55f},{-5.15f,-1.1f},{-4.75f,-0.9f},{-4.25f,-0.42f},{-3.6f,-0.2f}};
const float BXS[] = {4.4f,3.8f,3.0f,2.0f,1.0f,0,-0.6f}, BU[] = {0.06f,-0.18f,-0.38f,-0.5f,-0.55f,-0.58f,-0.62f}, BB[] = {-0.05f,-0.75f,-1.0f,-1.05f,-0.95f,-0.8f,-0.7f};
const V2 FIN[] = {{2.75f,-0.35f},{2.65f,-0.95f},{2.5f,-1.45f},{2.32f,-1.95f},{2.08f,-2.3f},{1.86f,-2.15f},{1.84f,-1.5f},{1.9f,-0.95f},{2.0f,-0.35f}};
const V2 SIDE_FIN[] = {{2.75f,-0.12f},{2.3f,-0.05f},{1.7f,-0.25f},{1.05f,-0.62f},{0.5f,-1.05f},{0.2f,-1.45f},{0.1f,-1.7f},{0.45f,-1.6f},{0.95f,-1.3f},{1.6f,-0.98f},{2.25f,-0.72f},{2.75f,-0.48f}};

struct Poly { V2 *p; int n; Kind k; };
V2 pool[900];
int poolUsed = 0;
Poly polys[6];
int npoly = 0;
V2 grooves[3][7];
bool haveGrooves = false;
V2 eyePt; bool haveEye = false;

int chaikin(const V2 *in, int n, V2 *out) {
  for (int i = 0; i < n; ++i) {
    const V2 a = in[i], b = in[(i + 1) % n];
    out[2 * i] = {0.75f * a.x + 0.25f * b.x, 0.75f * a.y + 0.25f * b.y};
    out[2 * i + 1] = {0.25f * a.x + 0.75f * b.x, 0.25f * a.y + 0.75f * b.y};
  }
  return 2 * n;
}
int bez(V2 a, V2 b, V2 c, V2 d, int n, V2 *out) {
  for (int i = 0; i <= n; ++i) {
    const float s = (float)i / n, u = 1 - s;
    out[i] = {u*u*u*a.x + 3*u*u*s*b.x + 3*u*s*s*c.x + s*s*s*d.x, u*u*u*a.y + 3*u*u*s*b.y + 3*u*s*s*c.y + s*s*s*d.y};
  }
  return n + 1;
}
inline V2 proj(float X, float Y, float Z) { return {W / 2 + X * F / Z, HZ + (CAMH - Y) * F / Z}; }
inline V2 bendPt(V2 p, float fl) {
  float y = p.y + 0.25f * (1 - (p.x / 4.6f) * (p.x / 4.6f));
  if (p.x < -1.8f) { const float u = (p.x + 1.8f) / 3.3f; y += fl * u * u; }
  return {p.x, y};
}
inline V2 breachTr(V2 p) {
  const float c = cosf(wh.th), s = sinf(wh.th);
  return proj(wh.Xn + wh.dir * (p.x * c - p.y * s) * BS, wh.yc + (p.x * s + p.y * c) * BS, wh.Z);
}
V2 *alloc(int n) { V2 *r = pool + poolUsed; poolUsed += n; return r; }
void addPoly(V2 *p, int n, Kind k) { polys[npoly++] = {p, n, k}; }

float flukeH(float a) {
  if (a < 1.7f) return -6 + 7.6f * smooth(0, 1.7f, a);
  if (a < 3.4f) return 1.6f + 0.15f * sinf(a * 3);
  return 1.6f - 7.6f * smooth(3.4f, 4.8f, a);
}
float spoutHump(float a) { return -0.5f + 0.9f * smooth(0, 0.6f, a) * (1 - smooth(2.2f, 3.0f, a)); }
inline V2 flukeWorld(float u, float v, float fh, float sw) { return {wh.X + (u + v * sw) * FS, fh * FS / 1.25f + v * FS}; }

// Builds one half of the raised fluke (tip on the +x side): lead edge then trailing edge.
int flukeHalf(float a, V2 *out, int *trailStart) {
  const float curl = 0.14f * sinf(a * 2.1f);
  const V2 B = {0.32f, -0.3f}, Tp = {3.05f, 2.0f + curl}, N = {0, 0.58f};
  int n = bez(B, {1.5f, -0.6f}, {2.8f, 0.4f}, Tp, 10, out);
  V2 tr[13];
  bez(Tp, {2.6f, 1.7f + curl * 0.5f}, {1.1f, 1.5f}, N, 12, tr);
  *trailStart = n - 1;
  for (int i = 1; i <= 12; ++i) {
    V2 p = tr[i];
    if (i < 12) p.y += 0.05f * sinf(i * 2.7f);
    out[n++] = p;
  }
  return n;
}

void buildWhale() {
  poolUsed = 0; npoly = 0; haveGrooves = false; haveEye = false;
  const float a = wh.age;
  if (wh.kind == 0) {
    const float hb = spoutHump(a);
    if (hb > -0.4f) {
      V2 *p = alloc(13);
      for (int i = 0; i <= 12; ++i) {
        const float A = 2.6f, u = -A + 2 * A * i / 12;
        p[i] = proj(wh.Xn + u * wh.dir, -1 + (hb + 1) * sqrtf(fmaxf(0, 1 - (u / A) * (u / A))), wh.Z);
      }
      addPoly(p, 13, K_DARK);
    }
  } else if (wh.kind == 1) {
    const float fh = flukeH(a);
    if (fh > -5.5f) {
      const float sw = 0.12f * sinf(a * 2.2f);
      V2 half[30]; int ts;
      const int hn = flukeHalf(a, half, &ts);
      V2 wing[60]; int wn = 0;
      for (int i = 0; i < hn; ++i) wing[wn++] = half[i];
      for (int i = hn - 2; i >= 0; --i) wing[wn++] = {-half[i].x, half[i].y};
      V2 *p = alloc(wn * 2);
      const int cn = chaikin(wing, wn, p);
      for (int i = 0; i < cn; ++i) { const V2 w = flukeWorld(p[i].x, p[i].y, fh, sw); p[i] = proj(w.x, w.y, wh.Z); }
      addPoly(p, cn, K_FLUKE);
      V2 *s = alloc(18);
      for (int i = 0; i <= 8; ++i) {
        const float v = 0.1f - i * 0.95f, q = i / 8.0f, w = 0.36f + 0.42f * q * q;
        const V2 l = flukeWorld(-w, v, fh, sw), r = flukeWorld(w, v, fh, sw);
        s[i] = proj(l.x, l.y, wh.Z);
        s[17 - i] = proj(r.x, r.y, wh.Z);
      }
      addPoly(s, 18, K_FLUKE);
    }
  } else {
    const float fl = 0.4f * sinf(wh.T * 4), fs = 0.18f * sinf(wh.T * 3.2f);
    V2 tmp[40];
    // Near-side pectoral fin lying on the body.
    {
      const float ca = cosf(fs), sa = sinf(fs), px = 2.6f, py = -0.3f;
      const int n = sizeof(SIDE_FIN) / sizeof(V2);
      for (int i = 0; i < n; ++i) {
        const float dx = SIDE_FIN[i].x - px, dy = SIDE_FIN[i].y - py;
        tmp[i] = bendPt({px + dx * ca - dy * sa, py + dx * sa + dy * ca}, fl);
      }
      V2 *p = alloc(n * 2); const int cn = chaikin(tmp, n, p);
      for (int i = 0; i < cn; ++i) p[i] = breachTr(p[i]);
      addPoly(p, cn, K_SIDE);
    }
    // Pale throat and belly.
    {
      V2 *p = alloc(14);
      for (int i = 0; i < 7; ++i) p[i] = breachTr(bendPt({BXS[i], BU[i]}, fl));
      for (int i = 6; i >= 0; --i) p[13 - i] = breachTr(bendPt({BXS[i], BB[i]}, fl));
      addPoly(p, 14, K_BELLY);
    }
    // Body outline.
    {
      int n = 0;
      for (const V2 &q : BODY_TOP) tmp[n++] = bendPt(q, fl);
      for (const V2 &q : BODY_BOT) tmp[n++] = bendPt(q, fl);
      V2 *p = alloc(n * 2); const int cn = chaikin(tmp, n, p);
      for (int i = 0; i < cn; ++i) p[i] = breachTr(p[i]);
      addPoly(p, cn, K_DARK);
    }
    // Far-side pectoral fin hanging below.
    {
      const int n = sizeof(FIN) / sizeof(V2);
      V2 *p = alloc(n);
      for (int i = 0; i < n; ++i) p[i] = breachTr(bendPt(FIN[i], fl));
      addPoly(p, n, K_LIGHT);
    }
    // Tail fluke.
    {
      const int n = sizeof(FLUKE_PTS) / sizeof(V2);
      for (int i = 0; i < n; ++i) tmp[i] = bendPt(FLUKE_PTS[i], fl);
      V2 *p = alloc(n * 2); const int cn = chaikin(tmp, n, p);
      for (int i = 0; i < cn; ++i) p[i] = breachTr(p[i]);
      addPoly(p, cn, K_DARK);
    }
    const float fr[3] = {0.3f, 0.55f, 0.8f};
    for (int g = 0; g < 3; ++g)
      for (int i = 0; i < 7; ++i)
        grooves[g][i] = breachTr(bendPt({BXS[i] + (i == 0 ? -0.15f * fr[g] : 0), BU[i] + (BB[i] - BU[i]) * fr[g]}, fl));
    haveGrooves = true;
    eyePt = breachTr(bendPt({3.45f, 0.28f}, fl)); haveEye = true;
  }
}

void fillPoly(const Poly &pl, uint8_t id) {
  float ymin = 1e9f, ymax = -1e9f;
  for (int i = 0; i < pl.n; ++i) { ymin = fminf(ymin, pl.p[i].y); ymax = fmaxf(ymax, pl.p[i].y); }
  const int y0 = (int)fmaxf(0, floorf(ymin)), y1 = (int)fminf(H - 1, ceilf(ymax));
  float xs[64];
  for (int y = y0; y <= y1; ++y) {
    const float yc = y + 0.5f;
    int c = 0;
    for (int i = 0, j = pl.n - 1; i < pl.n; j = i++) {
      const V2 a = pl.p[i], b = pl.p[j];
      if ((a.y > yc) != (b.y > yc) && c < 64) xs[c++] = a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y);
    }
    for (int i = 1; i < c; ++i) { const float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; --j; } xs[j + 1] = v; }
    for (int k = 0; k + 1 < c; k += 2) {
      int xa = (int)ceilf(xs[k] - 0.5f), xb = (int)floorf(xs[k + 1] - 0.5f);
      if (xa < 0) xa = 0;
      if (xb > W - 1) xb = W - 1;
      for (int x = xa; x <= xb; ++x) mask[y * W + x] = id;
    }
  }
}
inline uint8_t M(int x, int y) { return (x < 0 || x >= W || y < 0 || y >= H) ? 0 : mask[y * W + x]; }

void drawWhale() {
  buildWhale();
  if (!npoly) return;
  memset(mask, 0, sizeof(mask));
  for (int i = npoly - 1; i >= 0; --i) fillPoly(polys[i], i + 1);
  float fx0 = 1e9f, fx1 = -1e9f, fy0 = 1e9f, fy1 = -1e9f;
  for (int i = 0; i < npoly; ++i)
    for (int j = 0; j < polys[i].n; ++j) {
      fx0 = fminf(fx0, polys[i].p[j].x); fx1 = fmaxf(fx1, polys[i].p[j].x);
      fy0 = fminf(fy0, polys[i].p[j].y); fy1 = fmaxf(fy1, polys[i].p[j].y);
    }
  const int x0 = (int)fmaxf(0, floorf(fx0) - 1), x1 = (int)fminf(W - 1, ceilf(fx1) + 1);
  const int y0 = (int)fmaxf(0, floorf(fy0) - 1), y1 = (int)fminf(H - 1, ceilf(fy1) + 1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const uint8_t id = M(x, y);
      if (!id || y >= occl[x]) continue;
      const Kind kind = polys[id - 1].k;
      bool edge = kind == K_SIDE ? (M(x - 1, y) != id || M(x + 1, y) != id || M(x, y - 1) != id || M(x, y + 1) != id)
                                 : (!M(x - 1, y) || !M(x + 1, y) || !M(x, y - 1) || !M(x, y + 1));
      bool on = edge;
      if (!on) {
        const float b = BAYER[y & 3][x & 3] / 16.0f;
        if (kind == K_LIGHT) on = 0.7f > b;
        else if (kind == K_BELLY) on = 0.75f > b;
        else if (kind == K_SIDE) { on = M(x, y - 1) != id && 0.5f > b; }
        else if (kind == K_FLUKE) { on = !M(x, y - 1) || (0.07f > b && ((x + y) & 1) == 0); }
        else { int d = 0; while (d < 4 && M(x, y - d - 1)) ++d; on = d < 3 && (0.5f - d * 0.15f) > b; }
      }
      fb[y * W + x] = on;
    }
  if (haveGrooves) {
    for (int g = 0; g < 3; ++g)
      for (int i = 0; i < 6; ++i) {
        const V2 a = grooves[g][i], b = grooves[g][i + 1];
        const int n = (int)fmaxf(1, ceilf(hypotf(b.x - a.x, b.y - a.y)));
        for (int k = 0; k <= n; ++k) {
          const int xi = (int)lroundf(a.x + (b.x - a.x) * k / n), yi = (int)lroundf(a.y + (b.y - a.y) * k / n);
          const uint8_t id = M(xi, yi);
          if (id && polys[id - 1].k == K_BELLY && yi < occl[xi]) fb[yi * W + xi] = 0;
        }
      }
  }
  if (haveEye) {
    const int xi = (int)lroundf(eyePt.x), yi = (int)lroundf(eyePt.y);
    if (xi >= 0 && xi < W && yi >= 0 && yi < H && yi < occl[xi]) fb[yi * W + xi] = 1;
  }
}

// ---------- events ----------
const float DIRS[4][5] = {  // dx, dz, start, end, speed
  {0, -1, -70, -1.5f, 0.42f}, {0, 1, -2, 60, 0.4f}, {1, 0, -35, 35, 0.45f}, {-1, 0, -35, 35, 0.45f}};

void startWhale(int kind) {
  if (wh.on) return;
  if (kind < 0) { const float r = frand(); kind = r < 0.36f ? 0 : (r < 0.72f ? 1 : 2); }
  memset(&wh, 0, sizeof(wh));
  wh.on = true; wh.kind = kind; wh.ringT = -1;
  wh.Z = kind == 2 ? 9 + frand() : (kind == 1 ? 5.8f + frand() : 6.5f + frand() * 2.5f);
  const float sxT = kind == 2 ? 56 + frand() * 16 : (kind == 1 ? 40 + frand() * 48 : 34 + frand() * 60);
  wh.dir = frand() < 0.5f ? 1 : -1;
  wh.X = (sxT - 64) * wh.Z / F - (kind == 2 ? wh.dir * 3 : 0);
  wh.Xn = wh.X;
  nextWhale = WHALE_MIN_S + frand() * (WHALE_MAX_S - WHALE_MIN_S);
}

void whaleStep() {
  wh.age += DT;
  const float a = wh.age;
  if (wh.ringT >= 0) wh.ringT += DT;
  if (wh.kind == 0) {
    wh.Xn = wh.X + wh.dir * 0.6f * a;
    wh.wake = smooth(0, 0.6f, a) * (1 - smooth(2.4f, 3.2f, a));
    if (a > 0.35f && a < 1.8f) {
      const float hx = wh.Xn + wh.dir * 1.0f, hy = spoutHump(a) + 0.1f, pw = a < 1.2f ? 1 : 1 - (a - 1.2f) / 0.6f;
      for (int k = 0; k < 10; ++k)
        spawn(hx + (frand() - 0.5f) * 0.25f, hy, wh.Z + (frand() - 0.5f) * 0.2f, (frand() - 0.5f) * 0.03f + wh.dir * 0.012f,
              (0.19f + frand() * 0.08f) * (0.6f + 0.4f * pw), (frand() - 0.5f) * 0.02f, 0.75f, PF_MIST);
    }
    if (a > 5) wh.on = false;
  } else if (wh.kind == 1) {
    wh.Xn = wh.X;
    const float fh = flukeH(a), sw = 0.12f * sinf(a * 2.2f);
    wh.wake = fh > -3 ? 1 : 0.3f;
    if (fh > -1.5f) {
      V2 half[30]; int ts;
      const int hn = flukeHalf(a, half, &ts);
      for (int k = 0; k < 5; ++k) {
        const V2 p = half[ts + (int)(frand() * (hn - ts - 1))];
        const float s = frand() < 0.5f ? -1 : 1;
        const V2 q = flukeWorld(p.x * s, p.y, fh, sw);
        if (q.y > 0) spawn(q.x, q.y, wh.Z, (frand() - 0.5f) * 0.01f, -0.01f, 0, 1);
      }
      for (int s = -1; s <= 1; s += 2) {
        const V2 q = flukeWorld(3.0f * s, 2.0f, fh, sw);
        if (q.y > 0 && frand() < 0.7f) spawn(q.x, q.y, wh.Z, s * 0.01f, 0, 0, 1);
      }
    }
    if (a > 0.9f && !wh.fUp) { wh.fUp = true; burst(wh.X, wh.Z, 45, 0.9f, 2.5f); }
    if (a > 4.5f && !wh.fDown) { wh.fDown = true; burst(wh.X, wh.Z, 100, 1.3f, 3); }
    if (a > 5.8f) wh.on = false;
  } else {
    const float v = 24, g = 19.2f;
    const float vel = v - g * wh.T;
    if (!wh.fDown) wh.T += DT * (0.4f + 0.6f * fminf(1, fabsf(vel) / v)); else wh.T += DT;
    const float T = wh.T;
    wh.Xn = wh.X + wh.dir * 2.4f * T;
    wh.yc = -10 + v * T - 0.5f * g * T * T;
    wh.th = (60 - 90 * fminf(1, T / 2.5f)) * 0.0174533f;
    wh.wake = 1;
    if (wh.yc > -7 && !wh.fUp) { wh.fUp = true; burst(wh.Xn, wh.Z, 200, 2.2f, 4); }
    if (wh.fUp && !wh.fDown) {
      const float c = cosf(wh.th), s = sinf(wh.th);
      for (int k = 0; k < 8; ++k) {
        const float u = -4.5f + frand() * 9, Y = wh.yc + u * s * BS - 1.0f * BS;
        if (Y > 0) spawn(wh.Xn + wh.dir * u * c * BS, Y, wh.Z, (frand() - 0.5f) * 0.03f, -0.03f, 0, 1);
      }
    }
    if (vel < 0 && wh.yc < 1.8f && !wh.fDown) {
      wh.fDown = true; wh.ringT = 0; shake = 1;
      burst(wh.Xn, wh.Z, 220, 3.2f, 6);
      for (int k = 0; k < 70; ++k)
        spawn(wh.Xn + (frand() - 0.5f) * 4, 0, wh.Z, (frand() - 0.5f) * 0.08f, 0.5f + frand() * 0.3f, (frand() - 0.5f) * 0.02f, 1, PF_BIG);
    }
    if (wh.fDown && wh.T > 6) wh.on = false;
  }
}

float realPhase() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return moonPhase;
  const double ref = 947182440.0;  // 2000-01-06 18:14 UTC new moon
  double p = fmod((now - ref) / 86400.0 / 29.530588853, 1.0);
  if (p < 0) p += 1;
  return (float)p;
}
inline float illum(float p) { return (1 - cosf(TWO_PI_F * p)) / 2; }

// ---------- simulation step ----------
void step() {
  tD += DT; ++frame;
  for (int i = 0; i < 5; ++i) waveOff[i] = wrap2pi(WAVES[i].w * tD);
  sets = 1 + 0.3f * sinf(wrap2pi(0.23 * tD));
  far1 = wrap2pi(1.1 * tD); far2 = wrap2pi(1.6 * tD); far3 = wrap2pi(0.7 * tD);
  bigOff = wrap2pi(1.3 * tD); wakeOff = wrap2pi(6.0 * tD);

  nextGust -= DT;
  if (nextGust <= 0) { gust = 1.6f; nextGust = 5 + frand() * 7; }
  if (gust > 0) gust -= DT;
  gustK = gust > 0 ? sinf(3.14159265f * gust / 1.6f) : 0;
  const float g = gustK;

  for (BigWave &b : bigs) {
    if (!b.on) continue;
    b.pos += b.spd;
    const float pr = (b.pos - b.start) / (b.end - b.start);
    if (!b.breaking) {
      if (b.ty == 0) b.amp = 0.4f + 3.6f * powf(fmaxf(0, pr), 1.6f);
      else if (b.ty == 1) b.amp = 3.2f * fminf(1, pr / 0.12f);
      else b.amp = 2.6f * fminf(1, pr / 0.15f);
      if (pr > b.breakAt) b.breaking = true;
    } else if (b.ty != 0) b.amp *= 0.988f;
    if (b.breaking) {
      for (int k = 0; k < 6; ++k) {
        float X, Z;
        if (b.dz != 0) { Z = b.pos * b.dz; X = (frand() - 0.5f) * Z * 3.2f; }
        else { X = b.pos * b.dx; Z = 3 + frand() * frand() * 32; }
        if (Z < 2) continue;
        FOut o; field(X, Z, o);
        spawn(X, o.h, Z, b.dx * 0.08f + (frand() - 0.5f) * 0.05f, 0.05f + frand() * 0.1f, b.dz * 0.08f + (frand() - 0.5f) * 0.05f, 1);
      }
    }
    if (b.ty == 0 && b.pos > -3.2f && !b.splashed) {
      b.splashed = true; splash = 1;
      for (int k = 0; k < 90; ++k)
        spawn((frand() - 0.5f) * 9, 0.5f + frand() * 1.5f, 1.6f + frand() * 2, (frand() - 0.5f) * 0.08f, 0.05f + frand() * 0.16f, -0.02f, 1);
    }
    if (b.ty == 0 && b.pos > -2.5f) b.fade *= 0.85f;
    if (b.ty != 0 && pr > 0.9f) b.fade *= 0.92f;
    if (pr >= 1 || b.fade < 0.05f) b.on = false;
  }

  nextWhale -= DT;
  if (waveBoost > 0) waveBoost -= DT;
  if (!wh.on && nextWhale <= 0) startWhale(-1);
  if (wh.on) whaleStep();

  const int tries = 14 + (int)lroundf(g * 16);
  for (int n = 0; n < tries; ++n) {
    const float Z = 3 + frand() * 60, X = (frand() - 0.5f) * Z * 3.2f;
    FOut o; field(X, Z, o);
    if ((o.base > 0.44f * sets || o.fc > 0.7f) && frand() < 0.6f) {
      const int c = 1 + (int)(frand() * 3);
      const float up = fmaxf(1, Z / 12);
      for (int k = 0; k < c; ++k)
        spawn(X, o.h, Z, (0.01f + g * 0.06f + (frand() - 0.5f) * 0.03f) * up, (0.03f + frand() * 0.06f) * up, (-0.03f - frand() * 0.04f) * up, up);
    }
  }

  for (int i = np - 1; i >= 0; --i) {
    Particle &p = parts[i];
    if ((p.flags & PF_MIST) && p.vy < 0.06f) { p.vx += (frand() - 0.5f) * 0.012f + g * 0.002f; p.vy -= 0.004f; p.vx *= 0.97f; }
    else p.vy -= 0.012f * p.s;
    p.X += p.vx; p.Y += p.vy; p.Z += p.vz;
    bool dead = p.Z < 1.2f || p.Z > 80;
    if (!dead && p.vy < 0) { FOut o; field(p.X, p.Z, o); dead = p.Y < o.h; }
    if (dead) parts[i] = parts[--np];
  }
  if (splash > 0) splash = fmaxf(0, splash - DT * 0.9f);
  if (shake > 0) shake = fmaxf(0, shake - DT);

  nextShoot -= DT;
  if (!shoot.on && nextShoot <= 0) { shoot = {true, 20 + frand() * 70, 2 + frand() * 4, 12}; nextShoot = 5 + frand() * 8; }
  if (shoot.on) { shoot.x -= 2.3f; shoot.y += 0.7f; if (--shoot.life <= 0) shoot.on = false; }
  if (frame % 300 == 1) moonPhase = realPhase();
}

void drawMoon(float p) {
  const float k = cosf(TWO_PI_F * (p < 0.5f ? p : p - 0.5f));
  for (int y = -MOON_R; y <= MOON_R; ++y) {
    const float half = sqrtf((float)(MOON_R * MOON_R - y * y));
    for (int x = -MOON_R; x <= MOON_R; ++x) {
      if (x * x + y * y > MOON_R * MOON_R + 1) continue;
      const float nx = half > 0 ? x / (half + 0.5f) : 0;
      if (p < 0.5f ? nx > k : nx < k) P(MOON_X + x, MOON_Y + y);
    }
  }
  if (illum(p) < 0.06f)
    for (int a = 0; a < 24; a += 2) { const float an = a / 24.0f * TWO_PI_F; P(MOON_X + cosf(an) * MOON_R, MOON_Y + sinf(an) * MOON_R); }
}

void renderSea();

void render() {
  memset(fb, 0, sizeof(fb));
  for (const Star &st : stars) {
    const float v = sinf(wrap2pi(st.ph + tD * st.sp));
    if (v < -0.3f) continue;
    if (abs(st.x - MOON_X) <= MOON_R + 1 && abs(st.y - MOON_Y) <= MOON_R + 1) continue;
    P(st.x, st.y);
    if (st.big && v > 0.8f) { P(st.x - 1, st.y); P(st.x + 1, st.y); P(st.x, st.y - 1); P(st.x, st.y + 1); }
  }
  drawMoon(moonPhase);
  if (shoot.on) for (int k = 0; k < 6; ++k) if (k < shoot.life) P(shoot.x + k * 1.15f, shoot.y - k * 0.35f);

  renderSea();
  if (wh.on) drawWhale();
  for (int i = 0; i < np; ++i) {
    const Particle &p = parts[i];
    const V2 s = proj(p.X, p.Y, p.Z);
    P(s.x, s.y);
    if (p.Z < 3.5f || (p.flags & PF_BIG)) { P(s.x + 1, s.y); P(s.x, s.y + 1); P(s.x + 1, s.y + 1); }
  }
  if (splash > 0) {
    const float top = H - splash * 34;
    for (int y = (int)fmaxf(0, floorf(top)); y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float dns = splash * fminf(1, (y - top) / 12) * 0.85f;
        if (dns > hashf(x * 91 + y * 37 + (frame >> 1) * 613)) fb[y * W + x] = 1;
      }
  }
}

// Front-to-back ray march for screen columns [xa, xb).
void seaColumns(int xa, int xb) {
  const float il = illum(moonPhase);
  const float wz = wh.on ? wh.Z : 999;
  for (int x = xa; x < xb; ++x) {
    int yTop = H; bool prevVis = false, rec = false;
    occl[x] = H;
    const float sx = (x - W / 2) / F;
    for (int zi = 0; zi < nz; ++zi) {
      const ZPre &zp = zpre[zi];
      const float z = zp.z;
      if (!rec && z > wz) { occl[x] = yTop; rec = true; }
      const float X = sx * z;
      FOut o; field(X, z, o, &zp);
      const int ys = (int)lroundf(HZ + (CAMH - o.h) * F * zp.invZ);
      if (ys < yTop) {
        float nx = -o.gx, ny = 1, nz = -o.gz;
        const float inl = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
        nx *= inl; ny *= inl; nz *= inl;
        const float ndl = nx * LX + ny * LY + nz * LZ;
        float I = clampf((ndl - 0.2f) * 1.5f, 0, 0.7f);
        const float fg = fminf(0.4f, z / 140);
        I = I * (1 - fg) + 0.15f * fg;
        float vx = X, vy = o.h - CAMH, vz = z;
        const float ivl = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
        vx *= ivl; vy *= ivl; vz *= ivl;
        const float vd = vx * nx + vy * ny + vz * nz;
        const float spec = (vx - 2 * vd * nx) * LX + (vy - 2 * vd * ny) * LY + (vz - 2 * vd * nz) * LZ;
        const float G = fmaxf(0, (spec - 0.93f) / 0.07f);
        const float fm = fmaxf(o.bf, fmaxf(fmaxf(0, (o.base - 0.42f * sets) / 0.25f), o.fc * 0.75f));
        const int yStart = ys < 0 ? 0 : ys;
        for (int y = yStart; y < yTop; ++y) {
          bool on = I * 16 > BAYER[y & 3][x & 3];
          if (!on && fm > 0 && fm * 0.9f > hashf(x * 131 + y * 71 + (frame >> 1) * 977)) on = true;
          if (!on && G > 0 && G * il * 1.3f > hashf(x * 57 + y * 113 + (frame >> 1) * 331)) on = true;
          fb[y * W + x] = on;
        }
        yTop = ys; prevVis = true;
        if (yTop <= 0) break;
      } else {
        if (prevVis && yTop >= 0 && yTop < H) fb[yTop * W + x] = 1;
        prevVis = false;
      }
    }
    if (!rec) occl[x] = yTop;
    if (yTop >= 0 && yTop < H) fb[yTop * W + x] = 1;
  }
}

#ifdef ESP32
// The left half of the sea is drawn on the other CPU core while this core draws the right half.
TaskHandle_t worker = nullptr;
SemaphoreHandle_t startSem = nullptr, doneSem = nullptr;
void workerTask(void *) {
  for (;;) {
    xSemaphoreTake(startSem, portMAX_DELAY);
    seaColumns(0, W / 2);
    xSemaphoreGive(doneSem);
  }
}
#endif

void renderSea() {
#ifdef ESP32
  if (worker) {
    xSemaphoreGive(startSem);
    seaColumns(W / 2, W);
    xSemaphoreTake(doneSem, portMAX_DELAY);
    return;
  }
#endif
  seaColumns(0, W);
}

}  // namespace

namespace Ocean {

void begin() {
  for (int i = 0; i <= 1024; ++i) SIN_LUT[i] = sinf(i * TWO_PI_F / 1024);
#ifdef ESP32
  rngState = esp_random() | 1;
#else
  rngState = 0x9E3779B9u;
#endif
  for (int i = 0; i < 30; ++i)
    stars[i] = {(uint8_t)(hashf(i * 3 + 1) * W), (uint8_t)(1 + hashf(i * 3 + 2) * (HZ - 12)), hashf(i * 3 + 3) * 6.28f, 0.6f + hashf(i * 7) * 2.2f, i < 5};
  nz = 0;
  for (float z = 2.5f; z < 80 && nz < MAXZ; z += 0.045f * z + 0.05f) fillZPre(zpre[nz++], z);
  moonPhase = realPhase();
  for (int i = 0; i < 30; ++i) step();
#ifdef ESP32
  if (!worker) {
    startSem = xSemaphoreCreateBinary();
    doneSem = xSemaphoreCreateBinary();
    if (startSem && doneSem && xTaskCreatePinnedToCore(workerTask, "ocean", 4096, nullptr, 1, &worker, 0) != pdPASS) worker = nullptr;
  }
#endif
}

void update(U8G2 &oled) {
  const uint32_t t0 = millis();
  step();
  render();
  int ox = 0, oy = 0;
  if (shake > 0) {
    const int a = (int)ceilf(shake * 3.5f);
    ox = (int)lroundf((frand() - 0.5f) * a);
    oy = (int)lroundf((frand() - 0.5f) * a);
  }
  oled.clearBuffer();
  oled.setDrawColor(1);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const int sx = x - ox, sy = y - oy;
      if (sx >= 0 && sx < W && sy >= 0 && sy < H && fb[sy * W + sx]) oled.drawPixel(x, y);
    }
  frameMs = millis() - t0;
  oled.sendBuffer();
}

void bigWave() {
  for (BigWave &b : bigs) {
    if (b.on) continue;
    const int ty = (int)(frand() * 4) & 3;
    const float *D = DIRS[ty];
    b = {};
    b.on = true; b.ty = ty; b.dx = D[0]; b.dz = D[1]; b.start = b.pos = D[2]; b.end = D[3]; b.spd = D[4];
    b.breakAt = ty == 0 ? 0.88f : (ty == 1 ? 0.18f + frand() * 0.08f : 0.42f + frand() * 0.15f);
    b.fade = 1;
    if (waveBoost <= 0 && !wh.on) {
      waveBoost = WAVE_BOOST_WINDOW;
      if (frand() < WAVE_WHALE_CHANCE) nextWhale = fminf(nextWhale, 3 + frand() * 5);
    }
    return;
  }
}

void whale(int kind) { startWhale(kind); }
uint32_t lastFrameMs() { return frameMs; }

}  // namespace Ocean
