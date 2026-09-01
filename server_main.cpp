// ============================================================================
// server_main.cpp — 패킷 처리와 서버 루프
// ============================================================================

#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/async_scope.hpp>

#include "protocol.h"
#include "net_core.h"
#include "world_object.h"
#include "world_grid.h"

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

// ----------------------------------------------------------------------------
// 전역
// ----------------------------------------------------------------------------

exec::async_scope g_scope;
SOCKET g_listen_socket = INVALID_SOCKET;
HANDLE g_iocp = nullptr;

SessionManager g_sessions;
NpcManager g_npcs;
WorldGrid g_grid;

std::atomic<bool> g_running{ true };

// ----------------------------------------------------------------------------
// 전송
// ----------------------------------------------------------------------------

// 세션당 정확히 하나만 도는 전송 워커.
// 대기 버퍼에 쌓인 패킷을 통째로 한 번의 WSASend로 내보낸다.
exec::task<void> SendWorker(std::shared_ptr<Session> session)
{
    for (;;) {
        if (!session->SwapSendBuffer()) co_return;   // 보낼 게 없으면 종료

        // m_send_active는 다음 SwapSendBuffer까지 바뀌지 않으므로
        // co_await 중에도 버퍼가 살아있다.
        const bool ok = co_await SendSender(
            session->GetSocket(), session->SendData(), session->SendSize());

        if (!ok) {
            session->AbortSending();
            co_return;
        }
    }
}

void SendPacket(const std::shared_ptr<Session>& session,
    const void* data, uint16_t len)
{
    if (!session) return;
    if (session->EnqueueSend(data, len)) {
        g_scope.spawn(SendWorker(session));
    }
}

template <typename T>
void SendPacket(const std::shared_ptr<Session>& session, const T& packet) {
    SendPacket(session, &packet, packet.h.size);
}

// ----------------------------------------------------------------------------
// 오브젝트 조회 — 플레이어와 NPC를 하나의 인터페이스로
// ----------------------------------------------------------------------------

bool TryGetSnapshot(int32_t id, WorldObject::Snapshot& out)
{
    if (id >= NPC_ID_START) {
        NpcEntity* npc = g_npcs.Get(id);
        if (!npc) return false;
        out = npc->MakeSnapshot();
        return true;
    }

    auto session = g_sessions.Get(id);
    if (!session || session->GetState() != SessionState::Playing) return false;
    out = session->MakeSnapshot();
    return true;
}

// ----------------------------------------------------------------------------
// 패킷 조립
// ----------------------------------------------------------------------------

S2C_AddObject MakeAddObject(const WorldObject::Snapshot& s)
{
    S2C_AddObject packet{};
    InitHeader(packet, S2C_ADD_OBJECT);
    packet.object_id = s.id;
    packet.object_type = static_cast<uint8_t>(s.type);
    packet.visual_id = s.visual_id;
    std::memcpy(packet.obj_name, s.name, sizeof(packet.obj_name));
    packet.pos = s.pos;
    packet.yaw = s.yaw;
    packet.hp = s.hp;
    packet.max_hp = s.max_hp;
    packet.level = s.level;
    return packet;
}

S2C_MoveObject MakeMoveObject(const WorldObject::Snapshot& s, uint32_t move_time)
{
    S2C_MoveObject packet{};
    InitHeader(packet, S2C_MOVE_OBJECT);
    packet.object_id = s.id;
    packet.pos = s.pos;
    packet.yaw = s.yaw;
    packet.move_time = move_time;
    return packet;
}

S2C_RemoveObject MakeRemoveObject(int32_t id)
{
    S2C_RemoveObject packet{};
    InitHeader(packet, S2C_REMOVE_OBJECT);
    packet.object_id = id;
    return packet;
}

// ----------------------------------------------------------------------------
// 시야 갱신
//
// 이동할 때마다 "지금 보이는 것"과 "아까 보이던 것"을 비교해서
// 새로 들어온 것에는 ADD, 빠져나간 것에는 REMOVE를 보낸다.
// 이 처리가 없으면 클라이언트는 ADD를 받은 적 없는 오브젝트의
// MOVE 패킷을 받게 되고, 그 패킷은 조용히 버려진다.
// ----------------------------------------------------------------------------

void UpdateViewList(const std::shared_ptr<Session>& self)
{
    const auto my = self->MakeSnapshot();

    // 1. 지금 시야 안에 있는 것들을 모은다 (주변 3x3 섹터만 본다)
    std::unordered_set<int32_t> current;
    for (int32_t id : g_grid.QueryNear(my.pos)) {
        if (id == my.id) continue;

        WorldObject::Snapshot other;
        if (!TryGetSnapshot(id, other)) continue;
        if (!IsInViewRange(my.pos, other.pos)) continue;

        current.insert(id);
    }

    const auto previous = self->CopyViewList();

    // 2. 새로 들어온 것 → 나에게 ADD
    for (int32_t id : current) {
        if (previous.count(id)) continue;

        WorldObject::Snapshot other;
        if (!TryGetSnapshot(id, other)) continue;
        SendPacket(self, MakeAddObject(other));

        // 상대가 플레이어라면, 상대 시야에도 나를 넣어준다
        if (id < NPC_ID_START) {
            auto peer = g_sessions.Get(id);
            if (peer && peer->GetState() == SessionState::Playing &&
                !peer->IsInView(my.id)) {
                peer->AddToView(my.id);
                SendPacket(peer, MakeAddObject(my));
            }
        }
    }

    // 3. 빠져나간 것 → 나에게 REMOVE
    for (int32_t id : previous) {
        if (current.count(id)) continue;
        SendPacket(self, MakeRemoveObject(id));

        if (id < NPC_ID_START) {
            auto peer = g_sessions.Get(id);
            if (peer && peer->IsInView(my.id)) {
                peer->RemoveFromView(my.id);
                SendPacket(peer, MakeRemoveObject(my.id));
            }
        }
    }

    self->ReplaceViewList(std::move(current));
}

// 내 이동을 나를 보고 있는 플레이어들에게 알린다.
void BroadcastMove(const std::shared_ptr<Session>& self, uint32_t move_time)
{
    const auto my = self->MakeSnapshot();
    const auto packet = MakeMoveObject(my, move_time);

    for (int32_t id : g_grid.QueryNear(my.pos)) {
        if (id == my.id || id >= NPC_ID_START) continue;

        auto peer = g_sessions.Get(id);
        if (!peer || peer->GetState() != SessionState::Playing) continue;
        if (!peer->IsInView(my.id)) continue;   // 아직 ADD를 못 받은 상대는 건너뛴다

        SendPacket(peer, packet);
    }
}

// 내가 월드에서 사라짐을 알린다.
void BroadcastRemove(const std::shared_ptr<Session>& self)
{
    const auto my = self->MakeSnapshot();
    const auto packet = MakeRemoveObject(my.id);

    for (int32_t id : self->CopyViewList()) {
        if (id >= NPC_ID_START) continue;
        auto peer = g_sessions.Get(id);
        if (!peer) continue;
        peer->RemoveFromView(my.id);
        SendPacket(peer, packet);
    }
}

// ----------------------------------------------------------------------------
// 패킷 핸들러
//
// 기존에는 process_packet 안의 거대한 switch였다. 팀원 여럿이 각자 패킷을
// 추가하면 매번 그 한 함수에서 충돌이 났다.
// 여기서는 타입별 함수 포인터 테이블이라, 새 패킷은 함수 하나를 쓰고
// RegisterHandlers에 한 줄 추가하면 끝난다.
//
// 반환값: false면 이 세션을 끊는다.
// ----------------------------------------------------------------------------

using PacketHandler = bool(*)(const std::shared_ptr<Session>&, const uint8_t*);

PacketHandler g_handlers[PT_MAX]{};

bool HandleLogin(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Accepted) return false;  // 중복 로그인

    const auto* packet = reinterpret_cast<const C2S_Login*>(raw);

    // packet->username은 널 종료가 보장되지 않는다. 길이를 직접 재서 자른다.
    size_t len = 0;
    while (len < MAX_NAME_LEN && packet->username[len] != '\0') ++len;
    self->SetName(packet->username, len);

    // 시작 위치
    Vec3i start{};
    start.x = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
    start.y = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
    start.z = 0;
    self->SetPosition(start, 0);

    const auto my = self->MakeSnapshot();

    S2C_LoginResult result{};
    InitHeader(result, S2C_LOGIN_RESULT);
    result.success = 1;
    result.object_id = my.id;
    std::strncpy(result.message, "Login successful.", sizeof(result.message) - 1);
    SendPacket(self, result);

    S2C_AvatarInfo avatar{};
    InitHeader(avatar, S2C_AVATAR_INFO);
    avatar.object_id = my.id;
    avatar.visual_id = my.visual_id;
    avatar.pos = my.pos;
    avatar.yaw = my.yaw;
    avatar.hp = my.hp;
    avatar.max_hp = my.max_hp;
    avatar.level = my.level;
    SendPacket(self, avatar);

    // 상태를 Playing으로 올린 뒤에 그리드에 넣는다.
    // 순서가 반대면 아직 Playing이 아닌 나를 다른 세션이 조회해서 놓칠 수 있다.
    self->SetState(SessionState::Playing);
    g_grid.Add(my.id, my.pos);

    UpdateViewList(self);

    std::cout << "Player[" << my.id << "] logged in as " << my.name << "\n";
    return true;
}

bool HandleMove(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Playing) return false;

    const auto* packet = reinterpret_cast<const C2S_Move*>(raw);

    // 월드 밖 좌표는 거부
    if (packet->pos.x < WORLD_MIN || packet->pos.x > WORLD_MAX ||
        packet->pos.y < WORLD_MIN || packet->pos.y > WORLD_MAX) {
        std::cout << "Player[" << self->GetId() << "] out-of-bounds move\n";
        return false;
    }

    const Vec3i from = self->GetPosition();

    // 스피드핵 검사: 경과 시간 대비 이동 거리가 과하면 무시한다.
    // 지금은 클라이언트 권위 이동이라 이 정도가 최소한의 방어선이다.
    const uint32_t last = self->GetLastMoveTime();
    if (last != 0 && packet->client_time > last) {
        const uint32_t elapsed_ms = packet->client_time - last;
        const int64_t allowed =
            static_cast<int64_t>(MAX_MOVE_SPEED_PER_SEC) * elapsed_ms / 1000 + 100;
        if (Distance2DSq(from, packet->pos) > allowed * allowed) {
            // 끊지 않고 서버 위치를 다시 알려 되돌린다
            const auto my = self->MakeSnapshot();
            SendPacket(self, MakeMoveObject(my, packet->client_time));
            return true;
        }
    }
    self->SetLastMoveTime(packet->client_time);

    self->SetPosition(packet->pos, packet->yaw);
    g_grid.Move(self->GetId(), from, packet->pos);

    UpdateViewList(self);
    BroadcastMove(self, packet->client_time);
    return true;
}

bool HandleChat(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Playing) return false;

    const auto* in = reinterpret_cast<const C2S_Chat*>(raw);

    S2C_ChatMessage out{};
    InitHeader(out, S2C_CHAT_MESSAGE);
    out.object_id = self->GetId();
    std::memcpy(out.message, in->message, sizeof(out.message));
    out.message[MAX_CHAT_MSG_LEN - 1] = '\0';

    SendPacket(self, out);
    for (int32_t id : self->CopyViewList()) {
        if (id >= NPC_ID_START) continue;
        SendPacket(g_sessions.Get(id), &out, out.h.size);
    }
    return true;
}

bool HandleTeleport(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Playing) return false;

    const auto* packet = reinterpret_cast<const C2S_Teleport*>(raw);
    const Vec3i from = self->GetPosition();

    self->SetPosition(packet->pos, self->GetYaw());
    self->SetLastMoveTime(0);            // 순간이동은 속도 검사에서 제외
    g_grid.Move(self->GetId(), from, packet->pos);

    UpdateViewList(self);
    BroadcastMove(self, 0);
    return true;
}

bool HandleLogout(const std::shared_ptr<Session>&, const uint8_t*)
{
    return false;   // false = 세션 종료
}

void RegisterHandlers()
{
    g_handlers[C2S_LOGIN] = HandleLogin;
    g_handlers[C2S_MOVE] = HandleMove;
    g_handlers[C2S_CHAT] = HandleChat;
    g_handlers[C2S_TELEPORT] = HandleTeleport;
    g_handlers[C2S_LOGOUT] = HandleLogout;
    // 새 패킷은 여기에 한 줄 추가. 다른 파일은 건드릴 필요 없다.
}

// ----------------------------------------------------------------------------
// 세션 루프
// ----------------------------------------------------------------------------

exec::task<void> HandleSession(std::shared_ptr<Session> session)
{
    PacketBuffer& buffer = session->RecvBuffer();

    for (;;) {
        if (buffer.Writable() == 0) buffer.Compact();
        if (buffer.Writable() == 0) {
            std::cout << "[Session " << session->GetId()
                << "] recv buffer full — dropping\n";
            break;
        }

        // 세션이 소유한 버퍼에 직접 받는다. 중간 복사가 없다.
        const RecvResult r = co_await RecvSender(
            session->GetSocket(),
            buffer.WritePtr(),
            static_cast<ULONG>(buffer.Writable()));

        if (!r.ok || r.bytes == 0) break;
        buffer.Commit(r.bytes);

        bool disconnect = false;
        for (;;) {
            const uint8_t* raw = nullptr;
            uint16_t size = 0;
            if (!buffer.PeekPacket(raw, size)) {
                if (buffer.IsCorrupted()) {
                    std::cout << "[Session " << session->GetId()
                        << "] corrupted packet stream\n";
                    disconnect = true;
                }
                break;   // 덜 왔으면 다음 recv를 기다린다
            }

            const uint16_t type = reinterpret_cast<const PacketHeader*>(raw)->type;
            PacketHandler handler =
                (type < PT_MAX) ? g_handlers[type] : nullptr;

            if (!handler) {
                std::cout << "[Session " << session->GetId()
                    << "] unknown packet type " << type << "\n";
                disconnect = true;
                break;
            }

            // 핸들러는 코루틴이 아니다. 전송은 전부 fire-and-forget이라
            // 여기서 co_await할 이유가 없고, 패킷마다 코루틴 프레임을
            // 할당하지 않아도 된다.
            if (!handler(session, raw)) {
                disconnect = true;
                break;
            }
            buffer.Consume(size);
        }

        buffer.Compact();
        if (disconnect) break;
    }

    // ---- 정리 ----
    session->SetState(SessionState::Closing);

    if (session->GetSocket() != INVALID_SOCKET) {
        shutdown(session->GetSocket(), SD_BOTH);
    }

    const Vec3i last_pos = session->GetPosition();
    g_grid.Remove(session->GetId(), last_pos);
    BroadcastRemove(session);

    // 슬롯만 비운다. 다른 스레드가 이미 잡아둔 shared_ptr가 있으면
    // 그쪽이 끝난 뒤에 실제 소멸이 일어난다 (지연 삭제).
    g_sessions.Release(session->GetId());

    std::cout << "[Session " << session->GetId() << "] ended\n";
}

// ----------------------------------------------------------------------------
// accept 루프
// ----------------------------------------------------------------------------

exec::task<void> AcceptLoop()
{
    while (g_running.load()) {
        SOCKET client = co_await AcceptSender(g_listen_socket);
        if (client == INVALID_SOCKET) continue;

        // NPC는 g_npcs에 따로 있으므로 플레이어 슬롯을 잡아먹지 않는다.
        auto session = g_sessions.Acquire(client);
        if (!session) {
            std::cout << "Server full. Rejecting connection.\n";
            closesocket(client);
            continue;
        }

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(client), g_iocp, 0, 0);

        std::cout << "Client connected. id=" << session->GetId() << "\n";
        g_scope.spawn(HandleSession(session));
    }
}

// ----------------------------------------------------------------------------
// IOCP 디스패처
// ----------------------------------------------------------------------------

void IoDispatchLoop()
{
    for (;;) {
        DWORD num_bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED over = nullptr;

        BOOL ret = GetQueuedCompletionStatus(
            g_iocp, &num_bytes, &key, &over, INFINITE);

        if (over == nullptr) {
            if (key == static_cast<ULONG_PTR>(-1)) break;   // 종료 신호
            continue;
        }

        IoOpBase* io = reinterpret_cast<IoOpBase*>(over);
        io->bytes = num_bytes;
        io->success = (ret == TRUE);
        io->complete();
    }
}

// ----------------------------------------------------------------------------
// NPC 초기화
// ----------------------------------------------------------------------------

void InitializeNpcs(int32_t count)
{
    g_npcs.Initialize(count);
    for (int32_t i = 0; i < g_npcs.Count(); ++i) {
        NpcEntity* npc = g_npcs.At(i);

        Vec3i pos{};
        pos.x = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
        pos.y = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
        pos.z = 0;

        npc->SetPosition(pos, 0);
        const char name[] = "NPC";
        npc->SetName(name, sizeof(name) - 1);

        g_grid.Add(npc->GetId(), pos);
    }
    std::cout << "NPC initialized: " << g_npcs.Count() << "\n";
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

int main()
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    g_listen_socket =
        WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (g_listen_socket == INVALID_SOCKET) {
        std::cerr << "socket creation failed\n";
        return 1;
    }

    SOCKADDR_IN addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.S_un.S_addr = INADDR_ANY;

    if (::bind(g_listen_socket, reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << "\n";
        return 1;
    }
    listen(g_listen_socket, SOMAXCONN);

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_listen_socket), g_iocp, 0, 0);

    RegisterHandlers();
    InitializeNpcs(100);

    g_scope.spawn(AcceptLoop());

    std::cout << "Game server started on port " << SERVER_PORT << "\n";
    std::cout << "Grid: " << WorldGrid::GRID_DIM << " x " << WorldGrid::GRID_DIM
        << " sectors (" << SECTOR_SIZE / 100 << "m each)\n";

    unsigned int worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0) worker_count = 4;

    std::vector<std::thread> dispatchers;
    dispatchers.reserve(worker_count);
    for (unsigned int i = 0; i < worker_count; ++i) {
        dispatchers.emplace_back(IoDispatchLoop);
    }

    for (auto& t : dispatchers) t.join();

    closesocket(g_listen_socket);
    WSACleanup();
    return 0;
}
