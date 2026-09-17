#pragma once
// ============================================================================
// pathfinding.h — 시선 검사 + 지역 A*
//
// 왜 전체 월드 A*를 쓰지 않는가
//
//   NavGrid는 2m 격자라 10km 월드는 5000x5000 = 2500만 셀이다.
//   여기서 A*를 돌리면 당연히 안 된다.
//
//   그런데 몬스터는 그렇게 멀리 갈 일이 없다.
//     어그로 범위    15m -> 약 8셀
//     리쉬(귀환) 범위 30m -> 약 15셀
//
//   실제 탐색 범위는 아무리 넓게 잡아도 40m x 40m, 20x20 = 400노드다.
//   이 정도면 마이크로초 단위로 끝난다.
//
// 그래서 두 단계로 간다.
//   1. 목표까지 시선이 뚫려 있으면 직선 이동 (대부분 여기서 끝)
//   2. 막혔으면 제한된 창 안에서만 A*
//
// 직선 이동만 쓰면 몬스터가 벽 모서리에서 좌우로 떨고, 오목한 지형에
// 갇히면 절대 못 빠져나온다. 그래서 A*를 처음부터 같이 넣는다.
// ============================================================================

#include <cstdint>
#include <array>
#include <vector>
#include <queue>
#include <cstdlib>

#include "protocol.h"
#include "nav_grid.h"

namespace pathfinding {

// A*를 돌릴 창의 한 변 (셀). 64셀 x 2m = 128m.
inline constexpr int32_t WINDOW = 64;
inline constexpr int32_t WINDOW_CELLS = WINDOW * WINDOW;

inline int32_t ToCellX(int32_t world_x) { return (world_x - WORLD_MIN_CM) / NAV_CELL_SIZE; }
inline int32_t ToCellY(int32_t world_y) { return (world_y - WORLD_MIN_CM) / NAV_CELL_SIZE; }

// 셀 중심의 월드 좌표. 모서리를 주면 벽에 붙어 걷게 된다.
inline int32_t ToWorldX(int32_t cell_x) {
    return WORLD_MIN_CM + cell_x * NAV_CELL_SIZE + NAV_CELL_SIZE / 2;
}
inline int32_t ToWorldY(int32_t cell_y) {
    return WORLD_MIN_CM + cell_y * NAV_CELL_SIZE + NAV_CELL_SIZE / 2;
}

// ----------------------------------------------------------------------------
// 시선 검사 — 브레젠험으로 격자를 훑는다
//
// 이게 통과하면 A*가 아예 필요 없다. 열린 지형에서는 거의 항상 통과한다.
// ----------------------------------------------------------------------------
inline bool HasLineOfSight(const NavGrid& nav, const Vec3i& from, const Vec3i& to)
{
    int32_t x0 = ToCellX(from.x), y0 = ToCellY(from.y);
    const int32_t x1 = ToCellX(to.x), y1 = ToCellY(to.y);

    const int32_t dx = std::abs(x1 - x0);
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sx = (x0 < x1) ? 1 : -1;
    const int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    // 대각선으로 벽 모서리를 뚫고 지나가는 것을 막기 위해,
    // 한 스텝에 x와 y를 동시에 옮기지 않는다.
    for (int guard = 0; guard < 4096; ++guard) {
        if (!nav.IsWalkable(ToWorldX(x0), ToWorldY(y0))) return false;
        if (x0 == x1 && y0 == y1) return true;

        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        else          { err += dx; y0 += sy; }
    }
    return false;
}

// ----------------------------------------------------------------------------
// 지역 A*
//
// 스크래치 버퍼를 매번 지우지 않고 "방문 도장(stamp)"으로 무효화한다.
// 4096칸을 매 탐색마다 memset하면 그게 탐색보다 비싸다.
//
// AI 페이즈는 단일 스레드라 정적 버퍼를 공유해도 안전하다.
// ----------------------------------------------------------------------------
struct AStarScratch {
    std::array<uint32_t, WINDOW_CELLS> stamp{};
    std::array<int32_t,  WINDOW_CELLS> g_cost{};
    std::array<int32_t,  WINDOW_CELLS> parent{};
    uint32_t current_stamp = 0;
};

inline AStarScratch& Scratch() {
    static AStarScratch s;
    return s;
}

// 옥타일 거리. 대각 이동을 허용할 때의 최단 거리 하한이다.
inline int32_t Heuristic(int32_t dx, int32_t dy) {
    dx = std::abs(dx);
    dy = std::abs(dy);
    const int32_t lo = (dx < dy) ? dx : dy;
    const int32_t hi = (dx < dy) ? dy : dx;
    return 14 * lo + 10 * (hi - lo);
}

// 경로를 찾으면 out에 월드 좌표 웨이포인트를 담고 true.
// 목표가 창 밖이거나 막혀 있으면 false — 호출자는 직선 이동으로 버틴다.
inline bool FindPath(const NavGrid& nav,
                     const Vec3i& from, const Vec3i& to,
                     std::vector<Vec3i>& out)
{
    out.clear();

    const int32_t sx = ToCellX(from.x), sy = ToCellY(from.y);
    const int32_t gx = ToCellX(to.x),   gy = ToCellY(to.y);

    // 시작과 목표를 모두 담는 창을 잡는다.
    const int32_t min_x = (sx < gx ? sx : gx);
    const int32_t min_y = (sy < gy ? sy : gy);
    const int32_t max_x = (sx > gx ? sx : gx);
    const int32_t max_y = (sy > gy ? sy : gy);

    // 벽을 우회하려면 직선 구간 바깥으로 나가야 하므로 여유를 둔다.
    constexpr int32_t MARGIN = 8;
    const int32_t span_x = max_x - min_x + 1 + MARGIN * 2;
    const int32_t span_y = max_y - min_y + 1 + MARGIN * 2;
    if (span_x > WINDOW || span_y > WINDOW) return false;   // 너무 멀다

    const int32_t origin_x = min_x - MARGIN;
    const int32_t origin_y = min_y - MARGIN;

    auto index_of = [&](int32_t cx, int32_t cy) -> int32_t {
        const int32_t lx = cx - origin_x;
        const int32_t ly = cy - origin_y;
        if (lx < 0 || lx >= WINDOW || ly < 0 || ly >= WINDOW) return -1;
        return ly * WINDOW + lx;
    };

    AStarScratch& s = Scratch();
    ++s.current_stamp;

    const int32_t start_index = index_of(sx, sy);
    const int32_t goal_index  = index_of(gx, gy);
    if (start_index < 0 || goal_index < 0) return false;
    if (!nav.IsWalkable(to.x, to.y)) return false;

    using Node = std::pair<int32_t, int32_t>;   // (f, index)
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    s.stamp[start_index]  = s.current_stamp;
    s.g_cost[start_index] = 0;
    s.parent[start_index] = -1;
    open.emplace(Heuristic(gx - sx, gy - sy), start_index);

    static constexpr int32_t DX[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
    static constexpr int32_t DY[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };
    static constexpr int32_t COST[8] = { 10, 10, 10, 10, 14, 14, 14, 14 };

    bool found = false;
    int guard = 0;

    while (!open.empty() && ++guard < WINDOW_CELLS * 2) {
        const int32_t index = open.top().second;
        open.pop();

        if (index == goal_index) { found = true; break; }

        const int32_t cx = origin_x + (index % WINDOW);
        const int32_t cy = origin_y + (index / WINDOW);
        const int32_t base_cost = s.g_cost[index];

        for (int d = 0; d < 8; ++d) {
            const int32_t nx = cx + DX[d];
            const int32_t ny = cy + DY[d];
            const int32_t ni = index_of(nx, ny);
            if (ni < 0) continue;
            if (!nav.IsWalkable(ToWorldX(nx), ToWorldY(ny))) continue;

            // 대각 이동은 양옆이 모두 뚫려 있을 때만.
            // 안 그러면 벽 모서리를 대각선으로 통과한다.
            if (d >= 4) {
                if (!nav.IsWalkable(ToWorldX(nx), ToWorldY(cy))) continue;
                if (!nav.IsWalkable(ToWorldX(cx), ToWorldY(ny))) continue;
            }

            const int32_t next_cost = base_cost + COST[d];
            if (s.stamp[ni] == s.current_stamp && s.g_cost[ni] <= next_cost) continue;

            s.stamp[ni]  = s.current_stamp;
            s.g_cost[ni] = next_cost;
            s.parent[ni] = index;
            open.emplace(next_cost + Heuristic(gx - nx, gy - ny), ni);
        }
    }

    if (!found) return false;

    // 역추적
    std::vector<Vec3i> reversed;
    for (int32_t i = goal_index; i >= 0; i = s.parent[i]) {
        const int32_t cx = origin_x + (i % WINDOW);
        const int32_t cy = origin_y + (i / WINDOW);
        Vec3i p{};
        p.x = ToWorldX(cx);
        p.y = ToWorldY(cy);
        p.z = nav.SampleHeight(p.x, p.y);
        reversed.push_back(p);
        if (s.parent[i] < 0) break;
    }

    // 경로 다듬기 (string pulling).
    // A*는 격자를 따라가므로 계단처럼 꺾인다. 시선이 뚫린 지점까지
    // 건너뛰면 자연스러운 직선이 된다. 시선 검사를 이미 만들어뒀으니 공짜다.
    for (size_t i = reversed.size(); i-- > 0; ) {
        if (out.empty()) { out.push_back(reversed[i]); continue; }

        // 다음 점까지 시선이 뚫려 있으면 중간 점은 버린다.
        if (i > 0 && HasLineOfSight(nav, out.back(), reversed[i - 1])) continue;
        out.push_back(reversed[i]);
    }
    return out.size() > 1;
}

} // namespace pathfinding
