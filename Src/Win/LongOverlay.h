#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

// Detects compact, high-contrast UI elements that stay at the same screen position while the
// underlying long-capture body moves. The tracker is intentionally limited to the left/right
// edge zones so ordinary page content and fixed wallpapers are not broadly rewritten.
class LongOverlayTracker
{
public:
    struct ObserveStats
    {
        int candidateBlocks{ 0 };
        int acceptedComponents{ 0 };
        int currentBlocks{ 0 };
        int persistentBlocks{ 0 };
    };

    LongOverlayTracker() = default;
    LongOverlayTracker(int width, int height) { reset(width, height); }

    void reset(int width, int height)
    {
        w = width;
        h = height;
        cols = (w + blockSize - 1) / blockSize;
        rows = (h + blockSize - 1) / blockSize;
        edgePx = std::clamp(w * 14 / 100, 40, std::max(40, std::min(w / 3, 180)));
        votes.assign(static_cast<size_t>(cols) * rows, 0);
        current.assign(static_cast<size_t>(cols) * rows, 0);
        persistent.assign(static_cast<size_t>(cols) * rows, 0);
    }

    ObserveStats observe(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        int offset, int bodyTop, int bodyBottom)
    {
        ObserveStats stats;
        if (w <= 0 || h <= 0 || offset <= 0 || oldFrame.size() != newFrame.size()) return stats;
        if (oldFrame.size() < static_cast<size_t>(w) * h * 4) return stats;

        bodyTop = std::clamp(bodyTop, 0, h);
        bodyBottom = std::clamp(bodyBottom, bodyTop, h);
        std::fill(current.begin(), current.end(), 0);
        std::vector<uint8_t> seed(current.size(), 0);

        for (int by = bodyTop / blockSize; by <= (bodyBottom - 1) / blockSize; ++by) {
            int y0 = std::max(bodyTop, by * blockSize);
            int y1 = std::min(bodyBottom, y0 + blockSize);
            if (y0 + offset >= bodyBottom) continue;
            y1 = std::min(y1, bodyBottom - offset);
            if (y1 <= y0) continue;

            for (int bx = 0; bx < cols; ++bx) {
                int x0 = bx * blockSize;
                int x1 = std::min(w, x0 + blockSize);
                bool inLeft = x0 < edgePx;
                bool inRight = x1 > w - edgePx;
                if (!inLeft && !inRight) continue;

                BlockMetric m = metric(oldFrame, newFrame, x0, x1, y0, y1, offset);
                // A fixed overlay should be almost unchanged at the same screen coordinates,
                // disagree with the scroll-aligned pixels, and contain enough local contrast to
                // avoid treating flat/faint wallpaper as a widget.
                if (m.sameMad <= 5.0 && m.scrollMad >= 18.0
                    && m.scrollMad - m.sameMad >= 12.0 && m.contrast >= 45.0) {
                    seed[index(bx, by)] = 1;
                    ++stats.candidateBlocks;
                }
            }
        }

        // If a whole edge becomes a field of candidates, it is more likely a fixed background or
        // sidebar texture than compact floating controls. Do not sanitize that side in this pass.
        suppressDenseSide(seed, true, bodyTop, bodyBottom);
        suppressDenseSide(seed, false, bodyTop, bodyBottom);

        std::vector<uint8_t> visited(seed.size(), 0);
        static constexpr int dx[4]{ 1, -1, 0, 0 };
        static constexpr int dy[4]{ 0, 0, 1, -1 };

        for (int by = 0; by < rows; ++by) {
            for (int bx = 0; bx < cols; ++bx) {
                size_t start = index(bx, by);
                if (!seed[start] || visited[start]) continue;

                std::queue<std::pair<int, int>> q;
                std::vector<std::pair<int, int>> component;
                visited[start] = 1;
                q.push({ bx, by });
                int minBx = bx, maxBx = bx, minBy = by, maxBy = by;

                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop();
                    component.push_back({ cx, cy });
                    minBx = std::min(minBx, cx);
                    maxBx = std::max(maxBx, cx);
                    minBy = std::min(minBy, cy);
                    maxBy = std::max(maxBy, cy);
                    for (int k = 0; k < 4; ++k) {
                        int nx = cx + dx[k], ny = cy + dy[k];
                        if (nx < 0 || nx >= cols || ny < 0 || ny >= rows) continue;
                        size_t ni = index(nx, ny);
                        if (!seed[ni] || visited[ni]) continue;
                        visited[ni] = 1;
                        q.push({ nx, ny });
                    }
                }

                int pixelW = (maxBx - minBx + 1) * blockSize;
                int pixelH = (maxBy - minBy + 1) * blockSize;
                bool compact = pixelW <= 120 && pixelH <= 136 && component.size() <= 90;
                if (!compact) continue;

                ++stats.acceptedComponents;
                // Grow by one block to cover the low-texture interior of a button whose edge was
                // the actual high-contrast seed.
                for (const auto& [cx, cy] : component) {
                    for (int yy = cy - 1; yy <= cy + 1; ++yy) {
                        for (int xx = cx - 1; xx <= cx + 1; ++xx) {
                            if (xx < 0 || xx >= cols || yy < 0 || yy >= rows) continue;
                            int px0 = xx * blockSize;
                            int px1 = std::min(w, px0 + blockSize);
                            if (px0 >= edgePx && px1 <= w - edgePx) continue;
                            current[index(xx, yy)] = 1;
                        }
                    }
                }
            }
        }

        for (size_t i = 0; i < votes.size(); ++i) {
            if (current[i]) votes[i] = static_cast<uint8_t>(std::min<int>(5, votes[i] + 1));
            else if (votes[i] > 0) --votes[i];
            persistent[i] = votes[i] >= 2 ? 1 : 0;
            if (current[i]) ++stats.currentBlocks;
            if (persistent[i]) ++stats.persistentBlocks;
        }
        return stats;
    }

    // Replaces only masked edge-widget pixels in an appended slice. The donor is the nearest
    // unmasked pixel toward the scrolling-content interior on the same row. This avoids repeated
    // floating buttons while leaving the central body untouched.
    int sanitizeChunk(std::vector<BYTE>& chunk, int sourceY, int chunkRows,
        const std::vector<BYTE>& frame) const
    {
        if (w <= 0 || h <= 0 || chunkRows <= 0 || chunk.empty() || frame.empty()) return 0;
        if (chunk.size() < static_cast<size_t>(w) * chunkRows * 4) return 0;
        int changed = 0;

        for (int localY = 0; localY < chunkRows; ++localY) {
            int screenY = sourceY + localY;
            if (screenY < 0 || screenY >= h) continue;
            for (int x = 0; x < w; ++x) {
                if (!masked(x, screenY)) continue;
                int donorX = findDonorX(x, screenY);
                if (donorX < 0 || donorX >= w || donorX == x) continue;
                size_t dst = (static_cast<size_t>(localY) * w + x) * 4;
                size_t src = (static_cast<size_t>(screenY) * w + donorX) * 4;
                chunk[dst] = frame[src];
                chunk[dst + 1] = frame[src + 1];
                chunk[dst + 2] = frame[src + 2];
                chunk[dst + 3] = frame[src + 3];
                ++changed;
            }
        }
        return changed;
    }

private:
    struct BlockMetric
    {
        double sameMad{ 255.0 };
        double scrollMad{ 0.0 };
        double contrast{ 0.0 };
    };

    static constexpr int blockSize = 8;
    int w{ 0 }, h{ 0 }, cols{ 0 }, rows{ 0 }, edgePx{ 0 };
    std::vector<uint8_t> votes;
    std::vector<uint8_t> current;
    std::vector<uint8_t> persistent;

    size_t index(int bx, int by) const { return static_cast<size_t>(by) * cols + bx; }

    static int gray(const BYTE* p)
    {
        return (static_cast<int>(p[0]) * 29 + static_cast<int>(p[1]) * 150
            + static_cast<int>(p[2]) * 77) >> 8;
    }

    BlockMetric metric(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        int x0, int x1, int y0, int y1, int offset) const
    {
        BlockMetric m;
        double same = 0.0, scroll = 0.0;
        int count = 0;
        int lo = 255, hi = 0;
        for (int y = y0; y < y1; y += 2) {
            for (int x = x0; x < x1; x += 2) {
                size_t n = (static_cast<size_t>(y) * w + x) * 4;
                size_t oSame = n;
                size_t oScroll = (static_cast<size_t>(y + offset) * w + x) * 4;
                same += std::abs(static_cast<int>(oldFrame[oSame]) - static_cast<int>(newFrame[n]));
                same += std::abs(static_cast<int>(oldFrame[oSame + 1]) - static_cast<int>(newFrame[n + 1]));
                same += std::abs(static_cast<int>(oldFrame[oSame + 2]) - static_cast<int>(newFrame[n + 2]));
                scroll += std::abs(static_cast<int>(oldFrame[oScroll]) - static_cast<int>(newFrame[n]));
                scroll += std::abs(static_cast<int>(oldFrame[oScroll + 1]) - static_cast<int>(newFrame[n + 1]));
                scroll += std::abs(static_cast<int>(oldFrame[oScroll + 2]) - static_cast<int>(newFrame[n + 2]));
                int g = gray(newFrame.data() + n);
                lo = std::min(lo, g);
                hi = std::max(hi, g);
                count += 3;
            }
        }
        if (count > 0) {
            m.sameMad = same / count;
            m.scrollMad = scroll / count;
            m.contrast = static_cast<double>(hi - lo);
        }
        return m;
    }

    void suppressDenseSide(std::vector<uint8_t>& seed, bool left, int bodyTop, int bodyBottom) const
    {
        int y0 = std::clamp(bodyTop / blockSize, 0, rows);
        int y1 = std::clamp((bodyBottom + blockSize - 1) / blockSize, 0, rows);
        int edgeCols = std::max(1, (edgePx + blockSize - 1) / blockSize);
        int x0 = left ? 0 : std::max(0, cols - edgeCols);
        int x1 = left ? std::min(cols, edgeCols) : cols;
        int total = std::max(1, (x1 - x0) * std::max(1, y1 - y0));
        int count = 0;
        for (int by = y0; by < y1; ++by)
            for (int bx = x0; bx < x1; ++bx)
                if (seed[index(bx, by)]) ++count;
        if (count * 100 <= total * 18) return;
        for (int by = y0; by < y1; ++by)
            for (int bx = x0; bx < x1; ++bx)
                seed[index(bx, by)] = 0;
    }

    bool masked(int x, int y) const
    {
        int bx = std::clamp(x / blockSize, 0, std::max(0, cols - 1));
        int by = std::clamp(y / blockSize, 0, std::max(0, rows - 1));
        size_t i = index(bx, by);
        return current[i] || persistent[i];
    }

    int findDonorX(int x, int y) const
    {
        bool left = x < w / 2;
        int dir = left ? 1 : -1;
        int limit = left ? std::min(w - 1, edgePx + blockSize * 2)
                         : std::max(0, w - edgePx - blockSize * 2);
        int start = x;
        for (int step = 1; step <= edgePx + blockSize * 2; ++step) {
            int xx = start + dir * step;
            if (xx < 0 || xx >= w) break;
            if (!masked(xx, y)) return xx;
            if ((left && xx >= limit) || (!left && xx <= limit)) break;
        }
        return std::clamp(left ? edgePx + 2 : w - edgePx - 3, 0, w - 1);
    }
};
