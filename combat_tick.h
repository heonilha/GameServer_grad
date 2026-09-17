#pragma once
// ============================================================================
// combat_tick.h — 틱 루프의 전투 페이즈
//
// 이 페이즈는 HP를 깎고 투사체를 움직이므로 "월드를 변경"한다.
// 따라서 시뮬레이션과 마찬가지로 단일 스레드에서 돈다.
//
// 순서:
//   1. 예약된 판정 중 이번 틱에 실행할 것 처리 (윈드업이 끝난 스킬)
//   2. 투사체 전진과 충돌 검사
//   3. 부활 처리
// ============================================================================

#include <vector>

#include "protocol.h"
#include "combat.h"
#include "skill_table.h"
#include "world_object.h"
#include "world_grid.h"
#include "nav_grid.h"
#include "world_command.h"
#include "world_command_apply.h"
#include "fixed_math.h"

extern SessionManager g_sessions;
extern NpcManager     g_npcs;
extern WorldGrid      g_grid;
extern NavGrid        g_nav;
extern SkillTable     g_skills;
extern CombatSystem   g_combat;
extern CommandBus     g_commands;

// 지금 스레드가 쓰는 출력함.
// 병렬 페이즈에서는 워커가 t_partition을 자기 번호로 설정해두므로
// 각자 다른 버퍼에 기록한다. 직렬 구간에서는 0번이다.
inline CommandOutbox& CombatOutbox() { return g_commands.For(t_partition); }

// 피해를 "적용"하지 않고 "기록"한다.
// 대상이 다른 섹터에 있을 수 있고, 적용 순서를 고정해야 하기 때문이다.
// 실제 적용은 FlushWorldCommands가 한다.
inline void EmitDamage(int32_t target_id, int32_t attacker_id, int32_t amount) {
    CombatOutbox().Damage(target_id, attacker_id, amount);
}

// server_main.cpp가 정의한다.
bool TryGetSnapshot(int32_t id, WorldObject::Snapshot& out);

// ----------------------------------------------------------------------------
// 대상 판별
//
// 지금은 PvP를 넣지 않았으므로 플레이어의 공격은 NPC/몬스터에만 들어간다.
// PvP를 넣게 되면 이 함수 하나만 고치면 된다.
// ----------------------------------------------------------------------------
inline bool IsHostile(int32_t attacker_id, int32_t target_id)
{
    if (attacker_id == target_id) return false;
    const bool attacker_is_player = (attacker_id < NPC_ID_START);
    const bool target_is_player   = (target_id   < NPC_ID_START);
    return attacker_is_player != target_is_player;
}

// ----------------------------------------------------------------------------
// 부채꼴 판정 실행 (전사 근접 공격)
// ----------------------------------------------------------------------------
inline void ExecuteConeHit(const PendingHit& hit, const SkillDef& def)
{
    auto caster = g_sessions.Get(hit.caster_id);
    if (!caster || caster->GetState() != SessionState::Playing) return;
    if (!caster->IsAlive()) return;

    // 판정 위치는 "시전 시점"이 아니라 "타격 프레임 시점"의 위치를 쓴다.
    // 윈드업 동안 시전자가 움직였다면 움직인 자리에서 판정하는 것이
    // 클라 화면과 맞는다.
    const Vec3i origin = caster->GetPosition();

    for (int32_t id : g_grid.QueryNear(origin)) {
        if (!IsHostile(hit.caster_id, id)) continue;

        WorldObject::Snapshot target;
        if (!TryGetSnapshot(id, target)) continue;
        if (target.hp <= 0) continue;

        if (!IsInCone(origin, hit.yaw, target.move.pos,
                      def.range_sq, def.half_angle_cos)) {
            continue;
        }
        EmitDamage(id, hit.caster_id, def.damage);
    }
}

// ----------------------------------------------------------------------------
// 투사체 생성 (파이어볼)
// ----------------------------------------------------------------------------
inline void SpawnProjectile(const PendingHit& hit, const SkillDef& def,
                            uint32_t now_tick)
{
    auto caster = g_sessions.Get(hit.caster_id);
    if (!caster || caster->GetState() != SessionState::Playing) return;
    if (!caster->IsAlive()) return;

    Projectile* proj = g_combat.Spawn();
    if (!proj) return;   // 슬롯이 없으면 조용히 버린다

    const Vec3i origin = caster->GetPosition();

    proj->owner_id = hit.caster_id;
    proj->skill_id = hit.skill_id;
    proj->pos      = origin;
    proj->pos.z   += 100;                       // 가슴 높이에서 나간다
    proj->vel_x    = static_cast<int32_t>(
        static_cast<int64_t>(Cos1e4(hit.yaw)) * def.proj_speed / 10000);
    proj->vel_y    = static_cast<int32_t>(
        static_cast<int64_t>(Sin1e4(hit.yaw)) * def.proj_speed / 10000);
    proj->vel_z    = 0;                         // 직선 비행. 중력 미적용
    proj->expire_tick = now_tick + def.life_ticks;

    S2C_Projectile packet{};
    InitHeader(packet, S2C_PROJECTILE);
    packet.projectile_id = proj->id;
    packet.owner_id      = proj->owner_id;
    packet.skill_id      = proj->skill_id;
    packet.pos           = proj->pos;
    packet.vel_x         = proj->vel_x;
    packet.vel_y         = proj->vel_y;
    packet.vel_z         = proj->vel_z;
    packet.server_tick   = now_tick;
    BroadcastNear(proj->pos, &packet, packet.h.size);
}

// ----------------------------------------------------------------------------
// 투사체 전진과 충돌
// ----------------------------------------------------------------------------
inline void EndProjectile(Projectile& proj, uint8_t reason)
{
    proj.alive = false;

    S2C_ProjectileEnd packet{};
    InitHeader(packet, S2C_PROJECTILE_END);
    packet.projectile_id = proj.id;
    packet.pos           = proj.pos;
    packet.reason        = reason;
    BroadcastNear(proj.pos, &packet, packet.h.size);
}

inline void AdvanceProjectiles(uint32_t now_tick)
{
    for (Projectile& proj : g_combat.All()) {
        if (!proj.alive) continue;

        if (now_tick >= proj.expire_tick) {
            EndProjectile(proj, PROJ_EXPIRED);
            continue;
        }

        const SkillDef* def = g_skills.Get(proj.skill_id);
        if (!def) { proj.alive = false; continue; }

        proj.pos.x += static_cast<int32_t>(
            static_cast<int64_t>(proj.vel_x) * TICK_MS / 1000);
        proj.pos.y += static_cast<int32_t>(
            static_cast<int64_t>(proj.vel_y) * TICK_MS / 1000);
        proj.pos.z += static_cast<int32_t>(
            static_cast<int64_t>(proj.vel_z) * TICK_MS / 1000);

        // 벽에 맞음
        if (!g_nav.IsWalkable(proj.pos.x, proj.pos.y)) {
            EndProjectile(proj, PROJ_HIT_WALL);
            continue;
        }

        // 대상에 맞음.
        // 먼저 몸통(ACTOR_RADIUS)에 닿았는지 보고, 닿았으면 그 지점에서
        // 스킬 반경만큼 광역 피해를 준다.
        int32_t struck = -1;
        for (int32_t id : g_grid.QueryNear(proj.pos)) {
            if (!IsHostile(proj.owner_id, id)) continue;

            WorldObject::Snapshot target;
            if (!TryGetSnapshot(id, target)) continue;
            if (target.hp <= 0) continue;

            if (Distance2DSq(proj.pos, target.move.pos) <=
                static_cast<int64_t>(ACTOR_RADIUS) * ACTOR_RADIUS) {
                struck = id;
                break;
            }
        }
        if (struck < 0) continue;

        for (int32_t id : g_grid.QueryNear(proj.pos)) {
            if (!IsHostile(proj.owner_id, id)) continue;

            WorldObject::Snapshot target;
            if (!TryGetSnapshot(id, target)) continue;
            if (target.hp <= 0) continue;
            if (!IsInCircle(proj.pos, target.move.pos, def->radius)) continue;

            EmitDamage(id, proj.owner_id, def->damage);
        }
        EndProjectile(proj, PROJ_HIT_TARGET);
    }
}

// ----------------------------------------------------------------------------
// 부활
// ----------------------------------------------------------------------------
inline void ProcessRespawns(uint32_t now_tick)
{
    for (int32_t i = 0; i < g_npcs.Count(); ++i) {
        NpcEntity* npc = g_npcs.At(i);
        const uint32_t at = npc->GetRespawnTick();
        if (at == 0 || now_tick < at) continue;

        npc->SetRespawnTick(0);
        npc->Revive(npc->GetSpawnPoint());

        // 그리드에 다시 넣으면 주변 플레이어의 다음 시야 갱신에서
        // 자동으로 ADD가 나간다. 그리드 변경이므로 커맨드로 미룬다.
        CombatOutbox().GridAdd(npc->GetId(), npc->GetSpawnPoint());
    }

    g_sessions.ForEach([&](const std::shared_ptr<Session>& session) {
        const uint32_t at = session->GetRespawnTick();
        if (at == 0 || now_tick < at) return;
        if (session->GetState() != SessionState::Playing) return;

        session->SetRespawnTick(0);

        const Vec3i from = session->GetPosition();
        Vec3i village{ VILLAGE_X, VILLAGE_Y, 0 };
        village.z = g_nav.SampleHeight(village.x, village.y);

        session->Revive(village);
        CombatOutbox().Migrate(session->GetId(), from, village);

        S2C_Respawn packet{};
        InitHeader(packet, S2C_RESPAWN);
        packet.object_id = session->GetId();
        packet.pos       = village;
        packet.hp        = session->MakeSnapshot().hp;
        BroadcastNear(village, &packet, packet.h.size);
    });
}

// ----------------------------------------------------------------------------
// 전투 페이즈 본체
// ----------------------------------------------------------------------------
inline void RunCombatPhase(uint32_t now_tick)
{
    static std::vector<PendingHit> due;

    g_combat.CollectIncoming();
    g_combat.TakeDueHits(now_tick, due);

    for (const PendingHit& hit : due) {
        const SkillDef* def = g_skills.Get(hit.skill_id);
        if (!def) continue;

        if (def->shape == SHAPE_CONE) {
            ExecuteConeHit(hit, *def);
        } else if (def->shape == SHAPE_PROJECTILE) {
            SpawnProjectile(hit, *def, now_tick);
        }
    }

    AdvanceProjectiles(now_tick);
    ProcessRespawns(now_tick);
}
