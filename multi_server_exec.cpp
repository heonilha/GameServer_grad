#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <windows.h>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include "protocol.h"
#include <tbb/concurrent_unordered_map.h>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/async_scope.hpp>

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

using namespace std;

exec::async_scope g_scope;

// ============================================================================
// 1. IoOpBase: WSAOVERLAPPED가 반드시 첫 멤버 (offset 0 보장)
// ============================================================================

struct IoOpBase {
    WSAOVERLAPPED overlapped{};
    DWORD bytes = 0;
    bool success = false;
    void (*complete_fn)(IoOpBase*) noexcept = nullptr;

    void complete() noexcept { complete_fn(this); }
};

// ============================================================================
// 2. RecvSender / SendSender / AcceptSender
// ============================================================================

struct RecvResult {
    bool ok;
    DWORD bytes;
    std::vector<char> data;
};

struct RecvSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(RecvResult)>;

    SOCKET socket;
    explicit RecvSender(SOCKET s) : socket(s) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET socket;
        char buffer[512]{};
        WSABUF wsabuf;

        operation(Receiver r, SOCKET s) : receiver(std::move(r)), socket(s) {
            wsabuf.buf = buffer;
            wsabuf.len = sizeof(buffer);
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            stdexec::set_value(std::move(self->receiver),
                RecvResult{
                    self->success,
                    self->bytes,
                    std::vector<char>(self->buffer, self->buffer + self->bytes)
                });
        }

        void start() noexcept {
            DWORD flags = 0;
            int ret = WSARecv(socket, &wsabuf, 1, nullptr, &flags, &overlapped, nullptr);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                bytes = 0;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), socket);
    }
};

struct SendSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(bool)>;

    SOCKET socket;
    std::vector<char> data;

    SendSender(SOCKET s, const char* buf, int len) : socket(s), data(buf, buf + len) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET socket;
        std::vector<char> data;
        WSABUF wsabuf;

        operation(Receiver r, SOCKET s, std::vector<char> d)
            : receiver(std::move(r)), socket(s), data(std::move(d)) {
            wsabuf.buf = data.data();
            wsabuf.len = static_cast<ULONG>(data.size());
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            stdexec::set_value(std::move(self->receiver), self->success);
        }

        void start() noexcept {
            int ret = WSASend(socket, &wsabuf, 1, nullptr, 0, &overlapped, nullptr);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), socket, data);
    }
};

struct AcceptSender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(SOCKET)>;

    SOCKET listen_socket;
    explicit AcceptSender(SOCKET s) : listen_socket(s) {}

    template <typename Receiver>
    struct operation : IoOpBase {
        Receiver receiver;
        SOCKET listen_socket;
        SOCKET accepted_socket;
        char addr_buf[(sizeof(SOCKADDR_IN) + 16) * 2]{};

        operation(Receiver r, SOCKET listener)
            : receiver(std::move(r)), listen_socket(listener) {
            accepted_socket = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            complete_fn = &complete_impl;
        }

        static void complete_impl(IoOpBase* base) noexcept {
            auto* self = static_cast<operation*>(base);
            SOCKET result = self->success ? self->accepted_socket : INVALID_SOCKET;
            stdexec::set_value(std::move(self->receiver), result);
        }

        void start() noexcept {
            DWORD bytes = 0;
            BOOL ret = AcceptEx(listen_socket, accepted_socket, addr_buf, 0,
                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
                &bytes, &overlapped);
            if (!ret && WSAGetLastError() != WSA_IO_PENDING) {
                success = false;
                complete();
            }
        }
    };

    template <typename Receiver>
    auto connect(Receiver r) noexcept {
        return operation<Receiver>(std::move(r), listen_socket);
    }
};

// ============================================================================
// 3. SESSION - 전송 큐 및 동기화 객체 추가
// ============================================================================

enum CL_STATE { CS_CONNECT, CS_PLAYING, CS_LOGOUT };

class SESSION {
public:
    SOCKET m_client;
    int m_id;
    std::atomic<CL_STATE> m_state{ CS_CONNECT };
    char m_username[MAX_NAME_LEN]{};
    int32_t m_x, m_y;
    int m_move_time = 0;

    // 🌟 세션별 전송 큐 및 제어 변수
    std::queue<std::vector<char>> m_send_queue;
    std::mutex m_send_lock;
    bool m_is_sending = false;

    SESSION(SOCKET s, int id) : m_client(s), m_id(id) {
        m_x = static_cast<int32_t>(rand() % WORLD_WIDTH);
        m_y = static_cast<int32_t>(rand() % WORLD_HEIGHT);
    }
    ~SESSION() { if (m_client != INVALID_SOCKET) closesocket(m_client); }
};

class NPC {
	int m_id;
    int32_t m_x, m_y;
};

tbb::concurrent_unordered_map<int, std::shared_ptr<SESSION>> clients;
SOCKET g_server;
HANDLE g_iocp;
std::atomic<int> player_index = 1;

// ============================================================================
// 4. 전송 큐 처리 코루틴 & do_send 헬퍼
// ============================================================================

// 🌟 소켓당 단 1개만 실행되며, 큐에 쌓인 패킷을 순차적으로 전송해주는 작업기
exec::task<void> process_send_queue(std::shared_ptr<SESSION> session)
{
    for (;;) {
        std::vector<char> send_buf;

        // 1. 전송할 패킷 복사
        {
            std::lock_guard<std::mutex> lock(session->m_send_lock);
            if (session->m_send_queue.empty()) {
                session->m_is_sending = false;
                co_return; // 보낼 패킷이 더 없으면 작업 종료
            }
            send_buf = session->m_send_queue.front();
        }

        // 2. 비동기 전송 완료까지 대기 (이 세션의 전송 큐 작업 스레드만 비동기 대기)
        bool ok = co_await SendSender(session->m_client, send_buf.data(), static_cast<int>(send_buf.size()));

        // 3. 전송 완료 처리 및 큐 정리
        {
            std::lock_guard<std::mutex> lock(session->m_send_lock);
            if (!session->m_send_queue.empty()) {
                session->m_send_queue.pop();
            }

            // 실패했거나 더 이상 전송할 패킷이 없으면 종료
            if (!ok || session->m_send_queue.empty()) {
                session->m_is_sending = false;
                co_return;
            }
        }
    }
}

// 🌟 큐에 넣고(Push) 전송기를 작동시키는 Push-and-Forget 헬퍼
void do_send(std::shared_ptr<SESSION> session, const char* data, int len)
{
    if (!session || session->m_state == CS_LOGOUT) return;

    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(session->m_send_lock);
        session->m_send_queue.emplace(data, data + len);

        // 현재 전송 중인 코루틴이 없다면 새롭게 전송 코루틴 시동
        if (!session->m_is_sending) {
            session->m_is_sending = true;
            start_worker = true;
        }
    }

    if (start_worker) {
        g_scope.spawn(process_send_queue(session));
    }
}

// ============================================================================
// 5. 패킷 발사 함수들 (do_send 호출하도록 변경)
// ============================================================================

void spawn_send_login_success(std::shared_ptr<SESSION> s)
{
    S2C_LoginResult packet;
    packet.size = sizeof(S2C_LoginResult);
    packet.type = S2C_LOGIN_RESULT;
    packet.success = true;
    strncpy_s(packet.message, "Login successful.", sizeof(packet.message));

    do_send(s, reinterpret_cast<char*>(&packet), packet.size);
}

void spawn_send_avatar_info(std::shared_ptr<SESSION> s)
{
    S2C_AvatarInfo packet;
    packet.size = sizeof(S2C_AvatarInfo);
    packet.type = S2C_AVATAR_INFO;
    packet.playerId = s->m_id;
    packet.x = s->m_x;
    packet.y = s->m_y;

    do_send(s, reinterpret_cast<char*>(&packet), packet.size);
}

void spawn_send_move_packet(std::shared_ptr<SESSION> to, int mover)
{
    auto pl = clients[mover];
    if (!pl) return;

    S2C_MoveObject packet;
    packet.size = sizeof(S2C_MoveObject);
    packet.type = S2C_MOVE_OBJECT;
    packet.object_id = mover;
    packet.x = pl->m_x;
    packet.y = pl->m_y;
    packet.move_time = pl->m_move_time;

    do_send(to, reinterpret_cast<char*>(&packet), packet.size);
}

void spawn_send_add_player(std::shared_ptr<SESSION> to, int player_id)
{
    auto pl = clients[player_id];
    if (!pl) return;

    S2C_AddObject packet;
    packet.size = sizeof(S2C_AddObject);
    packet.type = S2C_ADD_OBJECT;
    packet.object_id = player_id;
    memcpy(packet.obj_name, pl->m_username, sizeof(packet.obj_name));
    packet.x = pl->m_x;
    packet.y = pl->m_y;

    do_send(to, reinterpret_cast<char*>(&packet), packet.size);
}

void spawn_send_remove_player(std::shared_ptr<SESSION> to, int player_id)
{
    S2C_RemoveObject packet;
    packet.size = sizeof(S2C_RemoveObject);
    packet.type = S2C_REMOVE_OBJECT;
    packet.object_id = player_id;

    do_send(to, reinterpret_cast<char*>(&packet), packet.size);
}

// ============================================================================
// 6. 패킷 처리
// ============================================================================

exec::task<bool> process_packet(std::shared_ptr<SESSION> self, unsigned char* p)
{
    PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);

    switch (type) {
    case C2S_LOGIN: {
        C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);
        std::string username = packet->username;

        strncpy_s(self->m_username, packet->username, MAX_NAME_LEN);

        cout << "Player[" << self->m_id << "] logged in as " << self->m_username << endl;

        spawn_send_avatar_info(self);
        self->m_state = CS_PLAYING;

        // 1. 현재 접속 중인 유효한 모든 대상(플레이어 + NPC) 스냅샷 채우기
        std::vector<std::shared_ptr<SESSION>> snapshot;
        for (auto& [key, pl] : clients) {
            if (pl && pl->m_id != self->m_id && pl->m_state == CS_PLAYING) {
                snapshot.push_back(pl);
            }
        }

        // 2. 스냅샷을 순회하며 패킷 전송
        for (auto& pl : snapshot) {
            // 새로 로그인한 나(self)에게 대상(플레이어 or NPC)의 등장 패킷 전송
            spawn_send_add_player(self, pl->m_id);

            // 대상이 '진짜 플레이어'인 경우에만, 그 플레이어에게도 나(self)의 등장 패킷 전송
            // (NPC ID 범위 이상인 경우 소켓이 없으므로 패킷을 보내지 않음)
            if (pl->m_id < NPC_ID_START) {
                spawn_send_add_player(pl, self->m_id);
            }
        }

        co_return true;
    }
    case C2S_MOVE: {
        C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
        self->m_move_time = packet->move_time;
        self->m_x = packet->x;
        self->m_y = packet->y;

        if (packet->move_time != 0)
            spawn_send_move_packet(self, self->m_id);

        std::vector<std::shared_ptr<SESSION>> snapshot;
        for (auto& [key, pl] : clients)
            if (pl && pl->m_id != self->m_id && pl->m_state == CS_PLAYING)
                snapshot.push_back(pl);

        for (auto& pl : snapshot)
            spawn_send_move_packet(pl, self->m_id);

        co_return true;
    }
    default:
        cout << "Unknown packet type from player[" << self->m_id << "].\n";
        co_return false;
    }
}

// ============================================================================
// 7. 세션 루프
// ============================================================================

exec::task<void> handle_session(std::shared_ptr<SESSION> session)
{
    cout << "[Session " << session->m_id << "] started" << endl;
    spawn_send_login_success(session);

    std::vector<unsigned char> leftover;

    for (;;) {
        RecvResult r = co_await RecvSender(session->m_client);
        if (!r.ok || r.bytes == 0) break;

        leftover.insert(leftover.end(), r.data.begin(), r.data.end());

        size_t offset = 0;
        bool disconnect_requested = false;
        while (offset < leftover.size()) {
            unsigned char* p = leftover.data() + offset;
            int packet_size = p[0];
            if (static_cast<size_t>(packet_size) > leftover.size() - offset) break;

            bool keep_going = co_await process_packet(session, p);
            offset += packet_size;
            if (!keep_going) { disconnect_requested = true; break; }
        }
        leftover.erase(leftover.begin(), leftover.begin() + offset);

        if (disconnect_requested) break;
    }

    session->m_state = CS_LOGOUT;
    cout << "[Session " << session->m_id << "] disconnecting" << endl;

    std::vector<std::shared_ptr<SESSION>> snapshot;
    for (auto& [key, other] : clients)
        if (other && other->m_id != session->m_id && other->m_state == CS_PLAYING)
            snapshot.push_back(other);

    for (auto& other : snapshot)
        spawn_send_remove_player(other, session->m_id);

    clients.unsafe_erase(session->m_id);
    cout << "[Session " << session->m_id << "] ended" << endl;
}

// ============================================================================
// 8. accept_loop
// ============================================================================

exec::task<void> accept_loop()
{
    for (;;) {
        SOCKET client_sock = co_await AcceptSender(g_server);
        if (client_sock == INVALID_SOCKET) continue;

        if (clients.size() >= MAX_PLAYERS) {
            cout << "Server full. Rejecting connection." << endl;
            closesocket(client_sock);
            continue;
        }

        CreateIoCompletionPort((HANDLE)client_sock, g_iocp, 0, 0);

        int my_id = player_index++;
        auto new_session = std::make_shared<SESSION>(client_sock, my_id);
        clients[my_id] = new_session;

        cout << "Client connected. id=" << my_id << endl;

        g_scope.spawn(handle_session(new_session));
    }
}

// ============================================================================
// 9. 디스패처 루프
// ============================================================================

void io_dispatch_loop()
{
    for (;;) {
        DWORD num_bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED over = nullptr;

        BOOL ret = GetQueuedCompletionStatus(g_iocp, &num_bytes, &key, &over, INFINITE);

        if (over == nullptr) {
            if (key == static_cast<ULONG_PTR>(-1)) break;
            continue;
        }

        IoOpBase* io = reinterpret_cast<IoOpBase*>(over);
        io->bytes = num_bytes;
        io->success = (ret == TRUE);
        io->complete();
    }
}

void InitializeNPC()
{
    cout << "NPC initialize...\n";
    for (int i = NPC_ID_START; i < NPC_ID_START + 10; ++i) {
        // 소켓이 없는 NPC 세션을 생성 (INVALID_SOCKET)
        auto npc_session = std::make_shared<SESSION>(INVALID_SOCKET, i);
        npc_session->m_x = rand() % WORLD_WIDTH;
        npc_session->m_y = rand() % WORLD_HEIGHT;
        npc_session->m_state = CS_PLAYING;

        // clients 맵에 등록
        clients[i] = npc_session;
    }
    cout << "NPC setting complete\n";
}

// ============================================================================
// 10. main
// ============================================================================

int main()
{
    srand((unsigned int)time(nullptr));
    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 2), &WSAData);

    g_server = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

    SOCKADDR_IN server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.S_un.S_addr = INADDR_ANY;

    ::bind(g_server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    listen(g_server, SOMAXCONN);

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    CreateIoCompletionPort((HANDLE)g_server, g_iocp, 0, 0);

    InitializeNPC();
    g_scope.spawn(accept_loop());

    cout << "Game server started on port " << PORT << endl;

    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;

    std::vector<std::thread> dispatchers;
    for (unsigned int i = 0; i < n; ++i)
        dispatchers.emplace_back(io_dispatch_loop);

    for (auto& t : dispatchers) t.join();

    closesocket(g_server);
    WSACleanup();
    return 0;
}