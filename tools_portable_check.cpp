// OS 비의존 헤더만 모아 실제로 컴파일한다.
// MSVC에서 빌드하기 전에 문법/타입 오류를 여기서 걸러낸다.
#include "protocol.h"
#include "file_util.h"
#include "csv_util.h"
#include "fixed_math.h"
#include "nav_grid.h"
#include "movement.h"
#include "world_grid.h"
#include "pathfinding.h"
#include "skill_table.h"
#include "monster_table.h"
#include "world_command.h"
#include "tick_metrics.h"

#include <cstdio>

int main()
{
    // 스킬/몬스터 테이블 실제 로드
    SkillTable skills;
    const bool skills_ok = skills.LoadFromCsv("skills.csv");
    std::printf("skills.csv  : %s (%d)\n",
        skills_ok ? "OK" : "FAIL", skills.LoadedCount());
    if (const SkillDef* d = skills.Get(2)) {
        std::printf("  [2] %s shape=%d range=%d radius=%d dmg=%d cd=%d ticks windup=%d ticks\n",
            d->name, d->shape, d->range, d->radius, d->damage,
            d->cooldown_ticks, d->windup_ticks);
    }
    if (const SkillDef* d = skills.Get(1)) {
        std::printf("  [1] %s angle=%d halfcos=%d range_sq=%d\n",
            d->name, d->angle_deg, d->half_angle_cos, d->range_sq);
    }

    MonsterTable monsters;
    const bool monsters_ok = monsters.LoadFromCsv("monsters.csv");
    std::printf("monsters.csv: %s (%d)\n",
        monsters_ok ? "OK" : "FAIL", monsters.Count());
    for (int i = 0; i < monsters.Count(); ++i) {
        const MonsterDef* m = monsters.Get(monsters.IdAt(i));
        if (m) std::printf("  [%d] %-8s aggressive=%d hp=%d leash=%d\n",
                           m->id, m->name, m->aggressive, m->max_hp, m->leash_range);
    }

    // 이동 시뮬레이션 — 정수 연산이 의도대로 도는지
    NavGrid nav;
    MoveState st{};
    st.grounded = true;
    MoveInput in{};
    in.move_x = 1000;            // 정면 전속력
    for (int i = 0; i < TICK_RATE; ++i) SimulateStep(st, in, nav);
    std::printf("1초 이동 거리 : %d cm (WALK_SPEED=%d)\n", st.pos.x, WALK_SPEED);

    // 입력 정제 — 조작된 값이 깎이는지
    MoveInput hack{};
    hack.move_x = 30000;
    hack.move_y = 30000;
    MoveInput fixed = SanitizeInput(hack);
    std::printf("스피드핵 정제 : (%d,%d) -> (%d,%d)\n",
        hack.move_x, hack.move_y, fixed.move_x, fixed.move_y);

    // 경로 탐색 — 평지에서는 시선이 뚫려야 한다
    Vec3i a{0,0,0}, b{5000,3000,0};
    std::printf("시선 검사     : %s\n",
        pathfinding::HasLineOfSight(nav, a, b) ? "뚫림(직선 이동)" : "막힘");

    // 그리드 불변식
    std::printf("그리드        : %dx%d 섹터, 웨이브(0,0)=%d (2,0)=%d (1,0)=%d\n",
        WorldGrid::GRID_DIM, WorldGrid::GRID_DIM,
        WorldGrid::WaveOf(0), WorldGrid::WaveOf(2), WorldGrid::WaveOf(1));
    return 0;
}
