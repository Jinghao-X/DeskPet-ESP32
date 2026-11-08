// Space tour for a 128x64 SH1106 in 4-level fake grayscale.
// Each picture is split into 3 one-bit sub-frames; service() pushes them back to back
// while the picture itself is rendered on the other CPU core.
#include "Space.h"
#include "SpaceData.h"
#include <math.h>
#include <string.h>
#ifdef ESP32
#include <esp_random.h>
#endif

namespace {

constexpr int W = 128, H = 64;
constexpr float FOC = 110, DT = 1.0f / 30;
constexpr float PI_F = 3.14159265f, TWO_PI_F = 6.28318531f;

// Work buffers (48 KB) live on the heap in 1024-pixel chunks: once Wi-Fi is up the heap is too
// fragmented for big blocks, but small chunks fit. Brightness is 16-bit fixed point (0..4),
// plenty for at most 4 gray levels. The quantized levels reuse the depth buffer, which is
// no longer needed once the picture is drawn.
constexpr int CH = 1024, NCH = W * H / CH;
constexpr float VSCALE = 16000;
uint16_t *valc[NCH] = {};
float *zbc[NCH] = {};
bool haveBufs = false;
struct ValRef {
  uint16_t &r;
  operator float() const { return r * (1 / VSCALE); }
  ValRef &operator=(float v) { r = v <= 0 ? 0 : v >= 4.09f ? 65535 : (uint16_t)(v * VSCALE + 0.5f); return *this; }
  ValRef &operator=(const ValRef &o) { r = o.r; return *this; }
};
inline ValRef VAL(int k) { return {valc[k >> 10][k & 1023]}; }
inline float &ZB(int k) { return zbc[k >> 10][k & 1023]; }
inline uint8_t &LEV(int k) { return ((uint8_t *)zbc[k >> 10])[k & 1023]; }
uint8_t planes[2][3][W * H / 8];
volatile uint8_t frontBuf = 0;
volatile uint8_t planeCount[2] = {2, 2};
volatile uint8_t grayMode = 0;  // 0 = 3-level (2 sub-frames), 1 = 4-level (3), 2 = black and white
volatile bool active = false, pendingClick = false;
uint32_t frameMs = 0;
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// ---------- math ----------
struct V3 { float x, y, z; };
inline V3 v3(float x, float y, float z) { return {x, y, z}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return sqrtf(dot(a, a)); }
inline V3 norm(V3 a) { return a * (1.0f / sqrtf(dot(a, a))); }
inline V3 lerpV(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline float smooth(float a, float b, float x) { float u = (x - a) / (b - a); u = u < 0 ? 0 : (u > 1 ? 1 : u); return u * u * (3 - 2 * u); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

float SIN_LUT[1025], POW08[257];
inline float fsin(float x) {
  float idx = x * (1024.0f / TWO_PI_F);
  int i = (int)idx;
  if (idx < 0 && idx != (float)i) i -= 1;
  const float f = idx - (float)i;
  i &= 1023;
  return SIN_LUT[i] + (SIN_LUT[i + 1] - SIN_LUT[i]) * f;
}
inline float fcos(float x) { return fsin(x + 1.57079633f); }
inline float gamma08(float x) {
  x = clamp01(x) * 256;
  const int i = (int)x; if (i >= 256) return 1;
  return POW08[i] + (POW08[i + 1] - POW08[i]) * (x - i);
}
inline float fatan2(float y, float x) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float a = fminf(ax, ay) / (fmaxf(ax, ay) + 1e-12f), s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = PI_F - r;
  return y < 0 ? -r : r;
}
inline float fasin(float y) { y = y < -1 ? -1 : (y > 1 ? 1 : y); return fatan2(y, sqrtf(1 - y * y)); }
inline float hashf(uint32_t n) {
  n = (n ^ (n >> 16)) * 0x45d9f3bu; n = (n ^ (n >> 16)) * 0x45d9f3bu; n ^= n >> 16;
  return (float)n * (1.0f / 4294967296.0f);
}
uint32_t rngState = 0x2545F491u;
inline float frand() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return (float)(rngState >> 8) * (1.0f / 16777216.0f); }
inline void pmax(float x, float y, float v) {
  const int xi = (int)lroundf(x), yi = (int)lroundf(y);
  if (xi >= 0 && xi < W && yi >= 0 && yi < H) { ValRef d = VAL(yi * W + xi); if (v > d) d = v; }
}

// ---------- world ----------
const V3 SUN = {0, 0, 0}, EARTH = {34, 0, 8}, MOON = {40, 3, 4}, MARS = {50, -2, 30}, SAT = {88, 5, 62};
constexpr float SUN_R = 9, EARTH_R = 3.67f, MOON_R = 1, MARS_R = 2, SAT_R = 5;
V3 RING_N, GAL_N, GE1, GE2;
const V3 GAL = {150, 300, 900}, BH = {-500, -60, 1100};
constexpr float GAL_R = 140, BH_R = 20;
enum BodyKind : uint8_t { B_SUN, B_EARTH, B_MOON, B_MARS, B_SATURN };
struct Body { V3 c; float r, ext; BodyKind k; };
const Body BODIES[5] = {{SUN, SUN_R, 1, B_SUN}, {EARTH, EARTH_R, 1, B_EARTH}, {MOON, MOON_R, 1, B_MOON}, {MARS, MARS_R, 1, B_MARS}, {SAT, SAT_R, 2.4f, B_SATURN}};

struct Key { V3 P, T; };
constexpr int NKEYS = 7;
Key KEYS[NKEYS];
V3 MOON_FRONT, MF_R, MF_U;

Key keyFor(V3 C, float r, float px, float a, float side, float up) {
  const V3 L = norm(C - SUN), p = norm(cross(v3(0, 1, 0), L));
  const V3 d = norm(L * -cosf(a) + p * (side * sinf(a)) + v3(0, up, 0));
  return {C + d * (FOC * r / px), C};
}

struct Cam { V3 P, fw, rt, up; };
Cam basis(V3 P, V3 T, float roll) {
  const V3 fw = norm(T - P), rt0 = norm(cross(v3(0, 1, 0), fw)), up0 = cross(fw, rt0);
  const float c = cosf(roll), s = sinf(roll);
  return {P, fw, rt0 * c + up0 * s, up0 * c - rt0 * s};
}

struct Path { Key A, B; float D; V3 P1, P2; float sign; };
Path path;
int at = 0, moveTo = 0;
float mv = -1, dur = 3.4f;

void makePath(int a, int b) {
  path.A = KEYS[a]; path.B = KEYS[b];
  path.D = len(path.B.P - path.A.P);
  const V3 fa = norm(path.A.T - path.A.P), fb = norm(path.B.T - path.B.P);
  path.P1 = path.A.P - fa * (0.35f * path.D) + v3(0, 0.14f * path.D, 0);
  path.P2 = path.B.P - fb * (0.35f * path.D) + v3(0, 0.14f * path.D, 0);
  path.sign = (a & 1) ? 1 : -1;
}
V3 bez(float t) {
  const float u = 1 - t;
  return path.A.P * (u * u * u) + path.P1 * (3 * u * u * t) + path.P2 * (3 * u * t * t) + path.B.P * (t * t * t);
}
Cam camera() {
  if (mv < 0) return basis(KEYS[at].P, KEYS[at].T, 0);
  const float e = mv < 0.5f ? 4 * mv * mv * mv : 1 - powf(-2 * mv + 2, 3) / 2;
  return basis(bez(e), lerpV(path.A.T, path.B.T, smooth(0.1f, 0.9f, e)), 0.35f * path.sign * sinf(PI_F * e));
}
void startMove() {
  if (mv >= 0) return;
  moveTo = (at + 1) % NKEYS;
  makePath(at, moveTo);
  dur = 3.4f + fminf(2.6f, path.D / 350);
  mv = 0;
}

// ---------- animated things ----------
double tD = 0;
float t = 0;
uint32_t frame = 0;
struct Star { V3 d; float m, ph; };
constexpr int NSTARS = 520;
Star stars[NSTARS];
constexpr float BOX = 30;
struct Dust { V3 p; bool has; float px, py; };
Dust dust[90];
struct Debris { float r, a, b; };
Debris debris[70];
struct Ufo { bool on, gone, cap; float age, dur, dir, y0, z0, z1, bob; float t0, r0, a0, sc0; };
Ufo ufo = {};
float nextUfo = 30, ufoBoost = 0, flash = 0, shake = 0;

void callUfo() {
  if (ufo.on) return;
  ufo = {};
  ufo.on = true; ufo.dur = 3.6f + frand() * 1.5f; ufo.dir = frand() < 0.5f ? 1 : -1;
  ufo.y0 = -0.25f + frand() * 0.5f; ufo.z0 = 5 + frand() * 5; ufo.z1 = 2.5f + frand() * 3; ufo.bob = frand() * 6;
  nextUfo = 25 + frand() * 35;
}

void step() {
  tD += DT; ++frame;
  t = (float)fmod(tD, 6283.18530718);
  if (mv >= 0) {
    mv += DT / dur;
    if (mv >= 1) {
      mv = -1; at = moveTo;
      if (ufoBoost <= 0) { ufoBoost = 8; if (frand() < 0.5f) nextUfo = fminf(nextUfo, 1.5f + frand() * 3); }
    }
  }
  if (ufoBoost > 0) ufoBoost -= DT;
  nextUfo -= DT;
  if (!ufo.on && nextUfo <= 0) callUfo();
  if (ufo.on) { ufo.age += DT; if (ufo.age > ufo.dur || ufo.gone) ufo.on = false; }
  for (Debris &d : debris) {
    d.a += DT * 2.2f / powf(d.r, 1.5f);
    d.r -= DT * (0.08f + 0.35f / d.r) * d.b;
    if (d.r < 1.02f) { d.r = 4.5f + frand() * 1.5f; d.a = frand() * TWO_PI_F; }
  }
  if (flash > 0) flash = fmaxf(0, flash - DT * 1.6f);
  if (shake > 0) shake = fmaxf(0, shake - DT);
}

// ---------- shading ----------
float moonAlbedo(V3 n) {
  if (dot(n, MOON_FRONT) > 0) {
    int u = (int)((dot(n, MF_R) + 1) * 0.5f * 47.99f), v = (int)((1 - dot(n, MF_U)) * 0.5f * 47.99f);
    u = u < 0 ? 0 : (u > 47 ? 47 : u); v = v < 0 ? 0 : (v > 47 ? 47 : v);
    const int idx = v * 48 + u;
    const uint8_t b = pgm_read_byte(&MOON_TEX[idx >> 1]);
    const int nib = (idx & 1) ? (b & 15) : (b >> 4);
    return 0.25f + 0.85f * nib / 14.0f;
  }
  return 0.6f + 0.12f * fsin(n.x * 9 + n.y * 7);
}
inline bool landAt(int i, int j) { const int idx = j * 96 + i; return (pgm_read_byte(&EARTH_LAND[idx >> 3]) >> (idx & 7)) & 1; }

float shade(BodyKind k, V3 n, V3 S, V3 dir) {
  const float lit = dot(n, S);
  const float day = 0.06f + 0.94f * smooth(-0.15f, 0.35f, lit) * fmaxf(0.2f, lit + 0.2f) / 1.2f;
  switch (k) {
    case B_MOON: { const float rim = 1 - fabsf(dot(n, dir)); return gamma08(moonAlbedo(n) * day * (1 - 0.12f * rim)); }
    case B_EARTH: {
      const float lon = fatan2(n.x, -n.z) + t * 0.12f, lat = fasin(n.y);
      float lu = lon / TWO_PI_F + 0.5f; lu -= floorf(lu);
      int i = (int)(lu * 96); if (i > 95) i = 95;
      int j = (int)((0.5f - lat / PI_F) * 48); j = j < 0 ? 0 : (j > 47 ? 47 : j);
      const bool land = landAt(i, j);
      const float alb = land ? (fabsf(lat) > 1.15f ? 0.92f : 0.7f) : 0.3f;
      const float cl = fsin(lon * 3 + fsin(lat * 5 + t * 0.15f) * 1.6f + t * 0.05f) * fsin(lat * 6.5f + lon * 1.3f - t * 0.08f) + 0.5f * fsin(lon * 7 - lat * 4 + t * 0.1f);
      const float cloud = smooth(0.45f, 0.85f, cl);
      float v = (alb * (1 - cloud) + 0.97f * cloud) * day;
      if (!land && cloud < 0.3f) {
        const V3 r = dir - n * (2 * dot(dir, n));
        const float sp = fmaxf(0, dot(r, S));
        float p8 = sp * sp; p8 *= p8; p8 *= p8;  // sp^8
        v += p8 * p8 * p8 * 0.7f;                // sun glint, sp^24
      }
      if (lit < -0.1f && land && cloud < 0.4f && hashf(i * 131 + j * 977) > 0.8f) v = fmaxf(v, 0.34f);
      return gamma08(v);
    }
    case B_MARS: {
      const float lon = fatan2(n.x, -n.z) + t * 0.1f, lat = fasin(n.y);
      float alb = 0.62f;
      const float d = fsin(lon * 2 + 1) * fsin(lat * 3 + 0.5f) + 0.6f * fsin(lon * 5 - lat * 2) + 0.3f * fsin(lon * 11 + lat * 7);
      if (d > 0.55f) alb = 0.36f; else if (d > 0.35f) alb = 0.48f;
      if (fabsf(lat) > 1.18f) alb = 0.95f;
      return gamma08(alb * day);
    }
    case B_SATURN: {
      const float lat = fasin(dot(n, RING_N));
      const float alb = 0.66f + 0.13f * fsin(lat * 13) + 0.06f * fsin(lat * 31 + 0.5f) - 0.15f * smooth(1.1f, 1.5f, fabsf(lat));
      return gamma08(alb * day);
    }
    default: return 0.78f + 0.22f * fabsf(dot(n, dir));
  }
}

inline V3 rayDir(const Cam &c, int x, int y) { return norm(c.fw * FOC + c.rt * (x + 0.5f - W / 2) + c.up * (H / 2 - (y + 0.5f))); }
struct P2 { bool ok; float x, y, z, d2; };
inline P2 project(const Cam &c, V3 p) {
  const V3 v = p - c.P; const float z = dot(v, c.fw);
  if (z <= 0.05f) return {false, 0, 0, 0, 0};
  return {true, W / 2 + FOC * dot(v, c.rt) / z, H / 2 - FOC * dot(v, c.up) / z, z, dot(v, v)};
}
inline void bbox(float cx, float cy, float rx, float ry, int &x0, int &x1, int &y0, int &y1) {
  x0 = (int)fmaxf(0, floorf(cx - rx)); x1 = (int)fminf(W - 1, ceilf(cx + rx));
  y0 = (int)fmaxf(0, floorf(cy - ry)); y1 = (int)fminf(H - 1, ceilf(cy + ry));
}

void drawUfo(float px, float py, float sc, float tilt, bool lights) {
  int x0, x1, y0, y1; bbox(px, py, sc * 2.4f, sc * 1.6f, x0, x1, y0, y1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float lx = x + 0.5f - px, ly = (y + 0.5f - py) - lx * tilt;
      const bool body = (lx * lx) / (4 * sc * sc) + (ly * ly) / (0.1764f * sc * sc) <= 1;
      const bool dome = ly < 0 && (lx * lx) / (0.7225f * sc * sc) + (ly * ly) / (1.21f * sc * sc) <= 1;
      if (dome) VAL(y * W + x) = 0.45f;
      if (body) VAL(y * W + x) = ly < 0 ? 0.97f : 0.65f;
    }
  if (lights)
    for (int i = 0; i < 5; ++i) {
      const float lx = (i - 2) * sc * 0.75f, ly = sc * 0.12f + lx * tilt;
      const int x = (int)lroundf(px + lx), y = (int)lroundf(py + ly);
      if (x >= 0 && x < W && y >= 0 && y < H && (((frame >> 2) + i) % 3) == 0) VAL(y * W + x) = 0.05f;
    }
}

void render() {
  for (int i = 0; i < W * H; ++i) { VAL(i) = 0; ZB(i) = 1e9f; }
  const Cam cam = camera();
  const P2 bh = project(cam, BH);
  const float bhs = bh.ok ? FOC * BH_R / sqrtf(fmaxf(1, bh.d2 - BH_R * BH_R)) : 0;

  // Stars (bent around the black hole).
  for (const Star &s : stars) {
    const float z = dot(s.d, cam.fw);
    if (z <= 0.05f) continue;
    float x = W / 2 + FOC * dot(s.d, cam.rt) / z, y = H / 2 - FOC * dot(s.d, cam.up) / z;
    if (bh.ok && bhs > 0.5f) {
      const float dx = x - bh.x, dy = y - bh.y, d = sqrtf(dx * dx + dy * dy), E = 1.3f * bhs;
      if (d < E * 4) {
        const float d2 = (d + sqrtf(d * d + 4 * E * E)) / 2;
        if (d2 < bhs * 1.08f) continue;
        const float a = fatan2(dy, dx) + 0.6f * E * E / (d * d + E * E) * fsin(t * 0.3f);
        x = bh.x + fcos(a) * d2; y = bh.y + fsin(a) * d2;
      }
    }
    pmax(x, y, s.m > 0.93f ? 0.67f + 0.33f * (0.5f + 0.5f * fsin(t * (1 + s.m * 3) + s.ph)) : (s.m > 0.7f ? 0.67f : 0.36f));
  }

  // Dust streaks while flying.
  for (Dust &d : dust) {
    if (mv < 0) { d.has = false; continue; }
    auto wrapf = [](float v) { v = fmodf(v, BOX); if (v < 0) v += BOX; return v - BOX / 2; };
    const V3 r = v3(wrapf(d.p.x - cam.P.x), wrapf(d.p.y - cam.P.y), wrapf(d.p.z - cam.P.z));
    const float z = dot(r, cam.fw);
    if (z < 0.3f) { d.has = false; continue; }
    const float px = W / 2 + FOC * dot(r, cam.rt) / z, py = H / 2 - FOC * dot(r, cam.up) / z;
    if (d.has) {
      const int n = (int)ceilf(hypotf(px - d.px, py - d.py));
      if (n < 45)
        for (int k = 0; k <= n; ++k) {
          const float f = n ? (float)k / n : 0;
          pmax(d.px + (px - d.px) * f, d.py + (py - d.py) * f, fminf(0.8f, 5 / z));
        }
    }
    d.has = true; d.px = px; d.py = py;
  }

  // Milky Way disk.
  {
    const P2 g = project(cam, GAL);
    if (g.ok) {
      const float rp = FOC * GAL_R / sqrtf(g.d2) * 1.3f + 2;
      int x0, x1, y0, y1; bbox(g.x, g.y, rp, rp, x0, x1, y0, y1);
      const float tg = t * 0.04f;
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const V3 dir = rayDir(cam, x, y);
          const float dn = dot(dir, GAL_N);
          if (fabsf(dn) < 1e-4f) continue;
          const float tt = dot(GAL - cam.P, GAL_N) / dn;
          if (tt <= 0) continue;
          const V3 q = cam.P + dir * tt - GAL;
          const float u = dot(q, GE1) / GAL_R, v = dot(q, GE2) / GAL_R, r = sqrtf(u * u + v * v);
          if (r > 1.1f) continue;
          const float th = fatan2(v, u);
          float arm = 0.5f + 0.5f * fcos(2 * th - 5.5f * logf(r + 0.05f) - tg);
          arm = arm * arm * arm;
          float b = expf(-(r / 0.13f) * (r / 0.13f)) * 1.1f + expf(-r / 0.33f) * (0.12f + 0.88f * arm) * 0.95f;
          if (hashf(x * 131 + y * 977 + (uint32_t)(t * 2)) > 0.965f) b += 0.35f * expf(-r / 0.5f);
          b *= 1 - smooth(0.85f, 1.1f, r);
          ValRef dst = VAL(y * W + x);
          if (b > dst) dst = fminf(1, b);
        }
    }
  }

  // Sun corona.
  {
    const V3 v = SUN - cam.P; const float z = dot(v, cam.fw);
    if (z > 0) {
      const float cx = W / 2 + FOC * dot(v, cam.rt) / z, cy = H / 2 - FOC * dot(v, cam.up) / z;
      const float rad = FOC * SUN_R / sqrtf(fmaxf(0.01f, dot(v, v) - SUN_R * SUN_R)), R = rad * 2.6f;
      int x0, x1, y0, y1; bbox(cx, cy, R, R, x0, x1, y0, y1);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
          if (d < rad) continue;
          const float a = fatan2(dy, dx);
          const float g = expf(-(d - rad) / (0.5f * rad)) * (0.55f + 0.45f * fsin(a * 7 + t * 0.4f) * fsin(a * 4 - t * 0.25f)) * 0.9f;
          ValRef dst = VAL(y * W + x);
          if (g > dst) dst = g;
        }
    }
  }

  // Sun, planets, Saturn's rings.
  for (const Body &body : BODIES) {
    const V3 v = body.c - cam.P; const float z = dot(v, cam.fw);
    if (z <= 0) continue;
    const float cx = W / 2 + FOC * dot(v, cam.rt) / z, cy = H / 2 - FOC * dot(v, cam.up) / z, dd = dot(v, v);
    const float rad = FOC * body.r * body.ext / sqrtf(fmaxf(0.01f, dd - body.r * body.r)) * 1.15f + 3;
    int x0, x1, y0, y1; bbox(cx, cy, rad, rad, x0, x1, y0, y1);
    if (x0 > x1 || y0 > y1) continue;
    const V3 S = norm(SUN - body.c), oc = cam.P - body.c;
    const float occ = dot(oc, oc) - body.r * body.r;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const V3 dir = rayDir(cam, x, y);
        const float b = dot(oc, dir), disc = b * b - occ;
        const int k = y * W + x;
        float th = disc >= 0 ? -b - sqrtf(disc) : 1e9f;
        if (th <= 0) th = 1e9f;
        if (body.k == B_SATURN) {
          const float dn = dot(dir, RING_N);
          if (fabsf(dn) > 1e-4f) {
            const float tr = dot(body.c - cam.P, RING_N) / dn;
            if (tr > 0 && tr < th && tr < ZB(k)) {
              const V3 qo = cam.P + dir * tr - body.c;
              const float rr = len(qo) / body.r;
              if (rr > 1.25f && rr < 2.3f && !(rr > 1.93f && rr < 2.01f)) {
                float rb = (0.58f + 0.14f * fsin(rr * 37) + 0.08f * fsin(rr * 91)) * (0.4f + 0.6f * fabsf(dot(RING_N, S))) * (rr < 1.5f ? 0.7f : 1);
                const float sb = dot(qo, S), sc = dot(qo, qo) - body.r * body.r;
                if (sb < 0 && sb * sb - sc > 0) rb *= 0.15f;
                ZB(k) = tr; VAL(k) = gamma08(rb);
                continue;
              }
            }
          }
        }
        if (th >= 1e9f) {
          if (body.k == B_EARTH) {  // thin atmosphere glow
            const float dist = sqrtf(fmaxf(0, dot(oc, oc) - b * b)) - body.r;
            if (dist < body.r * 0.09f && -b > 0 && ZB(k) > -b) {
              const V3 q = norm(oc + dir * -b);
              VAL(k) = fmaxf(VAL(k), (1 - dist / (body.r * 0.09f)) * fmaxf(0, dot(q, S) + 0.25f) * 0.5f);
            }
          }
          continue;
        }
        if (th >= ZB(k)) continue;
        ZB(k) = th;
        VAL(k) = shade(body.k, norm(cam.P + dir * th - body.c), S, dir);
      }
  }

  // Black hole: shadow, photon ring, lensed accretion disk, infalling debris.
  if (bh.ok && bhs > 0.4f) {
    const float s = bhs;
    int x0, x1, y0, y1; bbox(bh.x, bh.y - 0.4f * s, 5.6f * s, 3.0f * s, x0, x1, y0, y1);
    auto streak = [](float r) { return 0.6f + 0.4f * fsin(r * 13 + t * 3.4f) * fsin(r * 4.7f + 1.3f + t * 0.8f); };
    const float T = 0.08f + 0.55f / s;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float X = (x + 0.5f - bh.x) / s, Y = (y + 0.5f - bh.y) / s, d = sqrtf(X * X + Y * Y), ax = fabsf(X);
        const float dop = 1 + 0.32f * (-X / (ax + 0.6f));
        float b = 0; bool front = false;
        const float bandHalf = T * (1 + 0.9f * expf(-ax * ax / 1.8f));
        if (fabsf(Y - 0.05f) < bandHalf && ax < 5.4f) {
          b = fmaxf(b, (ax < 1.2f ? 1 : 1.3f / (0.3f + ax * 0.6f)) * streak(ax) * dop * (1 - smooth(4, 5.4f, ax)) * (1 - 0.35f * fabsf(Y - 0.05f) / bandHalf));
          front = true;
        }
        if (Y < 0.05f) {
          const float rr = sqrtf(X * X + Y * Y * 0.9025f);
          if (rr > 1.22f && rr < 3.0f) {
            const float f = (rr - 1.22f) / 1.78f;
            b = fmaxf(b, powf(1 - f, 1.15f) * (0.5f + 0.5f * smooth(0, 0.12f, f)) * streak(rr * 1.1f) * dop * 1.12f);
          }
          const float wing = expf(-((ax - 1.9f) / 1.6f) * ((ax - 1.9f) / 1.6f)) * 0.9f;
          if (-Y < wing && ax > 1.2f && ax < 4.6f) b = fmaxf(b, (1 - (-Y) / wing) * 0.85f / (0.4f + ax * 0.35f) * streak(ax * 1.2f) * dop);
        }
        if (Y > bandHalf) {
          const float rr = sqrtf(X * X + Y * Y * 1.3225f);
          if (rr > 1.12f && rr < 2.15f) b = fmaxf(b, 0.8f * powf(1 - (rr - 1.12f) / 1.03f, 1.1f) * streak(rr + 0.3f) * dop);
        }
        if (!front && fabsf(d - 1.07f) < fmaxf(0.045f, 0.55f / s)) b = fmaxf(b, 0.9f + flash * 0.1f);
        if (flash > 0 && d > 1 && d < 1.6f) b = fmaxf(b, flash * (1 - (d - 1) / 0.6f));
        ValRef dst = VAL(y * W + x);
        if (d < 1 && !front) dst = 0;
        else if (b > 0) dst = fmaxf(dst, fminf(1, b));
      }
    for (const Debris &p : debris) {
      const float sx = bh.x + fcos(p.a) * p.r * s, sy = bh.y + 0.05f * s + fsin(p.a) * p.r * 0.22f * s;
      const bool behind = fsin(p.a) < 0;
      const float dx = (sx - bh.x) / s, dy = (sy - bh.y) / s;
      if (behind && dx * dx + dy * dy < 1.1f) continue;
      pmax(sx, sy, 0.5f + 0.5f * smooth(3, 1.2f, p.r));
    }
  }

  // UFO (gets swallowed at the black hole stop).
  if (ufo.on) {
    const float u = ufo.age / ufo.dur, sxn = ufo.dir * (-1.3f + 2.6f * u), z = ufo.z0 + (ufo.z1 - ufo.z0) * u;
    float px = W / 2 + sxn * W / 2, py = H / 2 - (ufo.y0 + 0.12f * fsin(ufo.age * 3 + ufo.bob)) * H / 2;
    float sc = 26 / z, tilt = 0.18f * fsin(ufo.age * 2.2f) * ufo.dir;
    bool lights = true;
    if (bh.ok && bhs > 5 && mv < 0) {
      if (!ufo.cap && ufo.age > 0.9f) {
        const float dx = px - bh.x, dy = py - bh.y;
        ufo.cap = true; ufo.t0 = ufo.age; ufo.r0 = sqrtf(dx * dx + dy * dy * 4.84f); ufo.a0 = fatan2(dy * 2.2f, dx); ufo.sc0 = sc; ufo.dur = 99;
      }
      if (ufo.cap) {
        const float tau = fminf(1, (ufo.age - ufo.t0) / 2.6f), r = ufo.r0 * powf(1 - tau, 1.4f), a = ufo.a0 - ufo.dir * tau * tau * 10;
        px = bh.x + fcos(a) * r; py = bh.y + 0.05f * bhs + fsin(a) * r / 2.2f;
        sc = ufo.sc0 * fmaxf(0.12f, powf(fmaxf(r, 1e-3f) / ufo.r0, 0.7f)); tilt = fsin(a) * 0.8f; lights = ((frame >> 1) & 1) == 0;
        if (r < bhs * 1.05f) { ufo.gone = true; flash = 1; shake = 0.5f; }
        for (int k = 1; k < 5; ++k) {
          const float a2 = a + ufo.dir * k * 0.12f, r2 = r * (1 + k * 0.06f);
          pmax(bh.x + fcos(a2) * r2, bh.y + 0.05f * bhs + fsin(a2) * r2 / 2.2f, 0.4f - k * 0.07f);
        }
      }
    }
    if (!ufo.gone) drawUfo(px, py, sc, tilt, lights);
  }

  // Quantize to the current gray mode: nsub sub-frames give nsub+1 levels, with ordered
  // dithering between levels.
  const int nsub = grayMode == 0 ? 2 : (grayMode == 1 ? 3 : 1);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float q = clamp01(VAL(y * W + x)) * nsub;
      int L = (int)q;
      if (L < nsub && (q - L) * 16 > BAYER[y & 3][x & 3] + 0.5f) ++L;
      LEV(y * W + x) = L > nsub ? nsub : L;
    }
}

// Pack the picture into sub-frames (display memory layout, rotated 180 degrees). Neighbouring
// pixels light in different sub-frames, so each sub-frame carries about the same total light
// and large gray areas shimmer finely instead of pulsing.
int buildPlanes(uint8_t dst[3][W * H / 8]) {
  const int nsub = grayMode == 0 ? 2 : (grayMode == 1 ? 3 : 1);
  memset(dst, 0, 3 * W * H / 8);
  int ox = 0, oy = 0;
  if (shake > 0) { const int a = (int)ceilf(shake * 4); ox = (int)lroundf((frand() - 0.5f) * a); oy = (int)lroundf((frand() - 0.5f) * a); }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const int sx = x - ox, sy = y - oy;
      if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
      const uint8_t L = LEV(sy * W + sx);
      if (!L) continue;
      const int off = nsub == 3 ? (x + 2 * y) % 3 : ((x + y) & 1);
      const int px = W - 1 - x, py = H - 1 - y, idx = (py >> 3) * W + px;
      const uint8_t bit = 1 << (py & 7);
      for (int k = 0; k < nsub; ++k) if (L > (k + off) % nsub) dst[k][idx] |= bit;
    }
  return nsub;
}

void produceFrame() {
  if (!haveBufs) return;
  const uint32_t t0 = millis();
  if (pendingClick) { pendingClick = false; startMove(); }
  step();
  render();
  const uint8_t back = frontBuf ^ 1;
  planeCount[back] = buildPlanes(planes[back]);
  frontBuf = back;
  frameMs = millis() - t0;
}

// The 48 KB of work buffers exist only while the space screen shows: Wi-Fi needs that
// memory to connect (with it taken at boot, the router's password handshake times out).
// Only this task touches them, so it allocates and frees them itself.
bool allocBuffers() {
  if (haveBufs) return true;
  for (int i = 0; i < NCH; ++i) {
    if (!valc[i]) valc[i] = (uint16_t *)malloc(sizeof(uint16_t) * CH);
    if (!zbc[i]) zbc[i] = (float *)malloc(sizeof(float) * CH);
    if (!valc[i] || !zbc[i]) return false;
  }
  return haveBufs = true;
}
void freeBuffers() {
  haveBufs = false;
  for (int i = 0; i < NCH; ++i) { free(valc[i]); free(zbc[i]); valc[i] = nullptr; zbc[i] = nullptr; }
}
#ifdef ESP32
TaskHandle_t worker = nullptr;
void workerTask(void *) {
  for (;;) {
    if (!active) {  // screen left: give back the memory and end this task; setActive starts a new one
      freeBuffers();
      worker = nullptr;
      vTaskDelete(nullptr);
    }
    if (!allocBuffers()) {  // out of memory: try again later
      static uint32_t warnedAt = 0;
      if (millis() - warnedAt > 5000) { warnedAt = millis(); Serial.printf("Space: not enough memory (free %u bytes)\n", (unsigned)ESP.getFreeHeap()); }
      vTaskDelay(pdMS_TO_TICKS(200)); continue;
    }
    const uint32_t t0 = millis();
    produceFrame();
    // Always sleep a little, even when a frame took longer than 33 ms: core 0's idle task
    // must run, or the task watchdog resets the board after 5 seconds.
    const uint32_t spent = millis() - t0;
    vTaskDelay(pdMS_TO_TICKS(spent + 4 < 33 ? 33 - spent : 4));
  }
}
#endif

}  // namespace

namespace Space {

void begin() {
#ifndef ESP32
  allocBuffers();
#endif
  for (int i = 0; i <= 1024; ++i) SIN_LUT[i] = sinf(i * TWO_PI_F / 1024);
  for (int i = 0; i <= 256; ++i) POW08[i] = powf(i / 256.0f, 0.8f);
#ifdef ESP32
  rngState = esp_random() | 1;
#endif
  RING_N = norm(v3(0.28f, 1, 0.12f));
  GAL_N = norm(v3(0.25f, 1, 0.35f)); GE1 = norm(cross(GAL_N, v3(0, 0, 1))); GE2 = cross(GAL_N, GE1);
  KEYS[0] = keyFor(MOON, MOON_R, 26, 0.75f, -1, 0.15f);
  KEYS[1] = keyFor(EARTH, EARTH_R, 26, 0.6f, 1, 0.15f);
  KEYS[2] = keyFor(MARS, MARS_R, 26, 0.7f, 1, 0.15f);
  KEYS[3] = keyFor(SAT, 11.5f, 52, 0.5f, 1, 0.4f);
  KEYS[4] = {v3(-20, 16, -48), v3(22, -3, 22)};
  { const V3 p = norm(cross(v3(0, 1, 0), GAL_N)); KEYS[5] = {GAL + norm(GAL_N * 0.75f + p * 0.66f) * (FOC * GAL_R / 62), GAL}; }
  KEYS[6] = {BH + v3(8, 3, -183), BH + v3(0, 6.5f, 0)};
  MOON_FRONT = norm(KEYS[0].P - MOON);
  MF_R = norm(cross(v3(0, 1, 0), MOON_FRONT * -1)); MF_U = cross(MOON_FRONT * -1, MF_R);
  for (int i = 0; i < NSTARS; ++i) {
    const float u = hashf(i * 7 + 1) * 2 - 1, a = hashf(i * 7 + 2) * TWO_PI_F, r = sqrtf(1 - u * u);
    stars[i] = {v3(r * cosf(a), u, r * sinf(a)), hashf(i * 7 + 3), hashf(i * 7 + 4) * 6.28f};
  }
  for (int i = 0; i < 90; ++i) dust[i] = {v3(hashf(i * 5 + 1) * BOX, hashf(i * 5 + 2) * BOX, hashf(i * 5 + 3) * BOX), false, 0, 0};
  for (Debris &d : debris) d = {1.3f + frand() * 4.5f, frand() * TWO_PI_F, 0.4f + frand() * 0.6f};
#ifndef ESP32
  produceFrame();
#endif
}

// The render task (and its 72 KB) exists only while the space screen shows.
void setActive(bool a) {
  active = a;
#ifdef ESP32
  if (a && !worker) xTaskCreatePinnedToCore(workerTask, "space", 12288, nullptr, 1, &worker, 0);
#endif
}

void service(U8G2 &oled) {
  static uint8_t sub = 0;
#ifndef ESP32
  if (sub == 0) produceFrame();  // host builds: no worker task, render in line
#endif
  const uint8_t f = frontBuf, n = planeCount[f];
  if (sub >= n) sub = 0;
  memcpy(oled.getBufferPtr(), planes[f][sub], W * H / 8);
  oled.sendBuffer();
  sub = (sub + 1) % n;
}

void click() { pendingClick = true; }
void cycleGrayMode() { grayMode = (grayMode + 1) % 3; }
uint32_t lastFrameMs() { return frameMs; }

}  // namespace Space
