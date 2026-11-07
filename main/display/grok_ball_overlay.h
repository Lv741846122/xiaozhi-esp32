#pragma once

// Grok Ball style overlay for EmoteDisplay.
//
// The browser grok-ball engine is SVG/JS and cannot run on the ESP32, and EAF
// frames use a private "ZZ_S" compression that we cannot generate offline.
// We reuse the engine's real 25 eye-ring contours (grok_ball_data.h) and
// repaint the middle band in the flush callback.
//
// Two 1bpp masks are kept: a static ball outline and the solid eyes. The eyes
// are sampled through a time-varying vertical transform on every flush, which
// gives breathing, blinking and a speaking rhythm without ever re-running the
// polygon fill in the render loop. The status bar and caption strip are left
// untouched so text keeps rendering normally.

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <esp_timer.h>

#include "grok_ball_data.h"

namespace emote {

// Portrait comparison build (native ILI9341 240x320).
// For landscape use 320/240, band 46..200, centre (160,123), radius 72,
// scale 144/229 and eyeYOffset 10.
constexpr int kGrokScreenW = 240;
constexpr int kGrokScreenH = 320;
constexpr int kGrokBandTop = 46;      // keep top status bar / icons / clock
constexpr int kGrokBandBottom = 272;  // keep the caption strip below
constexpr int kGrokCX = 120;
constexpr int kGrokCY = 154;
constexpr int kGrokRadius = 100;
constexpr float kGrokScale = 200.0f / 229.0f;
constexpr float kGrokViewCenter = 114.2705f;
constexpr int kGrokEyeYOffset = 0;
constexpr int kGrokMaskBytes = (kGrokScreenW * kGrokScreenH + 7) / 8;

static std::atomic<int> s_grok_ring{0};
static std::atomic<bool> s_grok_mask_ready{false};
static std::atomic<bool> s_grok_active{false};
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

inline void GrokCircle(uint8_t* mask, int cx, int cy, int radius, int r) {
    int steps = static_cast<int>(2 * 3.1415926f * radius);
    for (int i = 0; i < steps; ++i) {
        float a = 2 * 3.1415926f * i / steps;
        GrokDot(mask, cx + static_cast<int>(radius * std::cos(a)),
                cy + static_cast<int>(radius * std::sin(a)), r);
    }
}

// Even-odd point-in-polygon test, used to fill the 48-point eye rings so the
// eyes are solid white shapes instead of thin outlines.
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
        ys[i] = kGrokCY + static_cast<int>((pts[i].y / 10.0f - kGrokViewCenter) * kGrokScale) + kGrokEyeYOffset;
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
        // The server sends emotion names the EAF pack does not carry
        // (e.g. "robot_2"). Match keywords first, then a stable hash so
        // every distinct emotion still changes the face.
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

// Time-driven animation. Cheap: two integer params reused for the whole block.
inline void GrokFramePose(int64_t now_us, int* bob, int* eye_scale) {
    // Quantise to 40ms: a full frame is flushed as many 16-row blocks, and
    // every block must use the same pose or the face tears horizontally.
    int t_ms = static_cast<int>(now_us / 40000) * 40;
    // Breathing: one cycle every 3.2s, eyes drift +/-3px.
    float phase = static_cast<float>(t_ms % 3200) / 3200.0f * 6.2831853f;
    *bob = static_cast<int>(3.0f * std::sin(phase));
    // Idle blink: 130ms every 4.2s, eyes squash to a thin line. Start at
    // 2s so the very first frame after boot is not a blink.
    *eye_scale = 100;
    int blink_phase = t_ms % 4200;
    if (blink_phase >= 2000 && blink_phase < 2130) {
        *eye_scale = 12;
    }
    // While listening or speaking: faster open/close rhythm ("talking").
    if (s_grok_active.load(std::memory_order_relaxed)) {
        *eye_scale = (t_ms % 300 < 150) ? 55 : 100;
    }
}

// Repaint one dirty rectangle before it goes to the panel.
inline void GrokApplyToBlock(int x0, int y0, int x1, int y1, const void* data) {
    if (!s_grok_mask_ready.load(std::memory_order_relaxed)) {
        GrokRebuild(0);
    }
    int bob = 0, eye_scale = 100;
    GrokFramePose(esp_timer_get_time(), &bob, &eye_scale);

    uint16_t* buf = static_cast<uint16_t*>(const_cast<void*>(data));
    int w = x1 - x0;
    int h = y1 - y0;
    for (int y = 0; y < h; ++y) {
        int gy = y0 + y;
        if (gy < kGrokBandTop || gy >= kGrokBandBottom) {
            continue;
        }
        // Inverse of the squash transform: sampling y = CY + (screenY - CY)/s
        // compresses the eyes toward CY. Multiplying by s instead would smear
        // the eye centre column into a full-height white bar.
        int dy = gy - kGrokCY;
        int eye_y = kGrokCY + dy * 100 / eye_scale - bob;
        for (int x = 0; x < w; ++x) {
            int gx = x0 + x;
            if (gx < 0 || gx >= kGrokScreenW) {
                continue;
            }
            bool on = GrokGetPixel(s_ball_mask, gx, gy) ||
                      GrokGetPixel(s_eye_mask, gx, eye_y);
            buf[y * w + x] = on ? 0xFFFF : 0x0000;
        }
    }
}

}  // namespace emote
