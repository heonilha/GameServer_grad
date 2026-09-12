#pragma once
// ============================================================================
// skill_table.h — 스킬 수치 테이블
//
// [이 파일의 핵심은 CSV를 서버와 클라이언트가 같이 읽는다는 점이다]
//
// 서버는 이 값으로 판정하고, 클라이언트의 GAS GameplayAbility는 같은 값으로
// 쿨타임과 마나를 표시하고 몽타주 타이밍을 잡는다.
//
// 코드에 수치를 박아두면 서버와 클라 밸런스가 반드시 어긋난다.
// 이렇게 두면 밸런싱이 코드 작업이 아니라 데이터 작업이 되고,
// 팀원이 숫자만 고쳐도 양쪽에 동시에 반영된다.
// protocol.h, movement.h와 같은 "공유 파일" 취급을 해야 한다.
//
// ---------------------------------------------------------------------------
// CSV 포맷 (첫 줄은 헤더, # 로 시작하면 주석)
//
//   skill_id,name,class_id,shape,range,angle,radius,damage,mp_cost,
//   cooldown_ms,windup_ms,proj_speed,proj_life_ms
//
//   class_id : 0=전사, 1=마법사
//   shape    : 0=부채꼴(cone), 1=투사체(projectile)
//   range    : cone은 사거리, projectile은 최대 비행 거리 (cm)
//   angle    : cone의 전체 각도 (도). projectile은 미사용
//   radius   : projectile 착탄 반경 (cm). cone은 미사용
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <array>
#include <string>

#include "protocol.h"
#include "csv_util.h"

struct SkillDef {
    uint16_t id = 0;
    char     name[32]{};
    uint8_t  class_id = 0;
    uint8_t  shape = SHAPE_CONE;

    int32_t range = 0;      // cm
    int32_t angle_deg = 0;      // 전체 각도 (도)
    int32_t radius = 0;      // cm
    int32_t damage = 0;
    int32_t mp_cost = 0;
    int32_t cooldown_ms = 0;
    int32_t windup_ms = 0;
    int32_t proj_speed = 0;      // cm/s
    int32_t proj_life_ms = 0;

    // 로드할 때 미리 계산해두는 값들.
    // 판정마다 다시 구하지 않기 위해서다.
    int32_t range_sq = 0;      // 사거리 제곱
    int32_t half_angle_cos = 0;      // cos(각도/2) * 10000
    int32_t cooldown_ticks = 0;
    int32_t windup_ticks = 0;
    int32_t life_ticks = 0;

    bool valid = false;
};

class SkillTable {
public:
    bool LoadFromCsv(const std::string& path) {
        std::FILE* fp = OpenFile(path.c_str(), "r");
        if (fp == nullptr) return false;

        char line[512];
        bool header_skipped = false;
        int loaded = 0;

        while (std::fgets(line, sizeof(line), fp)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            if (!header_skipped) { header_skipped = true; continue; }

            SkillDef def{};
            if (!ParseLine(line, def)) continue;
            if (def.id >= MAX_SKILLS) continue;

            Finalize(def);
            m_skills[def.id] = def;
            ++loaded;
        }
        std::fclose(fp);
        m_loaded_count = loaded;
        return loaded > 0;
    }

    // 없는 스킬이면 nullptr. 호출부에서 반드시 확인해야 한다.
    const SkillDef* Get(uint16_t id) const {
        if (id >= MAX_SKILLS) return nullptr;
        const SkillDef& def = m_skills[id];
        return def.valid ? &def : nullptr;
    }

    int LoadedCount() const { return m_loaded_count; }

private:
    // 컬럼 수. CSV 헤더와 반드시 일치해야 한다.
    //   skill_id, name, class_id, shape, range, angle, radius, damage,
    //   mp_cost, cooldown_ms, windup_ms, proj_speed, proj_life_ms
    static constexpr int FIELD_COUNT = 13;

    static bool ParseLine(char* line, SkillDef& def) {
        const char* f[FIELD_COUNT]{};
        if (SplitCsvLine(line, f, FIELD_COUNT) < FIELD_COUNT) return false;

        def.id           = static_cast<uint16_t>(std::atoi(f[0]));
        CopyFixed(def.name, sizeof(def.name), f[1]);
        def.class_id     = static_cast<uint8_t>(std::atoi(f[2]));
        def.shape        = static_cast<uint8_t>(std::atoi(f[3]));
        def.range        = std::atoi(f[4]);
        def.angle_deg    = std::atoi(f[5]);
        def.radius       = std::atoi(f[6]);
        def.damage       = std::atoi(f[7]);
        def.mp_cost      = std::atoi(f[8]);
        def.cooldown_ms  = std::atoi(f[9]);
        def.windup_ms    = std::atoi(f[10]);
        def.proj_speed   = std::atoi(f[11]);
        def.proj_life_ms = std::atoi(f[12]);
        return true;
    }

    static void Finalize(SkillDef& def) {
        def.range_sq = def.range * def.range;

        // 부채꼴 판정을 내적으로 하기 위해 cos(반각)을 미리 구해둔다.
        //
        // 여기서는 부동소수점을 써도 된다. 이동 시뮬레이션과 달리
        // 전투 판정은 서버만 하기 때문이다. 클라는 서버가 알려준 결과를
        // 받을 뿐이라 비트 단위로 일치할 필요가 없다.
        const double half_rad = def.angle_deg * 0.5 * 3.14159265358979 / 180.0;
        def.half_angle_cos = static_cast<int32_t>(std::cos(half_rad) * 10000.0);

        def.cooldown_ticks = (def.cooldown_ms + TICK_MS - 1) / TICK_MS;
        def.windup_ticks = def.windup_ms / TICK_MS;
        def.life_ticks = (def.proj_life_ms + TICK_MS - 1) / TICK_MS;

        def.valid = true;
    }

    std::array<SkillDef, MAX_SKILLS> m_skills{};
    int m_loaded_count = 0;
};
