#pragma once
// ============================================================================
// protocol.h — 서버와 언리얼 클라이언트가 "파일 단위로" 공유하는 헤더.
//
// 이 파일을 고칠 때는 반드시 양쪽을 같이 고쳐야 한다.
// 서버 저장소에 원본을 두고, 클라이언트는 심볼릭 링크나 빌드 스크립트 복사로
// 가져가는 방식을 권장한다. (복사본 두 개를 손으로 맞추면 반드시 어긋난다)
// ============================================================================

#include <cstdint>

// ----------------------------------------------------------------------------
// 월드 상수
// ----------------------------------------------------------------------------

inline constexpr uint16_t SERVER_PORT = 3500;

// 좌표계는 언리얼 월드 좌표(cm)를 그대로 쓴다.
// 예전처럼 타일 좌표로 변환하지 않으므로 클라이언트에서 캐스팅이 필요 없다.
inline constexpr int32_t WORLD_MIN = -500000;   // -5km
inline constexpr int32_t WORLD_MAX = 500000;   // +5km

// 시야 처리용 섹터 한 변의 길이(cm). 128m.
inline constexpr int32_t SECTOR_SIZE = 12800;

// 플레이어가 서로를 인지하는 거리(cm). 100m.
inline constexpr int32_t VIEW_RANGE = 10000;

// [불변식] VIEW_RANGE <= SECTOR_SIZE
// 이 조건이 지켜져야 "내가 속한 섹터 + 주변 8개" 총 9개만 검사해도
// 시야 안의 모든 오브젝트를 빠짐없이 찾을 수 있다.
// SECTOR_SIZE를 줄이거나 VIEW_RANGE를 늘릴 때 반드시 이 관계를 유지할 것.
static_assert(VIEW_RANGE <= SECTOR_SIZE,
    "VIEW_RANGE는 SECTOR_SIZE 이하여야 3x3 섹터 검사로 충분하다");

inline constexpr int32_t MAX_PLAYERS = 10000;
inline constexpr int32_t MAX_NPCS = 1000;
inline constexpr int32_t NPC_ID_START = 1000000;

inline constexpr int32_t MAX_NAME_LEN = 20;
inline constexpr int32_t MAX_CHAT_MSG_LEN = 200;

// 패킷 상한. size가 uint16이라 이론상 65535까지 되지만,
// 세션 수신 버퍼 크기를 산정하려면 상한을 못박아야 한다.
inline constexpr uint16_t MAX_PACKET_SIZE = 1024;

// 이동 패킷 검증용: 1초에 이동 가능한 최대 거리(cm).
// 캐릭터 최고 속도 400 + 여유. 이 값을 넘으면 스피드핵으로 간주한다.
inline constexpr int32_t MAX_MOVE_SPEED_PER_SEC = 1200;

// ----------------------------------------------------------------------------
// 패킷 타입
//
// [중요] enum을 패킷에 그대로 담지 않는다.
// 순수 enum의 크기는 구현체 정의라서, 서버(MSVC)와 클라(UE/Clang)가
// 서로 다른 크기를 쓰면 조용히 깨진다. 밑에 깔린 타입을 uint16으로 못박는다.
// ----------------------------------------------------------------------------

enum PacketType : uint16_t {
    PT_NONE = 0,

    C2S_LOGIN = 1,
    C2S_MOVE = 2,
    C2S_CHAT = 3,
    C2S_ATTACK = 4,
    C2S_TELEPORT = 5,   // 스트레스 테스트용: 시작 지점 밀집 방지
    C2S_LOGOUT = 6,

    S2C_LOGIN_RESULT = 100,
    S2C_AVATAR_INFO = 101,
    S2C_ADD_OBJECT = 102,
    S2C_REMOVE_OBJECT = 103,
    S2C_MOVE_OBJECT = 104,
    S2C_CHAT_MESSAGE = 105,
    S2C_STATUS_CHANGE = 106,

    PT_MAX = 256,       // 핸들러 테이블 크기. 타입 번호는 이 미만이어야 한다
};

// ----------------------------------------------------------------------------
// 오브젝트 종류
//
// 예전에는 `if (object_id >= NPC_ID_START)`로 종류를 구분했다.
// 몬스터/투사체/아이템이 늘어날 때마다 ID 범위 분기가 늘어나므로,
// 서버가 종류를 명시해서 보낸다.
// ----------------------------------------------------------------------------

enum ObjectType : uint8_t {
    OBJ_PLAYER = 0,
    OBJ_NPC = 1,
    OBJ_MONSTER = 2,
};

#pragma pack(push, 1)

// ----------------------------------------------------------------------------
// 공통 헤더 — 모든 패킷의 첫 4바이트
//
// size가 uint16이 된 이유:
//   기존 `unsigned char size`는 상한이 255인데 클라 쪽 static_assert는
//   `<= 256`이었다. 256바이트 패킷이 통과되면 size에 0이 들어가고,
//   수신부의 `size <= 0` 검사에 걸려 그 뒤 스트림 전체가 버려졌다.
// ----------------------------------------------------------------------------
struct PacketHeader {
    uint16_t size;
    uint16_t type;
};

// 3D 좌표. 오픈월드이므로 z가 필요하다.
struct Vec3i {
    int32_t x;
    int32_t y;
    int32_t z;
};

// ---- Client to Server ----

struct C2S_Login {
    PacketHeader h;
    char username[MAX_NAME_LEN];
};

struct C2S_Move {
    PacketHeader h;
    Vec3i pos;
    int16_t yaw;            // 바라보는 방향. 0~35999 (0.01도 단위)
    uint32_t client_time;   // 클라 기준 밀리초. 스피드핵 검증에 쓴다
};

struct C2S_Chat {
    PacketHeader h;
    char message[MAX_CHAT_MSG_LEN];
};

struct C2S_Attack {
    PacketHeader h;
    int32_t target_id;      // 대상 없는 공격이면 -1
};

struct C2S_Teleport {
    PacketHeader h;
    Vec3i pos;
};

struct C2S_Logout {
    PacketHeader h;
};

// ---- Server to Client ----

struct S2C_LoginResult {
    PacketHeader h;
    uint8_t success;        // bool 대신 uint8 (bool 크기도 구현체 정의)
    int32_t object_id;
    char message[50];
};

struct S2C_AvatarInfo {
    PacketHeader h;
    int32_t object_id;
    int32_t visual_id;
    Vec3i pos;
    int16_t yaw;
    int32_t hp;
    int32_t max_hp;
    uint64_t exp;
    uint8_t level;
};

struct S2C_AddObject {
    PacketHeader h;
    int32_t object_id;
    uint8_t object_type;    // ObjectType
    int32_t visual_id;
    char obj_name[MAX_NAME_LEN];
    Vec3i pos;
    int16_t yaw;
    int32_t hp;
    int32_t max_hp;
    uint8_t level;
};

struct S2C_RemoveObject {
    PacketHeader h;
    int32_t object_id;
};

struct S2C_MoveObject {
    PacketHeader h;
    int32_t object_id;
    Vec3i pos;
    int16_t yaw;
    uint32_t move_time;
};

struct S2C_ChatMessage {
    PacketHeader h;
    int32_t object_id;
    char message[MAX_CHAT_MSG_LEN];
};

struct S2C_StatusChange {
    PacketHeader h;
    int32_t object_id;
    int32_t hp;
    int32_t max_hp;
    uint64_t exp;
    uint8_t level;
};

#pragma pack(pop)

// ----------------------------------------------------------------------------
// 패킷 조립 헬퍼
//
// 항상 `T packet{};`처럼 값 초기화한 뒤 이 함수로 헤더를 채운다.
// 초기화를 빼먹으면 스택 쓰레기값이 그대로 네트워크로 나간다.
// ----------------------------------------------------------------------------
template <typename T>
constexpr void InitHeader(T& packet, PacketType type) {
    static_assert(sizeof(T) <= MAX_PACKET_SIZE, "패킷이 MAX_PACKET_SIZE를 넘는다");
    packet.h.size = static_cast<uint16_t>(sizeof(T));
    packet.h.type = static_cast<uint16_t>(type);
}

// 컴파일 타임 크기 검증
static_assert(sizeof(PacketHeader) == 4, "헤더는 4바이트여야 한다");
static_assert(sizeof(Vec3i) == 12, "Vec3i는 12바이트여야 한다");
