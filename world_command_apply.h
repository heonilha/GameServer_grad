#pragma once
// ============================================================================
// world_command_apply.h — 커맨드 적용 (직렬 구간)
//
// 시뮬레이션/AI/전투 페이즈가 쌓아둔 커맨드를 여기서 한꺼번에 적용한다.
// 이 함수는 항상 단일 스레드에서 돈다. 그래서 락 없이 마음껏 변경해도 된다.
//
// 적용 순서는 파티션 번호 -> 기록 순서로 고정되어 있다.
// 같은 입력에 같은 결과가 나와야 재현과 디버깅이 가능하다.
// ============================================================================

#include <cstdint>

#include "protocol.h"
#include "world_command.h"
#include "world_object.h"
#include "world_grid.h"

extern SessionManager g_sessions;
extern NpcManager     g_npcs;
extern WorldGrid      g_grid;
extern CommandBus     g_commands;

// server_main.cpp가 정의한다.
void BroadcastNear(const Vec3i& pos, const void* data, uint16_t len);

// ----------------------------------------------------------------------------
// 커맨드가 가리키는 객체 찾기
//
// 플레이어와 NPC는 저장소가 다르므로 여기서 흡수한다.
// shared_ptr를 반환하지 않는 이유는, 이 함수가 직렬 구간에서만 불리고
// NPC는 프로그램 수명 내내 살아있기 때문이다.
// ----------------------------------------------------------------------------
inline WorldObject* ResolveObject(int32_t id, std::shared_ptr<Session>& keep_alive)
{
    if (id >= NPC_ID_START) {
        return g_npcs.Get(id);
    }
    keep_alive = g_sessions.Get(id);
    if (!keep_alive) return nullptr;
    if (keep_alive->GetState() != SessionState::Playing) return nullptr;
    return keep_alive.get();
}

// ----------------------------------------------------------------------------
// 피해 적용
//
// 예전에는 전투/AI 페이즈가 순회 도중에 직접 HP를 깎았다.
// 그러면 (1) 남의 섹터 객체를 변경하게 되고
//       (2) 적용 순서가 그리드 순회 순서에 따라 달라진다.
// 이제 여기서만 깎는다.
// ----------------------------------------------------------------------------
inline void ResolveDamage(const WorldCommand& cmd, uint32_t now_tick)
{
    std::shared_ptr<Session> keep;
    WorldObject* target = ResolveObject(cmd.target_id, keep);
    if (!target || !target->IsAlive()) return;

    bool killed = false;
    const int32_t remaining = target->ApplyDamage(cmd.value, killed);
    const Vec3i pos = target->GetPosition();

    if (cmd.target_id >= NPC_ID_START) {
        NpcEntity* npc = static_cast<NpcEntity*>(target);
        if (killed) {
            // 그리드에서 빼면 주변 플레이어의 다음 시야 갱신에서
            // 자동으로 REMOVE가 나간다.
            g_grid.Remove(cmd.target_id, pos);
            npc->SetOwnerSector(-1);
            npc->SetRespawnTick(now_tick + NPC_RESPAWN_TICKS);
        } else {
            // 수동형 몬스터의 반격. 능동형은 시야만으로도 덤비지만,
            // 다른 대상을 때리다 나에게 맞았을 때 대상을 바꾸는 효과가 있다.
            npc->OnDamaged(cmd.source_id);
        }
    } else if (killed) {
        keep->SetRespawnTick(now_tick + PLAYER_RESPAWN_TICKS);
        // 기획상 사망 시 경험치가 감소한다. 경험치 시스템이 들어오면 여기.
    }

    S2C_Damage packet{};
    InitHeader(packet, S2C_DAMAGE);
    packet.target_id = cmd.target_id;
    packet.attacker_id = cmd.source_id;
    packet.amount = cmd.value;
    packet.remaining_hp = remaining;
    packet.flags = killed ? DMG_KILLED : 0;
    BroadcastNear(pos, &packet, packet.h.size);

    if (killed) {
        S2C_Death death{};
        InitHeader(death, S2C_DEATH);
        death.object_id = cmd.target_id;
        death.killer_id = cmd.source_id;
        BroadcastNear(pos, &death, death.h.size);
    }
}

// ----------------------------------------------------------------------------
// 섹터 소속 변경
//
// 그리드의 두 섹터를 동시에 건드리는 유일한 연산이다.
// 시뮬레이션 중에 하면 남의 섹터를 변경하는 셈이므로 여기로 미룬다.
// ----------------------------------------------------------------------------
inline void ResolveMigrate(const WorldCommand& cmd)
{
    std::shared_ptr<Session> keep;
    WorldObject* object = ResolveObject(cmd.target_id, keep);
    if (!object) return;

    g_grid.Move(cmd.target_id, cmd.from, cmd.to);
    object->SetOwnerSector(WorldGrid::SectorIndexOf(cmd.to));
}

inline void ResolveGridRemove(const WorldCommand& cmd)
{
    g_grid.Remove(cmd.target_id, cmd.from);

    std::shared_ptr<Session> keep;
    if (WorldObject* object = ResolveObject(cmd.target_id, keep)) {
        object->SetOwnerSector(-1);
    }
}

inline void ResolveGridAdd(const WorldCommand& cmd)
{
    g_grid.Add(cmd.target_id, cmd.to);

    std::shared_ptr<Session> keep;
    if (WorldObject* object = ResolveObject(cmd.target_id, keep)) {
        object->SetOwnerSector(WorldGrid::SectorIndexOf(cmd.to));
    }
}

// ----------------------------------------------------------------------------
// 적용 본체
// ----------------------------------------------------------------------------
inline size_t FlushWorldCommands(uint32_t now_tick)
{
    const size_t count = g_commands.Count();
    if (count == 0) return 0;

    g_commands.ForEachCommand([&](const WorldCommand& cmd) {
        switch (cmd.type) {
        case CommandType::Damage:     ResolveDamage(cmd, now_tick); break;
        case CommandType::Migrate:    ResolveMigrate(cmd);          break;
        case CommandType::GridRemove: ResolveGridRemove(cmd);       break;
        case CommandType::GridAdd:    ResolveGridAdd(cmd);          break;
        }
    });

    g_commands.Clear();
    return count;
}
