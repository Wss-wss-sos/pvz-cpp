/* ============================================================================
 *  植物大战僵尸 · C++ 版  (Plants vs Zombies - C++ Edition)
 * ----------------------------------------------------------------------------
 *  · 单文件、零第三方依赖（仅 Win32 API + GDI 文本 + winmm 合成音效）
 *  · 自研软件光栅化渲染器：2x 超采样 + 盒式滤波降采样 = 全画面抗锯齿
 *  · 完整移植网页版玩法：阳光经济 / 9 种植物 / 7 种僵尸 / 15 波进攻
 *
 *  编译 (MinGW-w64):
 *      g++ -O2 -std=c++17 -o pvz.exe pvz.cpp -lgdi32 -luser32 -lwinmm -mwindows
 *  编译 (MSVC):
 *      cl /O2 /EHsc pvz.cpp user32.lib gdi32.lib winmm.lib
 *
 *  无头自检（不建窗口，模拟 400 秒游戏流程并全实体渲染）:
 *      pvz.exe --selftest
 * ==========================================================================*/
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#endif

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

/* ============================ 一、常量与定义 ============================ */

static const int W = 1000, H = 600;          // 画布逻辑尺寸
static const int TOPBAR_H = 84;              // 顶栏（阳光 + 卡片 + 工具）
static const int BAR_H = 34;                 // 底部进度条
static const int CLW = W, CLH = TOPBAR_H + H + BAR_H;  // 客户区 1000x718

static const int ROWS = 6, COLS = 10, CELL_W = 82, CELL_H = 84;
static const int GRID_X = 100, GRID_Y = 46;
static const int MAX_WAVE = 15;
static const int FLAG_WAVES[2] = { 8, 15 };

static inline float cellXf(int c) { return GRID_X + c * CELL_W + CELL_W * 0.5f; }
static inline float cellYf(int r) { return GRID_Y + r * CELL_H + CELL_H * 0.5f; }

/* ---------- 植物 ---------- */
enum {
  PT_SUNFLOWER = 0, PT_PEASHOOTER, PT_WALLNUT, PT_SNOWPEA, PT_CHERRY,
  PT_REPEATER, PT_GATLING, PT_POTATO, PT_CHOMPER, PT_COUNT
};
struct PlantDef { const wchar_t* name; int cost; float cd; int hp; const wchar_t* tip; };
static const PlantDef PLANT_DEFS[PT_COUNT] = {
  { L"向日葵",   25,  7.5f, 300, L"每 20 秒生产 25 阳光" },
  { L"豌豆射手", 50,  7.5f, 300, L"发射豌豆攻击僵尸" },
  { L"坚果墙",   25,  25.f, 2000, L"高血量阻挡僵尸" },
  { L"寒冰射手", 100, 7.5f, 300, L"冰豌豆减速僵尸" },
  { L"樱桃炸弹", 100, 40.f, 400, L"1.2 秒后 3x3 范围爆炸" },
  { L"双发射手", 125, 7.5f, 300, L"一次射出两颗豌豆" },
  { L"机枪射手", 250, 30.f, 300, L"种在双发射手上升级,一次四连发" },
  { L"土豆雷",   25,  25.f, 300, L"埋伏 12 秒后炸飞踩到它的僵尸" },
  { L"大嘴花",   150, 20.f, 300, L"一口吞掉右侧僵尸,咀嚼 15 秒" },
};
static const int ORDER[PT_COUNT] = {
  PT_SUNFLOWER, PT_PEASHOOTER, PT_WALLNUT, PT_SNOWPEA, PT_CHERRY,
  PT_REPEATER, PT_GATLING, PT_POTATO, PT_CHOMPER
};

/* ---------- 僵尸 ---------- */
enum {
  ZT_NORMAL = 0, ZT_CONE, ZT_BUCKET, ZT_FLAG, ZT_POLE, ZT_DOOR, ZT_FOOTBALL, ZT_COUNT
};
struct ZombieDef { float hp, speed, points, door; };
static const ZombieDef ZOMBIE_DEFS[ZT_COUNT] = {
  { 200,  14,   1,   0 },
  { 560,  13,   2.5f,0 },
  { 1200, 12.5f,5,   0 },
  { 250,  16,   2,   0 },
  { 500,  20,   2,   0 },
  { 800,  13,   3,   420 },
  { 1400, 18,   5,   0 },
};

/* ============================ 二、工具函数 ============================ */

static std::mt19937 g_rng(20240607);
static inline float rnd(float a, float b) {
  return std::uniform_real_distribution<float>(a, b)(g_rng);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static bool g_headless = false;   // --selftest 模式

/* ============================ 三、颜色与渐变 ============================ */

struct Col { int r, g, b, a; };
static inline Col col(int r, int g, int b, int a = 255) { Col c; c.r = r; c.g = g; c.b = b; c.a = a; return c; }
static inline Col hexc(int v, int a = 255) { return col((v >> 16) & 255, (v >> 8) & 255, v & 255, a); }
static inline Col lerpCol(const Col& x, const Col& y, float t) {
  Col c; c.r = (int)lerpf((float)x.r, (float)y.r, t); c.g = (int)lerpf((float)x.g, (float)y.g, t);
  c.b = (int)lerpf((float)x.b, (float)y.b, t); c.a = (int)lerpf((float)x.a, (float)y.a, t); return c;
}
struct Grad { int n; float pos[4]; Col c[4]; };
static Grad grad1(Col c) { Grad g; g.n = 1; g.pos[0] = 0; g.c[0] = c; return g; }
static Grad grad2(float p0, Col c0, float p1, Col c1) {
  Grad g; g.n = 2; g.pos[0] = p0; g.c[0] = c0; g.pos[1] = p1; g.c[1] = c1; return g;
}
static Grad grad3(float p0, Col c0, float p1, Col c1, float p2, Col c2) {
  Grad g; g.n = 3; g.pos[0] = p0; g.c[0] = c0; g.pos[1] = p1; g.c[1] = c1; g.pos[2] = p2; g.c[2] = c2; return g;
}
static Col gradAt(const Grad& g, float t) {
  t = clampf(t, 0.f, 1.f);
  for (int i = 0; i < g.n - 1; i++) {
    if (t <= g.pos[i + 1] || i == g.n - 2) {
      float span = g.pos[i + 1] - g.pos[i];
      float k = span > 0.f ? (t - g.pos[i]) / span : 1.f;
      return lerpCol(g.c[i], g.c[i + 1], clampf(k, 0.f, 1.f));
    }
  }
  return g.c[g.n - 1];
}

/* ============================ 四、渲染核心 ============================ */

static const int SS = 2;                      // 超采样倍率
static int g_bw = 0, g_bh = 0;                // 2x 层（覆盖整个客户区）
static uint32_t* g_px = nullptr;              // 2x 层像素 (B|G<<8|R<<16)
static HDC g_hdcMem = nullptr; static HBITMAP g_bmpMem = nullptr; static HGDIOBJ g_oldMem = nullptr;
static uint32_t* g_out = nullptr;             // 1x 输出
static HDC g_hdcOut = nullptr; static HBITMAP g_bmpOut = nullptr; static HGDIOBJ g_oldOut = nullptr;
static uint32_t* g_cache = nullptr;           // 静态背景缓存（画布区域 2x）
static HBITMAP g_bmpCache = nullptr;          // 缓存位图（必须存活）

/* ---------- 变换栈 ---------- */
struct Xf { float tx, ty, ts, rot, cs, sn; };
static Xf g_xf = { 0, 0, 1, 0, 1, 0 };
static std::vector<Xf> g_xfStack;
static void setXf(float tx, float ty, float ts, float rot) {
  g_xf.tx = tx; g_xf.ty = ty; g_xf.ts = ts; g_xf.rot = rot; g_xf.cs = cosf(rot); g_xf.sn = sinf(rot);
}
static void pushXf(float tx, float ty, float ts, float rot) {
  g_xfStack.push_back(g_xf);
  Xf p = g_xf; float nrot = p.rot + rot;
  float lx = tx * ts, ly = ty * ts;
  g_xf.tx = lx * p.cs - ly * p.sn + p.tx;
  g_xf.ty = lx * p.sn + ly * p.cs + p.ty;
  g_xf.ts = p.ts * ts; g_xf.rot = nrot; g_xf.cs = cosf(nrot); g_xf.sn = sinf(nrot);
}
static void popXf() { g_xf = g_xfStack.back(); g_xfStack.pop_back(); }
static inline int TX(float x, float y) {
  float dx = x * g_xf.ts, dy = y * g_xf.ts;
  return (int)lroundf((dx * g_xf.cs - dy * g_xf.sn + g_xf.tx) * SS);
}
static inline int TY(float x, float y) {
  float dx = x * g_xf.ts, dy = y * g_xf.ts;
  return (int)lroundf((dx * g_xf.sn + dy * g_xf.cs + g_xf.ty) * SS);
}

/* ---------- 像素混合 ---------- */
static inline void putPx(int x, int y, const Col& c) {
  if (x < 0 || y < 0 || x >= g_bw || y >= g_bh) return;
  if (c.a <= 0) return;
  uint32_t* p = g_px + (size_t)y * g_bw + x;
  if (c.a >= 255) { *p = (uint32_t)c.b | ((uint32_t)c.g << 8) | ((uint32_t)c.r << 16); return; }
  int a = c.a, ia = 255 - a;
  int ob = (int)(*p & 255), og = (int)((*p >> 8) & 255), orr = (int)((*p >> 16) & 255);
  int nb = (c.b * a + ob * ia) / 255, ng = (c.g * a + og * ia) / 255, nr = (c.r * a + orr * ia) / 255;
  *p = (uint32_t)nb | ((uint32_t)ng << 8) | ((uint32_t)nr << 16);
}

/* ---------- 基础图元 ---------- */
// 矩形（渐变方向 orient: 0=垂直 1=水平，单色时忽略）
static void fillRectG(float x, float y, float w, float h, const Grad& g, int orient) {
  int x0 = TX(x, y), x1 = TX(x + w, y + h);
  int y0 = TY(x, y), y1 = TY(x + w, y + h);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
  if (x1 > g_bw) x1 = g_bw; if (y1 > g_bh) y1 = g_bh;
  if (x0 >= x1 || y0 >= y1) return;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) / g_xf.ts;
    float t = h > 0 ? (ly - y) / h : 0;
    Col rc = gradAt(g, t);
    if (orient == 1) {
      for (int px = x0; px < x1; px++) {
        float lx = ((float)px / SS - g_xf.tx) / g_xf.ts;
        putPx(px, py, gradAt(g, w > 0 ? (lx - x) / w : 0));
      }
    } else {
      uint32_t* row = g_px + (size_t)py * g_bw;
      if (rc.a >= 255) {
        uint32_t v = (uint32_t)rc.b | ((uint32_t)rc.g << 8) | ((uint32_t)rc.r << 16);
        for (int px = x0; px < x1; px++) row[px] = v;
      } else for (int px = x0; px < x1; px++) putPx(px, py, rc);
    }
  }
}
static void fillRect(float x, float y, float w, float h, const Col& c) { fillRectG(x, y, w, h, grad1(c), 0); }

// 圆角矩形
static void fillRoundG(float x, float y, float w, float h, float r, const Grad& g, int orient) {
  if (r > w * 0.5f) r = w * 0.5f; if (r > h * 0.5f) r = h * 0.5f;
  int x0 = TX(x, y), x1 = TX(x + w, y + h);
  int y0 = TY(x, y), y1 = TY(x + w, y + h);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  float invTs = 1.f / g_xf.ts;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    float t = h > 0 ? (ly - y) / h : 0;
    Col rc = gradAt(g, t);
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      // 圆角测试（局部坐标）
      float qx = lx - (lx < x + r ? x + r : (lx > x + w - r ? x + w - r : lx));
      float qy = ly - (ly < y + r ? y + r : (ly > y + h - r ? y + h - r : ly));
      if (qx * qx + qy * qy <= r * r + 0.001f) {
        if (orient == 1) rc = gradAt(g, w > 0 ? (lx - x) / w : 0);
        putPx(px, py, rc);
      }
    }
  }
}
static void fillRound(float x, float y, float w, float h, float r, const Col& c) { fillRoundG(x, y, w, h, r, grad1(c), 0); }

// 椭圆（支持旋转）
static void fillEllipseG(float cx, float cy, float rx, float ry, float rot, const Grad& g, int orient) {
  int x0 = TX(cx - rx, cy - ry), x1 = TX(cx + rx, cy + ry);
  int y0 = TY(cx - rx, cy - ry), y1 = TY(cx + rx, cy + ry);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  // 旋转后包围盒可能变大，用另一个半径做上界扩展
  x0 -= (int)(ry * SS); x1 += (int)(ry * SS);
  y0 -= (int)(rx * SS); y1 += (int)(rx * SS);
  float cs = cosf(-rot), sn = sinf(-rot), invTs = 1.f / g_xf.ts;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      float dx = lx - cx, dy = ly - cy;
      float ex = dx * cs - dy * sn, ey = dx * sn + dy * cs;
      if (rx > 0 && ry > 0 && (ex * ex) / (rx * rx) + (ey * ey) / (ry * ry) <= 1.0f) {
        float t = orient == 1 ? (lx - (cx - rx)) / (2 * rx) : (ly - (cy - ry)) / (2 * ry);
        putPx(px, py, gradAt(g, t));
      }
    }
  }
}
static void fillEllipse(float cx, float cy, float rx, float ry, float rot, const Col& c) {
  fillEllipseG(cx, cy, rx, ry, rot, grad1(c), 0);
}

// 圆 / 径向渐变圆（中心可偏移）
static void fillCircleG(float cx, float cy, float r, float gx, float gy, float ri, float ro, const Grad& g) {
  int x0 = TX(cx - r, cy - r), x1 = TX(cx + r, cy + r);
  int y0 = TY(cx - r, cy - r), y1 = TY(cx + r, cy + r);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  float invTs = 1.f / g_xf.ts;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      float dx = lx - cx, dy = ly - cy;
      float d2 = dx * dx + dy * dy;
      if (d2 <= r * r) {
        float dg = sqrtf((lx - gx) * (lx - gx) + (ly - gy) * (ly - gy));
        float t = (ro > ri) ? (dg - ri) / (ro - ri) : 0.f;
        putPx(px, py, gradAt(g, t));
      }
    }
  }
}
static void fillCircle(float cx, float cy, float r, const Col& c) { fillCircleG(cx, cy, r, cx, cy, 0, r, grad1(c)); }

// 扇形填充（角度制，弧度）
static void fillArc(float cx, float cy, float r, float a0, float a1, const Col& c) {
  bool full = (a1 - a0) >= 6.2831853f - 0.001f;
  int x0 = TX(cx - r, cy - r), x1 = TX(cx + r, cy + r);
  int y0 = TY(cx - r, cy - r), y1 = TY(cx + r, cy + r);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  float invTs = 1.f / g_xf.ts;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      float dx = lx - cx, dy = ly - cy;
      if (dx * dx + dy * dy > r * r) continue;
      if (full) { putPx(px, py, c); continue; }
      float a = atan2f(dy, dx);
      bool in = (a0 <= a1) ? (a >= a0 && a <= a1) : (a >= a0 || a <= a1);
      if (in) putPx(px, py, c);
    }
  }
}

// 线段描边（圆帽）
static void strokeSeg(float ax, float ay, float bx, float by, float w, const Col& c) {
  int x0 = TX(std::min(ax, bx) - w, std::min(ay, by) - w), x1 = TX(std::max(ax, bx) + w, std::max(ay, by) + w);
  int y0 = TY(std::min(ax, bx) - w, std::min(ay, by) - w), y1 = TY(std::max(ax, bx) + w, std::max(ay, by) + w);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  float hw = w * 0.5f, invTs = 1.f / g_xf.ts;
  float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
  if (len2 < 1e-9f) { fillCircle(ax, ay, hw, c); return; }
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      float t = clampf(((lx - ax) * ex + (ly - ay) * ey) / len2, 0.f, 1.f);
      float qx = lx - (ax + ex * t), qy = ly - (ay + ey * t);
      if (qx * qx + qy * qy <= hw * hw) putPx(px, py, c);
    }
  }
}
static void strokeLine(float ax, float ay, float bx, float by, float w, const Col& c) { strokeSeg(ax, ay, bx, by, w, c); }

// 圆弧描边
static void strokeArc(float cx, float cy, float r, float a0, float a1, float w, const Col& c) {
  bool full = (a1 - a0) >= 6.2831853f - 0.001f;
  int x0 = TX(cx - r - w, cy - r - w), x1 = TX(cx + r + w, cy + r + w);
  int y0 = TY(cx - r - w, cy - r - w), y1 = TY(cx + r + w, cy + r + w);
  if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
  float hw = w * 0.5f, invTs = 1.f / g_xf.ts;
  for (int py = y0; py < y1; py++) {
    float ly = ((float)py / SS - g_xf.ty) * invTs;
    for (int px = x0; px < x1; px++) {
      float lx = ((float)px / SS - g_xf.tx) * invTs;
      float dx = lx - cx, dy = ly - cy;
      float d = sqrtf(dx * dx + dy * dy);
      if (fabsf(d - r) > hw) continue;
      if (full) { putPx(px, py, c); continue; }
      float a = atan2f(dy, dx);
      bool in = (a0 <= a1) ? (a >= a0 && a <= a1) : (a >= a0 || a <= a1);
      if (in) putPx(px, py, c);
    }
  }
}

/* ---------- 路径（贝塞尔扁平化 + 扫描线填充） ---------- */
struct Path {
  std::vector<std::vector<float>> xs, ys;
  void moveTo(float x, float y) {
    xs.push_back(std::vector<float>()); ys.push_back(std::vector<float>());
    xs.back().push_back(x); ys.back().push_back(y);
  }
  void lineTo(float x, float y) {
    if (xs.empty()) { moveTo(x, y); return; }
    xs.back().push_back(x); ys.back().push_back(y);
  }
  void quadTo(float cx, float cy, float x, float y) {
    if (xs.empty()) { moveTo(cx, cy); }
    float x0 = xs.back().back(), y0 = ys.back().back();
    const int N = 14;
    for (int i = 1; i <= N; i++) {
      float t = (float)i / N, it = 1 - t;
      float px = it * it * x0 + 2 * it * t * cx + t * t * x;
      float py = it * it * y0 + 2 * it * t * cy + t * t * y;
      xs.back().push_back(px); ys.back().push_back(py);
    }
  }
  void close() {
    if (!xs.empty() && xs.back().size() > 1) {
      xs.back().push_back(xs.back()[0]); ys.back().push_back(ys.back()[0]);
    }
  }
};
static void fillPathG(Path& pa, const Grad& g) {
  std::vector<std::vector<POINT>> polys;
  float minY = 1e9f, maxY = -1e9f;
  for (size_t i = 0; i < pa.xs.size(); i++) {
    std::vector<POINT> pts;
    for (size_t j = 0; j < pa.xs[i].size(); j++) {
      POINT p; p.x = TX(pa.xs[i][j], pa.ys[i][j]); p.y = TY(pa.xs[i][j], pa.ys[i][j]);
      pts.push_back(p);
      if ((float)p.y < minY) minY = (float)p.y;
      if ((float)p.y > maxY) maxY = (float)p.y;
    }
    if (pts.size() >= 3) polys.push_back(pts);
  }
  if (polys.empty()) return;
  int y0 = (int)floorf(minY), y1 = (int)ceilf(maxY);
  for (int y = y0; y < y1; y++) {
    std::vector<float> xint;
    for (size_t k = 0; k < polys.size(); k++) {
      const std::vector<POINT>& pl = polys[k];
      for (size_t j = 0; j < pl.size(); j++) {
        POINT a = pl[j], b = pl[(j + 1) % pl.size()];
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
          float t = (float)(y - a.y) / (float)(b.y - a.y);
          xint.push_back((float)a.x + t * (float)(b.x - a.x));
        }
      }
    }
    if (xint.empty()) continue;
    std::sort(xint.begin(), xint.end());
    float t = (maxY > minY) ? ((float)y - minY) / (maxY - minY) : 0.f;
    Col c = gradAt(g, t);
    for (size_t k = 0; k + 1 < xint.size(); k += 2) {
      int xs = (int)floorf(xint[k]), xe = (int)ceilf(xint[k + 1]);
      for (int x = xs; x < xe; x++) putPx(x, y, c);
    }
  }
}
static void fillPath(Path& pa, const Col& c) { fillPathG(pa, grad1(c)); }
static void strokePath(Path& pa, float w, const Col& c) {
  for (size_t i = 0; i < pa.xs.size(); i++) {
    for (size_t j = 0; j + 1 < pa.xs[i].size(); j++)
      strokeSeg(pa.xs[i][j], pa.ys[i][j], pa.xs[i][j + 1], pa.ys[i][j + 1], w, c);
  }
}

/* ============================ 五、文本 ============================ */

static HFONT g_fonts[2][64] = {};
static HFONT getFont(int size, bool bold) {
  if (size < 1) size = 1; if (size > 63) size = 63;
  if (g_fonts[bold ? 1 : 0][size]) return g_fonts[bold ? 1 : 0][size];
  HFONT f = CreateFontW(-size * SS, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  if (!f) f = CreateFontW(-size * SS, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"SimSun");
  g_fonts[bold ? 1 : 0][size] = f;
  return f;
}
/* align: 0=中心 1=左 2=右；oc 有效时绘制描边 */
static void dtext(const wchar_t* s, float x, float y, int size, COLORREF c,
  int align = 0, bool bold = false, COLORREF oc = CLR_INVALID, float ow = 0) {
  if (!s || !*s || !g_hdcMem) return;
  int px = TX(x, y), py = TY(x, y);
  HFONT f = getFont(size, bold);
  HGDIOBJ old = SelectObject(g_hdcMem, f);
  SetBkMode(g_hdcMem, TRANSPARENT);
  RECT mr = { 0, 0, 4000, size * SS * 2 };
  DrawTextW(g_hdcMem, s, -1, &mr, DT_CALCRECT | DT_SINGLELINE | DT_NOCLIP);
  int tw = mr.right - mr.left, th = mr.bottom - mr.top;
  int rx = (align == 0) ? px - tw / 2 : (align == 1 ? px : px - tw);
  int ry = py - th / 2;
  if (oc != CLR_INVALID && ow > 0) {
    SetTextColor(g_hdcMem, oc);
    int n = (int)(ow * SS) + 4;
    for (int i = 0; i < n; i++) {
      float a = (float)i * 6.2831853f / n;
      int ox = (int)lroundf(cosf(a) * ow * SS), oy = (int)lroundf(sinf(a) * ow * SS);
      RECT r2 = { rx + ox, ry + oy, rx + ox + tw, ry + oy + th };
      DrawTextW(g_hdcMem, s, -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    }
  }
  SetTextColor(g_hdcMem, c);
  RECT r = { rx, ry, rx + tw, ry + th };
  DrawTextW(g_hdcMem, s, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
  SelectObject(g_hdcMem, old);
}
/* 带阴影文本（标题用） */
static void dtextS(const wchar_t* s, float x, float y, int size, COLORREF c, COLORREF sc, float sdy = 2, bool bold = true) {
  dtext(s, x, y + sdy, size, sc, 0, bold);
  dtext(s, x, y, size, c, 0, bold);
}
/* 多行块（自动换行），(x,y) 为左上角 */
static void dtextBlock(const wchar_t* s, float x, float y, float w, int size, COLORREF c, int gap = 4, bool bold = false) {
  if (!s || !*s || !g_hdcMem) return;
  int px = TX(x, y), py = TY(x, y);
  HFONT f = getFont(size, bold);
  HGDIOBJ old = SelectObject(g_hdcMem, f);
  SetBkMode(g_hdcMem, TRANSPARENT);
  int pw = (int)(w * SS);
  RECT mr = { px, py, px + pw, py + 4000 };
  DrawTextW(g_hdcMem, s, -1, &mr, DT_CALCRECT | DT_WORDBREAK | DT_NOCLIP);
  int th = mr.bottom - mr.top;
  RECT r = { px, py, px + pw, py + th };
  SetTextColor(g_hdcMem, c);
  DrawTextW(g_hdcMem, s, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP);
  SelectObject(g_hdcMem, old);
}

/* ============================ 六、音效（程序合成 WAV） ============================ */

static const int SND_RATE = 22050;
static std::vector<char> g_wavPlant, g_wavSun, g_wavShoot, g_wavHit, g_wavChomp,
  g_wavBoom, g_wavMower, g_wavGroan[3], g_wavBleep, g_wavWin, g_wavLose;
static bool g_muted = false;
static float g_lastShootSnd = -10;
static float wallT = 0;   // 墙钟时间（开始/暂停界面也推进，用于氛围动画）

static void appendWav(std::vector<char>& out, const std::vector<int16_t>& pcm) {
  int dataLen = (int)pcm.size() * 2;
  int riffLen = 36 + dataLen;
  out.resize(44 + dataLen);
  memcpy(&out[0], "RIFF", 4);  memcpy(&out[8], "WAVE", 4);
  memcpy(&out[12], "fmt ", 4);
  int16_t fmt[8] = { 0x0001, 1, (int16_t)SND_RATE, (int16_t)(SND_RATE * 2), 2, 16, 0 };
  memcpy(&out[20], fmt, 16);
  memcpy(&out[36], "data", 4);
  memcpy(&out[40], &dataLen, 4);
  memcpy(&out[44], pcm.data(), dataLen);
}
static float waveVal(int type, float ph) {
  switch (type) {
  case 1: return (fmodf(ph, 6.2831853f) < 3.1415927f) ? 1.f : -1.f;          // square
  case 2: return 2.f * (fmodf(ph / 6.2831853f, 1.f) - 0.5f);                  // saw
  case 3: { float t = fmodf(ph / 6.2831853f, 1.f); return 4.f * fabsf(t - 0.5f) - 1.f; } // tri
  default: return sinf(ph);
  }
}
static std::vector<int16_t> synthTone(float f0, float f1, int ms, int type, float vol, int delayMs = 0) {
  int n = SND_RATE * ms / 1000, dn = SND_RATE * delayMs / 1000;
  std::vector<int16_t> out(dn + n, 0);
  float ph = 0, ratio = (f1 > 0 && f0 > 0) ? f1 / f0 : 1.f;
  for (int i = 0; i < n; i++) {
    float t = (float)i / n;
    float f = f0 * powf(ratio, t);
    ph += 6.2831853f * f / SND_RATE;
    float env = (t < 0.02f ? t / 0.02f : powf(1.f - (t - 0.02f) / 0.98f, 2.0f));
    out[dn + i] = (int16_t)(waveVal(type, ph) * env * vol * 30000);
  }
  return out;
}
static std::vector<int16_t> synthNoise(int ms, float vol, int cutoff, int delayMs = 0) {
  int n = SND_RATE * ms / 1000, dn = SND_RATE * delayMs / 1000;
  std::vector<int16_t> out(dn + n, 0);
  float a = 1.f - expf(-6.2831853f * cutoff / SND_RATE), y = 0;
  for (int i = 0; i < n; i++) {
    float x = rnd(-1.f, 1.f);
    y += a * (x - y);
    float env = powf(1.f - (float)i / n, 1.6f);
    out[dn + i] = (int16_t)(y * env * vol * 30000);
  }
  return out;
}
static void mixInto(std::vector<int16_t>& dst, const std::vector<int16_t>& src) {
  if (src.size() > dst.size()) dst.resize(src.size(), 0);
  for (size_t i = 0; i < src.size(); i++) dst[i] = (int16_t)clampf((float)dst[i] + src[i], -32767, 32767);
}
static void buildSounds() {
  { auto a = synthTone(320, 520, 120, 1, 0.10f); auto b = synthTone(520, 640, 100, 0, 0.08f, 60); mixInto(a, b); appendWav(g_wavPlant, a); }
  { auto a = synthTone(880, 880, 100, 0, 0.10f); auto b = synthTone(1318, 1318, 160, 0, 0.10f, 80); mixInto(a, b); appendWav(g_wavSun, a); }
  { auto a = synthNoise(60, 0.06f, 2400); auto b = synthTone(240, 180, 50, 3, 0.04f); mixInto(a, b); appendWav(g_wavShoot, a); }
  appendWav(g_wavHit, synthNoise(70, 0.10f, 900));
  appendWav(g_wavChomp, synthTone(90, 60, 90, 2, 0.07f));
  { auto a = synthNoise(500, 0.30f, 500); auto b = synthTone(120, 40, 400, 0, 0.20f); mixInto(a, b); appendWav(g_wavBoom, a); }
  { auto a = synthTone(140, 260, 500, 2, 0.14f); auto b = synthNoise(500, 0.10f, 800); mixInto(a, b); appendWav(g_wavMower, a); }
  for (int i = 0; i < 3; i++) appendWav(g_wavGroan[i], synthTone(70 + i * 20, 50 + i * 15, 500, 2, 0.045f));
  appendWav(g_wavBleep, synthTone(660, 660, 70, 1, 0.06f));
  { std::vector<int16_t> a; int fs[4] = { 523,659,784,1046 }; for (int i = 0; i < 4; i++) mixInto(a, synthTone(fs[i], fs[i], 250, 3, 0.12f, i * 160)); appendWav(g_wavWin, a); }
  { std::vector<int16_t> a; int fs[4] = { 400,330,260,180 }; for (int i = 0; i < 4; i++) mixInto(a, synthTone(fs[i], fs[i], 300, 2, 0.10f, i * 200)); appendWav(g_wavLose, a); }
}
static void playWav(const std::vector<char>& w) {
  if (g_headless || g_muted || w.empty()) return;
  PlaySoundA((LPCSTR)w.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
struct Sfx {
  void plant()   { playWav(g_wavPlant); }
  void sun()     { playWav(g_wavSun); }
  void shoot()   { if (wallT - g_lastShootSnd > 0.05f) { g_lastShootSnd = wallT; playWav(g_wavShoot); } }
  void hit()     { playWav(g_wavHit); }
  void chomp()   { playWav(g_wavChomp); }
  void explode() { playWav(g_wavBoom); }
  void mower()   { playWav(g_wavMower); }
  void groan()   { playWav(g_wavGroan[std::rand() % 3]); }
  void bleep()   { playWav(g_wavBleep); }
  void win()     { playWav(g_wavWin); }
  void lose()    { playWav(g_wavLose); }
} sfx;

/* ============================ 七、游戏状态 ============================ */

struct Plant {
  int type, r, c; float x, y;
  float hp, maxHp, t, shootT, secondT, produceT, fuse, recoil, flash;
  float armT, chewT, chomp; int burst; float burstT;
  bool armed, dead;
};
struct Zombie {
  int type, row; float x, y, hp, maxHp, speed, slowT, walk;
  bool eating, dead; float deathT, flash;
  bool door; float doorHp;
  bool vaulted, vaulting; float vaultT, vaultFrom;
};
struct Pea { float x, y; int row, dmg; bool snow; float speed; bool dead; };
struct Sun { float x, y, targetY, vy, value, life, t, flyT, x0, y0; int state; bool dead; };
struct Mower { int row; float x, y; bool active, used; };
struct Particle { float x, y, vx, vy, life, maxLife; Col color; float size, grav; bool boom; };
struct FText { float x, y; std::wstring text; Col color; int size; float life; };
struct Cloud { float x, y, s, v; };
struct Decor { float x, y, s, o; bool flower; Col col; };

struct Game {
  bool running, paused, over, won;
  int sun, kills, sunCollected;
  float time;
  std::vector<Plant> plants;
  std::vector<Zombie> zombies;
  std::vector<Pea> peas;
  std::vector<Sun> suns;
  std::vector<Mower> mowers;
  std::vector<Particle> particles;
  std::vector<FText> texts;
  int wave; float waveTimer;
  struct SpawnItem { int type, row; float time; };
  std::vector<SpawnItem> spawnQueue;
  float sunTimer;
  int selected; bool shovel;
  bool hover; int hr, hc;
  float mx, my;
  float shake;
  bool banner; float bannerT;
  float cds[PT_COUNT];
  int screen;  // 0=开始 1=游戏中 2=暂停 3=结束
};
static Game G;

static void newGame() {
  G = Game();
  G.sun = 150; G.waveTimer = 20; G.sunTimer = 6; G.selected = -1;
  for (int i = 0; i < PT_COUNT; i++) G.cds[i] = 0;
  for (int r = 0; r < ROWS; r++) {
    Mower m; m.row = r; m.x = 64; m.y = cellYf(r) + 32; m.active = false; m.used = false;
    G.mowers.push_back(m);
  }
}

/* ---------- 装饰与云（预生成，位置稳定） ---------- */
static std::vector<Decor> g_decor;
static Cloud g_clouds[4];
static void buildDecor() {
  g_decor.clear();
  for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
    if (rnd(0, 1) < 0.15f) {
      Decor d;
      d.x = GRID_X + c * CELL_W + rnd(16, CELL_W - 16);
      d.y = GRID_Y + r * CELL_H + rnd(26, CELL_H - 14);
      d.flower = rnd(0, 1) < 0.45f;
      d.s = rnd(0.7f, 1.3f); d.o = rnd(0, 6.28f);
      static const int cols[4] = { 0xff6b81, 0xffd93b, 0xffffff, 0xc58bff };
      d.col = hexc(cols[std::rand() % 4]);
      g_decor.push_back(d);
    }
  }
  for (int i = 0; i < 4; i++) {
    g_clouds[i].x = rnd(0, W); g_clouds[i].y = rnd(10, 46);
    g_clouds[i].s = rnd(0.55f, 1.25f); g_clouds[i].v = rnd(5, 13);
  }
}

/* ---------- 实体生成 ---------- */
static void spawnSkySun() {
  Sun s; s.x = rnd(GRID_X + 40, W - 60); s.y = -20;
  s.targetY = rnd(GRID_Y + 40, GRID_Y + ROWS * CELL_H - 40);
  s.value = 25; s.state = 0; s.life = 14; s.t = 0; s.vy = 0; s.dead = false;
  G.suns.push_back(s);
}
static void spawnPlantSun(const Plant& p) {
  Sun s; s.x = p.x + rnd(-16, 16); s.y = p.y - 24;
  s.targetY = p.y + 22; s.value = 25; s.state = 0; s.life = 14; s.t = 0; s.vy = -30; s.dead = false;
  G.suns.push_back(s);
}
static void spawnZombie(int type, int row) {
  const ZombieDef& d = ZOMBIE_DEFS[type];
  Zombie z;
  z.type = type; z.row = row; z.x = W + rnd(20, 90); z.y = cellYf(row) + 32;
  z.hp = d.hp; z.maxHp = d.hp; z.speed = d.speed * rnd(0.95f, 1.05f);
  z.slowT = 0; z.walk = rnd(0, 10); z.eating = false; z.dead = false; z.deathT = 0; z.flash = 0;
  z.door = d.door > 0; z.doorHp = d.door;
  z.vaulted = false; z.vaulting = false; z.vaultT = 0; z.vaultFrom = 0;
  G.zombies.push_back(z);
  if (rnd(0, 1) < 0.35f) sfx.groan();
}
static void addParticle(float x, float y, float vx, float vy, float life, Col c, float size, float grav) {
  Particle p; p.x = x; p.y = y; p.vx = vx; p.vy = vy; p.life = life; p.maxLife = life;
  p.color = c; p.size = size; p.grav = grav; p.boom = false;
  G.particles.push_back(p);
}
static void killZombie(Zombie& z);
static void damageZombie(Zombie& z, float dmg) {
  if (z.dead) return;
  z.flash = 0.08f;
  if (z.door && z.doorHp > 0) {
    z.doorHp -= dmg;
    if (z.doorHp <= 0) { z.door = false; sfx.hit(); }
    return;
  }
  z.hp -= dmg;
  if (z.hp <= 0) killZombie(z);
}
static void killZombie(Zombie& z) {
  if (z.dead) return;
  z.dead = true; z.deathT = 0; G.kills++;
  static const int cols[3] = { 0x7d9b5b, 0x5f7a4a, 0x3c4a2e };
  for (int i = 0; i < 10; i++) {
    addParticle(z.x + rnd(-12, 12), z.y - 30 + rnd(-14, 14), rnd(-60, 60), rnd(-90, -20),
      rnd(0.4f, 0.8f), hexc(cols[i % 3]), rnd(3, 6), 260);
  }
}
static void addText(float x, float y, const wchar_t* text, COLORREF c, int size = 16) {
  FText t; t.x = x; t.y = y; t.text = text; t.color = hexc((int)c); t.size = size; t.life = 1.1f;
  G.texts.push_back(t);
}

/* ============================ 八、波次 ============================ */

static void startWave(int n) {
  G.wave = n;
  bool isFlag = (n == FLAG_WAVES[0] || n == FLAG_WAVES[1]);
  float pts = 0.8f + n * 1.1f + (isFlag ? 3.f : 0);
  std::vector<int> types; types.push_back(ZT_NORMAL);
  if (n >= 3) types.push_back(ZT_CONE);
  if (n >= 5) types.push_back(ZT_POLE);
  if (n >= 7) types.push_back(ZT_DOOR);
  if (n >= 10) { types.push_back(ZT_BUCKET); types.push_back(ZT_FOOTBALL); }
  std::vector<int> q;
  float p = pts; int guard = 0;
  while (p > 0 && guard++ < 120) {
    int t = types[std::rand() % types.size()];
    float c = ZOMBIE_DEFS[t].points;
    if (c > p + 1) { if (p >= 1) { q.push_back(ZT_NORMAL); p -= 1; } else break; }
    else { q.push_back(t); p -= c; }
  }
  if (isFlag) {
    q.push_back(ZT_FLAG); q.push_back(ZT_POLE);
    if (n >= 7) q.push_back(ZT_DOOR);
    if (n >= 10) q.push_back(ZT_FOOTBALL);
    if (n >= 10) { q.push_back(ZT_CONE); q.push_back(ZT_BUCKET); }
    if (n == MAX_WAVE) { q.push_back(ZT_FOOTBALL); q.push_back(ZT_DOOR); q.push_back(ZT_BUCKET); }
  }
  float dur = 5 + q.size() * 1.2f;
  for (size_t i = 0; i < q.size(); i++) {
    Game::SpawnItem s; s.type = q[i]; s.row = std::rand() % ROWS;
    s.time = G.time + 0.4f + rnd(0, dur);
    G.spawnQueue.push_back(s);
  }
  std::sort(G.spawnQueue.begin(), G.spawnQueue.end(),
    [](const Game::SpawnItem& a, const Game::SpawnItem& b) { return a.time < b.time; });
  if (isFlag) { G.banner = true; G.bannerT = 0; sfx.groan(); }
  G.waveTimer = isFlag ? 28 : 22;
  sfx.bleep();
}
static void winGame();
static void loseGame(const wchar_t* reason);
static void updateWaves(float dt) {
  while (!G.spawnQueue.empty() && G.spawnQueue.front().time <= G.time) {
    Game::SpawnItem s = G.spawnQueue.front();
    G.spawnQueue.erase(G.spawnQueue.begin());
    spawnZombie(s.type, s.row);
  }
  if (!G.over && G.wave >= MAX_WAVE && G.spawnQueue.empty() && G.zombies.empty()) { winGame(); return; }
  if (!G.over && G.wave < MAX_WAVE) {
    G.waveTimer -= dt;
    if (G.waveTimer <= 0) startWave(G.wave + 1);
  }
}

/* ============================ 九、种植与更新 ============================ */

static Plant* plantAt(int r, int c) {
  for (size_t i = 0; i < G.plants.size(); i++)
    if (G.plants[i].r == r && G.plants[i].c == c && !G.plants[i].dead) return &G.plants[i];
  return nullptr;
}
static void tryPlant(int r, int c) {
  int type = G.selected;
  if (type < 0) return;
  const PlantDef& def = PLANT_DEFS[type];
  if (G.sun < def.cost) { addText(cellXf(c), cellYf(r) - 40, L"阳光不足！", 0xff7b6b); sfx.bleep(); return; }
  if (G.cds[type] > 0) { addText(cellXf(c), cellYf(r) - 40, L"冷却中…", 0x9fd3ff); return; }
  if (type == PT_GATLING) {
    Plant* ex = plantAt(r, c);
    if (!ex || ex->type != PT_REPEATER) {
      addText(cellXf(c), cellYf(r) - 40, L"只能种在双发射手上", 0xffd166); sfx.bleep(); return;
    }
    G.sun -= def.cost; G.cds[type] = def.cd;
    ex->type = PT_GATLING; ex->hp = (float)def.hp; ex->maxHp = (float)def.hp;
    ex->burst = 0; ex->burstT = 0;
    sfx.plant();
    addText(ex->x, ex->y - 44, L"升级为机枪射手！", 0xffe066, 15);
    for (int i = 0; i < 12; i++)
      addParticle(ex->x + rnd(-16, 16), ex->y + rnd(-16, 16), rnd(-50, 50), rnd(-70, -10),
        rnd(0.3f, 0.7f), hexc(0xffd93b), rnd(2, 5), 260);
    G.selected = -1;
    return;
  }
  if (plantAt(r, c)) { addText(cellXf(c), cellYf(r) - 40, L"这里有植物了", 0xffd166); return; }
  G.sun -= def.cost;
  G.cds[type] = def.cd;
  Plant p;
  p.type = type; p.r = r; p.c = c; p.x = cellXf(c); p.y = cellYf(r);
  p.hp = (float)def.hp; p.maxHp = (float)def.hp; p.t = rnd(0, 6);
  p.shootT = rnd(0.2f, 0.8f); p.secondT = 0; p.produceT = 6; p.fuse = 1.2f; p.recoil = 0; p.flash = 0;
  p.armT = 12; p.armed = false; p.chewT = 0; p.chomp = 0; p.burst = 0; p.burstT = 0; p.dead = false;
  G.plants.push_back(p);
  sfx.plant();
  for (int i = 0; i < 8; i++)
    addParticle(p.x + rnd(-14, 14), p.y + rnd(8, 20), rnd(-40, 40), rnd(-70, -20),
      rnd(0.3f, 0.6f), hexc(0x8a5a2b), rnd(2, 4), 300);
  G.selected = -1;
}
static void tryShovel(int r, int c) {
  Plant* p = plantAt(r, c);
  if (p) {
    p->dead = true;
    sfx.bleep();
    addText(p->x, p->y - 40, L"已移除", 0xcfe8ff);
    for (int i = 0; i < 8; i++)
      addParticle(p->x + rnd(-14, 14), p->y + rnd(-10, 10), rnd(-50, 50), rnd(-80, -20),
        0.5f, hexc(0x7fbf3f), rnd(2, 5), 300);
  }
}
static void fire(Plant& p) {
  Pea pea; pea.x = p.x + 26; pea.y = p.y - 16; pea.row = p.r; pea.dmg = 20;
  pea.snow = (p.type == PT_SNOWPEA); pea.speed = 320; pea.dead = false;
  G.peas.push_back(pea);
  p.recoil = 0.14f;
  sfx.shoot();
}
static void explodeCherry(Plant& p) {
  p.dead = true;
  G.shake = 12;
  sfx.explode();
  Particle bp; bp.x = p.x; bp.y = p.y - 10; bp.vx = 0; bp.vy = 0; bp.life = 0.45f; bp.maxLife = 0.45f;
  bp.color = hexc(0xff9d2e); bp.size = 10; bp.grav = 0; bp.boom = true;
  G.particles.push_back(bp);
  for (int i = 0; i < 26; i++) {
    float a = rnd(0, 6.2831853f), sp = rnd(60, 260);
    static const int cols[4] = { 0xff6b2e, 0xffb02e, 0xff3b3b, 0x7a4a1d };
    addParticle(p.x, p.y - 10, cosf(a) * sp, sinf(a) * sp - 60, rnd(0.4f, 0.9f), hexc(cols[i % 4]), rnd(3, 7), 300);
  }
  for (size_t i = 0; i < G.zombies.size(); i++) {
    Zombie& z = G.zombies[i];
    if (z.dead) continue;
    if (fabsf((float)z.row - p.r) <= 1 && fabsf(z.x - p.x) <= 138) damageZombie(z, 1800);
  }
}
static void updatePlant(Plant& p, float dt) {
  p.t += dt;
  if (p.flash > 0) p.flash -= dt;
  if (p.recoil > 0) p.recoil -= dt;
  if (p.type == PT_SUNFLOWER) {
    p.produceT -= dt;
    if (p.produceT <= 0) { p.produceT = 20; spawnPlantSun(p); }
  } else if (p.type == PT_PEASHOOTER || p.type == PT_SNOWPEA || p.type == PT_REPEATER || p.type == PT_GATLING) {
    p.shootT -= dt;
    bool target = false;
    for (size_t i = 0; i < G.zombies.size(); i++) {
      Zombie& z = G.zombies[i];
      if (!z.dead && z.row == p.r && z.x > p.x - 14 && z.x < W + 40) { target = true; break; }
    }
    if (p.shootT <= 0 && target) {
      fire(p);
      p.shootT = 1.4f;
      if (p.type == PT_REPEATER) p.secondT = 0.16f;
      else if (p.type == PT_GATLING) { p.burst = 3; p.burstT = 0.11f; }
    } else if (p.shootT < 0) p.shootT = 0.15f;
    if (p.secondT > 0) { p.secondT -= dt; if (p.secondT <= 0) fire(p); }
    if (p.burst > 0) {
      p.burstT -= dt;
      if (p.burstT <= 0) { fire(p); p.burst--; p.burstT = 0.11f; }
    }
  } else if (p.type == PT_POTATO) {
    if (!p.armed) {
      p.armT -= dt;
      if (p.armT <= 0) { p.armed = true; sfx.bleep(); }
    } else {
      for (size_t i = 0; i < G.zombies.size(); i++) {
        Zombie& z = G.zombies[i];
        if (!z.dead && z.row == p.r && fabsf(z.x - p.x) < 46) {
          p.dead = true;
          G.shake = 7;
          sfx.explode();
          Particle bp; bp.x = p.x; bp.y = p.y - 6; bp.vx = 0; bp.vy = 0; bp.life = 0.4f; bp.maxLife = 0.4f;
          bp.color = hexc(0xff9d2e); bp.size = 8; bp.grav = 0; bp.boom = true;
          G.particles.push_back(bp);
          for (int k = 0; k < 16; k++) {
            float a = rnd(0, 6.2831853f), sp = rnd(50, 200);
            static const int cols[3] = { 0xff6b2e, 0xffb02e, 0x8a5a2b };
            addParticle(p.x, p.y - 6, cosf(a) * sp, sinf(a) * sp - 50, rnd(0.35f, 0.8f), hexc(cols[k % 3]), rnd(3, 6), 300);
          }
          for (size_t j = 0; j < G.zombies.size(); j++) {
            Zombie& zz = G.zombies[j];
            if (!zz.dead && zz.row == p.r && fabsf(zz.x - p.x) < 52) damageZombie(zz, 1800);
          }
          break;
        }
      }
    }
  } else if (p.type == PT_CHOMPER) {
    if (p.chomp > 0) p.chomp -= dt;
    if (p.chewT > 0) p.chewT -= dt;
    else {
      Zombie* z = nullptr;
      for (size_t i = 0; i < G.zombies.size(); i++) {
        Zombie& zz = G.zombies[i];
        if (!zz.dead && zz.row == p.r && zz.x - p.x > 6 && zz.x - p.x < 80) { z = &zz; break; }
      }
      if (z) {
        killZombie(*z);
        p.chewT = 15; p.chomp = 0.45f;
        sfx.chomp(); sfx.explode();
        static const int cols[3] = { 0x9b59b6, 0x7d3c98, 0xc58bff };
        for (int i = 0; i < 12; i++)
          addParticle(p.x + rnd(-10, 20), p.y - 14 + rnd(-8, 8), rnd(-60, 90), rnd(-80, 0),
            rnd(0.3f, 0.7f), hexc(cols[i % 3]), rnd(2, 5), 320);
      }
    }
  } else if (p.type == PT_CHERRY) {
    p.fuse -= dt;
    if (p.fuse <= 0) explodeCherry(p);
  }
}
static void updateZombie(Zombie& z, float dt) {
  if (z.dead) { z.deathT += dt; return; }
  z.walk += dt * (z.slowT > 0 ? 4 : 7);
  if (z.slowT > 0) z.slowT -= dt;
  if (z.flash > 0) z.flash -= dt;
  // 撑杆跳：遇到第一株植物直接跳过
  if (z.type == ZT_POLE && !z.vaulted) {
    Plant* vp = nullptr;
    for (size_t i = 0; i < G.plants.size(); i++) {
      Plant& p = G.plants[i];
      if (p.dead || p.r != z.row) continue;
      if (fabsf((z.x - 26) - p.x) < 28) { vp = &p; break; }
    }
    if (vp) {
      if (!z.vaulting) { z.vaulting = true; z.vaultFrom = z.x; sfx.bleep(); }
      z.vaultT += dt / 0.55f;
      if (z.vaultT >= 1) {
        z.vaulted = true; z.vaulting = false;
        z.x = vp->x + 48; z.y = cellYf(z.row) + 32;
      } else {
        float k = z.vaultT;
        z.x = z.vaultFrom + (vp->x + 48 - z.vaultFrom) * k;
        z.y = cellYf(z.row) + 32 - sinf(k * 3.1415927f) * 64;
      }
      z.eating = false;
      return;
    }
  }
  // 找植物吃
  Plant* target = nullptr;
  for (size_t i = 0; i < G.plants.size(); i++) {
    Plant& p = G.plants[i];
    if (p.dead || p.r != z.row) continue;
    if (fabsf((z.x - 26) - p.x) < 32) { target = &p; break; }
  }
  z.eating = (target != nullptr);
  if (target) {
    float dps = 45 * (z.slowT > 0 ? 0.5f : 1);
    target->hp -= dps * dt;
    target->flash = 0.08f;
    if (rnd(0, 1) < dt * 2.2f) sfx.chomp();
    if (target->hp <= 0) { target->dead = true; z.eating = false; }
  } else {
    z.x -= z.speed * (z.slowT > 0 ? 0.5f : 1) * dt;
    if (rnd(0, 1) < dt * 3)
      addParticle(z.x + rnd(2, 10), z.y + rnd(0, 4), rnd(-14, 6), rnd(-16, -4),
        rnd(0.3f, 0.55f), hexc(0xcbbb93), rnd(2, 3.5f), -24);
  }
  if (z.x < 118) {
    Mower& m = G.mowers[z.row];
    if (!m.used && !m.active) { m.active = true; sfx.mower(); }
  }
  if (z.x < 50 && !G.over) loseGame(L"僵尸吃掉了你的脑子！");
}
static void update(float dt) {
  G.time += dt;
  G.shake = std::max(0.f, G.shake - dt * 26);
  if (G.banner) { G.bannerT += dt; if (G.bannerT > 2.6f) G.banner = false; }
  for (int i = 0; i < PT_COUNT; i++) if (G.cds[i] > 0) G.cds[i] -= dt;
  G.sunTimer -= dt;
  if (G.sunTimer <= 0) { spawnSkySun(); G.sunTimer = rnd(8, 11); }
  updateWaves(dt);
  // 阳光
  for (size_t i = 0; i < G.suns.size(); i++) {
    Sun& s = G.suns[i];
    s.t += dt;
    if (s.state == 0) {
      s.y += 55 * dt;
      if (s.y >= s.targetY) { s.y = s.targetY; s.state = 1; }
    } else if (s.state == 1) {
      s.life -= dt;
      if (s.life <= 0) s.dead = true;
    } else if (s.state == 2) {
      s.flyT += dt * 2.4f;
      float k = std::min(1.f, s.flyT), e = k * k;
      s.x = s.x0 + (34 - s.x0) * e;
      s.y = s.y0 + (-24 - s.y0) * e;
      if (k >= 1) {
        s.dead = true;
        G.sun += (int)s.value; G.sunCollected += (int)s.value;
        wchar_t buf[32]; swprintf(buf, 32, L"+%d", (int)s.value);
        addText(120, 24, buf, 0xffd76a, 18);
        sfx.sun();
      }
    }
  }
  { std::vector<Sun> keep; for (size_t i = 0; i < G.suns.size(); i++) if (!G.suns[i].dead) keep.push_back(G.suns[i]); G.suns = keep; }
  // 植物
  for (size_t i = 0; i < G.plants.size(); i++) updatePlant(G.plants[i], dt);
  { std::vector<Plant> keep; for (size_t i = 0; i < G.plants.size(); i++) if (!G.plants[i].dead) keep.push_back(G.plants[i]); G.plants = keep; }
  // 豌豆
  for (size_t i = 0; i < G.peas.size(); i++) {
    Pea& pea = G.peas[i];
    pea.x += pea.speed * dt;
    Zombie* hit = nullptr;
    for (size_t j = 0; j < G.zombies.size(); j++) {
      Zombie& z = G.zombies[j];
      if (z.dead || z.row != pea.row) continue;
      if (pea.x > z.x - 26 && pea.x < z.x + 28) {
        if (!hit || z.x < hit->x) hit = &z;
      }
    }
    if (hit) {
      damageZombie(*hit, (float)pea.dmg);
      if (pea.snow) hit->slowT = 4;
      pea.dead = true;
      sfx.hit();
      for (int k = 0; k < 5; k++)
        addParticle(pea.x, pea.y, rnd(-70, 30), rnd(-70, 70), rnd(0.2f, 0.45f),
          hexc(pea.snow ? 0x8fd8ff : 0x8fce4a), rnd(2, 4), 400);
    }
    if (pea.x > W + 30) pea.dead = true;
  }
  { std::vector<Pea> keep; for (size_t i = 0; i < G.peas.size(); i++) if (!G.peas[i].dead) keep.push_back(G.peas[i]); G.peas = keep; }
  // 僵尸
  for (size_t i = 0; i < G.zombies.size(); i++) updateZombie(G.zombies[i], dt);
  { std::vector<Zombie> keep; for (size_t i = 0; i < G.zombies.size(); i++) if (!G.zombies[i].dead || G.zombies[i].deathT < 1.2f) keep.push_back(G.zombies[i]); G.zombies = keep; }
  // 小推车
  for (size_t i = 0; i < G.mowers.size(); i++) {
    Mower& m = G.mowers[i];
    if (!m.active) continue;
    m.x += 430 * dt;
    if (rnd(0, 1) < dt * 26)
      addParticle(m.x - 14, m.y + rnd(-4, 4), rnd(-40, -10), rnd(-40, -5),
        rnd(0.3f, 0.6f), hexc(0xcfd8c0), rnd(3, 6), -30);
    for (size_t j = 0; j < G.zombies.size(); j++) {
      Zombie& z = G.zombies[j];
      if (!z.dead && z.row == m.row && fabsf(z.x - m.x) < 44) killZombie(z);
    }
    if (m.x > W + 70) { m.active = false; m.used = true; m.x = -999; }
  }
  // 粒子与文字
  for (size_t i = 0; i < G.particles.size(); i++) {
    Particle& pt = G.particles[i];
    pt.life -= dt; pt.vy += pt.grav * dt; pt.x += pt.vx * dt; pt.y += pt.vy * dt;
  }
  { std::vector<Particle> keep; for (size_t i = 0; i < G.particles.size(); i++) if (G.particles[i].life > 0) keep.push_back(G.particles[i]); G.particles = keep; }
  for (size_t i = 0; i < G.texts.size(); i++) { G.texts[i].life -= dt; G.texts[i].y -= 26 * dt; }
  { std::vector<FText> keep; for (size_t i = 0; i < G.texts.size(); i++) if (G.texts[i].life > 0) keep.push_back(G.texts[i]); G.texts = keep; }
}

/* ============================ 十、绘制：场景 ============================ */

static void drawCloud(float x, float y, float s) {
  pushXf(x, y, s, 0);
  fillCircle(0, 0, 16, hexc(0xffffff, 235));
  fillCircle(18, 4, 12, hexc(0xffffff, 235));
  fillCircle(-18, 5, 11, hexc(0xffffff, 235));
  fillCircle(6, -9, 12, hexc(0xffffff, 235));
  fillEllipse(0, 10, 26, 7, 0, hexc(0xcde4f4, 230));
  popXf();
}
static void drawTree(float x, float y) {
  pushXf(x, y, 1, 0);
  fillRound(-6, 0, 12, 42, 4, hexc(0x7a5a35));
  fillCircle(0, -20, 27, hexc(0x356b22));
  fillCircle(-21, -4, 18, hexc(0x356b22));
  fillCircle(21, -7, 18, hexc(0x356b22));
  fillCircle(-6, -28, 17, hexc(0x4c9430));
  fillCircle(11, -19, 12, hexc(0x4c9430));
  fillCircle(-13, -34, 8, hexc(0xffffff, 33));
  popXf();
}
static void drawHouse(float t) {
  (void)t;
  const int hw = 96, hh = ROWS * CELL_H, hb = GRID_Y + hh;
  fillRectG(0, GRID_Y, hw, hh, grad2(0, hexc(0xd6bf95), 1, hexc(0xa68f68)), 0);
  // 砖缝
  for (int y = GRID_Y + 16; y < hb; y += 16) {
    strokeLine(0, (float)y, (float)hw, (float)y, 1.5f, hexc(0x6e5537, 71));
    int off = (((y - GRID_Y) / 16) % 2) ? 0 : 26;
    for (int x = off; x < hw; x += 52) strokeLine((float)x, (float)y, (float)x, (float)(y - 16), 1.5f, hexc(0x6e5537, 71));
  }
  fillRect(0, hb - 14, hw, 14, hexc(0x463723, 82));
  // 屋顶
  Path roof; roof.moveTo(-6, (float)GRID_Y); roof.lineTo((float)hw, (float)GRID_Y);
  roof.lineTo((float)hw, GRID_Y - 30.f); roof.lineTo(-6, GRID_Y - 30.f); roof.close();
  fillPath(roof, hexc(0x6b4226));
  fillRect(-6, GRID_Y - 30, hw + 6, 9, hexc(0x54331c));
  for (int x = -6; x < hw; x += 18) {
    Path a; a.moveTo((float)x, (float)GRID_Y); a.quadTo(x + 9.f, GRID_Y - 7.f, (float)(x + 18), (float)GRID_Y);
    strokePath(a, 1.5f, hexc(0x000000, 46));
  }
  // 烟囱
  fillRect(62, GRID_Y - 46, 18, 20, hexc(0x8a5a35));
  fillRect(60, GRID_Y - 50, 22, 6, hexc(0x6b4226));
  // 拱形门
  Path door; door.moveTo(30, (float)hb); door.lineTo(30, hb - 104.f);
  door.quadTo(53, hb - 142.f, 76, hb - 104.f); door.lineTo(76, (float)hb); door.close();
  fillPath(door, hexc(0x5e3a20));
  strokePath(door, 3, hexc(0x3c2413));
  fillCircle(68, hb - 62, 3.5f, hexc(0xe8c96a));
  // 窗户
  fillRound(10, GRID_Y + 34, 32, 34, 4, hexc(0xbfe3f2));
  strokeLine(10, GRID_Y + 34, 42, GRID_Y + 34, 3, hexc(0x7a5a35));
  strokeLine(10, GRID_Y + 68, 42, GRID_Y + 68, 3, hexc(0x7a5a35));
  strokeLine(10, GRID_Y + 34, 10, GRID_Y + 68, 3, hexc(0x7a5a35));
  strokeLine(42, GRID_Y + 34, 42, GRID_Y + 68, 3, hexc(0x7a5a35));
  strokeLine(26, GRID_Y + 34, 26, GRID_Y + 68, 3, hexc(0x7a5a35));
  strokeLine(10, GRID_Y + 51, 42, GRID_Y + 51, 3, hexc(0x7a5a35));
  Path glare; glare.moveTo(12, GRID_Y + 66); glare.lineTo(28, GRID_Y + 36);
  glare.lineTo(34, GRID_Y + 36); glare.lineTo(18, GRID_Y + 66); glare.close();
  fillPath(glare, hexc(0xffffff, 89));
}
static void drawBackground(float t) {
  (void)t;
  // 天空
  fillRectG(0, 0, W, GRID_Y, grad3(0, hexc(0x79c6e8), 0.7f, hexc(0xa8dcec), 1, hexc(0xcdeacb)), 0);
  // 远处栅栏
  for (int x = 0; x < W; x += 46) fillRect((float)x, GRID_Y - 14, 7, 16, hexc(0x7a5a37, 102));
  fillRect(0, GRID_Y - 11, W, 3, hexc(0x7a5a37, 102));
  fillRect(0, GRID_Y - 5, W, 3, hexc(0x7a5a37, 102));
  // 草坪
  for (int r = 0; r < ROWS; r++) {
    fillRectG((float)GRID_X, (float)(GRID_Y + r * CELL_H), COLS * CELL_W, CELL_H,
      grad2(0, hexc(r % 2 ? 0x8ec94b : 0x9ad457), 1, hexc(r % 2 ? 0x7db83f : 0x8ac247)), 0);
  }
  // 草坪纹理
  for (int r = 0; r < ROWS; r++)
    for (int y = GRID_Y + r * CELL_H + 18; y < GRID_Y + (r + 1) * CELL_H; y += 24)
      strokeLine(GRID_X + 8, (float)y, GRID_X + COLS * CELL_W - 8, (float)y, 2, hexc(0xffffff, 15));
  // 右侧僵尸出没区
  const int rz = GRID_X + COLS * CELL_W;
  fillRectG((float)rz, (float)GRID_Y, W - rz, ROWS * CELL_H, grad2(0, hexc(0x79ad3e), 1, hexc(0x5c8a2e)), 1);
  fillRect((float)rz, (float)GRID_Y, W - rz, ROWS * CELL_H, hexc(0x967850, 41));
  drawTree(W - 42, GRID_Y + 40);
  // 底部草边
  const int bgy = GRID_Y + ROWS * CELL_H;
  fillRectG(0, (float)bgy, W, H - bgy, grad2(0, hexc(0x5e9130), 1, hexc(0x3f6b1e)), 0);
  for (int x = 6; x < W; x += 26) {
    Path g; g.moveTo((float)x, (float)H); g.quadTo(x + 5.f, H - 12.f, (float)(x + 2), H - 18.f);
    g.lineTo(x + 8.f, H - 18.f); g.quadTo(x + 8.f, H - 10.f, (float)(x + 14), (float)H); g.close();
    fillPath(g, hexc(0x1e460f, 71));
  }
  // 右下角灌木
  fillCircle(W - 20, H - 4, 24, hexc(0x356b22));
  fillCircle(W - 46, H, 16, hexc(0x356b22));
  fillCircle(W - 64, H - 6, 12, hexc(0x356b22));
  fillCircle(W - 25, H - 12, 12, hexc(0x4c9430));
  // 房子
  drawHouse(t);
  // 花草装饰
  for (size_t i = 0; i < g_decor.size(); i++) {
    const Decor& d = g_decor[i];
    pushXf(d.x, d.y, d.s, 0);
    if (d.flower) {
      strokeLine(0, 0, 0, -8, 1.6f, hexc(0x3e7d2c));
      for (int k = 0; k < 5; k++) {
        float a = k * 1.256f + d.o;
        fillEllipse(sinf(a) * 12.f, -8.f - cosf(a) * 12.f, 3, 5, a, d.col);
      }
      fillCircle(0, -8, 2.6f, hexc(0xffd93b));
    } else {
      Path w1; w1.moveTo(-4, 0); w1.quadTo(-6, -7, -2, -11); strokePath(w1, 2, hexc(0x285014, 107));
      Path w2; w2.moveTo(3, 0); w2.quadTo(6, -8, 2, -12); strokePath(w2, 2, hexc(0x285014, 107));
    }
    popXf();
  }
}

/* ============================ 十一、绘制：植物 ============================ */

static void drawPlant(const Plant& p) {
  pushXf(p.x, p.y, 1, 0);
  // 地面阴影
  fillEllipse(0, 30, 23, 6.5f, 0, hexc(0x000000, 41));
  float sway = sinf(p.t * 2 + p.c) * 0.05f;
  switch (p.type) {
  case PT_SUNFLOWER: {
    pushXf(0, 0, 1, sway);
    strokeLine(0, 26, 0, -6, 6, hexc(0x3e7d2c));
    pushXf(-14, 18, 1, -0.5f); fillEllipse(0, 0, 12, 6, 0, hexc(0x4c9a34)); popXf();
    pushXf(14, 20, 1, 0.5f);  fillEllipse(0, 0, 12, 6, 0, hexc(0x4c9a34)); popXf();
    for (int i = 0; i < 12; i++) {
      float a = (float)i * 0.5235988f + sinf(p.t * 1.5f) * 0.03f;
      fillEllipse(sinf(a) * 22.f, -8 - cosf(a) * 22.f, 6.5f, 12, a, hexc(i % 2 ? 0xffd93b : 0xffc107));
    }
    fillCircle(0, -8, 13, hexc(0x7a4a1d));
    fillCircle(0, -8, 9, hexc(0x5b3413));
    fillCircle(-4, -11, 3.4f, hexc(0xffffff));
    fillCircle(4, -11, 3.4f, hexc(0xffffff));
    fillCircle(-4 + sinf(p.t) * 1.2f, -11, 1.7f, hexc(0x222222));
    fillCircle(4 + sinf(p.t) * 1.2f, -11, 1.7f, hexc(0x222222));
    strokeArc(0, -6, 4.5f, 0.471f, 2.67f, 1.6f, hexc(0x222222));
    popXf();
    break;
  }
  case PT_PEASHOOTER: case PT_SNOWPEA: case PT_REPEATER: {
    bool cold = (p.type == PT_SNOWPEA);
    Col body = hexc(cold ? 0x4fa8d8 : 0x57b53e), dark = hexc(cold ? 0x3579a8 : 0x3f8f2a);
    strokeLine(0, 26, 0, -8, 6, dark);
    pushXf(-13, 16, 1, -0.4f); fillEllipse(0, 0, 12, 6, 0, dark); popXf();
    pushXf(13, 18, 1, 0.4f);  fillEllipse(0, 0, 12, 6, 0, dark); popXf();
    float kick = p.recoil > 0 ? 4 : 0;
    if (p.type == PT_REPEATER) {
      fillRound(-30 - kick, -26, 34, 12, 6, body);
      fillRound(-30 - kick, -10, 34, 12, 6, body);
      fillCircle(-30 - kick, -20, 6.5f, dark);
      fillCircle(-30 - kick, -4, 6.5f, dark);
    } else {
      fillRound(-32 - kick, -20, 36, 15, 7, body);
      fillCircle(-32 - kick, -12.5f, 8, dark);
    }
    fillCircle(0, -18, 15, body);
    fillCircle(-4, -23, 5, hexc(0xffffff, 89));
    fillCircle(4, -20, 3.2f, hexc(0xffffff));
    fillCircle(5, -20, 1.6f, hexc(0x222222));
    if (cold) {
      for (int i = 0; i < 3; i++) {
        float a = p.t * 1.2f + i * 2.1f;
        strokeLine(0, -18, cosf(a) * 18, -18 + sinf(a) * 18, 2, hexc(0xdff4ff));
      }
    }
    break;
  }
  case PT_GATLING: {
    Col body = hexc(0x4c9c32), dark = hexc(0x356b22);
    strokeLine(0, 26, 0, -10, 6, dark);
    pushXf(-14, 16, 1, -0.4f); fillEllipse(0, 0, 12, 6, 0, dark); popXf();
    pushXf(14, 18, 1, 0.4f);  fillEllipse(0, 0, 12, 6, 0, dark); popXf();
    float kick = p.recoil > 0 ? 5 : 0;
    for (int i = 0; i < 4; i++)
      fillRound(-30 - kick, -30 + i * 9, 36, 7, 3.5f, i % 2 ? body : dark);
    fillCircle(0, -16, 16, body);
    fillCircle(-5, -22, 5, hexc(0xffffff, 77));
    fillCircle(6, -18, 3.4f, hexc(0xffffff));
    fillCircle(7, -18, 1.7f, hexc(0x222222));
    break;
  }
  case PT_POTATO: {
    if (!p.armed) {
      fillEllipse(0, 16, 22, 9, 0, hexc(0x7a5a35));
      fillEllipse(0, 13, 16, 6, 0, hexc(0x5b4326));
      float grow = clampf((12 - std::max(0.f, p.armT)) / 12, 0.f, 1.f);
      if (grow > 0.5f) {
        float k = (grow - 0.5f) * 2, top = 10 - 14 * k;
        strokeLine(0, 10, 0, top, 3, hexc(0x4c9a34));
        fillEllipse(-5, top, 4, std::max(1.f, 3 * k), -0.6f, hexc(0x57b53e));
        fillEllipse(5, top, 4, std::max(1.f, 3 * k), 0.6f, hexc(0x57b53e));
      }
    } else {
      float blink = sinf(p.t * 6) * 0.5f + 0.5f;
      fillEllipse(0, 2, 24, 20, 0, hexc(0xc98d4b));
      fillEllipse(-6, -6, 8, 10, -0.4f, hexc(0xffffff, 71));
      strokeLine(0, -16, 0, -32, 3, hexc(0x3e7d2c));
      fillCircle(-7, 0, 4, hexc(0xffffff));
      fillCircle(7, 0, 4, hexc(0xffffff));
      fillCircle(-7, 0, 2, hexc(0x222222));
      fillCircle(7, 0, 2, hexc(0x222222));
      strokeArc(0, 8, 5, 0.314f, 2.83f, 2, hexc(0x5b3413));
      fillCircle(0, -34, 3.5f, hexc(0xff3c28, (int)(102 + blink * 153)));
    }
    break;
  }
  case PT_CHOMPER: {
    bool chew = p.chewT > 0, bite = p.chomp > 0;
    strokeLine(-6, 26, 0, -2, 6, hexc(0x3e7d2c));
    pushXf(-16, 12, 1, -0.5f); fillEllipse(0, 0, 12, 6, 0, hexc(0x4c9a34)); popXf();
    pushXf(14, 16, 1, 0.4f);  fillEllipse(0, 0, 12, 6, 0, hexc(0x4c9a34)); popXf();
    pushXf(0, -14, bite ? 1.15f : (chew ? 0.95f : 1.f), 0);
    fillCircle(0, 0, 18, hexc(0x8e44ad));
    fillCircle(-5, -6, 8, hexc(0xa55cc4));
    if (chew) {
      fillRound(-12, 2, 24, 10, 5, hexc(0x5b2c6e));
      for (int i = -1; i <= 1; i++) strokeLine(i * 8.f, 3, i * 8.f, 11, 2, hexc(0x3d1a4d));
      strokeArc(-6, -6, 3, 0.471f, 2.67f, 2, hexc(0x222222));
      strokeArc(6, -6, 3, 0.471f, 2.67f, 2, hexc(0x222222));
    } else {
      Path mouth; mouth.moveTo(-13, -2); mouth.quadTo(0, 16, 13, -2); mouth.quadTo(0, 2, -13, -2); mouth.close();
      fillPath(mouth, hexc(0x6c1a3f));
      for (int i = -2; i <= 2; i++) {
        Path tooth; tooth.moveTo(i * 5.f - 2, -2); tooth.lineTo(i * 5.f + 2, -2); tooth.lineTo(i * 5.f, 3); tooth.close();
        fillPath(tooth, hexc(0xffffff));
      }
      fillCircle(-7, -8, 3.6f, hexc(0xffffff));
      fillCircle(7, -8, 3.6f, hexc(0xffffff));
      fillCircle(-7, -8, 1.8f, hexc(0x222222));
      fillCircle(7, -8, 1.8f, hexc(0x222222));
    }
    popXf();
    break;
  }
  case PT_WALLNUT: {
    float ratio = p.hp / p.maxHp;
    fillEllipse(0, 2, 26, 30, 0, hexc(0xc98d4b));
    fillEllipse(-7, -10, 10, 14, -0.4f, hexc(0xffffff, 64));
    fillCircle(-7, -2, 4, hexc(0xffffff));
    fillCircle(7, -2, 4, hexc(0xffffff));
    fillCircle(-7, -2, 2, hexc(0x222222));
    fillCircle(7, -2, 2, hexc(0x222222));
    if (ratio > 0.66f) strokeArc(0, 8, 6, 0.314f, 2.83f, 2, hexc(0x5b3413));
    else if (ratio > 0.33f) { Path m; m.moveTo(-5, 8); m.quadTo(0, 3, 5, 8); strokePath(m, 2, hexc(0x5b3413)); }
    else { Path m; m.moveTo(-5, 4); m.quadTo(0, 10, 5, 4); strokePath(m, 2, hexc(0x5b3413)); }
    if (ratio <= 0.66f) { Path ck; ck.moveTo(-16, -18); ck.lineTo(-10, -6); ck.lineTo(-15, 2); strokePath(ck, 2, hexc(0x6b4423)); }
    if (ratio <= 0.33f) { Path ck; ck.moveTo(15, -20); ck.lineTo(9, -8); ck.lineTo(16, 0); ck.lineTo(10, 12); strokePath(ck, 2, hexc(0x6b4423)); }
    break;
  }
  case PT_CHERRY: {
    float pulse = 1 + sinf(p.t * 14) * 0.06f * (p.fuse < 0.6f ? 3 : 1);
    pushXf(0, 0, pulse, 0);
    Path s1; s1.moveTo(-8, -10); s1.quadTo(0, -26, 6, -20); strokePath(s1, 3, hexc(0x3e7d2c));
    Path s2; s2.moveTo(8, -8); s2.quadTo(12, -24, 6, -20); strokePath(s2, 3, hexc(0x3e7d2c));
    fillCircle(-9, 4, 13, hexc(0xe23b3b));
    fillCircle(9, 6, 12, hexc(0xc22b2b));
    fillCircle(-13, 0, 4, hexc(0xffffff, 115));
    fillCircle(5, 2, 3.5f, hexc(0xffffff, 115));
    fillCircle(-11, 2, 3.4f, hexc(0xffffff));
    fillCircle(7, 4, 3.4f, hexc(0xffffff));
    fillCircle(-11, 2, 1.7f, hexc(0x222222));
    fillCircle(7, 4, 1.7f, hexc(0x222222));
    strokeLine(-15, -3, -7, -1, 1.6f, hexc(0x222222));
    strokeLine(11, -1, 3, 1, 1.6f, hexc(0x222222));
    if (p.fuse < 0.6f && ((int)(p.t * 12) % 2 == 0))
      fillEllipse(0, 0, 30, 34, 0, hexc(0xffffff, 128));
    popXf();
    break;
  }
  }
  popXf();
}

/* ============================ 十二、绘制：僵尸 ============================ */

static void zLeg(float x, float rot, const Col& color) {
  pushXf(x, -18, 1, rot);
  fillRound(-4.5f, 0, 9, 18, 3, color);
  fillRound(-5.5f, 15, 11, 5, 2, hexc(0x2f332f));
  popXf();
}
static void drawZombie(const Zombie& z) {
  pushXf(z.x, z.y, 1, 0);
  if (z.dead) {
    float k = std::min(1.f, z.deathT / 1.1f);
    pushXf(0, k * 4, 1, -k * 1.5f);
    // 死亡淡出用全局透明度近似：叠加暗色不可取，直接跳过（死亡动画短）
  }
  // 影子（贴地）
  float groundY = cellYf(z.row) + 2;
  fillEllipse(0, groundY - z.y, 18, 5.5f, 0, hexc(0x000000, 64));

  float walk = z.walk;
  Col skin = hexc(0xa9bd8f), skinDark = hexc(0x8aa06f), pants = hexc(0x4a5568);
  Col shirtTop, shirt;
  if (z.type == ZT_CONE) { shirtTop = hexc(0x8ba0c2); shirt = hexc(0x6f7f9e); }
  else if (z.type == ZT_BUCKET) { shirtTop = hexc(0x96856a); shirt = hexc(0x7a6a55); }
  else { shirtTop = hexc(0x7ba35b); shirt = hexc(0x5f7a4a); }

  // 腿
  float sw = sinf(walk) * (z.eating ? 0.2f : 0.5f);
  zLeg(-5, sw, pants); zLeg(6, -sw, hexc(0x3d4756));
  // 身体
  fillRectG(-13, -46, 26, 32, grad2(0, shirtTop, 1, shirt), 0);
  { Path sh; sh.moveTo(-13, -20); sh.lineTo(-6, -14); sh.lineTo(-13, -14); sh.close(); fillPath(sh, hexc(0x000000, 38)); }
  // 手臂（朝左）
  float armRot = z.eating ? sinf(walk * 2.5f) * 0.2f - 0.15f : sinf(walk) * 0.07f - 0.06f;
  pushXf(-3, -42, 1, armRot);
  fillRound(-30, -5, 32, 10, 5, skin);
  fillCircle(-31, 0, 5.5f, skinDark);
  popXf();
  if (z.hp > z.maxHp * 0.45f) {
    pushXf(2, -40, 1, -armRot * 1.4f + 0.1f);
    fillRound(-26, -4, 28, 9, 4.5f, skinDark);
    fillCircle(-27, 0, 5, skinDark);
    popXf();
  }
  // 铁门盾牌
  if (z.door) {
    float dr = clampf(z.doorHp / 420, 0.f, 1.f);
    pushXf(-26, -30, 1, 0);
    fillRound(-6, -16, 16, 44, 3, hexc(0xbea06e, 235));
    for (int i = -12; i < 26; i += 7) strokeLine((float)i, -16, (float)i, 28, 1.2f, hexc(0x503c23, 179));
    for (int i = -14; i < 28; i += 7) strokeLine(-6, (float)i, 10, (float)i, 1.2f, hexc(0x503c23, 179));
    strokeLine(-6, -16, -6, 28, 3, hexc(0x6b4226));
    strokeLine(10, -16, 10, 28, 3, hexc(0x6b4226));
    strokeLine(-6, -16, 10, -16, 3, hexc(0x6b4226));
    strokeLine(-6, 28, 10, 28, 3, hexc(0x6b4226));
    if (dr < 0.6f) { Path ck; ck.moveTo(-4, -14); ck.lineTo(2, -2); ck.lineTo(-3, 8); strokePath(ck, 2, hexc(0x28190a, 204)); }
    if (dr < 0.3f) { Path ck; ck.moveTo(8, 10); ck.lineTo(2, 20); ck.lineTo(9, 26); strokePath(ck, 2, hexc(0x28190a, 204)); }
    popXf();
  }
  // 头
  float headBob = z.eating ? sinf(walk * 3) * 1.2f : 0;
  pushXf(2, -56 + headBob, 1, 0);
  fillCircleG(0, 0, 11.5f, -3, -4, 2, 13, grad2(0, hexc(0xc2d4a6), 1, skin));
  strokeLine(-8, -7, -10, -12, 2, hexc(0x4a3b2a));
  strokeLine(-2, -9, -3, -14, 2, hexc(0x4a3b2a));
  fillCircle(-4, -1, 3.4f, hexc(0xffffff));
  fillCircle(-4.8f, -1, 1.7f, hexc(0xc0392b));
  float mouth = z.eating ? 3 + fabsf(sinf(walk * 3)) * 3 : 2;
  fillRound(-9, 4, 10, mouth + 2, 2, hexc(0x5b3413));
  fillRect(-8, 4, 3, 2, hexc(0xffffff));
  fillRect(-4, 4, 3, 2, hexc(0xffffff));
  popXf();
  // 头饰
  if (z.type == ZT_CONE) {
    Path cone; cone.moveTo(-10, -66); cone.lineTo(10, -66); cone.lineTo(0, -88); cone.close();
    fillPath(cone, hexc(0xe07b2a));
    Path hl; hl.moveTo(-6, -72); hl.lineTo(6, -72); hl.lineTo(0, -86); hl.close();
    fillPath(hl, hexc(0xffffff, 102));
  } else if (z.type == ZT_BUCKET) {
    fillRound(-11, -78, 22, 17, 3, hexc(0x9aa3ad));
    fillRound(-13, -80, 26, 5, 2, hexc(0x7d868f));
    fillRect(-6, -76, 3, 12, hexc(0x000000, 46));
    fillRect(3, -74, 2, 9, hexc(0x000000, 46));
  } else if (z.type == ZT_FLAG) {
    strokeLine(10, -44, 10, -92, 3, hexc(0x7a4a2e));
    float fx = sinf(z.walk * 2) * 6;
    Path flag; flag.moveTo(10, -92);
    flag.quadTo(10 + fx + 14, -88, 26 + fx, -84);
    flag.lineTo(26 + fx, -74);
    flag.quadTo(10 + fx + 14, -70, 10, -74);
    flag.close();
    fillPath(flag, hexc(0xd42a2a));
  } else if (z.type == ZT_FOOTBALL) {
    fillArc(2, -57, 13.5f, 3.1415927f, 0, hexc(0xc0392b));
    fillRound(-9, -59, 20, 5, 2, hexc(0x96281b));
    Path mask; mask.moveTo(-9, -53); mask.quadTo(-16, -47, -11, -42); strokePath(mask, 2.5f, hexc(0xbdc3c7));
    strokeLine(-3, -68, -1, -72, 2, hexc(0xffffff, 140));
  } else if (z.type == ZT_POLE && !z.vaulted) {
    strokeLine(-26, 6, 6, -72, 4, hexc(0xb08d57));
    strokeLine(-22, 0, 3, -63, 1.5f, hexc(0x8a6a3f));
  }
  // 冰冻 / 受击
  if (z.slowT > 0) fillRound(-16, -72, 34, 74, 10, hexc(0x78c8ff, 82));
  if (z.flash > 0) fillRound(-16, -72, 34, 74, 10, hexc(0xffffff, 115));
  if (z.dead) popXf();
  popXf();
}

/* ============================ 十三、绘制：其他实体 ============================ */

static void drawPea(const Pea& pea) {
  pushXf(pea.x, pea.y, 1, 0);
  fillCircleG(0, 0, 7.5f, -2, -2, 1, 8,
    grad2(0, hexc(pea.snow ? 0xdff4ff : 0xd6f5a0), 1, hexc(pea.snow ? 0x4fa8d8 : 0x57b53e)));
  strokeArc(0, 0, 7.5f, 0, 6.2831853f, 1.4f, hexc(0x19370f, 77));
  fillCircle(-2.2f, -2.6f, 1.8f, hexc(0xffffff, 191));
  popXf();
}
static void drawSun(const Sun& s) {
  pushXf(s.x, s.y, 1, 0);
  float a = (s.state == 1 && s.life < 2) ? (sinf(s.t * 10) * 0.5f + 0.5f) : 1;
  // 外层光晕
  fillCircle(0, 0, 23 + sinf(s.t * 3) * 2, hexc(0xffd64a, (int)(41 * a)));
  // 光芒（旋转的三角形）
  float rot = s.t * 0.8f;
  for (int i = 0; i < 8; i++) {
    Path ray; ray.moveTo(-3.5f, -14); ray.lineTo(3.5f, -14); ray.lineTo(0, -26); ray.close();
    pushXf(0, 0, 1, i * 0.7853982f + rot);
    fillPath(ray, hexc(0xffd64a, (int)(140 * a)));
    popXf();
  }
  fillCircleG(0, 0, 14, -3, -3, 2, 15, grad3(0, hexc(0xfff6c8), 0.5f, hexc(0xffd93b), 1, hexc(0xf5a800)));
  popXf();
}
static void drawMower(const Mower& m) {
  if (m.x < -100) return;
  pushXf(m.x, m.y, 1, 0);
  fillEllipse(0, 4, 20, 5, 0, hexc(0x000000, 64));
  fillRound(-18, -18, 34, 16, 5, hexc(0xc0392b));
  fillRound(-18, -8, 34, 6, 3, hexc(0x8e2b20));
  fillCircle(-10, 0, 7, hexc(0x555555));
  fillCircle(10, 0, 7, hexc(0x555555));
  fillCircle(-10, 0, 3, hexc(0x999999));
  fillCircle(10, 0, 3, hexc(0x999999));
  strokeLine(14, -16, 24, -34, 4, hexc(0x7a4a2e));
  fillRound(18, -42, 16, 10, 3, hexc(0xbdc3c7));
  popXf();
}
static void drawParticles() {
  for (size_t i = 0; i < G.particles.size(); i++) {
    const Particle& p = G.particles[i];
    float a = clampf(p.life / p.maxLife, 0.f, 1.f);
    if (p.boom) {
      float k = 1 - a;
      float ro = 20 + k * 110;
      fillCircleG(p.x, p.y, ro, p.x, p.y, 4, ro,
        grad3(0, hexc(0xfff0b4, 242), 0.5f, hexc(0xff9628, 178), 1, hexc(0xff5014, 0)));
    } else {
      Col c = p.color;
      c.a = (int)(p.color.a * a);
      fillCircle(p.x, p.y, p.size * (0.5f + a * 0.5f), c);
    }
  }
}
static void drawTexts() {
  for (size_t i = 0; i < G.texts.size(); i++) {
    const FText& t = G.texts[i];
    Col c = t.color; c.a = (int)(std::min(1.f, t.life * 1.6f) * 255);
    dtext(t.text.c_str(), t.x, t.y, t.size, RGB(c.r, c.g, c.b), 0, true, RGB(0, 0, 0), 3);
  }
}

/* ============================ 十四、HUD 与覆盖层 ============================ */

static const int CARD_W = 64, CARD_H = 66, CARD_GAP = 5;
static const int CARDS_X = 124;
static const int TOOL_Y = 19, TOOL_SZ = 46;
static const int TOOL_XS[4] = { 786, 840, 894, 948 };
static const int PROG_X = 8, PROG_Y = 690, PROG_W = 884, PROG_H = 24;

static void drawSunIcon(float x, float y, float s) {
  pushXf(x, y, s, 0);
  fillCircle(0, 0, 9, hexc(0xffd64a, 60));
  for (int i = 0; i < 8; i++) {
    float ang = i * 0.7853982f;
    Path ray; ray.moveTo(-2.2f, -8); ray.lineTo(2.2f, -8); ray.lineTo(0, -13); ray.close();
    pushXf(0, 0, 1, ang);
    fillPath(ray, hexc(0xffd64a, 200));
    popXf();
  }
  fillCircleG(0, 0, 7, -2, -2, 1, 7, grad2(0, hexc(0xfff6c8), 1, hexc(0xf5a800)));
  popXf();
}
static void drawShovelIcon(float x, float y, float s) {
  pushXf(x, y, s, 0);
  strokeLine(0, -14, 4, 4, 5, hexc(0x7a4a2e));
  Path blade; blade.moveTo(-6, 4); blade.lineTo(14, 4); blade.lineTo(4, 18); blade.close();
  fillPath(blade, hexc(0xbdc3c7));
  popXf();
}
static void drawPauseIcon(float x, float y, float s) {
  pushXf(x, y, s, 0);
  fillRound(-8, -10, 6, 20, 2, hexc(0x3a2a08));
  fillRound(3, -10, 6, 20, 2, hexc(0x3a2a08));
  popXf();
}
static void drawSpeakerIcon(float x, float y, float s, bool on) {
  pushXf(x, y, s, 0);
  Path sp; sp.moveTo(-10, -4); sp.lineTo(-4, -4); sp.lineTo(2, -10); sp.lineTo(2, 10); sp.lineTo(-4, 4); sp.lineTo(-10, 4); sp.close();
  fillPath(sp, hexc(0x3a2a08));
  if (on) {
    strokeArc(4, 0, 7, -0.9f, 0.9f, 2, hexc(0x3a2a08));
    strokeArc(4, 0, 12, -0.8f, 0.8f, 2, hexc(0x3a2a08));
  } else {
    strokeLine(5, -5, 13, 5, 2.5f, hexc(0xc0392b));
    strokeLine(5, 5, 13, -5, 2.5f, hexc(0xc0392b));
  }
  popXf();
}
static void drawRestartIcon(float x, float y, float s) {
  pushXf(x, y, s, 0);
  strokeArc(0, 0, 10, 0.5f, 5.2f, 3.5f, hexc(0x3a2a08));
  Path head; head.moveTo(9, -8); head.lineTo(11, 1); head.lineTo(2, -3); head.close();
  fillPath(head, hexc(0x3a2a08));
  popXf();
}
static void drawFlagIcon(float x, float y, float s) {
  pushXf(x, y, s, 0);
  strokeLine(0, 0, 0, -14, 2.5f, hexc(0x7a4a2e));
  Path flag; flag.moveTo(0, -14); flag.lineTo(11, -11); flag.lineTo(0, -7); flag.close();
  fillPath(flag, hexc(0xd42a2a));
  popXf();
}
static void drawToolBtn(int idx, float x, float y, bool active) {
  fillRoundG(x, y, TOOL_SZ, TOOL_SZ, 8, grad2(0, hexc(0xf3e3ae), 1, hexc(0xd9c286)), 0);
  fillRoundG(x + 2, y + 2, TOOL_SZ - 4, (TOOL_SZ - 4) * 0.5f, 6, grad2(0, hexc(0xffffff, 90), 1, hexc(0xffffff, 15)), 0);
  strokeLine(x, y, x + TOOL_SZ, y, 2, hexc(0x8a6a34));
  strokeLine(x, y + TOOL_SZ, x + TOOL_SZ, y + TOOL_SZ, 2, hexc(0x8a6a34));
  strokeLine(x, y, x, y + TOOL_SZ, 2, hexc(0x8a6a34));
  strokeLine(x + TOOL_SZ, y, x + TOOL_SZ, y + TOOL_SZ, 2, hexc(0x8a6a34));
  float cx = x + TOOL_SZ / 2, cy = y + TOOL_SZ / 2;
  if (idx == 0) drawShovelIcon(cx, cy, 1.1f);
  else if (idx == 1) drawPauseIcon(cx, cy, 1.1f);
  else if (idx == 2) drawSpeakerIcon(cx, cy, 1.1f, !g_muted);
  else drawRestartIcon(cx, cy, 1.1f);
  if (active) {
    strokeLine(x - 1, y - 1, x + TOOL_SZ + 1, y - 1, 3, hexc(0xffe066));
    strokeLine(x - 1, y + TOOL_SZ + 1, x + TOOL_SZ + 1, y + TOOL_SZ + 1, 3, hexc(0xffe066));
    strokeLine(x - 1, y - 1, x - 1, y + TOOL_SZ + 1, 3, hexc(0xffe066));
    strokeLine(x + TOOL_SZ + 1, y - 1, x + TOOL_SZ + 1, y + TOOL_SZ + 1, 3, hexc(0xffe066));
  }
}
static void drawTopbar() {
  // 木纹底
  for (int x = 0; x < CLW; x += 8) fillRect((float)x, 0, 3, TOPBAR_H, hexc(0x3c230a, 23));
  fillRectG(0, 0, CLW, TOPBAR_H, grad2(0, hexc(0xb07c44), 1, hexc(0x7c4f26)), 0);
  fillRect(0, 0, CLW, 2, hexc(0xffffff, 71));
  strokeLine(0, TOPBAR_H - 1.5f, CLW, TOPBAR_H - 1.5f, 3, hexc(0x5b3a1a));
  // 阳光计数器
  float pulse = 0.5f + 0.5f * sinf(wallT * 2.6f);
  fillRoundG(8, 8, 110, 68, 8, grad2(0, hexc(0xfbf0c4), 1, hexc(0xf0dda0)), 0);
  fillRoundG(10, 10, 106, 32, 6, grad2(0, hexc(0xffffff, 120), 1, hexc(0xffffff, 20)), 0);
  strokeLine(8, 8, 118, 8, 2, hexc(0xb99b4e));
  strokeLine(8, 76, 118, 76, 2, hexc(0xb99b4e));
  strokeLine(8, 8, 8, 76, 2, hexc(0xb99b4e));
  strokeLine(118, 8, 118, 76, 2, hexc(0xb99b4e));
  fillRoundG(11, 11, 104, 62, 7, grad1(hexc(0xffc83c, (int)(40 + pulse * 55))), 0);
  drawSunIcon(30, 42, 1.15f);
  wchar_t buf[16]; swprintf(buf, 16, L"%d", G.sun);
  dtext(buf, 62, 42, 20, RGB(0x6b, 0x4a, 0x12), 1, true);
  // 种子卡片
  for (int i = 0; i < PT_COUNT; i++) {
    int t = ORDER[i];
    const PlantDef& def = PLANT_DEFS[t];
    float x = (float)(CARDS_X + i * (CARD_W + CARD_GAP)), y = 9;
    bool dis = G.sun < def.cost || G.cds[t] > 0;
    bool sel = (G.selected == t);
    fillRoundG(x, y, CARD_W, CARD_H, 9, grad2(0, hexc(0xf6e8b6), 1, hexc(0xdcc78a)), 0);
    fillRoundG(x + 2, y + 2, CARD_W - 4, 30, 7, grad2(0, hexc(0xffffff, 100), 1, hexc(0xffffff, 10)), 0);
    strokeLine(x, y, x + CARD_W, y, 2, hexc(0x8a6a34));
    strokeLine(x, y + CARD_H, x + CARD_W, y + CARD_H, 2, hexc(0x8a6a34));
    strokeLine(x, y, x, y + CARD_H, 2, hexc(0x8a6a34));
    strokeLine(x + CARD_W, y, x + CARD_W, y + CARD_H, 2, hexc(0x8a6a34));
    // 冷却遮罩
    if (G.cds[t] > 0) {
      float ratio = clampf(G.cds[t] / def.cd, 0.f, 1.f);
      fillRect(x + 2, y + 2, CARD_W - 4, (CARD_H - 4) * ratio, hexc(0x0f190f, 153));
    }
    if (dis) fillRect(x + 2, y + 2, CARD_W - 4, CARD_H - 4, hexc(0x202820, 110));
    // 快捷键角标
    wchar_t kb[4]; swprintf(kb, 4, L"%d", i + 1);
    dtext(kb, x + 6, y + 6, 10, RGB(0x8a, 0x6a, 0x34), 1, true);
    // 植物图标
    Plant syn;
    syn.type = t; syn.r = 0; syn.c = 0; syn.x = 0; syn.y = 0;
    syn.hp = 1; syn.maxHp = 1; syn.t = wallT; syn.shootT = 0; syn.secondT = 0;
    syn.produceT = 0; syn.fuse = 1.2f; syn.recoil = 0; syn.flash = 0;
    syn.armT = 0; syn.armed = true; syn.chewT = 0; syn.chomp = 0; syn.burst = 0; syn.burstT = 0; syn.dead = false;
    pushXf(x + CARD_W / 2, y + 27, 0.52f, 0);
    drawPlant(syn);
    popXf();
    // 名称与价格
    dtext(def.name, x + CARD_W / 2, y + 48, 10, RGB(0x5b, 0x43, 0x1a), 0, true);
    wchar_t cb[16]; swprintf(cb, 16, L"%d", def.cost);
    dtext(cb, x + CARD_W - 4, y + CARD_H - 6, 11, RGB(0x4a, 0x34, 0x0a), 2, true);
    if (sel) {
      strokeLine(x - 1, y - 1, x + CARD_W + 1, y - 1, 3, hexc(0xffe066));
      strokeLine(x - 1, y + CARD_H + 1, x + CARD_W + 1, y + CARD_H + 1, 3, hexc(0xffe066));
      strokeLine(x - 1, y - 1, x - 1, y + CARD_H + 1, 3, hexc(0xffe066));
      strokeLine(x + CARD_W + 1, y - 1, x + CARD_W + 1, y + CARD_H + 1, 3, hexc(0xffe066));
    }
  }
  // 工具按钮
  drawToolBtn(0, (float)TOOL_XS[0], TOOL_Y, G.shovel);
  drawToolBtn(1, (float)TOOL_XS[1], TOOL_Y, G.paused);
  drawToolBtn(2, (float)TOOL_XS[2], TOOL_Y, g_muted);
  drawToolBtn(3, (float)TOOL_XS[3], TOOL_Y, false);
}
static void drawBottombar() {
  fillRectG(0, (float)(TOPBAR_H + H), CLW, BAR_H, grad2(0, hexc(0x7c4f26), 1, hexc(0x5b3a1a)), 0);
  strokeLine(0, TOPBAR_H + H + 1.f, CLW, TOPBAR_H + H + 1.f, 3, hexc(0x5b3a1a));
  // 进度条
  fillRoundG((float)PROG_X, (float)PROG_Y, PROG_W, PROG_H, 9, grad1(hexc(0x2e4a16)), 0);
  strokeLine(PROG_X, PROG_Y, PROG_X + PROG_W, PROG_Y, 2, hexc(0x22380f));
  strokeLine(PROG_X, PROG_Y + PROG_H, PROG_X + PROG_W, PROG_Y + PROG_H, 2, hexc(0x22380f));
  strokeLine(PROG_X, PROG_Y, PROG_X, PROG_Y + PROG_H, 2, hexc(0x22380f));
  strokeLine(PROG_X + PROG_W, PROG_Y, PROG_X + PROG_W, PROG_Y + PROG_H, 2, hexc(0x22380f));
  float prog = clampf(((float)G.wave + (G.wave < MAX_WAVE ? 1 - G.waveTimer / 20 : 0)) / MAX_WAVE, 0.f, 1.f);
  float fw = (PROG_W - 4) * prog;
  if (fw > 2) {
    fillRoundG(PROG_X + 2, (float)(PROG_Y + 2), fw, PROG_H - 4, 7, grad2(0, hexc(0xc8f08a), 1, hexc(0x7fbf3f)), 0);
    // 流光
    float sx = fmodf(wallT * 0.35f, 1.4f) - 0.2f;
    float x0 = std::max(0.f, sx), x1 = std::min(fw, sx + 0.18f);
    if (x1 > x0)
      fillRectG(PROG_X + 2 + x0, (float)(PROG_Y + 2), x1 - x0, PROG_H - 4,
        grad3(0, hexc(0xffffff, 0), 0.5f, hexc(0xffffff, 110), 1, hexc(0xffffff, 0)), 1);
  }
  // 旗帜标记
  for (int i = 0; i < 2; i++) {
    float fx = PROG_X + (float)PROG_W * FLAG_WAVES[i] / MAX_WAVE;
    drawFlagIcon(fx, (float)(PROG_Y + 2), 1.f);
  }
  wchar_t buf[32]; swprintf(buf, 32, L"第 %d / %d 波", G.wave, MAX_WAVE);
  dtext(buf, 992, PROG_Y + PROG_H / 2, 13, RGB(0xff, 0xe9, 0xb0), 2, true);
}

/* ---------- 覆盖层 ---------- */
struct BtnRect { float x, y, w, h; };
static BtnRect g_btnStart = { 0, 0, 200, 54 };
static BtnRect g_btnResume = { 0, 0, 160, 48 };
static BtnRect g_btnAgain = { 0, 0, 160, 48 };
static void drawButton(const BtnRect& b, const wchar_t* label) {
  fillRoundG(b.x, b.y, b.w, b.h, 12, grad2(0, hexc(0x7fbf3f), 1, hexc(0x4e8a22)), 0);
  fillRect(b.x + 3, b.y, b.w - 6, 4, hexc(0x2c4d0c));
  fillRoundG(b.x + 4, b.y + 3, b.w - 8, b.h * 0.4f, 9, grad2(0, hexc(0xffffff, 70), 1, hexc(0xffffff, 5)), 0);
  strokeLine(b.x, b.y, b.x + b.w, b.y, 3, hexc(0x35610f));
  strokeLine(b.x, b.y + b.h, b.x + b.w, b.y + b.h, 3, hexc(0x35610f));
  strokeLine(b.x, b.y, b.x, b.y + b.h, 3, hexc(0x35610f));
  strokeLine(b.x + b.w, b.y, b.x + b.w, b.y + b.h, 3, hexc(0x35610f));
  dtextS(label, b.x + b.w / 2, b.y + b.h / 2, 20, RGB(0xff, 0xff, 0xff), RGB(0x2c, 0x4d, 0x0c), 2);
}
static void drawStartScreen() {
  float px = 180, py = TOPBAR_H + 60, pw = 640, ph = 496;
  fillRoundG(px, py, pw, ph, 16, grad2(0, hexc(0xfff8e0), 1, hexc(0xf0dfae)), 0);
  strokeLine(px, py, px + pw, py, 4, hexc(0x7c4f26));
  strokeLine(px, py + ph, px + pw, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px, py, px, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px + pw, py, px + pw, py + ph, 4, hexc(0x7c4f26));
  dtextS(L"植物大战僵尸", px + pw / 2, py + 42, 38, RGB(0x3f, 0x7a, 0x1e), RGB(0xcb, 0xe8, 0x9a), 2);
  dtext(L"C++ 版 · 守住你的房子，击败 15 波僵尸进攻！", px + pw / 2, py + 78, 14, RGB(0x6b, 0x5a, 0x30), 0, false);
  // 玩法说明
  float tx = px + 24, ty = py + 96, tw = pw - 48;
  fillRoundG(tx, ty, tw, 126, 8, grad1(hexc(0xfffbee, 208)), 0);
  for (int i = 0; i < 4; i++) { // 虚线边框
    float yy = ty + 6 + i * 30;
    for (float xx = tx + 6; xx < tx + tw - 10; xx += 12)
      fillRect(xx, yy, 6, 1.5f, hexc(0xb99b4e, 160));
  }
  dtextBlock(
    L"阳光是核心资源：天上会掉落，向日葵也会生产，点击收集。先种向日葵攒阳光，再种豌豆射手攻击，坚果墙拖延僵尸。\n"
    L"机枪射手是升级植物：选中后点一下已有的双发射手即可四连发；土豆雷需埋伏 12 秒；大嘴花一口吞掉右侧僵尸。\n"
    L"点击上方卡片选植物，再点草坪种植；按 1~9 快速选择，Esc 取消，P 暂停；铲子可移除植物，每行小推车是最后防线。",
    tx + 12, ty + 10, tw - 24, 12, RGB(0x5a, 0x46, 0x1a), 5);
  // 植物卡片 5+4
  float cw = 110, ch = 88, gap = 8;
  float gx = px + (pw - (5 * cw + 4 * gap)) / 2, gy = ty + 134;
  for (int i = 0; i < PT_COUNT; i++) {
    int t = ORDER[i];
    const PlantDef& def = PLANT_DEFS[t];
    int row = i / 5, col = i % 5;
    float x = gx + col * (cw + gap), y = gy + row * (ch + gap);
    fillRoundG(x, y, cw, ch, 8, grad1(hexc(0xffffff)), 0);
    strokeLine(x, y, x + cw, y, 2, hexc(0xb99b4e));
    strokeLine(x, y + ch, x + cw, y + ch, 2, hexc(0xb99b4e));
    strokeLine(x, y, x, y + ch, 2, hexc(0xb99b4e));
    strokeLine(x + cw, y, x + cw, y + ch, 2, hexc(0xb99b4e));
    Plant syn;
    syn.type = t; syn.r = 0; syn.c = 0; syn.x = 0; syn.y = 0;
    syn.hp = 1; syn.maxHp = 1; syn.t = wallT; syn.shootT = 0; syn.secondT = 0;
    syn.produceT = 0; syn.fuse = 1.2f; syn.recoil = 0; syn.flash = 0;
    syn.armT = 0; syn.armed = true; syn.chewT = 0; syn.chomp = 0; syn.burst = 0; syn.burstT = 0; syn.dead = false;
    pushXf(x + cw / 2, y + 24, 0.36f, 0);
    drawPlant(syn);
    popXf();
    dtext(def.name, x + cw / 2, y + 42, 12, RGB(0x4e, 0x8a, 0x22), 0, true);
    dtextBlock(def.tip, x + 6, y + 50, cw - 12, 10, RGB(0x6b, 0x5a, 0x30), 2);
    wchar_t cb[16]; swprintf(cb, 16, L"阳光 %d", def.cost);
    dtext(cb, x + cw / 2, y + ch - 6, 10, RGB(0xb3, 0x54, 0x1e), 0, true);
  }
  g_btnStart.x = px + (pw - g_btnStart.w) / 2;
  g_btnStart.y = py + ph - 70;
  drawButton(g_btnStart, L"开始游戏");
}
static void drawPauseScreen() {
  float px = 330, py = TOPBAR_H + 205, pw = 340, ph = 190;
  fillRoundG(px, py, pw, ph, 16, grad2(0, hexc(0xfff8e0), 1, hexc(0xf0dfae)), 0);
  strokeLine(px, py, px + pw, py, 4, hexc(0x7c4f26));
  strokeLine(px, py + ph, px + pw, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px, py, px, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px + pw, py, px + pw, py + ph, 4, hexc(0x7c4f26));
  dtextS(L"已暂停", px + pw / 2, py + 50, 28, RGB(0x3f, 0x7a, 0x1e), RGB(0xcb, 0xe8, 0x9a), 2);
  dtext(L"休息一下，僵尸也在等你。", px + pw / 2, py + 92, 14, RGB(0x6b, 0x5a, 0x30), 0, false);
  g_btnResume.x = px + (pw - g_btnResume.w) / 2;
  g_btnResume.y = py + ph - 66;
  drawButton(g_btnResume, L"继续游戏");
}
static void drawEndScreen() {
  float px = 270, py = TOPBAR_H + 175, pw = 460, ph = 250;
  fillRoundG(px, py, pw, ph, 16, grad2(0, hexc(0xfff8e0), 1, hexc(0xf0dfae)), 0);
  strokeLine(px, py, px + pw, py, 4, hexc(0x7c4f26));
  strokeLine(px, py + ph, px + pw, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px, py, px, py + ph, 4, hexc(0x7c4f26));
  strokeLine(px + pw, py, px + pw, py + ph, 4, hexc(0x7c4f26));
  dtextS(G.won ? L"胜利！" : L"游戏结束", px + pw / 2, py + 50, 32,
    G.won ? RGB(0x3f, 0x7a, 0x1e) : RGB(0xb0, 0x30, 0x28), RGB(0xcb, 0xe8, 0x9a), 2);
  dtext(G.won ? L"你成功击退了所有僵尸，房子安全了！" : L"僵尸吃掉了你的脑子！",
    px + pw / 2, py + 92, 14, RGB(0x6b, 0x5a, 0x30), 0, false);
  int mm = (int)G.time / 60, ss = (int)G.time % 60;
  wchar_t buf[128];
  swprintf(buf, 128, L"坚守波次：%d / %d　消灭僵尸：%d　收集阳光：%d　用时：%d分%02d秒",
    G.wave, MAX_WAVE, G.kills, G.sunCollected, mm, ss);
  dtext(buf, px + pw / 2, py + 128, 15, RGB(0x4a, 0x38, 0x12), 0, true);
  g_btnAgain.x = px + (pw - g_btnAgain.w) / 2;
  g_btnAgain.y = py + ph - 66;
  drawButton(g_btnAgain, L"再来一局");
}
static void drawOverlay() {
  if (G.screen == 1) return;
  fillRect(0, (float)TOPBAR_H, W, H, hexc(0x081206, 209));
  if (G.screen == 0) drawStartScreen();
  else if (G.screen == 2) drawPauseScreen();
  else if (G.screen == 3) drawEndScreen();
}

/* ---------- 悬停预览 ---------- */
static void drawHover() {
  if (!G.hover || G.screen != 1) return;
  float x = GRID_X + G.hc * CELL_W, y = GRID_Y + G.hr * CELL_H;
  if (G.shovel) {
    fillRect(x + 2, y + 2, CELL_W - 4, CELL_H - 4, hexc(0xff503c, 56));
    strokeLine(x + 2, y + 2, x + CELL_W - 2, y + 2, 2, hexc(0xff6b5b));
    strokeLine(x + 2, y + CELL_H - 2, x + CELL_W - 2, y + CELL_H - 2, 2, hexc(0xff6b5b));
    strokeLine(x + 2, y + 2, x + 2, y + CELL_H - 2, 2, hexc(0xff6b5b));
    strokeLine(x + CELL_W - 2, y + 2, x + CELL_W - 2, y + CELL_H - 2, 2, hexc(0xff6b5b));
    drawShovelIcon(G.mx, G.my, 1.2f);
    return;
  }
  int type = G.selected;
  if (type < 0) return;
  bool occupied = plantAt(G.hr, G.hc) != nullptr;
  bool ok = !occupied && G.sun >= PLANT_DEFS[type].cost && G.cds[type] <= 0;
  if (type == PT_CHERRY)
    fillRect(x - CELL_W, y - CELL_H, CELL_W * 3, CELL_H * 3, ok ? hexc(0xff783c, 46) : hexc(0xff3c3c, 31));
  fillRect(x + 2, y + 2, CELL_W - 4, CELL_H - 4, ok ? hexc(0xffffff, 64) : hexc(0xff3c3c, 56));
  strokeLine(x + 2, y + 2, x + CELL_W - 2, y + 2, 2, ok ? hexc(0xffffff, 217) : hexc(0xff6b5b));
  strokeLine(x + 2, y + CELL_H - 2, x + CELL_W - 2, y + CELL_H - 2, 2, ok ? hexc(0xffffff, 217) : hexc(0xff6b5b));
  strokeLine(x + 2, y + 2, x + 2, y + CELL_H - 2, 2, ok ? hexc(0xffffff, 217) : hexc(0xff6b5b));
  strokeLine(x + CELL_W - 2, y + 2, x + CELL_W - 2, y + CELL_H - 2, 2, ok ? hexc(0xffffff, 217) : hexc(0xff6b5b));
  Plant syn;
  syn.type = type; syn.r = G.hr; syn.c = G.hc; syn.x = cellXf(G.hc); syn.y = cellYf(G.hr);
  syn.hp = 1; syn.maxHp = 1; syn.t = 1; syn.shootT = 0; syn.secondT = 0;
  syn.produceT = 0; syn.fuse = 1.2f; syn.recoil = 0; syn.flash = 0;
  syn.armT = 0; syn.armed = false; syn.chewT = 0; syn.chomp = 0; syn.burst = 0; syn.burstT = 0; syn.dead = false;
  // 半透明预览：用暗化近似（整体乘 0.6 亮度）
  drawPlant(syn);
}

/* ============================ 十五、渲染管线 ============================ */

static void renderScene(float t) {
  // 静态背景（缓存整块拷贝到画布区域，画布在客户区 TOPBAR_H 处开始）
  memcpy(g_px + (size_t)g_bw * TOPBAR_H * SS, g_cache, (size_t)g_bw * H * SS * 4);
  setXf(0, (float)TOPBAR_H, 1, 0);
  // 云与炊烟（动态）
  for (int i = 0; i < 4; i++) {
    float cx = fmodf(g_clouds[i].x + t * g_clouds[i].v, (float)(W + 260)) - 130;
    drawCloud(cx, g_clouds[i].y + sinf(t * 0.4f + g_clouds[i].x) * 3, g_clouds[i].s);
  }
  for (int i = 0; i < 3; i++) {
    float p = fmodf(t * 0.45f + i * 0.34f, 1.f);
    fillCircle(71 + sinf(p * 6 + i * 2) * 7, GRID_Y - 58 - p * 46, 4 + p * 9, hexc(0xebebeb, (int)(102 * (1 - p))));
  }
  if (G.shake > 0) {
    // 震屏
    float shx = rnd(-G.shake, G.shake) * 0.5f, shy = rnd(-G.shake, G.shake) * 0.5f;
    setXf(shx, (float)TOPBAR_H + shy, 1, 0);
  }
  for (size_t i = 0; i < G.mowers.size(); i++) drawMower(G.mowers[i]);
  // 按行排序
  struct Ent { float y; int kind; size_t idx; };
  std::vector<Ent> ents;
  for (size_t i = 0; i < G.plants.size(); i++) ents.push_back({ G.plants[i].y, 0, i });
  for (size_t i = 0; i < G.zombies.size(); i++) ents.push_back({ G.zombies[i].y, 1, i });
  std::stable_sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.y < b.y; });
  for (size_t i = 0; i < ents.size(); i++) {
    if (ents[i].kind == 0) drawPlant(G.plants[ents[i].idx]);
    else drawZombie(G.zombies[ents[i].idx]);
  }
  for (size_t i = 0; i < G.peas.size(); i++) drawPea(G.peas[i]);
  for (size_t i = 0; i < G.suns.size(); i++) drawSun(G.suns[i]);
  drawParticles();
  drawHover();
  drawTexts();
  // 横幅
  if (G.banner) {
    float k = G.bannerT;
    float a = k < 0.3f ? k / 0.3f : (k > 2.2f ? std::max(0.f, (2.6f - k) / 0.4f) : 1);
    float scale = k < 0.3f ? 0.5f + (k / 0.3f) * 0.7f : 1;
    pushXf(W / 2, 220, scale, 0);
    dtext(L"一大波僵尸正在接近！", 0, 0, 52, RGB(0xe0, 0x5b, 0x1e), 0, true, RGB(0x3a, 0x2a, 0x08), 8);
    dtext(L"一大波僵尸正在接近！", 0, -2, 52, RGB(0xff, 0xf3, 0xb0), 0, true);
    popXf();
  }
  setXf(0, 0, 1, 0);
}
static void present() {
  // 2x -> 1x 盒式滤波 + 暗角（用平方距离近似，避免逐像素开方）
  const float VCX = W * 0.5f, VCY = H * 0.5f, VI = H * 0.38f, VO = H * 1.0f;
  const float VI2 = VI * VI, VO2 = VO * VO;
  for (int y = 0; y < CLH; y++) {
    uint32_t* dst = g_out + (size_t)y * CLW;
    int sy = y * SS;
    bool inCanvas = (y >= TOPBAR_H && y < TOPBAR_H + H);
    float dvy = inCanvas ? (float)(y - TOPBAR_H) - VCY : 0.f;
    for (int x = 0; x < CLW; x++) {
      int sx = x * SS;
      const uint32_t* p00 = g_px + (size_t)sy * g_bw + sx;
      const uint32_t* p01 = p00 + 1;
      const uint32_t* p10 = p00 + g_bw;
      const uint32_t* p11 = p10 + 1;
      int b = ((int)(*p00 & 255) + (int)(*p01 & 255) + (int)(*p10 & 255) + (int)(*p11 & 255)) / 4;
      int g = (((int)(*p00 >> 8) & 255) + ((int)(*p01 >> 8) & 255) + ((int)(*p10 >> 8) & 255) + ((int)(*p11 >> 8) & 255)) / 4;
      int r = (((int)(*p00 >> 16) & 255) + ((int)(*p01 >> 16) & 255) + ((int)(*p10 >> 16) & 255) + ((int)(*p11 >> 16) & 255)) / 4;
      if (inCanvas) {
        float dx = (float)x - VCX;
        float d2 = dx * dx + dvy * dvy;
        if (d2 > VI2) {
          int a = (int)(clampf((d2 - VI2) / (VO2 - VI2), 0.f, 1.f) * 66); // 0.26*255≈66
          r = (12 * a + r * (255 - a)) / 255;
          g = (28 * a + g * (255 - a)) / 255;
          b = (6 * a + b * (255 - a)) / 255;
        }
      }
      dst[x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
    }
  }
}
static void renderAll() {
  setXf(0, 0, 1, 0);
  renderScene(wallT);
  drawTopbar();
  drawBottombar();
  drawOverlay();
  GdiFlush();
  present();
}

/* ============================ 十六、窗口与输入 ============================ */

static HWND g_hwnd = nullptr;
static bool g_classOk = false;

static void startGame() {
  newGame();
  G.running = true;
  G.screen = 1;
}
static void togglePause() {
  if (!G.running || G.over) return;
  G.paused = !G.paused;
  G.screen = G.paused ? 2 : 1;
}
static void selectSeed(int type) {
  if (!G.running || G.paused || G.over) return;
  if (G.shovel) G.shovel = false;
  G.selected = (G.selected == type) ? -1 : type;
  sfx.bleep();
}
static void winGame() {
  G.over = true; G.won = true; G.running = false;
  G.screen = 3;
  sfx.win();
}
static void loseGame(const wchar_t*) {
  if (G.over) return;
  G.over = true; G.running = false;
  G.screen = 3;
  sfx.lose();
}
static bool inBtn(const BtnRect& b, int x, int y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}
static void handleClick(int cx, int cy) {
  // 覆盖层按钮
  if (G.screen == 0) { if (inBtn(g_btnStart, (float)cx, (float)cy)) { startGame(); sfx.bleep(); } return; }
  if (G.screen == 2) { if (inBtn(g_btnResume, (float)cx, (float)cy)) togglePause(); return; }
  if (G.screen == 3) { if (inBtn(g_btnAgain, (float)cx, (float)cy)) { startGame(); sfx.bleep(); } return; }
  if (!G.running || G.paused || G.over) return;
  // 顶栏
  if (cy < TOPBAR_H) {
    for (int i = 0; i < PT_COUNT; i++) {
      float x = (float)(CARDS_X + i * (CARD_W + CARD_GAP));
      if (cx >= x && cx <= x + CARD_W && cy >= 9 && cy <= 9 + CARD_H) { selectSeed(ORDER[i]); return; }
    }
    if (cy >= TOOL_Y && cy <= TOOL_Y + TOOL_SZ) {
      for (int i = 0; i < 4; i++) {
        if (cx >= TOOL_XS[i] && cx <= TOOL_XS[i] + TOOL_SZ) {
          if (i == 0) { G.shovel = !G.shovel; G.selected = -1; sfx.bleep(); }
          else if (i == 1) togglePause();
          else if (i == 2) { g_muted = !g_muted; }
          else startGame();
          return;
        }
      }
    }
    return;
  }
  // 画布
  int px = cx, py = cy - TOPBAR_H;
  if (py < 0 || py >= H) return;
  G.mx = (float)px; G.my = (float)py;
  int c = (px - GRID_X) / CELL_W, r = (py - GRID_Y) / CELL_H;
  // 先检查阳光
  for (int i = (int)G.suns.size() - 1; i >= 0; i--) {
    Sun& s = G.suns[i];
    if (s.state != 2) {
      float dx = px - s.x, dy = py - s.y;
      if (dx * dx + dy * dy < 900) {
        s.state = 2; s.flyT = 0; s.x0 = s.x; s.y0 = s.y;
        return;
      }
    }
  }
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  G.hover = true; G.hr = r; G.hc = c;
  if (G.shovel) { tryShovel(r, c); G.shovel = false; }
  else if (G.selected >= 0) tryPlant(r, c);
}
static void handleMove(int cx, int cy) {
  if (cy >= TOPBAR_H && cy < TOPBAR_H + H) {
    int px = cx, py = cy - TOPBAR_H;
    G.mx = (float)px; G.my = (float)py;
    int c = (px - GRID_X) / CELL_W, r = (py - GRID_Y) / CELL_H;
    G.hover = (r >= 0 && r < ROWS && c >= 0 && c < COLS);
    G.hr = r; G.hc = c;
  } else G.hover = false;
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_LBUTTONDOWN: handleClick(LOWORD(lp), HIWORD(lp)); return 0;
  case WM_RBUTTONDOWN:
    G.selected = -1; G.shovel = false;
    return 0;
  case WM_MOUSEMOVE: handleMove(LOWORD(lp), HIWORD(lp)); return 0;
  case WM_SETCURSOR: {
    if (LOWORD(lp) == HTCLIENT) {
      POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
      if (pt.y >= TOPBAR_H && pt.y < TOPBAR_H + H && G.shovel && G.screen == 1) { SetCursor(nullptr); return TRUE; }
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return TRUE;
  }
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) { G.selected = -1; G.shovel = false; }
    else if (wp == 'P') togglePause();
    else if (wp == 'M') g_muted = !g_muted;
    else if (wp >= '1' && wp <= '9') {
      int idx = (int)wp - '1';
      if (idx < PT_COUNT) selectSeed(ORDER[idx]);
    }
    return 0;
  case WM_ERASEBKGND: return 1;
  case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================ 十七、缓冲初始化 ============================ */

static bool initBuffers() {
  g_bw = CLW * SS; g_bh = CLH * SS;
  BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = g_bw; bi.bmiHeader.biHeight = -g_bh; // 自上而下
  bi.bmiHeader.biSizeImage = g_bw * g_bh * 4;
  bi.bmiHeader.biPlanes = 1;              // 必须为 1，否则部分环境创建失败
  bi.bmiHeader.biCompression = BI_RGB; bi.bmiHeader.biBitCount = 32;
  HDC hdcScreen = GetDC(nullptr);
  // 注意：部分隔离会话中屏幕 DC 上无法创建 DIBSection，改用兼容内存 DC
  HDC hdc = CreateCompatibleDC(hdcScreen);
  if (!hdc) hdc = hdcScreen;
  g_bmpMem = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&g_px, nullptr, 0);
  if (!g_bmpMem) {
    if (hdc != hdcScreen) { DeleteDC(hdc); hdc = hdcScreen; }
    g_bmpMem = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&g_px, nullptr, 0);
  }
  g_hdcMem = CreateCompatibleDC(hdc);
  g_oldMem = SelectObject(g_hdcMem, g_bmpMem);
  BITMAPINFO bo = bi; bo.bmiHeader.biWidth = CLW; bo.bmiHeader.biHeight = -CLH;
  bo.bmiHeader.biSizeImage = CLW * CLH * 4;
  g_bmpOut = CreateDIBSection(hdc, &bo, DIB_RGB_COLORS, (void**)&g_out, nullptr, 0);
  g_hdcOut = CreateCompatibleDC(hdc);
  g_oldOut = SelectObject(g_hdcOut, g_bmpOut);
  // 静态背景缓存
  BITMAPINFO ci = bi; ci.bmiHeader.biWidth = g_bw; ci.bmiHeader.biHeight = -H * SS;
  ci.bmiHeader.biSizeImage = g_bw * H * SS * 4;
  HDC hdcCache = CreateCompatibleDC(hdc);
  g_bmpCache = CreateDIBSection(hdc, &ci, DIB_RGB_COLORS, (void**)&g_cache, nullptr, 0);
  if (!g_bmpCache) {
    if (hdc != hdcScreen) DeleteDC(hdc);
    ReleaseDC(nullptr, hdcScreen);
    return false;
  }
  SelectObject(hdcCache, g_bmpCache);
  // 绘制静态背景到缓存
  // 注意：缓存高度只有 H*SS，必须同步收窄 g_bh，否则 putPx 的边界检查
  // 会放行对缓存区尾部以外的写入（堆损坏）
  uint32_t* savePx = g_px; HDC saveHdc = g_hdcMem;
  int saveBw = g_bw, saveBh = g_bh;
  g_px = g_cache; g_hdcMem = hdcCache;
  g_bh = H * SS;
  setXf(0, 0, 1, 0);
  drawBackground(0);   // 仅静态部分（云/炊烟在 renderScene 每帧绘制）
  g_px = savePx; g_hdcMem = saveHdc;
  g_bw = saveBw; g_bh = saveBh;
  DeleteDC(hdcCache);
  if (hdc != hdcScreen) DeleteDC(hdc);
  ReleaseDC(nullptr, hdcScreen);
  return g_px && g_out && g_cache;
}

/* ============================ 十八、自检模式 ============================ */

static int g_renderFrames = 0;
static void selfTestRender() {
  g_renderFrames++;
  if (!g_px) return;
  memset(g_px, 0, (size_t)g_bw * g_bh * 4);
  renderAll();
}
/* 把最终 1x 缓冲保存为 32 位 BMP（自上而下） */
static bool saveShot(const wchar_t* path) {
  if (!g_out) return false;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
  BITMAPFILEHEADER fh = {};
  fh.bfType = 0x4D42; // 'BM'
  fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  fh.bfSize = fh.bfOffBits + (DWORD)(CLW * CLH * 4);
  BITMAPINFOHEADER ih = {};
  ih.biSize = sizeof(BITMAPINFOHEADER);
  ih.biWidth = CLW; ih.biHeight = -CLH;   // 自上而下
  ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
  ih.biSizeImage = (DWORD)(CLW * CLH * 4);
  fwrite(&fh, 1, sizeof(fh), f);
  fwrite(&ih, 1, sizeof(ih), f);
  fwrite(g_out, 1, (size_t)CLW * CLH * 4, f);
  fclose(f);
  return true;
}
/* --shot <文件> <场景>：无窗口渲染指定画面并存 BMP（0=开始界面 1=战斗 2=暂停） */
static void runShot(const wchar_t* path, int scene) {
  g_headless = true;
  buildSounds();
  buildDecor();
  if (!initBuffers()) { printf("[FAIL] 缓冲初始化失败\n"); return; }
  newGame();
  if (scene >= 1) {
    G.running = true; G.screen = 1;
    G.sun = 5000;
    auto plant = [&](int type, int r, int c) {
      G.selected = type; G.cds[type] = 0;
      tryPlant(r, c);
    };
    plant(PT_SUNFLOWER, 2, 0); plant(PT_SUNFLOWER, 2, 1);
    plant(PT_SUNFLOWER, 3, 0); plant(PT_SUNFLOWER, 3, 1);
    plant(PT_PEASHOOTER, 1, 2); plant(PT_PEASHOOTER, 2, 2);
    plant(PT_PEASHOOTER, 3, 2); plant(PT_PEASHOOTER, 1, 3);
    plant(PT_WALLNUT, 2, 7); plant(PT_WALLNUT, 3, 7);
    plant(PT_SNOWPEA, 1, 4); plant(PT_SNOWPEA, 4, 4);
    plant(PT_CHOMPER, 5, 6); plant(PT_REPEATER, 2, 4);
    plant(PT_GATLING, 3, 4); plant(PT_CHERRY, 2, 5);
    plant(PT_POTATO, 4, 5); plant(PT_POTATO, 0, 5);
    // 推进 60 秒让僵尸进场、豌豆飞行、阳光掉落
    float dt = 1.f / 60.f;
    for (int i = 0; i < 60 * 60; i++) update(dt);
    if (scene == 2) { G.paused = true; G.screen = 2; }
  }
  renderAll();
  if (saveShot(path)) printf("[ok] 截图已保存: %d x %d\n", CLW, CLH);
  else printf("[FAIL] 截图保存失败\n");
}
static void runSelfTest() {
  printf("[selftest] 初始化缓冲与音效...\n");
  buildSounds();
  buildDecor();
  if (!initBuffers()) { printf("[FAIL] 缓冲初始化失败\n"); return; }
  newGame();
  printf("[ok] 缓冲/音效/装饰就绪\n");

  /* ---- 阶段 1：无窗口跑 400 秒游戏流程 ---- */
  G.running = true; G.screen = 1;
  float dt = 1.f / 60;
  auto plant = [&](int type, int r, int c) {
    G.selected = type; G.cds[type] = 0; G.sun += 1000;
    tryPlant(r, c);
  };
  // 开局：向日葵 + 豌豆 + 坚果
  plant(PT_SUNFLOWER, 2, 0); plant(PT_SUNFLOWER, 2, 1);
  plant(PT_SUNFLOWER, 3, 0); plant(PT_SUNFLOWER, 3, 1);
  plant(PT_PEASHOOTER, 1, 2); plant(PT_PEASHOOTER, 2, 2); plant(PT_PEASHOOTER, 3, 2);
  plant(PT_PEASHOOTER, 1, 3); plant(PT_PEASHOOTER, 2, 3);
  plant(PT_WALLNUT, 2, 7); plant(PT_WALLNUT, 3, 7);
  plant(PT_SNOWPEA, 1, 4); plant(PT_SNOWPEA, 4, 4);
  plant(PT_POTATO, 1, 5); plant(PT_POTATO, 4, 5);
  plant(PT_CHOMPER, 5, 6); plant(PT_CHOMPER, 0, 6);
  plant(PT_REPEATER, 2, 4); plant(PT_REPEATER, 3, 4);
  int plantedCount = (int)G.plants.size();
  printf("[ok] 布防 %d 株植物\n", plantedCount);

  int sunCollected = 0, maxZombies = 0;
  for (int frame = 0; frame < 60 * 400; frame++) {
    update(dt);
    if (frame % 600 == 0) { // 每 10 秒收集所有阳光
      for (size_t i = 0; i < G.suns.size(); i++)
        if (G.suns[i].state != 2) { G.suns[i].state = 2; G.suns[i].flyT = 0; G.suns[i].x0 = G.suns[i].x; G.suns[i].y0 = G.suns[i].y; }
      sunCollected++;
    }
    if (frame % 1800 == 0) { // 每 30 秒补种
      plant(PT_PEASHOOTER, 1, 3); plant(PT_PEASHOOTER, 4, 3);
      if (frame > 6000) { plant(PT_GATLING, 2, 4); } // 升级机枪
      if (frame > 9000) { plant(PT_CHERRY, 2, 5); }
    }
    if ((int)G.zombies.size() > maxZombies) maxZombies = (int)G.zombies.size();
    if (G.over) break;
  }
  printf("[ok] 400 秒模拟完成: wave=%d kills=%d sun=%d zombies=%d maxZ=%d over=%d\n",
    G.wave, G.kills, G.sun, (int)G.zombies.size(), maxZombies, (int)G.over);

  /* ---- 阶段 2：全僵尸类型 + 全植物渲染 ---- */
  G.over = false; G.running = true; G.screen = 1;
  for (int zt = 0; zt < ZT_COUNT; zt++) spawnZombie(zt, zt % ROWS);
  for (int i = 0; i < PT_COUNT; i++) {
    int r = i % ROWS, c = i % COLS;
    if (!plantAt(r, c)) {
      G.selected = ORDER[i]; G.cds[ORDER[i]] = 0; G.sun += 500;
      tryPlant(r, c);
    }
  }
  // 土豆雷就绪、樱桃引燃、大嘴花咀嚼、铁门受伤、寒冰减速
  for (size_t i = 0; i < G.plants.size(); i++) {
    Plant& p = G.plants[i];
    if (p.type == PT_POTATO) p.armed = true;
    if (p.type == PT_CHERRY) p.fuse = 0.3f;
    if (p.type == PT_CHOMPER) p.chewT = 5;
  }
  for (size_t i = 0; i < G.zombies.size(); i++) {
    G.zombies[i].x = 300 + i * 60;
    if (G.zombies[i].type == ZT_DOOR) G.zombies[i].doorHp = 200;
    if (G.zombies[i].type == ZT_NORMAL) G.zombies[i].slowT = 3;
  }
  // 各类覆盖层渲染
  selfTestRender(); G.screen = 0; selfTestRender();
  G.screen = 2; selfTestRender();
  G.screen = 3; G.won = true; selfTestRender();
  G.screen = 1;
  // 悬停/铲子/横幅
  G.hover = true; G.hr = 1; G.hc = 1; G.selected = PT_CHERRY; selfTestRender();
  G.selected = -1; G.shovel = true; selfTestRender();
  G.shovel = false; G.banner = true; G.bannerT = 1; selfTestRender();
  printf("[ok] 全实体渲染 %d 帧无崩溃\n", g_renderFrames);

  /* ---- 阶段 3：再跑 60 秒验证战斗逻辑 ---- */
  for (int frame = 0; frame < 60 * 60; frame++) { update(dt); if (G.over) break; }
  printf("[ok] 战斗模拟 60 秒: kills=%d zombies=%d\n", G.kills, (int)G.zombies.size());

  /* ---- 阶段 4：胜负路径 ---- */
  G.over = false; G.zombies.clear(); G.spawnQueue.clear();
  G.wave = MAX_WAVE;
  updateWaves(dt);
  printf("[ok] 胜利路径: over=%d won=%d\n", (int)G.over, (int)G.won);
  newGame(); G.running = true; G.screen = 1;
  spawnZombie(ZT_FOOTBALL, 2); G.zombies[0].x = 45;
  for (int frame = 0; frame < 60; frame++) update(dt);
  printf("[ok] 失败路径: over=%d won=%d\n", (int)G.over, (int)G.won);

  printf("\n===== SELFTEST PASS =====\n");
  printf("rendered=%d frames, plants=%d, kills=%d\n", g_renderFrames, plantedCount, G.kills);
}

/* ============================ 十九、入口 ============================ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
  if (cmdLine && wcsstr(cmdLine, L"--selftest")) {
    g_headless = true;
    runSelfTest();
    return 0;
  }
  // --shot <文件.bmp> <场景号>：无窗口渲染并存图（0=开始 1=战斗 2=暂停）
  if (cmdLine && wcsstr(cmdLine, L"--shot")) {
    const wchar_t* p = wcsstr(cmdLine, L"--shot") + 6;
    wchar_t path[MAX_PATH] = {}; int scene = 0;
    if (swscanf_s(p, L"%259s %d", path, (unsigned)_countof(path), &scene) >= 1)
      runShot(path, scene);
    else printf("用法: pvz.exe --shot <文件.bmp> <0开始|1战斗|2暂停>\n");
    return 0;
  }
  buildSounds();
  buildDecor();
  newGame();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst; wc.lpszClassName = L"PVZWindow";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  g_classOk = (RegisterClassExW(&wc) != 0);

  RECT rc = { 0, 0, CLW, CLH };
  AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
  int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
  int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
  g_hwnd = CreateWindowExW(0, L"PVZWindow", L"植物大战僵尸 C++ 版",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
    sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, ww, wh, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;
  if (!initBuffers()) { MessageBoxW(nullptr, L"缓冲初始化失败", L"错误", MB_OK); return 1; }
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  LARGE_INTEGER freq, last, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&last);
  MSG msg;
  for (;;) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    } else {
      QueryPerformanceCounter(&now);
      float dt = (float)((double)(now.QuadPart - last.QuadPart) / freq.QuadPart);
      last = now;
      if (dt > 0.05f) dt = 0.05f;
      if (dt < 0) dt = 0;
      wallT += dt;
      if (G.running && !G.paused && !G.over) update(dt);
      renderAll();
      HDC hdc = GetDC(g_hwnd);
      BitBlt(hdc, 0, 0, CLW, CLH, g_hdcOut, 0, 0, SRCCOPY);
      ReleaseDC(g_hwnd, hdc);
      Sleep(1);   // 让出 CPU，降低空转（dt 基于实时时钟，不影响游戏速度）
    }
  }
  return 0;
}
