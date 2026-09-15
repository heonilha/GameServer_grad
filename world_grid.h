#pragma once
// ============================================================================
// world_grid.h — 섹터 그리드 (시야 처리의 핵심)
//
// 기존 코드는 이동 패킷 하나마다 접속자 전체를 순회했다.
//
//   for (auto& [key, pl] : clients)
//       if (pl && pl->m_id != self->m_id && pl->m_state == CS_PLAYING)
//           snapshot.push_back(pl);
//
// 이동은 초당 수십 번 일어나므로 이건 O(N²)다. MAX_PLAYERS가 10000인데
// 동접 100명만 돼도 여기서 무너진다. RIO를 붙여도 이 비용은 그대로 남는다.
//
// 월드를 SECTOR_SIZE 격자로 나누고 각 섹터가 자기 안의 오브젝트 id만
// 들고 있으면, 브로드캐스트 대상은 "내 섹터 + 주변 8개" 안에서만 찾으면 된다.
// protocol.h의 VIEW_RANGE <= SECTOR_SIZE 불변식이 이걸 보장한다.
// ============================================================================

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "protocol.h"

class WorldGrid {
public:
    static constexpr int32_t GRID_DIM =
        (WORLD_MAX_CM - WORLD_MIN_CM + SECTOR_SIZE - 1) / SECTOR_SIZE;

    WorldGrid() : m_sectors(std::make_unique<Sector[]>(GRID_DIM* GRID_DIM)) {}

    struct SectorCoord { int32_t sx, sy; };

    // 섹터를 하나의 정수로. 소유권 비교와 커맨드 라우팅에 쓴다.
    // 좌표 쌍을 그때그때 비교하는 것보다 실수가 적다.
    static int32_t SectorIndexOf(const Vec3i& pos) {
        const auto [sx, sy] = ToSector(pos);
        return sy * GRID_DIM + sx;
    }

    // 같은 섹터인가. 소유권 판정의 기본 연산이다.
    static bool SameSector(const Vec3i& a, const Vec3i& b) {
        const auto sa = ToSector(a);
        const auto sb = ToSector(b);
        return sa.sx == sb.sx && sa.sy == sb.sy;
    }

    // 웨이브 번호 (0~3). 체커보드 병렬화를 붙일 때 쓴다.
    //   간격이 2 이상인 섹터들끼리 같은 웨이브에 묶인다.
    // 지금은 실행에 쓰이지 않지만, 소유권 개념을 코드에 남겨두기 위해 넣는다.
    static int32_t WaveOf(int32_t sector_index) {
        const int32_t sx = sector_index % GRID_DIM;
        const int32_t sy = sector_index / GRID_DIM;
        return (sy & 1) * 2 + (sx & 1);
    }

    // 월드 좌표 → 섹터 좌표. z는 쓰지 않는다.
    // 3D지만 시야 판정은 수평 거리로만 해도 충분하고, 3D 격자는 메모리가
    // 세제곱으로 늘어난다.
    static SectorCoord ToSector(const Vec3i& pos) {
        int32_t sx = (pos.x - WORLD_MIN_CM) / SECTOR_SIZE;
        int32_t sy = (pos.y - WORLD_MIN_CM) / SECTOR_SIZE;
        return { Clamp(sx), Clamp(sy) };
    }

    void Add(int32_t id, const Vec3i& pos) {
        auto [sx, sy] = ToSector(pos);
        Sector& s = At(sx, sy);
        std::lock_guard lock(s.lock);
        s.objects.insert(id);
    }

    void Remove(int32_t id, const Vec3i& pos) {
        auto [sx, sy] = ToSector(pos);
        Sector& s = At(sx, sy);
        std::lock_guard lock(s.lock);
        s.objects.erase(id);
    }

    // 섹터가 안 바뀌었으면 아무 일도 하지 않는다.
    // 이동 대부분은 같은 섹터 안에서 일어나므로 이 조기 반환이 중요하다.
    void Move(int32_t id, const Vec3i& from, const Vec3i& to) {
        auto a = ToSector(from);
        auto b = ToSector(to);
        if (a.sx == b.sx && a.sy == b.sy) return;

        {
            Sector& s = At(a.sx, a.sy);
            std::lock_guard lock(s.lock);
            s.objects.erase(id);
        }
        {
            Sector& s = At(b.sx, b.sy);
            std::lock_guard lock(s.lock);
            s.objects.insert(id);
        }
    }

    // 한 섹터의 오브젝트 id를 복사해 온다.
    //
    // 웨이브 병렬 페이즈에서 섹터 하나를 처리할 때 쓴다.
    // 그 구간에는 이 섹터를 건드리는 스레드가 하나뿐이고 그리드 소속 변경도
    // 커맨드로 미뤄져 있어서 사실 락이 필요 없지만, 다른 곳에서도 부를 수
    // 있으므로 잠근다. 경합이 없어서 비용은 무시할 수준이다.
    void CopyObjects(int32_t sector_index, std::vector<int32_t>& out) const {
        out.clear();
        if (sector_index < 0 || sector_index >= GRID_DIM * GRID_DIM) return;
        Sector& s = m_sectors[sector_index];
        std::lock_guard lock(s.lock);
        out.assign(s.objects.begin(), s.objects.end());
    }

    static constexpr int32_t SectorCount() { return GRID_DIM * GRID_DIM; }

    // 주변 3x3 섹터의 오브젝트 id를 모아 돌려준다.
    // 콜백을 락 안에서 부르면 데드락 위험이 있으므로, 복사해서 나온 뒤 처리한다.
    std::vector<int32_t> QueryNear(const Vec3i& center) const {
        std::vector<int32_t> result;
        result.reserve(32);

        auto [cx, cy] = ToSector(center);
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const int32_t sx = cx + dx;
                const int32_t sy = cy + dy;
                if (sx < 0 || sx >= GRID_DIM || sy < 0 || sy >= GRID_DIM) continue;

                Sector& s = At(sx, sy);
                std::lock_guard lock(s.lock);
                result.insert(result.end(), s.objects.begin(), s.objects.end());
            }
        }
        return result;
    }

private:
    struct Sector {
        mutable std::mutex lock;
        std::unordered_set<int32_t> objects;
    };

    static int32_t Clamp(int32_t v) {
        if (v < 0) return 0;
        if (v >= GRID_DIM) return GRID_DIM - 1;
        return v;
    }

    Sector& At(int32_t sx, int32_t sy) const {
        return m_sectors[static_cast<size_t>(sy) * GRID_DIM + sx];
    }

    std::unique_ptr<Sector[]> m_sectors;
};

// 수평 거리 제곱. int64로 계산해야 5km 좌표에서 오버플로가 안 난다.
inline int64_t Distance2DSq(const Vec3i& a, const Vec3i& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - b.x;
    const int64_t dy = static_cast<int64_t>(a.y) - b.y;
    return dx * dx + dy * dy;
}

inline bool IsInViewRange(const Vec3i& a, const Vec3i& b) {
    constexpr int64_t range_sq =
        static_cast<int64_t>(VIEW_RANGE) * VIEW_RANGE;
    return Distance2DSq(a, b) <= range_sq;
}
