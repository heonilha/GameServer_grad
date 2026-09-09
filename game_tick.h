#pragma once
// ============================================================================
// game_tick.h — 고정 주기 시뮬레이션 루프
//
// 한 틱의 구조 (페이즈 분리):
//
//   페이즈 1  시뮬레이션 + 그리드 갱신   월드를 변경    -> 단일 스레드
//   페이즈 2  시야 목록 갱신             자기 것만 변경 -> 병렬
//   페이즈 3  스냅샷 조립 + 전송         읽기만         -> 병렬
//
// 1번을 병렬화하지 않는 이유는, "A가 B를 보는 시점"과 "B가 움직이는 시점"이
// 겹치면 같은 틱인데 플레이어마다 다른 월드를 보게 되기 때문이다.
// 비용의 대부분은 3번이고 그쪽은 읽기 전용이라 안전하게 쪼개진다.
//
// ---------------------------------------------------------------------------
// 시야 목록은 각 세션이 자기 것만 소유한다
//
// 이전 판에서는 내가 누군가를 발견하면 상대의 시야 목록에도 나를 밀어넣었다.
// 그렇게 하면 (1) 시야 상한을 넘길 수 있고 (2) 세션 간 교차 변경이 생겨
// 페이즈 2를 병렬화할 수 없다.
//
// 지금은 각자 자기 시야만 계산한다. 상대는 자기 차례에 나를 발견한다.
// 최대 VIEW_UPDATE_INTERVAL 틱(약 100ms)이 늦지만 체감되지 않고,
// 접속 종료 시 REMOVE도 자동으로 처리된다 — 그리드에서 빠지면 다음 계산에서
// 사라진 것으로 잡히기 때문이다.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_set>

#include "protocol.h"
#include "movement.h"
#include "nav_grid.h"
#include "world_object.h"
#include "world_grid.h"
#include "tick_worker_pool.h"
#include "tick_metrics.h"
#include "world_command.h"
#include "world_command_apply.h"
#include "combat_tick.h"
#include "monster_ai.h"

// server_main.cpp가 정의한다
extern SessionManager    g_sessions;
extern NpcManager        g_npcs;
extern WorldGrid         g_grid;
extern NavGrid           g_nav;
extern std::atomic<bool> g_running;

bool TryGetSnapshot(int32_t id, WorldObject::Snapshot& out);
S2C_AddObject    MakeAddObject(const WorldObject::Snapshot& s);
S2C_RemoveObject MakeRemoveObject(int32_t id);

// 배치 전송을 시동한다 (server_main.cpp)
void FlushSendBatch(const std::shared_ptr<Session>& session,
                    const std::vector<char>& batch);

// ----------------------------------------------------------------------------
// 틱 루프
// ----------------------------------------------------------------------------

class GameTickLoop {
public:
    explicit GameTickLoop(unsigned int extra_workers)
        : m_pool(extra_workers) {}

    void Run() {
        using clock = std::chrono::steady_clock;
        auto next_tick = clock::now();

        // 파티션마다 자기 버퍼를 쓴다. 매 틱 할당하지 않도록 미리 잡아둔다.
        m_scratch.resize(m_pool.PartitionCount());

        while (g_running.load(std::memory_order_relaxed)) {
            next_tick += std::chrono::milliseconds(TICK_MS);
            ++m_tick;

            m_metrics.BeginTick(m_tick, g_sessions.Count());

            PhaseSimulate();

            // 이동으로 생긴 섹터 이동을 먼저 반영한다.
            // AI와 전투가 그리드를 조회하므로 그 전에 맞춰둬야 한다.
            int64_t commands = static_cast<int64_t>(FlushWorldCommands(m_tick));
            m_metrics.EndPhaseSimulate();

            // AI와 전투는 남의 섹터 객체를 건드릴 수 있으므로
            // 직접 바꾸지 않고 커맨드만 쌓는다.
            RunAiPhase(m_tick);
            RunCombatPhase(m_tick);

            // 피해, 사망, 섹터 이동을 정해진 순서로 한꺼번에 적용한다.
            // 여기가 시야 갱신보다 앞서야 이번 틱의 사망/이동이
            // 시야에 바로 반영된다.
            commands += static_cast<int64_t>(FlushWorldCommands(m_tick));
            m_metrics.SetCommandCount(commands);

            const bool view_tick = (m_tick % VIEW_UPDATE_INTERVAL == 0);
            if (view_tick) PhaseUpdateViews();
            m_metrics.EndPhaseView();

            const bool snapshot_tick = (m_tick % SNAPSHOT_INTERVAL == 0);
            if (snapshot_tick) PhaseSendSnapshots();
            m_metrics.EndPhaseSnapshot();

            m_metrics.EndTick(TICK_MS * 1000);

            // 틱이 밀렸으면 따라잡으려 하지 않고 기준 시각을 리셋한다.
            // 밀린 만큼 몰아서 돌리면 부하가 더 심해지는 악순환이 생긴다.
            const auto now = clock::now();
            if (next_tick > now) {
                std::this_thread::sleep_until(next_tick);
            } else {
                next_tick = now;
            }
        }
    }

    uint32_t CurrentTick() const { return m_tick; }
    const TickMetrics& Metrics() const { return m_metrics; }
    TickMetrics& Metrics() { return m_metrics; }

    // 로그인 직후처럼 다음 시야 갱신 틱을 기다릴 수 없을 때 쓴다.
    void UpdateViewNow(const std::shared_ptr<Session>& session) {
        std::vector<char> batch;
        UpdateViewList(session, batch);
        FlushSendBatch(session, batch);
    }

private:
    // ------------------------------------------------------------------------
    // 페이즈 1 — 시뮬레이션 (단일 스레드)
    // ------------------------------------------------------------------------
    void PhaseSimulate() {
        g_moved.clear();

        std::vector<MoveInput> inputs;
        inputs.reserve(MAX_INPUTS_PER_TICK);

        g_sessions.ForEach([&](const std::shared_ptr<Session>& session) {
            if (session->GetState() != SessionState::Playing) return;

            session->PopInputs(inputs);

            MoveState state = session->GetMoveState();
            const Vec3i from = state.pos;

            if (inputs.empty()) {
                // 입력이 없어도 공중에 있으면 중력을 적용해야 한다.
                // 안 그러면 점프 중 패킷이 끊긴 캐릭터가 공중에 멈춘다.
                if (state.grounded) return;

                MoveInput idle{};
                idle.yaw = state.yaw;
                SimulateStep(state, idle, g_nav);
            } else {
                for (const MoveInput& in : inputs) {
                    SimulateStep(state, in, g_nav);
                    session->SetLastProcessedInput(in.sequence);
                }
            }

            session->SetMoveState(state);
            g_moved.insert(session->GetId());

            // 섹터를 넘었을 때만 커맨드를 남긴다.
            // 이동 대부분은 같은 섹터 안이라 이 검사에서 걸러진다.
            if (!WorldGrid::SameSector(from, state.pos)) {
                g_commands.For(0).Migrate(session->GetId(), from, state.pos);
            }
        });
    }

    // ------------------------------------------------------------------------
    // 페이즈 2 — 시야 갱신 (병렬)
    // 각 세션이 자기 시야 목록만 변경하므로 교차 경합이 없다.
    // ------------------------------------------------------------------------
    void PhaseUpdateViews() {
        m_pool.RunParallel([this](unsigned int part, unsigned int total) {
            std::vector<char>& batch = m_scratch[part];
            const int32_t span = (SessionManager::SlotCount() + total - 1) / total;
            const int32_t begin = static_cast<int32_t>(part) * span;

            g_sessions.ForEachInRange(begin, begin + span,
                [&](const std::shared_ptr<Session>& session) {
                    if (session->GetState() != SessionState::Playing) return;
                    batch.clear();
                    UpdateViewList(session, batch);
                    FlushSendBatch(session, batch);
                });
        });
    }

    // ------------------------------------------------------------------------
    // 페이즈 3 — 스냅샷 전송 (병렬, 읽기 전용)
    // ------------------------------------------------------------------------
    void PhaseSendSnapshots() {
        m_pool.RunParallel([this](unsigned int part, unsigned int total) {
            std::vector<char>& batch = m_scratch[part];
            const int32_t span = (SessionManager::SlotCount() + total - 1) / total;
            const int32_t begin = static_cast<int32_t>(part) * span;

            int64_t packets = 0;
            int64_t bytes = 0;

            g_sessions.ForEachInRange(begin, begin + span,
                [&](const std::shared_ptr<Session>& session) {
                    if (session->GetState() != SessionState::Playing) return;
                    batch.clear();
                    BuildSnapshots(session, batch, packets);
                    bytes += static_cast<int64_t>(batch.size());
                    FlushSendBatch(session, batch);
                });

            m_metrics.AddPackets(packets, bytes);
        });
    }

    // ------------------------------------------------------------------------
    // 시야 목록 계산
    //
    // 주변 3x3 섹터에서 후보를 모으고, 거리순으로 잘라 MAX_VIEW_OBJECTS개만
    // 남긴다. 이 상한이 밀집 지역의 N^2를 막는 유일한 장치다.
    // ------------------------------------------------------------------------
    static void UpdateViewList(const std::shared_ptr<Session>& self,
                               std::vector<char>& batch) {
        std::lock_guard update_guard(self->ViewUpdateLock());

        const auto my = self->MakeSnapshot();

        struct Candidate {
            int32_t id;
            int64_t dist_sq;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        for (int32_t id : g_grid.QueryNear(my.move.pos)) {
            if (id == my.id) continue;

            WorldObject::Snapshot other;
            if (!TryGetSnapshot(id, other)) continue;

            const int64_t d = Distance2DSq(my.move.pos, other.move.pos);
            if (d > static_cast<int64_t>(VIEW_RANGE) * VIEW_RANGE) continue;

            candidates.push_back({ id, d });
        }

        // 상한을 넘으면 가까운 것부터 남긴다.
        // 전체 정렬 대신 nth_element를 쓰는 이유는, 순서 자체는 필요 없고
        // "가까운 N개"라는 집합만 필요하기 때문이다.
        if (candidates.size() > static_cast<size_t>(MAX_VIEW_OBJECTS)) {
            std::nth_element(
                candidates.begin(),
                candidates.begin() + MAX_VIEW_OBJECTS,
                candidates.end(),
                [](const Candidate& a, const Candidate& b) {
                    return a.dist_sq < b.dist_sq;
                });
            candidates.resize(MAX_VIEW_OBJECTS);
        }

        std::unordered_set<int32_t> current;
        current.reserve(candidates.size() * 2);
        for (const Candidate& c : candidates) current.insert(c.id);

        const auto previous = self->CopyViewList();

        // 새로 들어온 것 -> ADD
        for (int32_t id : current) {
            if (previous.count(id)) continue;

            WorldObject::Snapshot other;
            if (!TryGetSnapshot(id, other)) continue;

            const auto add = MakeAddObject(other);
            Append(batch, &add, add.h.size);
        }

        // 빠져나간 것 -> REMOVE
        // 접속을 끊은 상대도 여기서 자동으로 처리된다.
        // 그리드에서 빠지면 current에 안 잡히기 때문이다.
        for (int32_t id : previous) {
            if (current.count(id)) continue;
            const auto rm = MakeRemoveObject(id);
            Append(batch, &rm, rm.h.size);
        }

        self->ReplaceViewList(std::move(current));
    }

    // ------------------------------------------------------------------------
    // 스냅샷 조립
    //
    // 보낼 것:
    //   - 자기 상태 (입력 ack 포함) : 스냅샷 틱마다 항상
    //   - 시야 안에서 이번에 움직인 오브젝트
    //       가까운 것(NEAR_RANGE 안) : 스냅샷 틱마다
    //       먼 것                     : FAR_SNAPSHOT_INTERVAL마다
    // ------------------------------------------------------------------------
    void BuildSnapshots(const std::shared_ptr<Session>& session,
                        std::vector<char>& batch,
                        int64_t& packet_count) const {
        const auto my = session->MakeSnapshot();

        S2C_SelfState self_state{};
        InitHeader(self_state, S2C_SELF_STATE);
        self_state.last_processed_input = session->GetLastProcessedInput();
        self_state.server_tick = m_tick;
        self_state.pos   = my.move.pos;
        self_state.yaw   = my.move.yaw;
        self_state.vel_x = ClampToI16(my.move.vel_x);
        self_state.vel_y = ClampToI16(my.move.vel_y);
        self_state.vel_z = ClampToI16(my.move.vel_z);
        self_state.flags = my.move.grounded ? MF_GROUNDED : 0;
        Append(batch, &self_state, self_state.h.size);
        ++packet_count;

        const bool far_tick = (m_tick % FAR_SNAPSHOT_INTERVAL == 0);
        constexpr int64_t near_sq =
            static_cast<int64_t>(NEAR_RANGE) * NEAR_RANGE;

        for (int32_t id : session->CopyViewList()) {
            if (!g_moved.count(id)) continue;   // 안 움직였으면 보낼 필요가 없다

            WorldObject::Snapshot other;
            if (!TryGetSnapshot(id, other)) continue;

            const bool is_near =
                Distance2DSq(my.move.pos, other.move.pos) <= near_sq;
            if (!is_near && !far_tick) continue;

            S2C_MoveObject packet{};
            InitHeader(packet, S2C_MOVE_OBJECT);
            packet.object_id   = other.id;
            packet.server_tick = m_tick;
            packet.pos   = other.move.pos;
            packet.yaw   = other.move.yaw;
            packet.vel_x = ClampToI16(other.move.vel_x);
            packet.vel_y = ClampToI16(other.move.vel_y);
            Append(batch, &packet, packet.h.size);
            ++packet_count;
        }
    }

    static void Append(std::vector<char>& batch, const void* data, uint16_t len) {
        const char* p = static_cast<const char*>(data);
        batch.insert(batch.end(), p, p + len);
    }

    static int16_t ClampToI16(int32_t v) {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return static_cast<int16_t>(v);
    }

    TickWorkerPool m_pool;
    TickMetrics    m_metrics;

    uint32_t m_tick = 0;

    // 파티션별 조립 버퍼. 매 틱 할당하지 않기 위해 재사용한다.
    std::vector<std::vector<char>> m_scratch;
};
