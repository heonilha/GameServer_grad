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

protected:
    const int32_t    m_id;
    const ObjectType m_type;

    mutable std::mutex m_state_lock;
    char      m_name[MAX_NAME_LEN]{};
    MoveState m_move{};
    int32_t   m_hp = 100;
    int32_t   m_max_hp = 100;
    uint8_t   m_level = 1;
    int32_t   m_visual_id = 0;
};

// ----------------------------------------------------------------------------
// NpcEntity — 소켓이 없는 가벼운 객체
// ----------------------------------------------------------------------------

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

};

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
