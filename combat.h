#pragma once
// ============================================================================
// combat.h — 전투 판정
//
// 판정은 전부 서버에서 하고, 골격 히트박스는 쓰지 않는다.
// 스킬마다 단순한 도형을 정의하고 서버가 자기가 아는 위치로 판정한다.
//
//   전사 근접 공격 : 시전자 기준 부채꼴
//   파이어볼       : 투사체 -> 충돌 지점 원형
//
// ---------------------------------------------------------------------------
// 윈드업을 왜 서버도 기다리는가
//
// 몽타주는 300~500ms 뒤에 타격 프레임이 온다. 서버가 시전 즉시 판정하면
// 클라 화면에서는 아직 칼을 휘두르지도 않았는데 피가 깎인다.
// 반대로 피하려고 뒤로 뺀 대상이 이미 맞아 있는 일도 생긴다.
//
// 그래서 시전 시점에는 S2C_SkillUsed만 보내 몽타주를 재생시키고,
// 판정은 windup_ticks 뒤로 예약한다. 예약된 판정은 PendingHit에 쌓인다.
//
// ---------------------------------------------------------------------------
// 투사체를 매 틱 보내지 않는 이유
//
// 시작 위치와 속도만 주면 클라가 같은 식으로 날려서 그릴 수 있다.
// 파이어볼 하나에 초당 15개씩 위치 패킷을 보내는 것은 낭비다.
// 명중 판정은 서버가 하고 결과만 S2C_ProjectileEnd로 알린다.
// ============================================================================

#include <cstdint>
#include <mutex>
#include <vector>

#include "protocol.h"
#include "skill_table.h"
#include "world_object.h"
#include "world_grid.h"
#include "nav_grid.h"
#include "fixed_math.h"

// ----------------------------------------------------------------------------
// 판정 도형
// ----------------------------------------------------------------------------

// 부채꼴 안에 있는가.
//
// 삼각함수 없이 내적으로 판정한다.
//   dot(facing, dir) >= |dir| * cos(반각)
// 양변에 |dir|이 곱해져 있으므로, 미리 구해둔 cos(반각)만 있으면
// 나눗셈 없이 정수 비교로 끝난다.
inline bool IsInCone(const Vec3i& origin, int16_t yaw,
                     const Vec3i& target,
                     int32_t range_sq, int32_t half_angle_cos)
{
    const int64_t dx = static_cast<int64_t>(target.x) - origin.x;
    const int64_t dy = static_cast<int64_t>(target.y) - origin.y;
    const int64_t dist_sq = dx * dx + dy * dy;

    if (dist_sq > range_sq) return false;
    if (dist_sq == 0) return true;          // 정확히 겹쳐 있으면 명중

    // 시전자가 바라보는 방향 (1e4 배율)
    const int64_t fx = Cos1e4(yaw);
    const int64_t fy = Sin1e4(yaw);

    // dot(facing, dir) — facing이 1e4 배율이므로 결과도 1e4 배율
    const int64_t dot = fx * dx + fy * dy;
    if (dot <= 0 && half_angle_cos > 0) return false;   // 뒤쪽

    // dot >= |dir| * cos(반각)  ->  양변 제곱해서 sqrt를 없앤다.
    // 부호를 잃으므로 위에서 dot <= 0을 먼저 걸렀다.
    const int64_t lhs = dot * dot;                       // 1e8 배율
    const int64_t rhs = dist_sq * half_angle_cos * half_angle_cos;
    return lhs >= rhs;
}

inline bool IsInCircle(const Vec3i& center, const Vec3i& target, int32_t radius)
{
    const int64_t r = static_cast<int64_t>(radius) + ACTOR_RADIUS;
    return Distance2DSq(center, target) <= r * r;
}

// ----------------------------------------------------------------------------
// 예약된 판정과 투사체
// ----------------------------------------------------------------------------

struct PendingHit {
    uint32_t execute_tick = 0;
    int32_t  caster_id = 0;
    uint16_t skill_id = 0;
    int16_t  yaw = 0;        // 시전 시점의 방향을 쓴다
};

struct Projectile {
    int32_t  id = 0;
    int32_t  owner_id = 0;
    uint16_t skill_id = 0;
    Vec3i    pos{};
    int32_t  vel_x = 0;
    int32_t  vel_y = 0;
    int32_t  vel_z = 0;
    uint32_t expire_tick = 0;
    bool     alive = false;
};

// ----------------------------------------------------------------------------
// CombatSystem
//
// 예약 큐에 넣는 것(Enqueue)은 I/O 스레드에서, 소비하는 것(Tick)은
// 틱 스레드에서 한다. 그래서 큐만 락으로 보호하고 나머지는 틱 스레드
// 전용으로 둔다.
// ----------------------------------------------------------------------------

class CombatSystem {
public:
    // ---- I/O 스레드에서 호출 ----

    void EnqueueHit(const PendingHit& hit) {
        std::lock_guard lock(m_incoming_lock);
        m_incoming.push_back(hit);
    }

    // ---- 틱 스레드에서 호출 ----

    // 새로 들어온 예약을 작업 목록으로 옮긴다.
    void CollectIncoming() {
        std::lock_guard lock(m_incoming_lock);
        for (const PendingHit& h : m_incoming) m_pending.push_back(h);
        m_incoming.clear();
    }

    // 이번 틱에 실행할 판정만 골라 out에 담고 목록에서 뺀다.
    void TakeDueHits(uint32_t now_tick, std::vector<PendingHit>& out) {
        out.clear();
        size_t write = 0;
        for (size_t i = 0; i < m_pending.size(); ++i) {
            if (m_pending[i].execute_tick <= now_tick) {
                out.push_back(m_pending[i]);
            } else {
                m_pending[write++] = m_pending[i];
            }
        }
        m_pending.resize(write);
    }

    // 투사체 슬롯을 하나 잡는다. 없으면 nullptr.
    Projectile* Spawn() {
        for (auto& p : m_projectiles) {
            if (!p.alive) {
                p = Projectile{};
                p.id = PROJECTILE_ID_START + m_next_serial;
                m_next_serial = (m_next_serial + 1) % MAX_PROJECTILES;
                p.alive = true;
                return &p;
            }
        }
        return nullptr;
    }

    std::vector<Projectile>& All() { return m_projectiles; }

    void Initialize() { m_projectiles.resize(MAX_PROJECTILES); }

private:
    std::mutex m_incoming_lock;
    std::vector<PendingHit> m_incoming;    // I/O 스레드가 채운다

    std::vector<PendingHit> m_pending;     // 틱 스레드 전용
    std::vector<Projectile> m_projectiles; // 틱 스레드 전용
    int32_t m_next_serial = 0;
};
