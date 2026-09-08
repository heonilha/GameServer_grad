// ============================================================================
// server_main.cpp — 패킷 처리와 서버 기동
//
// 이동 처리가 여기에 없다는 점이 핵심이다.
// HandleInput은 입력을 큐에 넣기만 하고, 위치 계산은 게임 틱 루프가
// 고정 주기로 수행한다. 패킷 처리(I/O 스레드)와 시뮬레이션(틱 스레드)이
// 분리된 구조다.
// ============================================================================

#include <iostream>
#include <iomanip>
#include <memory>
#include <thread>
#include <cstdlib>
#include <ctime>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/async_scope.hpp>

#include "protocol.h"
#include "net_core.h"
#include "movement.h"
#include "nav_grid.h"
#include "world_object.h"
#include "world_grid.h"
#include "skill_table.h"
#include "monster_table.h"
#include "combat.h"
#include "game_tick.h"

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

// ----------------------------------------------------------------------------
// 전역
// ----------------------------------------------------------------------------

exec::async_scope g_scope;
SOCKET g_listen_socket = INVALID_SOCKET;
HANDLE g_iocp = nullptr;

SessionManager g_sessions;
NpcManager     g_npcs;
WorldGrid      g_grid;
NavGrid        g_nav;
SkillTable     g_skills;
MonsterTable   g_monsters;
CombatSystem   g_combat;

// 이번 틱에 위치가 바뀐 오브젝트. 페이즈 1과 AI 페이즈가 채우고
// 페이즈 3이 읽는다. 선언은 world_object.h에 있다.
std::unordered_set<int32_t> g_moved;

std::atomic<bool> g_running{ true };

// 워커 수를 실행 시점에 정해야 해서 포인터로 둔다.
std::unique_ptr<GameTickLoop> g_tick_loop;

// ----------------------------------------------------------------------------
// 전송
// ----------------------------------------------------------------------------

exec::task<void> SendWorker(std::shared_ptr<Session> session)
{
    for (;;) {
        if (!session->SwapSendBuffer()) co_return;

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

// 틱 루프가 조립해둔 여러 패킷을 한 번에 밀어넣는다.
// 락 획득이 세션당 1회로 줄어드는 것이 이 함수의 존재 이유다.
void FlushSendBatch(const std::shared_ptr<Session>& session,
                    const std::vector<char>& batch)
{
    if (!session || batch.empty()) return;
    if (session->EnqueueSendBatch(batch)) {
        g_scope.spawn(SendWorker(session));
    }
}

// 특정 위치에서 일어난 일을 주변 플레이어에게 알린다.
//
// 시야 목록을 뒤지는 대신 그리드를 쓰는 이유는, 전투는 위치에서 일어나지
// 특정 세션에 속하지 않기 때문이다. 투사체 폭발처럼 시전자가 이미 멀리
// 있는 경우도 이 방식이 맞다.
void BroadcastNear(const Vec3i& pos, const void* data, uint16_t len)
{
    for (int32_t id : g_grid.QueryNear(pos)) {
        if (id >= NPC_ID_START) continue;

        auto session = g_sessions.Get(id);
        if (!session || session->GetState() != SessionState::Playing) continue;
        if (!IsInViewRange(pos, session->GetPosition())) continue;

        SendPacket(session, data, len);
    }
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
    packet.object_id   = s.id;
    packet.object_type = static_cast<uint8_t>(s.type);
    packet.visual_id   = s.visual_id;
    std::memcpy(packet.obj_name, s.name, sizeof(packet.obj_name));
    packet.pos    = s.move.pos;
    packet.yaw    = s.move.yaw;
    packet.hp     = s.hp;
    packet.max_hp = s.max_hp;
    packet.level  = s.level;
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
// 패킷 핸들러
//
// 타입별 함수 포인터 테이블. 거대한 switch를 쓰지 않는 이유는,
// 팀원 여럿이 각자 패킷을 추가할 때 매번 같은 함수에서 충돌하기 때문이다.
// 새 패킷은 함수 하나를 쓰고 RegisterHandlers에 한 줄 추가하면 끝난다.
//
// 반환값이 false면 이 세션을 끊는다.
// ----------------------------------------------------------------------------

using PacketHandler = bool(*)(const std::shared_ptr<Session>&, const uint8_t*);

PacketHandler g_handlers[PT_MAX]{};

bool HandleLogin(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Accepted) return false;   // 중복 로그인

    const auto* packet = reinterpret_cast<const C2S_Login*>(raw);

    // username은 널 종료가 보장되지 않는다. 길이를 직접 재서 자른다.
    size_t len = 0;
    while (len < MAX_NAME_LEN && packet->username[len] != '\0') ++len;
    self->SetName(packet->username, len);

    // 시작 상태. 지면 높이를 샘플링해 공중에서 시작하지 않게 한다.
    MoveState start{};
    for (int attempt = 0; attempt < 32; ++attempt) {
        start.pos.x = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
        start.pos.y = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
        if (g_nav.IsWalkable(start.pos.x, start.pos.y)) break;
    }
    start.pos.z   = g_nav.SampleHeight(start.pos.x, start.pos.y);
    start.grounded = true;
    self->SetMoveState(start);

    // 캐릭터 생성 화면이 아직 없어서 임시로 나눠 준다.
    // 생성 UI가 들어오면 C2S_Login에 class_id를 추가하고 여기를 바꾼다.
    self->SetClassId(static_cast<uint8_t>(self->GetId() % 2));

    const auto my = self->MakeSnapshot();

    S2C_LoginResult result{};
    InitHeader(result, S2C_LOGIN_RESULT);
    result.success   = 1;
    result.object_id = my.id;
    std::strncpy(result.message, "Login successful.", sizeof(result.message) - 1);
    SendPacket(self, &result, result.h.size);

    S2C_AvatarInfo avatar{};
    InitHeader(avatar, S2C_AVATAR_INFO);
    avatar.object_id = my.id;
    avatar.visual_id = my.visual_id;
    avatar.pos       = my.move.pos;
    avatar.yaw       = my.move.yaw;
    avatar.hp        = my.hp;
    avatar.max_hp    = my.max_hp;
    avatar.level     = my.level;
    SendPacket(self, &avatar, avatar.h.size);

    // 상태를 Playing으로 올린 뒤에 그리드에 넣는다.
    // 순서가 반대면 아직 Playing이 아닌 나를 다른 세션이 조회해서 놓친다.
    self->SetState(SessionState::Playing);
    g_grid.Add(my.id, my.move.pos);

    // 첫 시야는 즉시 계산한다. 다음 시야 갱신 틱까지 기다리면
    // 접속 직후 주변이 비어 보인다.
    if (g_tick_loop) g_tick_loop->UpdateViewNow(self);

    std::cout << "Player[" << my.id << "] logged in as " << my.name << "\n";
    return true;
}

// 서버 권위의 핵심.
//
// 예전 HandleMove는 클라가 보낸 위치를 그대로 대입했다.
// 지금은 입력을 큐에 넣기만 하고, 위치 계산은 틱 루프가 한다.
// 클라가 무엇을 보내든 위치는 서버 시뮬레이션 결과로만 정해진다.
bool HandleInput(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Playing) return false;

    const auto* packet = reinterpret_cast<const C2S_Input*>(raw);

    MoveInput input{};
    input.sequence = packet->sequence;
    input.move_x   = packet->move_x;
    input.move_y   = packet->move_y;
    input.yaw      = packet->yaw;
    input.buttons  = packet->buttons;

    // 값 검증은 SimulateStep 안의 SanitizeInput이 한다.
    // 검증 지점을 한 곳에 모아둬야 서버와 클라이언트의 동작이 일치한다.
    self->PushInput(input);
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

    SendPacket(self, &out, out.h.size);
    for (int32_t id : self->CopyViewList()) {
        if (id >= NPC_ID_START) continue;
        SendPacket(g_sessions.Get(id), &out, out.h.size);
    }
    return true;
}

// 스킬 시전.
//
// 여기서 판정하지 않는다는 점이 중요하다.
// 검증만 하고 예약 큐에 넣으면, 윈드업이 끝나는 틱에 전투 페이즈가 판정한다.
// 몽타주의 타격 프레임과 서버 판정 시점을 맞추기 위해서다.
bool HandleUseSkill(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
    if (self->GetState() != SessionState::Playing) return false;

    const auto* packet = reinterpret_cast<const C2S_UseSkill*>(raw);

    auto fail = [&](uint8_t reason) {
        S2C_SkillFailed out{};
        InitHeader(out, S2C_SKILL_FAILED);
        out.skill_id = packet->skill_id;
        out.reason   = reason;
        SendPacket(self, &out, out.h.size);
        return true;   // 시전 실패는 연결을 끊을 이유가 아니다
    };

    const SkillDef* def = g_skills.Get(packet->skill_id);
    if (!def)                       return fail(FAIL_UNKNOWN_SKILL);
    if (!self->IsAlive())           return fail(FAIL_DEAD);
    if (def->class_id != self->GetClassId()) return fail(FAIL_WRONG_CLASS);

    const uint32_t now = g_tick_loop ? g_tick_loop->CurrentTick() : 0;

    // 쿨타임을 먼저 본다. 마나를 먼저 깎으면 쿨타임에 걸렸을 때
    // 마나만 사라진다.
    if (!self->TryConsumeCooldown(packet->skill_id, now, def->cooldown_ticks)) {
        return fail(FAIL_COOLDOWN);
    }
    if (def->mp_cost > 0 && !self->ConsumeMp(def->mp_cost)) {
        return fail(FAIL_NOT_ENOUGH_MP);
    }

    // 시전 시작을 주변에 알린다. 클라는 이걸 받아 몽타주를 재생한다.
    // 데미지는 여기 없다. 윈드업 뒤에 S2C_Damage로 따로 간다.
    S2C_SkillUsed used{};
    InitHeader(used, S2C_SKILL_USED);
    used.caster_id   = self->GetId();
    used.skill_id    = packet->skill_id;
    used.yaw         = packet->yaw;
    used.server_tick = now;
    BroadcastNear(self->GetPosition(), &used, used.h.size);

    PendingHit hit{};
    hit.execute_tick = now + def->windup_ticks;
    hit.caster_id    = self->GetId();
    hit.skill_id     = packet->skill_id;
    hit.yaw          = packet->yaw;
    g_combat.EnqueueHit(hit);

    return true;
}

// 개발/테스트 전용. 배포 빌드에서는 등록하지 않는다.
bool HandleTeleport(const std::shared_ptr<Session>& self, const uint8_t* raw)
{
#ifdef _DEBUG
    if (self->GetState() != SessionState::Playing) return false;

    const auto* packet = reinterpret_cast<const C2S_Teleport*>(raw);
    if (!g_nav.IsWalkable(packet->pos.x, packet->pos.y)) return true;

    const Vec3i from = self->GetPosition();

    MoveState state = self->GetMoveState();
    state.pos   = packet->pos;
    state.pos.z = g_nav.SampleHeight(packet->pos.x, packet->pos.y);
    state.vel_x = state.vel_y = state.vel_z = 0;
    state.grounded = true;
    self->SetMoveState(state);

    g_grid.Move(self->GetId(), from, state.pos);
    if (g_tick_loop) g_tick_loop->UpdateViewNow(self);
    return true;
#else
    (void)self; (void)raw;
    return false;
#endif
}

bool HandleLogout(const std::shared_ptr<Session>&, const uint8_t*)
{
    return false;   // false = 세션 종료
}

void RegisterHandlers()
{
    g_handlers[C2S_LOGIN]     = HandleLogin;
    g_handlers[C2S_INPUT]     = HandleInput;
    g_handlers[C2S_CHAT]      = HandleChat;
    g_handlers[C2S_USE_SKILL] = HandleUseSkill;
    g_handlers[C2S_LOGOUT]    = HandleLogout;
#ifdef _DEBUG
    g_handlers[C2S_TELEPORT] = HandleTeleport;
#endif
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
                break;
            }

            const uint16_t type = reinterpret_cast<const PacketHeader*>(raw)->type;
            PacketHandler handler = (type < PT_MAX) ? g_handlers[type] : nullptr;

            if (!handler) {
                std::cout << "[Session " << session->GetId()
                          << "] unknown packet type " << type << "\n";
                disconnect = true;
                break;
            }

            // 핸들러는 코루틴이 아니다. 전송은 전부 fire-and-forget이라
            // co_await할 이유가 없고, 패킷마다 코루틴 프레임을 할당할 필요도 없다.
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

    const int32_t my_id = session->GetId();
    g_grid.Remove(my_id, session->GetPosition());

    // 나를 보고 있던 사람들에게 REMOVE를 직접 보내지 않는다.
    // 그리드에서 빠졌으므로 각자의 다음 시야 갱신에서 자동으로 처리된다.
    // (최대 VIEW_UPDATE_INTERVAL 틱, 약 100ms 지연)

    // 슬롯만 비운다. 다른 스레드가 이미 잡아둔 shared_ptr가 있으면
    // 그쪽 작업이 끝난 뒤에 실제 소멸이 일어난다 (지연 삭제).
    g_sessions.Release(my_id);

    std::cout << "[Session " << my_id << "] ended\n";
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
        io->bytes   = num_bytes;
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

        // 종류를 돌아가며 배정한다. 스폰 테이블(어느 지역에 무엇이 나오는지)이
        // 생기면 이 부분을 그 데이터로 바꾼다.
        const uint16_t monster_id = g_monsters.IdAt(i);
        const MonsterDef* def = g_monsters.Get(monster_id);
        if (!def) continue;

        MoveState state{};
        for (int attempt = 0; attempt < 32; ++attempt) {
            state.pos.x = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
            state.pos.y = WORLD_MIN + (std::rand() % (WORLD_MAX - WORLD_MIN));
            if (g_nav.IsWalkable(state.pos.x, state.pos.y)) break;
        }
        state.pos.z    = g_nav.SampleHeight(state.pos.x, state.pos.y);
        state.grounded = true;

        npc->monster_id = monster_id;
        npc->SetMoveState(state);
        npc->SetSpawnPoint(state.pos);   // 죽으면 원래 자리에서 되살아난다
        npc->SetName(def->name, std::strlen(def->name));
        npc->SetStats(def->max_hp, def->visual_id);
        npc->ClearAi();

        g_grid.Add(npc->GetId(), state.pos);
    }
    std::cout << "Monsters spawned: " << g_npcs.Count() << "\n";
}

// ----------------------------------------------------------------------------
// 상태 출력
//
// 부하 테스트 중에 콘솔로 바로 확인할 수 있게 한다.
// 평균만 보면 스파이크가 감춰지므로 p99를 같이 본다.
// ----------------------------------------------------------------------------

void StatsLoop()
{
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_tick_loop) continue;

        const auto s = g_tick_loop->Metrics().Summarize();
        if (s.samples == 0) continue;

        std::cout << std::fixed << std::setprecision(2)
                  << "[stat] players=" << s.players
                  << " tick avg=" << s.avg_us / 1000.0 << "ms"
                  << " p99=" << s.p99_us / 1000.0 << "ms"
                  << " max=" << s.max_us / 1000.0 << "ms"
                  << " | pkt/tick=" << s.avg_packets
                  << " out=" << (s.avg_bytes * TICK_RATE) / (1024.0 * 1024.0)
                  << "MB/s"
                  << " | overrun=" << g_tick_loop->Metrics().OverrunCount()
                  << "\n";
    }
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\nShutting down...\n";
        g_running.store(false);
        if (g_tick_loop) {
            if (g_tick_loop->Metrics().WriteCsv("tick_metrics.csv")) {
                std::cout << "Metrics written to tick_metrics.csv\n";
            }
        }
        // IOCP 대기 중인 디스패처들을 깨운다
        for (int i = 0; i < 64; ++i) {
            PostQueuedCompletionStatus(
                g_iocp, 0, static_cast<ULONG_PTR>(-1), nullptr);
        }
        return TRUE;
    }
    return FALSE;
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------

int main()
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 지형 데이터. 없으면 평지로 동작한다.
    if (g_nav.LoadFromFile("navdata.bin")) {
        std::cout << "Nav data loaded\n";
    } else {
        std::cout << "Nav data not found — flat world fallback\n";
    }

    g_listen_socket =
        WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (g_listen_socket == INVALID_SOCKET) {
        std::cerr << "socket creation failed\n";
        return 1;
    }

    SOCKADDR_IN addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    addr.sin_addr.S_un.S_addr = INADDR_ANY;

    if (::bind(g_listen_socket, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << "\n";
        return 1;
    }
    listen(g_listen_socket, SOMAXCONN);

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_listen_socket), g_iocp, 0, 0);

    if (!g_monsters.LoadFromCsv("monsters.csv")) {
        std::cerr << "monsters.csv 로드 실패\n";
        return 1;
    }
    std::cout << "Monsters loaded: " << g_monsters.Count() << " types\n";

    if (!g_skills.LoadFromCsv("skills.csv")) {
        std::cerr << "skills.csv 로드 실패 — 전투를 쓸 수 없습니다\n";
        return 1;
    }
    std::cout << "Skills loaded: " << g_skills.LoadedCount() << "\n";

    g_combat.Initialize();

    RegisterHandlers();
    InitializeNpcs(100);

    // ---- 스레드 배분 ----
    //
    // 코어를 셋으로 나눈다.
    //   1) 틱 스레드 1개              — 페이즈 1(시뮬레이션) 전담
    //   2) 틱 워커 (코어의 약 1/3)     — 페이즈 2, 3 병렬 처리
    //   3) 나머지                     — IOCP 디스패처
    //
    // 틱 워커는 페이즈 2, 3 동안만 일하고 나머지 시간은 대기한다.
    // 그래서 코어를 통째로 예약하지 않고 절반 이하만 준다.
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;

    const unsigned int tick_workers = (cores >= 6) ? (cores / 3) : 1;
    unsigned int io_threads = (cores > tick_workers + 1)
                            ? cores - tick_workers - 1 : 1;

    g_tick_loop = std::make_unique<GameTickLoop>(tick_workers);

    std::cout << "Game server started on port " << SERVER_PORT << "\n"
              << "Tick " << TICK_RATE << "Hz, snapshot "
              << (TICK_RATE / SNAPSHOT_INTERVAL) << "Hz"
              << ", view cap " << MAX_VIEW_OBJECTS << "\n"
              << "Grid " << WorldGrid::GRID_DIM << "x" << WorldGrid::GRID_DIM
              << " (" << SECTOR_SIZE / 100 << "m sectors)\n"
              << "Threads: 1 tick + " << tick_workers << " tick workers + "
              << io_threads << " io\n";

    g_scope.spawn(AcceptLoop());

    std::thread tick_thread([] { g_tick_loop->Run(); });
    std::thread stats_thread(StatsLoop);

    std::vector<std::thread> dispatchers;
    dispatchers.reserve(io_threads);
    for (unsigned int i = 0; i < io_threads; ++i) {
        dispatchers.emplace_back(IoDispatchLoop);
    }

    for (auto& t : dispatchers) t.join();

    g_running.store(false);
    tick_thread.join();
    stats_thread.join();

    closesocket(g_listen_socket);
    WSACleanup();
    return 0;
}
