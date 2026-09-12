#pragma once
// ============================================================================
// nav_grid.h — 서버가 아는 최소한의 지형 정보
//
// 서버 권위 이동의 가장 큰 난관은, 서버가 언리얼 월드를 모른다는 점이다.
// 클라이언트는 지형·벽·경사를 다 아는데 서버는 아무것도 모르므로,
// 그대로 두면 "벽 통과"를 막을 수 없다.
//
// 해결: 언리얼에서 두 가지를 파일로 뽑아 서버가 로드한다.
//   1) 이동 가능 비트맵 — 네비메시를 격자로 래스터화한 것
//   2) 높이맵          — 지형 높이를 격자로 샘플링한 것
//
// [중요] 언리얼과 완전히 같은 충돌을 재현하는 게 목표가 아니다.
// 서버는 "이 위치로 가는 게 말이 되는가"만 판정하면 된다.
// 미세한 차이는 클라이언트 예측 화해(reconciliation)가 흡수한다.
//
// 데이터가 없으면 전부 이동 가능한 평지로 동작한다.
// 지형이 준비되기 전에도 서버를 돌릴 수 있게 하기 위해서다.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

#include "protocol.h"
#include "file_util.h"

// 이동 가능 판정 해상도 (cm). 2m 격자.
inline constexpr int32_t NAV_CELL_SIZE = 200;
inline constexpr int32_t NAV_DIM = (WORLD_MAX - WORLD_MIN) / NAV_CELL_SIZE;

// 높이 샘플 해상도 (cm). 8m 격자를 쌍선형 보간해서 쓴다.
// 이동 가능 판정보다 성기게 잡아도 되는 이유는, 높이는 연속적으로
// 변하지만 통행 가능 여부는 벽 하나로 급격히 바뀌기 때문이다.
inline constexpr int32_t HEIGHT_CELL_SIZE = 800;
inline constexpr int32_t HEIGHT_DIM = (WORLD_MAX - WORLD_MIN) / HEIGHT_CELL_SIZE + 1;

class NavGrid {
public:
    NavGrid() {
        // 기본값: 전부 이동 가능, 높이 0인 평지
        m_walkable.assign((static_cast<size_t>(NAV_DIM) * NAV_DIM + 7) / 8, 0xFF);
        m_heights.assign(static_cast<size_t>(HEIGHT_DIM) * HEIGHT_DIM, 0);
    }

    // ------------------------------------------------------------------------
    // 파일 로드
    //
    // 포맷 (리틀 엔디언, 언리얼 에디터 커맨드릿에서 생성):
    //   [0..3]   magic     = 'N','A','V','1'
    //   [4..7]   int32     nav_dim        (NAV_DIM과 일치해야 함)
    //   [8..11]  int32     height_dim     (HEIGHT_DIM과 일치해야 함)
    //   [12..]   uint8[]   walkable 비트맵 (1비트 = 셀 하나, 1이면 통행 가능)
    //   [...]    int16[]   높이 (10cm 단위. int16이라 ±3276m까지 표현)
    // ------------------------------------------------------------------------
    bool LoadFromFile(const std::string& path) {
        std::FILE* fp = OpenFile(path.c_str(), "rb");
        if (fp == nullptr) return false;   // 파일이 없으면 평지 기본값 유지

        char magic[4]{};
        int32_t nav_dim = 0, height_dim = 0;
        bool ok = (std::fread(magic, 1, 4, fp) == 4)
            && (std::fread(&nav_dim, sizeof(int32_t), 1, fp) == 1)
            && (std::fread(&height_dim, sizeof(int32_t), 1, fp) == 1);

        if (!ok || magic[0] != 'N' || magic[1] != 'A' ||
            magic[2] != 'V' || magic[3] != '1') {
            std::fclose(fp);
            return false;
        }

        // 서버 상수와 파일이 어긋나면 잘못된 지형으로 시뮬레이션하게 된다.
        // 조용히 진행하지 말고 실패시킨다.
        if (nav_dim != NAV_DIM || height_dim != HEIGHT_DIM) {
            std::fclose(fp);
            return false;
        }

        ok = (std::fread(m_walkable.data(), 1, m_walkable.size(), fp)
            == m_walkable.size())
            && (std::fread(m_heights.data(), sizeof(int16_t), m_heights.size(), fp)
                == m_heights.size());

        std::fclose(fp);
        m_loaded = ok;
        return ok;
    }

    bool IsLoaded() const { return m_loaded; }

    // ------------------------------------------------------------------------
    // 조회
    // ------------------------------------------------------------------------

    bool IsWalkable(int32_t world_x, int32_t world_y) const {
        if (world_x < WORLD_MIN || world_x >= WORLD_MAX ||
            world_y < WORLD_MIN || world_y >= WORLD_MAX) {
            return false;
        }
        const int32_t cx = (world_x - WORLD_MIN) / NAV_CELL_SIZE;
        const int32_t cy = (world_y - WORLD_MIN) / NAV_CELL_SIZE;
        const size_t index = static_cast<size_t>(cy) * NAV_DIM + cx;
        return (m_walkable[index >> 3] >> (index & 7)) & 1;
    }

    // 지면 높이. 성긴 격자를 쌍선형 보간해서 경사가 계단처럼 보이지 않게 한다.
    // 정수 연산만 쓰는 이유는 클라이언트와 결과를 정확히 맞추기 위해서다.
    int32_t SampleHeight(int32_t world_x, int32_t world_y) const {
        const int64_t fx = static_cast<int64_t>(world_x) - WORLD_MIN;
        const int64_t fy = static_cast<int64_t>(world_y) - WORLD_MIN;

        int32_t gx = static_cast<int32_t>(fx / HEIGHT_CELL_SIZE);
        int32_t gy = static_cast<int32_t>(fy / HEIGHT_CELL_SIZE);
        gx = ClampGrid(gx);
        gy = ClampGrid(gy);
        const int32_t gx1 = ClampGrid(gx + 1);
        const int32_t gy1 = ClampGrid(gy + 1);

        // 셀 내부 위치를 0~1024 고정소수점으로
        const int64_t tx = ((fx - static_cast<int64_t>(gx) * HEIGHT_CELL_SIZE) * 1024)
            / HEIGHT_CELL_SIZE;
        const int64_t ty = ((fy - static_cast<int64_t>(gy) * HEIGHT_CELL_SIZE) * 1024)
            / HEIGHT_CELL_SIZE;

        const int64_t h00 = HeightAt(gx, gy);
        const int64_t h10 = HeightAt(gx1, gy);
        const int64_t h01 = HeightAt(gx, gy1);
        const int64_t h11 = HeightAt(gx1, gy1);

        const int64_t top = h00 + ((h10 - h00) * tx >> 10);
        const int64_t bottom = h01 + ((h11 - h01) * tx >> 10);
        const int64_t result = top + ((bottom - top) * ty >> 10);

        return static_cast<int32_t>(result);
    }

private:
    static int32_t ClampGrid(int32_t v) {
        if (v < 0) return 0;
        if (v >= HEIGHT_DIM) return HEIGHT_DIM - 1;
        return v;
    }

    // 저장은 10cm 단위 int16, 반환은 cm
    int32_t HeightAt(int32_t gx, int32_t gy) const {
        return static_cast<int32_t>(
            m_heights[static_cast<size_t>(gy) * HEIGHT_DIM + gx]) * 10;
    }

    std::vector<uint8_t> m_walkable;
    std::vector<int16_t> m_heights;
    bool m_loaded = false;
};
