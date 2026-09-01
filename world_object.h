#pragma once
// ============================================================================
// world_object.h — 월드에 존재하는 것들의 자료구조
//
// 기존 코드는 NPC를 SESSION으로 만들어 clients 맵에 섞어 넣었다.
//
//   auto npc_session = std::make_shared<SESSION>(INVALID_SOCKET, i);
//   clients[i] = npc_session;
//
// NPC 10마리에서는 동작하지만, 세션은 소켓 + 전송 큐 + 뮤텍스 + 수신 버퍼를
// 가진 무거운 객체다. 몬스터 1000마리를 이걸로 만들면 메모리도 낭비고
// Mass Entity 같은 데이터 지향 구조로 옮기기도 어렵다.
//
// 그래서 공통 부분만 WorldObject로 뽑고,
//   - Session   : 소켓/큐/시야목록을 가진 무거운 객체 (플레이어)
//   - NpcEntity : 좌표와 스탯만 가진 가벼운 객체 (NPC/몬스터)
// 로 나눈다.
// ============================================================================

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <cstring>

#include "protocol.h"
#include "packet_buffer.h"

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
        return m_pos;
    }

    void SetPosition(const Vec3i& p, int16_t yaw) {
        std::lock_guard lock(m_state_lock);
        m_pos = p;
        m_yaw = yaw;
    }

    int16_t GetYaw() const {
        std::lock_guard lock(m_state_lock);
        return m_yaw;
    }

    void SetName(const char* src, size_t len) {
        std::lock_guard lock(m_state_lock);
        const size_t n = (len < MAX_NAME_LEN - 1) ? len : MAX_NAME_LEN - 1;
        std::memset(m_name, 0, sizeof(m_name));
        std::memcpy(m_name, src, n);
    }

    // 다른 스레드에 넘길 스냅샷. 락을 한 번만 잡고 필요한 걸 한꺼번에 복사한다.
    struct Snapshot {
        int32_t id;
        ObjectType type;
        char name[MAX_NAME_LEN];
        Vec3i pos;
        int16_t yaw;
        int32_t hp;
        int32_t max_hp;
        uint8_t level;
        int32_t visual_id;
    };

    Snapshot MakeSnapshot() const {
        std::lock_guard lock(m_state_lock);
        Snapshot s{};
        s.id = m_id;
        s.type = m_type;
        std::memcpy(s.name, m_name, sizeof(s.name));
        s.pos = m_pos;
        s.yaw = m_yaw;
        s.hp = m_hp;
        s.max_hp = m_max_hp;
        s.level = m_level;
        s.visual_id = m_visual_id;
        return s;
    }

protected:
    const int32_t m_id;
    const ObjectType m_type;

    mutable std::mutex m_state_lock;
    char m_name[MAX_NAME_LEN]{};
    Vec3i m_pos{};
    int16_t m_yaw = 0;
    int32_t m_hp = 100;
    int32_t m_max_hp = 100;
    uint8_t m_level = 1;
    int32_t m_visual_id = 0;
};

// ----------------------------------------------------------------------------
// NpcEntity — 소켓이 없는 가벼운 객체
//
// 지금은 WorldObject를 그대로 상속하지만, 나중에 몬스터 수가 늘어나
// Mass Entity 스타일(구조체 배열)로 옮길 때는 이 클래스만 갈아끼우면 된다.
// 플레이어 세션과 섞여 있지 않으므로 그 작업이 국소적이다.
// ----------------------------------------------------------------------------

class NpcEntity : public WorldObject {
public:
    NpcEntity(int32_t id, ObjectType type) : WorldObject(id, type) {}
};

// ----------------------------------------------------------------------------
// Session — 플레이어 하나
// ----------------------------------------------------------------------------

enum class SessionState : uint8_t {
    Free,       // 빈 슬롯
    Accepted,   // 접속했지만 아직 로그인 전
    Playing,    // 로그인 완료, 월드에 존재
    Closing,    // 끊는 중. 브로드캐스트 대상에서 제외되지만 아직 참조가 남아있을 수 있다
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

    // ---- 전송 ----
    //
    // 기존 구조는 패킷마다 std::vector를 만들어 큐에 넣었다(패킷당 할당 1회).
    // 여기서는 대기 버퍼 하나에 이어붙이고, 워커가 통째로 한 번에 보낸다.
    //   - 할당: vector가 한 번 커진 뒤로는 없음
    //   - WSASend 호출 횟수: 패킷 N개 → 1회 (브로드캐스트가 많을수록 이득이 크다)

    // 반환값이 true면 호출자가 전송 워커를 시동해야 한다.
    bool EnqueueSend(const void* data, uint16_t len) {
        if (GetState() == SessionState::Closing) return false;
        if (m_socket == INVALID_SOCKET) return false;

        std::lock_guard lock(m_send_lock);
        const char* p = static_cast<const char*>(data);
        m_send_pending.insert(m_send_pending.end(), p, p + len);

        if (m_is_sending) return false;   // 이미 돌고 있는 워커가 처리한다
        m_is_sending = true;
        return true;
    }

    // 대기 버퍼를 전송 버퍼로 넘긴다.
    // false를 반환하면 보낼 게 없다는 뜻이고, 워커는 종료해야 한다.
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

    // 전송 실패 시 워커를 정리한다.
    void AbortSending() {
        std::lock_guard lock(m_send_lock);
        m_is_sending = false;
        m_send_pending.clear();
        m_send_active.clear();
    }

    const char* SendData() const { return m_send_active.data(); }
    ULONG SendSize() const { return static_cast<ULONG>(m_send_active.size()); }

    // ---- 시야 목록 ----
    //
    // 지금 이 플레이어가 "보고 있는" 오브젝트 id 집합.
    // 이게 있어야 시야에 들어온 순간 ADD, 벗어난 순간 REMOVE를 보낼 수 있다.
    // 없으면 클라이언트가 ADD를 받은 적 없는 오브젝트의 MOVE를 받게 된다.

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

    // ---- 이동 검증용 ----
    uint32_t GetLastMoveTime() const { return m_last_move_time; }
    void SetLastMoveTime(uint32_t t) { m_last_move_time = t; }

private:
    SOCKET m_socket = INVALID_SOCKET;
    std::atomic<SessionState> m_state{ SessionState::Accepted };

    PacketBuffer m_recv_buffer;

    mutable std::mutex m_send_lock;
    std::vector<char> m_send_pending;   // 쌓이는 곳
    std::vector<char> m_send_active;    // 지금 WSASend가 읽고 있는 곳
    bool m_is_sending = false;

    mutable std::mutex m_view_lock;
    std::unordered_set<int32_t> m_view_list;

    uint32_t m_last_move_time = 0;
};

// ----------------------------------------------------------------------------
// SessionManager — 지연 삭제
//
// 기존 코드의 문제:
//   tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
//   ...
//   clients.unsafe_erase(session->m_id);
//
//   이름 그대로 안전하지 않다. concurrent_unordered_map은 삽입과 순회는
//   동시에 안전하지만 삭제는 아니다. A가 끊기며 erase하는 동안 B가
//   브로드캐스트를 위해 순회 중이면 미정의 동작이다.
//   스트레스 테스트에서 간헐적으로만 터지는 종류의 버그였다.
//
//   또 하나, clients[id]는 없는 키를 조회하면 nullptr 원소를 삽입해버린다.
//   spawn_send_move_packet의 `auto pl = clients[mover];`가 그 경로였고,
//   나간 플레이어를 조회할 때마다 맵이 조용히 커졌다.
//
// 해결:
//   1) 맵 대신 고정 크기 배열. id가 곧 인덱스라 조회가 O(1)이고 삽입이 없다.
//   2) std::atomic<std::shared_ptr<T>> (C++20). load()가 참조를 하나 잡으므로
//      다른 스레드가 슬롯을 비워도 쓰던 쪽은 안전하다. 실제 소멸은 마지막
//      참조가 사라질 때 일어난다 — 이것이 지연 삭제다.
// ----------------------------------------------------------------------------

class SessionManager {
public:
    // 빈 슬롯을 찾아 세션을 만든다. 실패하면 nullptr(서버 만원).
    std::shared_ptr<Session> Acquire(SOCKET socket) {
        for (int32_t id = 0; id < MAX_PLAYERS; ++id) {
            if (m_slots[id].load() != nullptr) continue;

            auto session = std::make_shared<Session>(socket, id);

            // 같은 슬롯을 두 스레드가 동시에 집는 것을 막는다.
            std::shared_ptr<Session> expected = nullptr;
            if (m_slots[id].compare_exchange_strong(expected, session)) {
                m_count.fetch_add(1, std::memory_order_relaxed);
                return session;
            }
            // 졌으면 다음 슬롯으로
        }
        return nullptr;
    }

    // 없는 id를 넣어도 안전하다. 삽입이 일어나지 않는다.
    std::shared_ptr<Session> Get(int32_t id) const {
        if (id < 0 || id >= MAX_PLAYERS) return nullptr;
        return m_slots[id].load();
    }

    // 슬롯을 비운다. 다른 스레드가 이미 load()로 잡아둔 참조는 살아있고,
    // 그쪽 작업이 끝나면 그때 소멸한다.
    void Release(int32_t id) {
        if (id < 0 || id >= MAX_PLAYERS) return;
        if (m_slots[id].exchange(nullptr) != nullptr) {
            m_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    int32_t Count() const { return m_count.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<std::shared_ptr<Session>>, MAX_PLAYERS> m_slots;
    std::atomic<int32_t> m_count{ 0 };
};

// ----------------------------------------------------------------------------
// NpcManager — 시작할 때 한 번 만들고 프로그램이 끝날 때까지 안 지운다.
// 따라서 raw 포인터를 넘겨도 안전하다.
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
