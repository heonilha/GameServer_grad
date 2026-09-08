#pragma once
// ============================================================================
// monster_table.h — 몬스터 수치 테이블
//
// skills.csv와 같은 발상이다. 수치를 코드에서 빼내면 밸런싱이
// 데이터 작업이 되고, 팀원이 코드를 건드리지 않고 조정할 수 있다.
//
// aggressive 플래그 하나로 어그로 방식이 갈린다.
//   0 = 수동형. 맞아야 반격한다 (슬라임, 골렘)
//   1 = 능동형. 시야에 들어오면 먼저 덤빈다 (고블린)
//
// 초반 지역을 수동형으로 채우면 신규 플레이어가 안전하게 적응할 수 있다.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <array>
#include <string>

#include "protocol.h"

inline constexpr int32_t MAX_MONSTER_TYPES = 64;

struct MonsterDef {
    uint16_t id = 0;
    char     name[32]{};
    int32_t  visual_id = 0;
    uint8_t  aggressive = 0;

    int32_t max_hp = 100;
    int32_t damage = 10;
    int32_t move_speed = 300;   // cm/s
    int32_t aggro_range = 0;     // cm. 수동형은 0
    int32_t attack_range = 150;   // cm
    int32_t leash_range = 3000;  // cm. 스폰 지점에서 이만큼 벗어나면 복귀
    int32_t attack_cooldown_ms = 1500;
    int32_t attack_windup_ms = 400;
    int32_t exp_reward = 10;

    // 로드 시 미리 계산
    int64_t aggro_range_sq = 0;
    int64_t attack_range_sq = 0;
    int64_t leash_range_sq = 0;
    int32_t attack_cooldown_ticks = 0;
    int32_t attack_windup_ticks = 0;

    bool valid = false;
};

class MonsterTable {
public:
    bool LoadFromCsv(const std::string& path) {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "r") != 0 || fp == nullptr) return false;

        char line[512];
        bool header_skipped = false;
        int loaded = 0;

        while (std::fgets(line, sizeof(line), fp)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            if (!header_skipped) { header_skipped = true; continue; }

            MonsterDef def{};
            if (!ParseLine(line, def)) continue;
            if (def.id >= MAX_MONSTER_TYPES) continue;

            Finalize(def);
            m_defs[def.id] = def;
            m_order[loaded % MAX_MONSTER_TYPES] = def.id;
            ++loaded;
        }
        std::fclose(fp);
        m_count = loaded;
        return loaded > 0;
    }

    const MonsterDef* Get(uint16_t id) const {
        if (id >= MAX_MONSTER_TYPES) return nullptr;
        const MonsterDef& d = m_defs[id];
        return d.valid ? &d : nullptr;
    }

    int Count() const { return m_count; }

    // 스폰할 때 종류를 고르기 위한 순번 접근
    uint16_t IdAt(int index) const {
        if (m_count == 0) return 0;
        return m_order[index % m_count];
    }

private:
    static bool ParseLine(char* line, MonsterDef& def) {
        char* ctx = nullptr;
        auto next = [&]() -> const char* {
            return std::strtok_s(nullptr, ",\r\n", &ctx);
        };

        const char* first = std::strtok_s(line, ",\r\n", &ctx);
        if (!first) return false;
        def.id = static_cast<uint16_t>(std::atoi(first));

        const char* name = next();
        if (!name) return false;
        std::strncpy(def.name, name, sizeof(def.name) - 1);

        const char* f[10]{};
        for (int i = 0; i < 10; ++i) {
            f[i] = next();
            if (!f[i]) return false;
        }

        def.visual_id          = std::atoi(f[0]);
        def.aggressive         = static_cast<uint8_t>(std::atoi(f[1]));
        def.max_hp             = std::atoi(f[2]);
        def.damage             = std::atoi(f[3]);
        def.move_speed         = std::atoi(f[4]);
        def.aggro_range        = std::atoi(f[5]);
        def.attack_range       = std::atoi(f[6]);
        def.leash_range        = std::atoi(f[7]);
        def.attack_cooldown_ms = std::atoi(f[8]);
        def.attack_windup_ms   = std::atoi(f[9]);
        def.exp_reward         = 0;   // 경험치 시스템이 들어오면 컬럼 추가
        return true;
    }

    static void Finalize(MonsterDef& d) {
        d.aggro_range_sq  = static_cast<int64_t>(d.aggro_range)  * d.aggro_range;
        d.attack_range_sq = static_cast<int64_t>(d.attack_range) * d.attack_range;
        d.leash_range_sq  = static_cast<int64_t>(d.leash_range)  * d.leash_range;
        d.attack_cooldown_ticks =
            (d.attack_cooldown_ms + TICK_MS - 1) / TICK_MS;
        d.attack_windup_ticks = d.attack_windup_ms / TICK_MS;
        d.valid = true;
    }

    std::array<MonsterDef, MAX_MONSTER_TYPES> m_defs{};
    std::array<uint16_t, MAX_MONSTER_TYPES> m_order{};
    int m_count = 0;
};
