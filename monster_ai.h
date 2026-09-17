#pragma once
// ============================================================================
// monster_ai.h — 몬스터 AI 페이즈
//
// 세 가지가 이 파일의 골자다.
//
// 1. 섹터 휴면 (dormancy)
//    플레이어가 없는 섹터의 몬스터는 아예 갱신하지 않는다.
//    오픈월드에서 몬스터 대부분은 항상 아무도 안 보고 있어서,
//    이 하나로 AI 비용의 대부분이 사라진다.
//
// 2. 낮은 갱신 주기
//    몬스터는 30Hz가 필요 없다. 10Hz면 충분하다.
//
// 3. 시선 검사 우선 경로 탐색
//    목표까지 뚫려 있으면 직선으로 간다. 열린 지형에서는 거의 항상 통과하고,
//    막혔을 때만 지역 A*를 돌린다.
//
// AI 페이즈는 몬스터 위치와 HP를 바꾸므로 단일 스레드에서 돈다.
// ============================================================================

#include <cstdint>
#include <unordered_set>
#include <vector>
#include <cmath>

#include "protocol.h"
#include "monster_table.h"
#include "world_object.h"
#include "world_grid.h"
#include "nav_grid.h"
#include "movement.h"
#include "pathfinding.h"
#include "combat_tick.h"
#include "world_command.h"

extern SessionManager g_sessions;
extern NpcManager     g_npcs;
extern WorldGrid      g_grid;
extern NavGrid        g_nav;
extern MonsterTable   g_monsters;

// AI 갱신 주기(틱). 3틱 = 10Hz.
inline constexpr int32_t AI_TICK_INTERVAL = 3;

// 경로 재계산 주기(틱). 매번 A*를 돌리면 낭비다.
inline constexpr int32_t REPATH_INTERVAL = 10;

// 웨이포인트에 이만큼 가까워지면 도달로 본다 (cm).
inline constexpr int32_t WAYPOINT_TOLERANCE = 80;

// ----------------------------------------------------------------------------
// 몬스터 이동
//
// 플레이어의 SimulateStep과 달리 입력이 없고 목표 지점만 있다.
// 벽 슬라이딩은 같은 방식으로 처리한다.
// ----------------------------------------------------------------------------
inline bool MoveToward(NpcEntity& npc, const Vec3i& goal,
                       int32_t speed, int32_t dt_ms)
{
    const Vec3i from = npc.GetPosition();

    int64_t dx = static_cast<int64_t>(goal.x) - from.x;
    int64_t dy = static_cast<int64_t>(goal.y) - from.y;
    const int64_t dist_sq = dx * dx + dy * dy;
    if (dist_sq == 0) return false;

    const int64_t step = static_cast<int64_t>(speed) * dt_ms / 1000;
    if (step <= 0) return false;

    // 목표를 지나치지 않게 자른다
    const int64_t dist = movement_detail::ISqrt(dist_sq);
    int32_t move_x, move_y;
    if (dist <= step) {
        move_x = static_cast<int32_t>(dx);
        move_y = static_cast<int32_t>(dy);
    } else {
        move_x = static_cast<int32_t>(dx * step / dist);
        move_y = static_cast<int32_t>(dy * step / dist);
    }

    Vec3i next = from;
    const int32_t target_x = from.x + move_x;
    const int32_t target_y = from.y + move_y;

    if (g_nav.IsWalkable(target_x, target_y)) {
        next.x = target_x;
        next.y = target_y;
    } else if (move_x != 0 && g_nav.IsWalkable(target_x, from.y)) {
        next.x = target_x;
    } else if (move_y != 0 && g_nav.IsWalkable(from.x, target_y)) {
        next.y = target_y;
    } else {
        return false;   // 완전히 막혔다. 다음 갱신에서 재탐색된다
    }

    next.z = g_nav.SampleHeight(next.x, next.y);

    // 진행 방향을 바라본다 (0.01도 단위)
    const uint16_t yaw = static_cast<uint16_t>(
        (static_cast<int32_t>(std::atan2(
            static_cast<double>(next.y - from.y),
            static_cast<double>(next.x - from.x)) * 18000.0 / 3.14159265358979)
         + 36000) % 36000);

    npc.SetPosition(next, yaw);
    g_moved.Mark(npc.GetId());

    // 섹터를 넘었을 때만 커맨드를 남긴다.
    // 이동 대부분은 같은 섹터 안에서 일어나므로 이 검사가 중요하다.
    if (!WorldGrid::SameSector(from, next)) {
        CombatOutbox().Migrate(npc.GetId(), from, next);
    }
    return true;
}

// ----------------------------------------------------------------------------
// 목표까지 한 걸음
//
// 1. 시선이 뚫려 있으면 경로를 버리고 직선으로 간다 (대부분 여기)
// 2. 막혔으면 지역 A*로 경로를 만들고 웨이포인트를 따라간다
// ----------------------------------------------------------------------------
inline void StepTowardGoal(NpcEntity& npc, const MonsterDef& def,
                           const Vec3i& goal, uint32_t now_tick)
{
    const Vec3i pos = npc.GetPosition();
    const int32_t dt_ms = TICK_MS * AI_TICK_INTERVAL;

    if (pathfinding::HasLineOfSight(g_nav, pos, goal)) {
        npc.path.clear();
        npc.path_index = 0;
        MoveToward(npc, goal, def.move_speed, dt_ms);
        return;
    }

    const bool need_repath =
        npc.path.empty() ||
        npc.path_index >= npc.path.size() ||
        now_tick >= npc.repath_tick;

    if (need_repath) {
        npc.repath_tick = now_tick + REPATH_INTERVAL;
        if (!pathfinding::FindPath(g_nav, pos, goal, npc.path)) {
            // 경로를 못 찾으면 직선으로라도 붙어 본다.
            // 벽에 붙어 멈추더라도 다음 재탐색에서 풀린다.
            npc.path.clear();
            MoveToward(npc, goal, def.move_speed, dt_ms);
            return;
        }
        npc.path_index = 1;   // 0번은 현재 위치
    }

    if (npc.path_index >= npc.path.size()) return;

    const Vec3i& waypoint = npc.path[npc.path_index];
    if (Distance2DSq(pos, waypoint) <=
        static_cast<int64_t>(WAYPOINT_TOLERANCE) * WAYPOINT_TOLERANCE) {
        ++npc.path_index;
        if (npc.path_index >= npc.path.size()) return;
    }
    MoveToward(npc, npc.path[npc.path_index], def.move_speed, dt_ms);
}

// ----------------------------------------------------------------------------
// 어그로 대상 찾기 (능동형만)
// ----------------------------------------------------------------------------
inline int32_t FindAggroTarget(const NpcEntity& npc, const MonsterDef& def)
{
    if (!def.aggressive || def.aggro_range <= 0) return -1;

    const Vec3i pos = npc.GetPosition();
    int32_t best = -1;
    int64_t best_dist = def.aggro_range_sq;

    for (int32_t id : g_grid.QueryNear(pos)) {
        if (id >= NPC_ID_START) continue;

        auto session = g_sessions.Get(id);
        if (!session || session->GetState() != SessionState::Playing) continue;
        if (!session->IsAlive()) continue;

        const int64_t d = Distance2DSq(pos, session->GetPosition());
        if (d < best_dist) {
            best_dist = d;
            best = id;
        }
    }
    return best;
}

// ----------------------------------------------------------------------------
// 몬스터 한 마리
// ----------------------------------------------------------------------------
inline void UpdateMonster(NpcEntity& npc, uint32_t now_tick)
{
    const MonsterDef* def = g_monsters.Get(npc.monster_id);
    if (!def) return;
    if (!npc.IsAlive()) return;

    const Vec3i pos = npc.GetPosition();

    // 대상이 사라졌거나 죽었으면 놓는다
    if (npc.target_id >= 0) {
        auto target = g_sessions.Get(npc.target_id);
        if (!target || target->GetState() != SessionState::Playing ||
            !target->IsAlive()) {
            npc.target_id = -1;
            npc.ai_state = AiState::Return;
        }
    }

    // 리쉬. 스폰 지점에서 너무 멀어지면 추격을 포기한다.
    // 이게 없으면 플레이어가 몬스터를 월드 끝까지 끌고 다닐 수 있다.
    if (npc.ai_state == AiState::Chase || npc.ai_state == AiState::Attack) {
        if (Distance2DSq(pos, npc.GetSpawnPoint()) > def->leash_range_sq) {
            npc.target_id = -1;
            npc.ai_state = AiState::Return;
            npc.path.clear();
            npc.repath_tick = 0;
        }
    }

    switch (npc.ai_state) {
    case AiState::Idle: {
        const int32_t found = FindAggroTarget(npc, *def);
        if (found >= 0) {
            npc.target_id = found;
            npc.ai_state = AiState::Chase;
            npc.repath_tick = 0;
        }
        break;
    }

    case AiState::Chase: {
        auto target = g_sessions.Get(npc.target_id);
        if (!target) { npc.ai_state = AiState::Return; break; }

        const Vec3i target_pos = target->GetPosition();
        if (Distance2DSq(pos, target_pos) <= def->attack_range_sq) {
            npc.ai_state = AiState::Attack;
            npc.path.clear();
            break;
        }
        StepTowardGoal(npc, *def, target_pos, now_tick);
        break;
    }

    case AiState::Attack: {
        auto target = g_sessions.Get(npc.target_id);
        if (!target) { npc.ai_state = AiState::Return; break; }

        const Vec3i target_pos = target->GetPosition();
        if (Distance2DSq(pos, target_pos) > def->attack_range_sq) {
            npc.ai_state = AiState::Chase;   // 도망갔다
            break;
        }

        if (now_tick < npc.next_attack_tick) break;

        // 공격 모션을 먼저 알린다. 클라는 skill_id 0을 받으면
        // 그 몬스터의 기본 공격 몽타주를 재생한다.
        S2C_SkillUsed used{};
        InitHeader(used, S2C_SKILL_USED);
        used.caster_id   = npc.GetId();
        used.skill_id    = 0;
        used.yaw         = npc.MakeSnapshot().move.yaw;
        used.server_tick = now_tick;
        BroadcastNear(pos, &used, used.h.size);

        // 판정은 윈드업 뒤에. 플레이어 스킬과 같은 이유다.
        // 몬스터는 예약 큐 대신 다음 AI 갱신에서 사거리를 다시 보고 때린다.
        // (AI 주기가 100ms라 윈드업 오차가 그 정도인데, 체감되지 않는다)
        npc.next_attack_tick = now_tick + def->attack_cooldown_ticks;
        npc.pending_hit_tick = now_tick + def->attack_windup_ticks;
        break;
    }

    case AiState::Return: {
        const Vec3i& spawn = npc.GetSpawnPoint();
        if (Distance2DSq(pos, spawn) <=
            static_cast<int64_t>(WAYPOINT_TOLERANCE) * WAYPOINT_TOLERANCE) {
            npc.ClearAi();
            // 복귀하면 체력을 회복한다. 안 그러면 도망친 플레이어가
            // 계속 치고 빠지면서 몬스터를 무한히 깎을 수 있다.
            npc.Revive(spawn);
            break;
        }
        StepTowardGoal(npc, *def, spawn, now_tick);
        break;
    }
    }

    // 예약된 타격 실행
    if (npc.pending_hit_tick != 0 && now_tick >= npc.pending_hit_tick) {
        npc.pending_hit_tick = 0;

        auto target = g_sessions.Get(npc.target_id);
        if (target && target->GetState() == SessionState::Playing &&
            target->IsAlive() &&
            Distance2DSq(npc.GetPosition(), target->GetPosition())
                <= def->attack_range_sq) {
            EmitDamage(npc.target_id, npc.GetId(), def->damage);
        }
    }
}

// ----------------------------------------------------------------------------
// 섹터 하나의 몬스터를 갱신한다.
//
// 호출자(틱 루프)가 웨이브 단위로 섹터를 나눠 병렬 호출한다.
// 같은 웨이브의 섹터는 간격이 2 이상이라 서로의 객체를 건드릴 수 없고,
// 남의 객체를 바꿔야 하는 경우는 전부 커맨드로 빠져 있으므로 락이 없다.
//
// 섹터 휴면은 호출자가 처리한다. 웨이브 목록 자체를 "플레이어가 있는
// 섹터 주변 3x3"으로만 만들기 때문에, 아무도 보고 있지 않은 섹터는
// 애초에 여기까지 오지 않는다.
// ----------------------------------------------------------------------------
inline void UpdateMonstersInSector(int32_t sector_index, uint32_t now_tick,
                                   std::vector<int32_t>& scratch)
{
    g_grid.CopyObjects(sector_index, scratch);

    for (int32_t id : scratch) {
        if (id < NPC_ID_START) continue;          // 플레이어는 시뮬레이션 페이즈에서
        NpcEntity* npc = g_npcs.Get(id);
        if (!npc || !npc->IsAlive()) continue;
        UpdateMonster(*npc, now_tick);
    }
}
