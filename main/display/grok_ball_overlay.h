#pragma once

// Grok Ball style overlay for EmoteDisplay.
//
// The browser grok-ball engine is SVG/JS and cannot run on the ESP32, and EAF
// frames use a private "ZZ_S" compression that we cannot generate offline.
// Instead we reuse the engine's real eye-ring geometry (grok_ball_data.h) and
// repaint the emote band in the flush callback: black background, a white ball
// outline and white eyes selected by the current emotion. The status bar on
// top and the caption strip at the bottom are left untouched so the text and
// icons keep rendering normally.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "grok_ball_data.h"

namespace emote {

constexpr int kGrokScreenW = 320;
constexpr int kGrokScreenH = 240;
constexpr int kGrokBandTop = 46;      // keep top status bar / icons / clock
constexpr int kGrokBandBottom = 216;  // keep the subtitle strip (y 223..230)
constexpr int kGrokCX = 160;
constexpr int kGrokCY = 130;
constexpr int kGrokRadius = 78;
constexpr float kGrokScale = 156.0f / 229.0f;
constexpr float kGrokViewCenter = 114.2705f;
// Kept at 0: the source rings already sit correctly inside the ball. A larger
// offset pushes the long "happy"/"laughing" rings past the bottom outline.
constexpr int kGrokEyeYOffset = 0;

static std::atomic<int> s_grok_ring{0};
static std::atomic<bool> s_grok_mask_ready{false};
static uint8_t s_grok_mask[(kGrokScreenW * kGrokScreenH + 7) / 8];

inline void GrokSetPixel(int x, int y) {
    if (x < 0 || x >= kGrokScreenW || y < 0 || y >= kGrokScreenH) {
        return;
    }
    int idx = y * kGrokScreenW + x;
    s_grok_mask[idx >> 3] |= static_cast<uint8_t>(1 << (idx & 7));
}

inline void GrokDot(int x, int y, int r) {
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                GrokSetPixel(x + dx, y + dy);
            }
        }
    }
}

inline void GrokLine(int x0, int y0, int x1, int y1, int r) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int steps = dx > dy ? dx : dy;
    if (steps == 0) {
        GrokDot(x0, y0, r);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        GrokDot(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, r);
    }
}

inline void GrokCircle(int cx, int cy, int radius, int r) {
    int steps = static_cast<int>(2 * 3.1415926f * radius);
    for (int i = 0; i < steps; ++i) {
        float a = 2 * 3.1415926f * i / steps;
        GrokDot(cx + static_cast<int>(radius * std::cos(a)),
                cy + static_cast<int>(radius * std::sin(a)), r);
    }
}

inline void GrokRebuild(int ring) {
    if (ring < 0 || ring >= kGrokRingCount) {
        ring = 0;
    }
    memset(s_grok_mask, 0, sizeof(s_grok_mask));
    GrokCircle(kGrokCX, kGrokCY, kGrokRadius, 1);

    const GrokRing& g = kGrokRings[ring];
    for (int eye = 0; eye < 2; ++eye) {
        const GrokPoint* pts = eye == 0 ? g.left : g.right;
        for (int i = 0; i < 48; ++i) {
            int j = (i + 1) % 48;
            int ax = kGrokCX + static_cast<int>((pts[i].x / 10.0f - kGrokViewCenter) * kGrokScale);
            int ay = kGrokCY + static_cast<int>((pts[i].y / 10.0f - kGrokViewCenter) * kGrokScale) + kGrokEyeYOffset;
            int bx = kGrokCX + static_cast<int>((pts[j].x / 10.0f - kGrokViewCenter) * kGrokScale);
            int by = kGrokCY + static_cast<int>((pts[j].y / 10.0f - kGrokViewCenter) * kGrokScale) + kGrokEyeYOffset;
            GrokLine(ax, ay, bx, by, 1);
        }
    }
    s_grok_ring.store(ring, std::memory_order_relaxed);
    s_grok_mask_ready.store(true, std::memory_order_relaxed);
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
    if (ring >= 0 && ring != s_grok_ring.load(std::memory_order_relaxed)) {
        GrokRebuild(ring);
    }
}

// Repaint one dirty rectangle before it goes to the panel. The mask is a plain
// 1bpp lookup, so this stays cheap even when the emote engine flushes at 30fps.
inline void GrokApplyToBlock(int x0, int y0, int x1, int y1, const void* data) {
    if (!s_grok_mask_ready.load(std::memory_order_relaxed)) {
        GrokRebuild(0);
    }
    uint16_t* buf = static_cast<uint16_t*>(const_cast<void*>(data));
    int w = x1 - x0;
    int h = y1 - y0;
    for (int y = 0; y < h; ++y) {
        int gy = y0 + y;
        if (gy < kGrokBandTop || gy >= kGrokBandBottom) {
            continue;
        }
        int row = gy * kGrokScreenW;
        for (int x = 0; x < w; ++x) {
            int gx = x0 + x;
            if (gx < 0 || gx >= kGrokScreenW) {
                continue;
            }
            int idx = row + gx;
            bool on = (s_grok_mask[idx >> 3] >> (idx & 7)) & 1;
            buf[y * w + x] = on ? 0xFFFF : 0x0000;
        }
    }
}

}  // namespace emote
