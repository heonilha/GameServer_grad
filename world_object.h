#pragma once
// ============================================================================
// world_object.h — 월드에 존재하는 것들의 자료구조
//
// [서버 권위 이동 개정판]
//
// Session이 이제 MoveState(위치/속도/접지)를 소유한다.
// 위치는 더 이상 클라가 통보하는 값이 아니라 서버 틱 루프가 계산하는 값이다.
//
// 클라가 보내는 것은 입력뿐이고, 입력은 큐에 쌓였다가 틱 루프에서 소비된다.
// ============================================================================

#include <atomic>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <cstring>

#include "protocol.h"
#include "packet_buffer.h"
#include "movement.h"

// ----------------------------------------------------------------------------
// 공통 베이스
// ----------------------------------------------------------------------------

class WorldObject {
public:
    WorldObject(int32_t id, ObjectType type) : m_id(id), m_type(type) {}
    virtual ~WorldObject() = default;

    int32_t GetId() const { return m_id; }
    ObjectType GetType() const { return m_type; }

    Vec3i GetPosition() const {
        std::lock_guard lock(m_state_lock);
        return m_move.pos;
    }

    MoveState GetMoveState() const {
        std::lock_guard lock(m_state_lock);
        return m_move;
    }

    void SetMoveState(const MoveState& s) {
        std::lock_guard lock(m_state_lock);
        m_move = s;
    }

    void SetPosition(const Vec3i& p, int16_t yaw) {
        std::lock_guard lock(m_state_lock);
        m_move.pos = p;
        m_move.yaw = yaw;
    }

    void SetName(const char* src, size_t len) {
        std::lock_guard lock(m_state_lock);
        const size_t n = (len < MAX_NAME_LEN - 1) ? len : MAX_NAME_LEN - 1;
        std::memset(m_name, 0, sizeof(m_name));
        std::memcpy(m_name, src, n);
    }

    // 다른 스레드에 넘길 스냅샷. 락을 한 번만 잡고 한꺼번에 복사한다.
    struct Snapshot {
        int32_t    id;
        ObjectType type;
        char       name[MAX_NAME_LEN];
        MoveState  move;
        int32_t    hp;
        int32_t    max_hp;
        uint8_t    level;
        int32_t    visual_id;
    };

    Snapshot MakeSnapshot() const {
        std::lock_guard lock(m_state_lock);
        Snapshot s{};
        s.id = m_id;
        s.type = m_type;
        std::memcpy(s.name, m_name, sizeof(s.name));
        s.move = m_move;
        s.hp = m_hp;
        s.max_hp = m_max_hp;
        s.level = m_level;
        s.visual_id = m_visual_id;
        return s;
    }

    // ---- 전투 ----

    bool IsAlive() const {
        std::lock_guard lock(m_state_lock);
        return m_hp > 0;
    }

    // 피해를 적용하고 남은 HP를 돌려준다.
    // killed에는 "이번 타격으로 죽었는가"가 들어간다.
    //
    // 이미 죽은 대상에 두 번째 타격이 들어오는 경우가 실제로 생긴다.
    // (같은 틱에 파이어볼과 근접 공격이 겹치는 등)
    // 그때 killed를 다시 true로 주면 사망 처리가 두 번 돌아
    // 경험치가 이중 지급되므로, 살아있을 때만 true로 만든다.
    int32_t ApplyDamage(int32_t amount, bool& killed) {
        std::lock_guard lock(m_state_lock);
        killed = false;
        if (m_hp <= 0) return 0;

        m_hp -= amount;
        if (m_hp <= 0) {
            m_hp = 0;
            killed = true;
        }
        return m_hp;
    }

    // 마나를 소비한다. 부족하면 아무것도 하지 않고 false.
    // 확인과 차감을 한 락 안에서 해야 같은 틱에 두 번 시전되지 않는다.
    bool ConsumeMp(int32_t amount) {
        std::lock_guard lock(m_state_lock);
        if (m_mp < amount) return false;
        m_mp -= amount;
        return true;
    }

    int32_t GetMp() const {
        std::lock_guard lock(m_state_lock);
        return m_mp;
    }

    void RegenMp(int32_t amount) {
        std::lock_guard lock(m_state_lock);
        m_mp += amount;
        if (m_mp > m_max_mp) m_mp = m_max_mp;
    }

    void Revive(const Vec3i& pos) {
        std::lock_guard lock(m_state_lock);
        m_hp = m_max_hp;
        m_mp = m_max_mp;
        m_move = MoveState{};
        m_move.pos = pos;
        m_move.grounded = true;
    }

    uint8_t GetClassId() const { return m_class_id; }
    void SetClassId(uint8_t c) { m_class_id = c; }

    // ---- 섹터 소유권 ----
    //
    // 이 객체를 변경할 권한을 가진 섹터. 그리드 소속과 항상 일치해야 한다.
    // 위치(m_move.pos)는 시뮬레이션이 즉시 바꾸지만 소속은 커맨드 적용
    // 시점에 바뀌므로, 한 틱 안에서 둘이 어긋나는 구간이 존재한다.
    // 그래서 위치에서 매번 계산하지 않고 따로 들고 있는다.
    int32_t GetOwnerSector() const {
        return m_owner_sector.load(std::memory_order_relaxed);
    }
    void SetOwnerSector(int32_t index) {
        m_owner_sector.store(index, std::memory_order_relaxed);
    }

    // 테이블에서 읽은 값으로 초기화한다.
    void SetStats(int32_t max_hp, int32_t visual_id) {
        std::lock_guard lock(m_state_lock);
        m_max_hp = max_hp;
        m_hp = max_hp;
        m_visual_id = visual_id;
    }

protected:
    const int32_t    m_id;
    const ObjectType m_type;

    mutable std::mutex m_state_lock;
    char      m_name[MAX_NAME_LEN]{};
    MoveState m_move{};
    int32_t   m_hp = 100;
    int32_t   m_max_hp = 100;
    int32_t   m_mp = 100;
    int32_t   m_max_mp = 100;
    uint8_t   m_level = 1;
    std::atomic<int32_t> m_owner_sector{ -1 };
    uint8_t   m_class_id = 0;   // 0=전사, 1=마법사
    int32_t   m_visual_id = 0;
};

// ----------------------------------------------------------------------------
// NpcEntity — 소켓이 없는 가벼운 객체
// ----------------------------------------------------------------------------

// AI 상태 기계.
// 넷이면 졸작 범위의 몬스터 동작이 전부 표현된다.
enum class AiState : uint8_t {
    Idle,     // 제자리. 어그로 대상을 찾는다
    Chase,    // 대상을 향해 이동
    Attack,   // 사거리 안. 쿨타임마다 공격
    Return,   // 리쉬에 걸려 스폰 지점으로 복귀
};

class NpcEntity : public WorldObject {
public:
    NpcEntity(int32_t id, ObjectType type) : WorldObject(id, type) {}

    // 죽은 자리가 아니라 원래 자리에서 되살아나야 하므로 스폰 위치를 기억한다.
    void SetSpawnPoint(const Vec3i& p) { m_spawn_point = p; }
    const Vec3i& GetSpawnPoint() const { return m_spawn_point; }

    uint32_t GetRespawnTick() const {
        return m_respawn_tick.load(std::memory_order_relaxed);
    }
    void SetRespawnTick(uint32_t t) {
        m_respawn_tick.store(t, std::memory_order_relaxed);
    }

    // ---- AI ----
    //
    // 아래 필드들은 락이 없다. AI 페이즈와 전투 페이즈가 모두 틱 스레드에서
    // 단일 스레드로 돌기 때문이다. 다른 스레드에서 건드리면 안 된다.
    // (위치만은 WorldObject의 락으로 보호된다 — 스냅샷 전송이 병렬이라서다)

    uint16_t monster_id = 0;
    AiState  ai_state = AiState::Idle;
    int32_t  target_id = -1;
    uint32_t next_attack_tick = 0;
    uint32_t pending_hit_tick = 0;   // 0이면 예약된 타격 없음
    uint32_t repath_tick = 0;

    std::vector<Vec3i> path;
    size_t path_index = 0;

    // 수동형 몬스터가 맞았을 때 반격하게 한다.
    // 능동형은 시야에 들어오기만 해도 이 함수 없이 Chase로 간다.
    void OnDamaged(int32_t attacker_id) {
        if (attacker_id < 0) return;
        if (ai_state == AiState::Idle || ai_state == AiState::Return) {
            target_id = attacker_id;
            ai_state = AiState::Chase;
            path.clear();
            path_index = 0;
            repath_tick = 0;
        }
    }

    void ClearAi() {
        ai_state = AiState::Idle;
        target_id = -1;
        pending_hit_tick = 0;
        path.clear();
        path_index = 0;
        repath_tick = 0;
    }

private:
    Vec3i m_spawn_point{};
    std::atomic<uint32_t> m_respawn_tick{ 0 };
};

// ----------------------------------------------------------------------------
// Session — 플레이어 하나
// ----------------------------------------------------------------------------

enum class SessionState : uint8_t {
    Free,
    Accepted,   // 접속했지만 로그인 전
    Playing,    // 월드에 존재
    Closing,    // 끊는 중. 아직 참조가 남아있을 수 있다
};

class Session : public WorldObject {
public:
    Session(SOCKET socket, int32_t id)
        : WorldObject(id, OBJ_PLAYER), m_socket(socket) {}

    ~Session() override {
        if (m_socket != INVALID_SOCKET) closesocket(m_socket);
    }

    SOCKET GetSocket() const { return m_socket; }

    SessionState GetState() const { return m_state.load(std::memory_order_acquire); }
    void SetState(SessionState s) { m_state.store(s, std::memory_order_release); }

    PacketBuffer& RecvBuffer() { return m_recv_buffer; }

    // ------------------------------------------------------------------------
    // 입력 큐
    //
    // 수신 스레드가 넣고, 틱 루프가 꺼낸다.
    //
    // 큐가 무한정 자라지 않게 상한을 둔다. 상한을 넘으면 오래된 것부터
    // 버리는데, 최신 입력이 플레이어의 현재 의도에 가깝기 때문이다.
    // (오래된 것을 살리면 손을 뗀 뒤에도 캐릭터가 계속 움직인다)
    // ------------------------------------------------------------------------

    void PushInput(const MoveInput& input) {
        std::lock_guard lock(m_input_lock);

        // 재전송이나 순서 뒤바뀜으로 온 오래된 입력은 버린다
        if (input.sequence <= m_last_queued_sequence) return;
        m_last_queued_sequence = input.sequence;

        m_input_queue.push_back(input);
        while (m_input_queue.size() > MAX_INPUT_QUEUE) {
            m_input_queue.pop_front();
        }
    }

    // 틱 루프가 한 번에 가져갈 입력들. 최대 MAX_INPUTS_PER_TICK개.
    void PopInputs(std::vector<MoveInput>& out) {
        out.clear();
        std::lock_guard lock(m_input_lock);
        const size_t count =
            (m_input_queue.size() < MAX_INPUTS_PER_TICK)
            ? m_input_queue.size()
            : static_cast<size_t>(MAX_INPUTS_PER_TICK);

        for (size_t i = 0; i < count; ++i) {
            out.push_back(m_input_queue.front());
            m_input_queue.pop_front();
        }
    }

    uint32_t GetLastProcessedInput() const {
        return m_last_processed.load(std::memory_order_relaxed);
    }
    void SetLastProcessedInput(uint32_t seq) {
        m_last_processed.store(seq, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------------
    // 전송
    //
    // 패킷마다 vector를 만들지 않는다. 대기 버퍼에 이어붙였다가
    // 워커가 통째로 한 번의 WSASend로 내보낸다.
    // 서버 권위로 바뀌면서 틱마다 스냅샷이 나가므로 이 합치기가 더 중요해졌다.
    // ------------------------------------------------------------------------

    bool EnqueueSend(const void* data, uint16_t len) {
        if (GetState() == SessionState::Closing) return false;
        if (m_socket == INVALID_SOCKET) return false;

        std::lock_guard lock(m_send_lock);
        const char* p = static_cast<const char*>(data);
        m_send_pending.insert(m_send_pending.end(), p, p + len);

        if (m_is_sending) return false;
        m_is_sending = true;
        return true;
    }

    // 여러 패킷을 미리 이어붙인 버퍼를 한 번에 넣는다.
    //
    // 틱 루프는 한 세션에 수십 개의 스냅샷을 보낸다. EnqueueSend를 그만큼
    // 부르면 락도 그만큼 잡는데, 동접 3000 x 시야 50이면 틱당 15만 회다.
    // 호출자가 로컬 버퍼에 조립한 뒤 이 함수를 한 번만 부르면
    // 락 획득이 세션당 1회로 줄어든다.
    bool EnqueueSendBatch(const std::vector<char>& batch) {
        if (batch.empty()) return false;
        if (GetState() == SessionState::Closing) return false;
        if (m_socket == INVALID_SOCKET) return false;

        std::lock_guard lock(m_send_lock);
        m_send_pending.insert(m_send_pending.end(), batch.begin(), batch.end());

        if (m_is_sending) return false;
        m_is_sending = true;
        return true;
    }

    bool SwapSendBuffer() {
        std::lock_guard lock(m_send_lock);
        if (m_send_pending.empty()) {
            m_is_sending = false;
            return false;
        }
        m_send_active.clear();
        m_send_active.swap(m_send_pending);
        return true;
    }

    void AbortSending() {
        std::lock_guard lock(m_send_lock);
        m_is_sending = false;
        m_send_pending.clear();
        m_send_active.clear();
    }

    const char* SendData() const { return m_send_active.data(); }
    ULONG SendSize() const { return static_cast<ULONG>(m_send_active.size()); }

    // ------------------------------------------------------------------------
    // 시야 목록
    // ------------------------------------------------------------------------

    std::unordered_set<int32_t> CopyViewList() const {
        std::lock_guard lock(m_view_lock);
        return m_view_list;
    }

    void ReplaceViewList(std::unordered_set<int32_t>&& next) {
        std::lock_guard lock(m_view_lock);
        m_view_list = std::move(next);
    }

    bool IsInView(int32_t id) const {
        std::lock_guard lock(m_view_lock);
        return m_view_list.count(id) > 0;
    }

    void AddToView(int32_t id) {
        std::lock_guard lock(m_view_lock);
        m_view_list.insert(id);
    }

    void RemoveFromView(int32_t id) {
        std::lock_guard lock(m_view_lock);
        m_view_list.erase(id);
    }

    // 시야 "계산" 자체를 직렬화하는 락.
    // m_view_lock은 목록 접근만 보호하므로, 두 스레드가 동시에 시야를
    // 계산하면 각자 같은 previous를 읽고 같은 ADD를 두 번 보낼 수 있다.
    // (틱 루프의 페이즈 2와, 로그인 직후 I/O 스레드에서 부르는 즉시 갱신이
    //  겹치는 경우가 실제로 생긴다)
    std::mutex& ViewUpdateLock() { return m_view_update_lock; }

    // ------------------------------------------------------------------------
    // 스킬 쿨타임
    //
    // 확인과 기록을 한 번에 한다. 나눠 두면 같은 틱에 도착한 두 패킷이
    // 둘 다 "쿨타임 지났음"을 보고 통과할 수 있다.
    // ------------------------------------------------------------------------
    bool TryConsumeCooldown(uint16_t skill_id, uint32_t now_tick,
                            int32_t cooldown_ticks) {
        if (skill_id >= MAX_SKILLS) return false;
        std::lock_guard lock(m_cooldown_lock);

        const uint32_t ready_at = m_cooldown_until[skill_id];
        if (ready_at != 0 && now_tick < ready_at) return false;

        m_cooldown_until[skill_id] = now_tick + cooldown_ticks;
        return true;
    }

    // ---- 부활 ----
    uint32_t GetRespawnTick() const {
        return m_respawn_tick.load(std::memory_order_relaxed);
    }
    void SetRespawnTick(uint32_t t) {
        m_respawn_tick.store(t, std::memory_order_relaxed);
    }

private:
    SOCKET m_socket = INVALID_SOCKET;
    std::atomic<SessionState> m_state{ SessionState::Accepted };

    PacketBuffer m_recv_buffer;

    mutable std::mutex m_input_lock;
    std::deque<MoveInput> m_input_queue;
    uint32_t m_last_queued_sequence = 0;
    std::atomic<uint32_t> m_last_processed{ 0 };

    mutable std::mutex m_send_lock;
    std::vector<char> m_send_pending;
    std::vector<char> m_send_active;
    bool m_is_sending = false;

    mutable std::mutex m_view_lock;
    std::unordered_set<int32_t> m_view_list;
    std::mutex m_view_update_lock;

    std::mutex m_cooldown_lock;
    std::array<uint32_t, MAX_SKILLS> m_cooldown_until{};
    std::atomic<uint32_t> m_respawn_tick{ 0 };   // 0이면 부활 대기 아님
};

// ----------------------------------------------------------------------------
// 이번 틱에 위치가 바뀐 오브젝트 id.
//
// 페이즈 1(플레이어 이동)과 AI 페이즈(몬스터 이동)가 채우고,
// 페이즈 3(스냅샷 전송)이 읽는다. 채우는 쪽이 둘 다 단일 스레드이고
// 읽는 쪽은 그 뒤에 돌기 때문에 락이 필요 없다.
//
// 안 움직인 오브젝트의 위치를 보내지 않는 것이 대역폭 절감의 핵심이라
// 이 집합이 정확해야 한다.
// ----------------------------------------------------------------------------
extern std::unordered_set<int32_t> g_moved;

// ----------------------------------------------------------------------------
// SessionManager — 지연 삭제
//
// tbb::concurrent_unordered_map + unsafe_erase를 쓰지 않는 이유:
//   그 컨테이너는 삽입과 순회는 동시에 안전하지만 삭제는 아니다.
//   또 operator[]가 없는 키를 조회하면 nullptr 원소를 삽입해버려서,
//   나간 플레이어를 조회할 때마다 맵이 조용히 커졌다.
//
// 대신 id를 인덱스로 쓰는 고정 배열 + std::atomic<std::shared_ptr>.
// load()가 참조를 하나 잡으므로 다른 스레드가 슬롯을 비워도 안전하고,
// 실제 소멸은 마지막 참조가 사라질 때 일어난다 (지연 삭제).
// ----------------------------------------------------------------------------

class SessionManager {
public:
    std::shared_ptr<Session> Acquire(SOCKET socket) {
        for (int32_t id = 0; id < MAX_PLAYERS; ++id) {
            if (m_slots[id].load() != nullptr) continue;

            auto session = std::make_shared<Session>(socket, id);
            std::shared_ptr<Session> expected = nullptr;
            if (m_slots[id].compare_exchange_strong(expected, session)) {
                m_count.fetch_add(1, std::memory_order_relaxed);
                return session;
            }
        }
        return nullptr;
    }

    std::shared_ptr<Session> Get(int32_t id) const {
        if (id < 0 || id >= MAX_PLAYERS) return nullptr;
        return m_slots[id].load();
    }

    void Release(int32_t id) {
        if (id < 0 || id >= MAX_PLAYERS) return;
        if (m_slots[id].exchange(nullptr) != nullptr) {
            m_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // 틱 루프가 매 틱 전체를 훑는다. 슬롯 배열이라 순회가 단순하다.
    template <typename F>
    void ForEach(F&& f) const {
        for (int32_t id = 0; id < MAX_PLAYERS; ++id) {
            auto s = m_slots[id].load();
            if (s) f(s);
        }
    }

    // 병렬 페이즈용. 슬롯 범위를 스레드별로 나눠 처리한다.
    template <typename F>
    void ForEachInRange(int32_t begin, int32_t end, F&& f) const {
        if (begin < 0) begin = 0;
        if (end > MAX_PLAYERS) end = MAX_PLAYERS;
        for (int32_t id = begin; id < end; ++id) {
            auto s = m_slots[id].load();
            if (s) f(s);
        }
    }

    static constexpr int32_t SlotCount() { return MAX_PLAYERS; }

    int32_t Count() const { return m_count.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<std::shared_ptr<Session>>, MAX_PLAYERS> m_slots;
    std::atomic<int32_t> m_count{ 0 };
};

// ----------------------------------------------------------------------------
// NpcManager — 시작할 때 만들고 종료까지 유지. raw 포인터가 항상 유효하다.
// ----------------------------------------------------------------------------

class NpcManager {
public:
    void Initialize(int32_t count) {
        m_count = (count < MAX_NPCS) ? count : MAX_NPCS;
        m_npcs.reserve(m_count);
        for (int32_t i = 0; i < m_count; ++i) {
            m_npcs.emplace_back(
                std::make_unique<NpcEntity>(NPC_ID_START + i, OBJ_NPC));
        }
    }

    NpcEntity* Get(int32_t id) const {
        const int32_t index = id - NPC_ID_START;
        if (index < 0 || index >= m_count) return nullptr;
        return m_npcs[index].get();
    }

    int32_t Count() const { return m_count; }
    NpcEntity* At(int32_t index) const { return m_npcs[index].get(); }

private:
    std::vector<std::unique_ptr<NpcEntity>> m_npcs;
    int32_t m_count = 0;
};
