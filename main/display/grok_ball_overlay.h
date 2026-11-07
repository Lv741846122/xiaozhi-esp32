#pragma once

// Grok Ball overlay for EmoteDisplay, portrait 240x320.
//
// Screen is split into three bands, all driven from the flush callback:
//   y [0, 34)   fixed status bar: speaker / time+temperature / battery
//   y [40, 258) black stage with the white ball and animated solid eyes
//   y [258,320) untouched - the EAF caption label renders here
//
// Three 1bpp masks are kept: status bar, static ball outline, solid eyes.
// The animation timer computes one pose per frame and stores it; every flush
// block reads that cached pose so a frame never tears.

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_timer.h>

#include "grok_ball_data.h"

namespace emote {

constexpr int kGrokScreenW = 240;
constexpr int kGrokScreenH = 320;
constexpr int kGrokStatusH = 34;      // fixed status bar
constexpr int kGrokBandTop = 40;      // small gap below the status bar
constexpr int kGrokBandBottom = 258;  // caption strip starts here
constexpr int kGrokCX = 120;
constexpr int kGrokCY = 166;          // ball centre moved down
constexpr int kGrokRadius = 88;
constexpr float kGrokScale = 176.0f / 229.0f;
constexpr float kGrokViewCenter = 114.2705f;
constexpr int kGrokMaskBytes = (kGrokScreenW * kGrokScreenH + 7) / 8;
constexpr int kStatusMaskBytes = (kGrokScreenW * kGrokStatusH + 7) / 8;

static std::atomic<int> s_grok_ring{0};
static std::atomic<bool> s_grok_mask_ready{false};
static std::atomic<bool> s_grok_active{false};
// One pose per frame, cached so all 16-row flush blocks agree.
static std::atomic<int> s_pose_bob{0};
static std::atomic<int> s_pose_eye_scale{100};
static std::atomic<int> s_pose_gaze_x{0};
static std::atomic<int> s_pose_ball_scale{100};
// The status text is rebuilt once a second from the timer task while the
// render task reads it. Double buffer + atomic index avoids a torn frame.
static uint8_t s_status_mask[2][kStatusMaskBytes];
static std::atomic<int> s_status_front{0};
static uint8_t s_ball_mask[kGrokMaskBytes];
static uint8_t s_eye_mask[kGrokMaskBytes];

inline void GrokSetPixel(uint8_t* mask, int x, int y) {
    if (x < 0 || x >= kGrokScreenW || y < 0 || y >= kGrokScreenH) {
        return;
    }
    int idx = y * kGrokScreenW + x;
    mask[idx >> 3] |= static_cast<uint8_t>(1 << (idx & 7));
}

inline bool GrokGetPixel(const uint8_t* mask, int x, int y) {
    if (x < 0 || x >= kGrokScreenW || y < 0 || y >= kGrokScreenH) {
        return false;
    }
    int idx = y * kGrokScreenW + x;
    return (mask[idx >> 3] >> (idx & 7)) & 1;
}

inline void GrokDot(uint8_t* mask, int x, int y, int r) {
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                GrokSetPixel(mask, x + dx, y + dy);
            }
        }
    }
}

inline void GrokLine(uint8_t* mask, int x0, int y0, int x1, int y1, int r) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int steps = dx > dy ? dx : dy;
    if (steps == 0) {
        GrokDot(mask, x0, y0, r);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        GrokDot(mask, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, r);
    }
}

// ---- tiny 5x7 ASCII font, drawn at 2x (10x14 px per glyph) ----------------
struct GrokGlyph {
    char c;
    uint8_t rows[7];
};
static const GrokGlyph kGrokFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
};
constexpr int kGrokGlyphW = 12;  // 10px glyph + 2px spacing

inline void GrokDrawChar(uint8_t* mask, char c, int x, int y) {
    const GrokGlyph* g = nullptr;
    for (const auto& item : kGrokFont) {
        if (item.c == c) {
            g = &item;
            break;
        }
    }
    if (g == nullptr) {
        return;
    }
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((g->rows[row] >> (4 - col)) & 1) {
                for (int sy = 0; sy < 2; ++sy) {
                    for (int sx = 0; sx < 2; ++sx) {
                        GrokSetPixel(mask,x + col * 2 + sx, y + row * 2 + sy);
                    }
                }
            }
        }
    }
}

inline void GrokDrawText(uint8_t* mask, const char* text, int x, int y) {
    for (const char* p = text; *p; ++p) {
        GrokDrawChar(mask, *p, x, y);
        x += kGrokGlyphW;
    }
}

inline void GrokDrawSpeaker(uint8_t* mask, int x, int y) {
    // body
    for (int yy = y + 4; yy <= y + 10; ++yy) {
        for (int xx = x; xx <= x + 4; ++xx) {
            GrokSetPixel(mask,xx, yy);
        }
    }
    // cone
    for (int i = 0; i <= 8; ++i) {
        int half = i / 2;
        for (int d = -half; d <= half; ++d) {
            GrokSetPixel(mask,x + 5 + i, y + 7 + d);
        }
    }
    // two sound arcs
    for (int i = 0; i <= 6; ++i) {
        GrokSetPixel(mask,x + 16 + (i / 3), y + 2 + i);
        GrokSetPixel(mask,x + 16 + (i / 3), y + 12 - i);
    }
}

inline void GrokDrawBattery(uint8_t* mask, int x, int y, int percent, bool charging) {
    const int w = 17, h = 10;
    for (int i = 0; i < w; ++i) {
        GrokSetPixel(mask,x + i, y);
        GrokSetPixel(mask,x + i, y + h);
    }
    for (int i = 0; i <= h; ++i) {
        GrokSetPixel(mask,x, y + i);
        GrokSetPixel(mask,x + w, y + i);
    }
    for (int i = 2; i <= 5; ++i) {
        GrokSetPixel(mask,x + w + 1, y + i);
        GrokSetPixel(mask,x + w + 2, y + i);
    }
    int fill = (w - 4) * (percent < 0 ? 0 : (percent > 100 ? 100 : percent)) / 100;
    for (int i = 0; i < fill; ++i) {
        for (int j = 2; j < h - 1; ++j) {
            GrokSetPixel(mask,x + 2 + i, y + j);
        }
    }
    if (charging) {
        // small plus sign in the middle
        for (int i = 3; i <= 6; ++i) {
            GrokSetPixel(mask,x + 8, y + i);
            GrokSetPixel(mask,x + 6 + i, y + 5);
        }
    }
}

inline void GrokSetStatus(int hour, int minute, int temp_c, int battery, bool charging) {
    int back = 1 - s_status_front.load(std::memory_order_relaxed);
    uint8_t* mask = s_status_mask[back];
    memset(mask, 0, kStatusMaskBytes);
    GrokDrawSpeaker(mask, 10, 8);
    char mid[20];
    snprintf(mid, sizeof(mid), "%02d:%02d %dC", hour, minute, temp_c);
    GrokDrawText(mask, mid, (kGrokScreenW - static_cast<int>(strlen(mid)) * kGrokGlyphW) / 2, 10);
    GrokDrawBattery(mask, 184, 12, battery, charging);
    char bat[8];
    snprintf(bat, sizeof(bat), "%d", battery);
    GrokDrawText(mask, bat, 206, 10);
    s_status_front.store(back, std::memory_order_relaxed);
}

// ---- ball / eyes -----------------------------------------------------------
inline void GrokCircle(uint8_t* mask, int cx, int cy, int radius, int r) {
    int steps = static_cast<int>(2 * 3.1415926f * radius);
    for (int i = 0; i < steps; ++i) {
        float a = 2 * 3.1415926f * i / steps;
        GrokDot(mask, cx + static_cast<int>(radius * std::cos(a)),
                cy + static_cast<int>(radius * std::sin(a)), r);
    }
}

inline bool GrokInsidePolygon(float x, float y, const int* px, const int* py, int n) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float yi = static_cast<float>(py[i]);
        float yj = static_cast<float>(py[j]);
        if ((yi > y) != (yj > y)) {
            float cross = static_cast<float>(px[j] - px[i]) * (y - yi) / (yj - yi) + px[i];
            if (x < cross) {
                inside = !inside;
            }
        }
    }
    return inside;
}

inline void GrokFillEye(uint8_t* mask, const GrokPoint* pts) {
    int xs[48], ys[48];
    int minx = 10000, maxx = -10000, miny = 10000, maxy = -10000;
    for (int i = 0; i < 48; ++i) {
        xs[i] = kGrokCX + static_cast<int>((pts[i].x / 10.0f - kGrokViewCenter) * kGrokScale);
        ys[i] = kGrokCY + static_cast<int>((pts[i].y / 10.0f - kGrokViewCenter) * kGrokScale);
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    for (int y = miny; y <= maxy; ++y) {
        if (y < kGrokBandTop || y >= kGrokBandBottom) {
            continue;
        }
        for (int x = minx; x <= maxx; ++x) {
            if (x < 0 || x >= kGrokScreenW) {
                continue;
            }
            if (GrokInsidePolygon(x + 0.5f, y + 0.5f, xs, ys, 48)) {
                GrokSetPixel(mask, x, y);
            }
        }
    }
}

inline void GrokRebuild(int ring) {
    if (ring < 0 || ring >= kGrokRingCount) {
        ring = 0;
    }
    memset(s_ball_mask, 0, sizeof(s_ball_mask));
    memset(s_eye_mask, 0, sizeof(s_eye_mask));
    GrokCircle(s_ball_mask, kGrokCX, kGrokCY, kGrokRadius, 1);
    const GrokRing& g = kGrokRings[ring];
    GrokFillEye(s_eye_mask, g.left);
    GrokFillEye(s_eye_mask, g.right);
    s_grok_ring.store(ring, std::memory_order_relaxed);
    s_grok_mask_ready.store(true, std::memory_order_relaxed);
}

inline void GrokSetActive(bool active) {
    s_grok_active.store(active, std::memory_order_relaxed);
}

inline int GrokRingForName(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int i = 0; i < kGrokEmotionMapCount; ++i) {
        if (strcmp(name, kGrokEmotionMap[i].name) == 0) {
            return kGrokEmotionMap[i].ring;
        }
    }
    return -1;
}

inline void GrokSetEmotion(const char* name) {
    int ring = GrokRingForName(name);
    if (ring < 0) {
        static const struct { const char* key; int ring; } kHints[] = {
            {"happy", 2}, {"laugh", 11}, {"joy", 19}, {"smile", 2},
            {"sad", 4}, {"cry", 22}, {"sleep", 13}, {"tired", 13},
            {"angry", 7}, {"mad", 7}, {"surpris", 3}, {"shock", 21},
            {"think", 15}, {"confus", 8}, {"love", 24}, {"shy", 24},
            {"wink", 14}, {"silly", 5}, {"neutral", 0}, {"idle", 8},
        };
        char lower[32] = {0};
        if (name != nullptr) {
            strncpy(lower, name, sizeof(lower) - 1);
            for (char* p = lower; *p; ++p) {
                *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
            }
        }
        for (const auto& hint : kHints) {
            if (strstr(lower, hint.key) != nullptr) {
                ring = hint.ring;
                break;
            }
        }
        if (ring < 0) {
            uint32_t hash = 0;
            for (const char* p = name ? name : ""; *p; ++p) {
                hash = hash * 31 + static_cast<unsigned char>(*p);
            }
            ring = static_cast<int>(hash % kGrokRingCount);
        }
    }
    if (ring != s_grok_ring.load(std::memory_order_relaxed)) {
        GrokRebuild(ring);
    }
}

// Called once per animation tick. Everything is continuous; no square waves.
inline void GrokUpdatePose(int64_t now_us) {
    int t_ms = static_cast<int>(now_us / 1000);
    constexpr float kPi2 = 6.2831853f;

    // Breathing: eyes drift and squash gently, the ball itself pulses too.
    float breath = std::sin(static_cast<float>(t_ms % 2800) / 2800.0f * kPi2);
    int bob = static_cast<int>(3.0f * breath);
    int scale = 94 + static_cast<int>(6.0f * breath);
    int ball_scale = 100 + static_cast<int>(2.0f * breath);

    // Gaze: two incommensurate sines so it never looks metronomic, plus an
    // occasional quick glance to one side.
    float gaze = 8.0f * std::sin(static_cast<float>(t_ms % 2100) / 2100.0f * kPi2) +
                 4.0f * std::sin(static_cast<float>(t_ms % 830) / 830.0f * kPi2);
    int glance = t_ms % 6700;
    if (glance < 320) {
        gaze += 14.0f * std::sin(static_cast<float>(glance) / 320.0f * 3.1415926f);
    }
    int gaze_x = static_cast<int>(gaze);

    // Two overlapping blink periods make blinks feel spontaneous rather than
    // exactly every 5s. Skip the first 2.5s so boot never starts mid-blink.
    int blink_a = t_ms % 5000;
    int blink_b = t_ms % 7300;
    if (t_ms > 2500 && blink_a < 300) {
        int bs = 100 - static_cast<int>(80.0f * std::sin(static_cast<float>(blink_a) / 300.0f * 3.1415926f));
        if (bs < scale) scale = bs;
    } else if (t_ms > 2500 && blink_b < 260) {
        int bs = 100 - static_cast<int>(68.0f * std::sin(static_cast<float>(blink_b) / 260.0f * 3.1415926f));
        if (bs < scale) scale = bs;
    }

    // Talking: wider squashing and a faster gaze sweep.
    if (s_grok_active.load(std::memory_order_relaxed)) {
        float sp = std::sin(static_cast<float>(t_ms % 480) / 480.0f * kPi2);
        int talk = 82 + static_cast<int>(18.0f * sp);
        if (talk < scale) scale = talk;
        gaze_x += static_cast<int>(4.0f * std::sin(static_cast<float>(t_ms % 300) / 300.0f * kPi2));
    }
    if (scale < 20) scale = 20;
    s_pose_bob.store(bob, std::memory_order_relaxed);
    s_pose_eye_scale.store(scale, std::memory_order_relaxed);
    s_pose_gaze_x.store(gaze_x, std::memory_order_relaxed);
    s_pose_ball_scale.store(ball_scale, std::memory_order_relaxed);
}

inline void GrokApplyToBlock(int x0, int y0, int x1, int y1, const void* data) {
    if (!s_grok_mask_ready.load(std::memory_order_relaxed)) {
        GrokRebuild(0);
        GrokUpdatePose(esp_timer_get_time());
        GrokSetStatus(0, 0, 0, 100, false);
    }
    int bob = s_pose_bob.load(std::memory_order_relaxed);
    int eye_scale = s_pose_eye_scale.load(std::memory_order_relaxed);
    int gaze_x = s_pose_gaze_x.load(std::memory_order_relaxed);
    int ball_scale = s_pose_ball_scale.load(std::memory_order_relaxed);

    uint16_t* buf = static_cast<uint16_t*>(const_cast<void*>(data));
    int w = x1 - x0;
    int h = y1 - y0;
    for (int y = 0; y < h; ++y) {
        int gy = y0 + y;
        bool in_status = gy < kGrokStatusH;
        bool in_gap = gy >= kGrokStatusH && gy < kGrokBandTop;
        bool in_stage = gy >= kGrokBandTop && gy < kGrokBandBottom;
        if (!in_status && !in_gap && !in_stage) {
            continue;  // caption strip: leave the EAF label untouched
        }
        int dy = gy - kGrokCY;
        int ball_y = kGrokCY + dy * 100 / ball_scale;
        int eye_y = kGrokCY + dy * 100 / eye_scale - bob;
        for (int x = 0; x < w; ++x) {
            int gx = x0 + x;
            if (gx < 0 || gx >= kGrokScreenW) {
                continue;
            }
            bool on = false;
            if (in_status) {
                on = GrokGetPixel(s_status_mask[s_status_front.load(std::memory_order_relaxed)], gx, gy);
            } else if (in_stage) {
                int ball_x = kGrokCX + (gx - kGrokCX) * 100 / ball_scale;
                on = GrokGetPixel(s_ball_mask, ball_x, ball_y);
                if (!on) {
                    on = GrokGetPixel(s_eye_mask, gx - gaze_x, eye_y);
                }
            }
            buf[y * w + x] = on ? 0xFFFF : 0x0000;
        }
    }
}

}  // namespace emote
