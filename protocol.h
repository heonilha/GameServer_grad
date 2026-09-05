#pragma once
// ============================================================================
// protocol.h — 서버와 언리얼 클라이언트가 "파일 단위로" 공유하는 헤더.
//
// [서버 권위 이동 개정판]
//
// 바뀐 핵심:
//   기존 - 클라가 "나 여기 있다"(위치)를 보내고 서버는 받아 적었다.
//   현재 - 클라는 "이 방향으로 가고 싶다"(입력)를 보내고,
//          서버가 시뮬레이션해서 위치를 정한다.
//
// 이 파일을 고칠 때는 반드시 서버와 클라를 같이 고쳐야 한다.
// 서버 저장소에 원본을 두고 클라가 복사해 가는 방식을 권장한다.
// ============================================================================

#include <cstdint>

// ----------------------------------------------------------------------------
// 월드 상수
// ----------------------------------------------------------------------------

inline constexpr uint16_t SERVER_PORT   = 3500;

// 좌표계는 언리얼 월드 좌표(cm)를 그대로 쓴다.
inline constexpr int32_t  WORLD_MIN     = -500000;   // -5km
inline constexpr int32_t  WORLD_MAX     =  500000;   // +5km

inline constexpr int32_t  SECTOR_SIZE   = 12800;     // 시야 섹터 한 변 (128m)
inline constexpr int32_t  VIEW_RANGE    = 10000;     // 시야 거리 (100m)

// [불변식] VIEW_RANGE <= SECTOR_SIZE
// 이 조건이 지켜져야 주변 3x3 섹터만 검사해도 시야 안을 빠짐없이 찾는다.
static_assert(VIEW_RANGE <= SECTOR_SIZE,
    "VIEW_RANGE는 SECTOR_SIZE 이하여야 3x3 섹터 검사로 충분하다");

inline constexpr int32_t  MAX_PLAYERS   = 10000;
inline constexpr int32_t  MAX_NPCS      = 1000;
inline constexpr int32_t  NPC_ID_START  = 1000000;

inline constexpr int32_t  MAX_NAME_LEN     = 20;
inline constexpr int32_t  MAX_CHAT_MSG_LEN = 200;

inline constexpr uint16_t MAX_PACKET_SIZE  = 1024;

// ----------------------------------------------------------------------------
// 시뮬레이션 상수
//
// [중요] 이 값들은 서버와 클라이언트가 완전히 같아야 한다.
// 하나라도 어긋나면 클라이언트 예측이 서버와 다른 결과를 내고,
// 스냅샷이 도착할 때마다 캐릭터가 튄다.
// ----------------------------------------------------------------------------

inline constexpr int32_t TICK_RATE = 30;               // 초당 시뮬레이션 횟수
inline constexpr int32_t TICK_MS   = 1000 / TICK_RATE; // 33ms

// 한 틱에 처리할 입력 개수 상한.
// 렉으로 입력이 몰려 들어와도 한 번에 다 처리하면 순간이동처럼 보인다.
inline constexpr int32_t MAX_INPUTS_PER_TICK = 4;

// 입력 큐 상한. 넘으면 오래된 것부터 버린다.
inline constexpr int32_t MAX_INPUT_QUEUE = 60;

// 이동 속도 (cm/s). 언리얼 CharacterMovement의 MaxWalkSpeed와 맞춰야 한다.
inline constexpr int32_t WALK_SPEED   = 600;
inline constexpr int32_t SPRINT_SPEED = 900;

// 중력(cm/s^2)과 점프 초기 속도(cm/s)
inline constexpr int32_t GRAVITY        = 1960;
inline constexpr int32_t JUMP_VELOCITY  = 700;

// 시야 목록 재계산 주기(틱). 매 틱 할 필요는 없다.
inline constexpr int32_t VIEW_UPDATE_INTERVAL = 3;   // 약 100ms

// ----------------------------------------------------------------------------
// 관심 관리 (interest management)
//
// 동접 수천 규모에서 먼저 무너지는 것은 CPU가 아니라 "시야 안 오브젝트 수"다.
// 월드에 고루 퍼져 있으면 아무 문제가 없지만, MMORPG는 마을에 몰린다.
//
//   반경 200m 광장에 800명 -> 각자 600명이 보임
//   800 x 600 = 480,000 패킷/틱 -> 초당 460MB
//
// 스레드를 몇 개 쓰든 불가능한 양이다. N^2 자체를 잘라야 한다.
// ----------------------------------------------------------------------------

// 시야에 담을 오브젝트 상한. 가까운 순으로 자른다.
// 사람이 100명 겹쳐 보이는 화면에서 50번째 캐릭터가 정확히 동기화되는지는
// 아무도 신경 쓰지 않는다.
inline constexpr int32_t MAX_VIEW_OBJECTS = 50;

// 스냅샷 전송 주기(틱). 시뮬레이션은 30Hz로 돌리되 전송은 15Hz.
// 클라이언트가 보간하므로 눈에 띄지 않고 대역폭은 절반이 된다.
inline constexpr int32_t SNAPSHOT_INTERVAL = 2;

// 멀리 있는 오브젝트의 전송 주기(틱). 5Hz.
inline constexpr int32_t FAR_SNAPSHOT_INTERVAL = 6;

// 이 거리 안쪽은 매 스냅샷마다, 바깥은 FAR_SNAPSHOT_INTERVAL마다 보낸다.
inline constexpr int32_t NEAR_RANGE = 2000;   // 20m

// ----------------------------------------------------------------------------
// 패킷 타입
//
// enum을 패킷에 그대로 담지 않는다. 순수 enum의 크기는 구현체 정의라서
// 서버(MSVC)와 클라(UE)가 다르면 조용히 깨진다.
// ----------------------------------------------------------------------------

enum PacketType : uint16_t {
    PT_NONE            = 0,

    C2S_LOGIN          = 1,
    C2S_INPUT          = 2,   // 위치가 아니라 입력을 보낸다
    C2S_CHAT           = 3,
    C2S_TELEPORT       = 5,   // 개발/테스트 전용. 배포 시 막을 것
    C2S_LOGOUT         = 6,

    S2C_LOGIN_RESULT   = 100,
    S2C_AVATAR_INFO    = 101,
    S2C_ADD_OBJECT     = 102,
    S2C_REMOVE_OBJECT  = 103,
    S2C_SELF_STATE     = 104,  // 내 캐릭터의 권위 상태 + 입력 ack
    S2C_MOVE_OBJECT    = 105,  // 남의 캐릭터 상태
    S2C_CHAT_MESSAGE   = 106,
    S2C_STATUS_CHANGE  = 107,

    PT_MAX             = 256,
};

enum ObjectType : uint8_t {
    OBJ_PLAYER  = 0,
    OBJ_NPC     = 1,
    OBJ_MONSTER = 2,
};

// 입력 버튼 비트마스크
enum InputButton : uint8_t {
    BTN_JUMP   = 1 << 0,
    BTN_SPRINT = 1 << 1,
};

// 이동 상태 플래그
enum MoveFlag : uint8_t {
    MF_GROUNDED = 1 << 0,
};

#pragma pack(push, 1)

struct PacketHeader {
    uint16_t size;
    uint16_t type;
};

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

// 서버 권위 이동의 핵심 패킷.
//
// move_x / move_y는 정규화된 입력 방향을 1000배한 정수다.
// float를 그대로 보내지 않는 이유는, 시뮬레이션 전체를 정수로 돌려서
// 서버와 클라이언트가 같은 입력에 같은 결과를 내게 하기 위해서다.
// float 연산은 컴파일러와 최적화 옵션에 따라 미세하게 달라지고,
// 그 오차가 매 틱 누적되면 예측이 계속 어긋난다.
//
// sequence는 클라가 붙이는 단조 증가 번호다. 서버가 S2C_SelfState로
// "몇 번까지 처리했다"고 알려주면, 클라는 그 뒤 입력만 다시 적용한다.
struct C2S_Input {
    PacketHeader h;
    uint32_t sequence;
    int16_t  move_x;    // -1000 ~ 1000
    int16_t  move_y;    // -1000 ~ 1000
    int16_t  yaw;       // 0 ~ 35999 (0.01도)
    uint8_t  buttons;   // InputButton 비트마스크
};

struct C2S_Chat {
    PacketHeader h;
    char message[MAX_CHAT_MSG_LEN];
};

struct C2S_Attack {
    PacketHeader h;
    int32_t target_id;
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
    uint8_t success;
    int32_t object_id;
    char    message[50];
};

struct S2C_AvatarInfo {
    PacketHeader h;
    int32_t  object_id;
    int32_t  visual_id;
    Vec3i    pos;
    int16_t  yaw;
    int32_t  hp;
    int32_t  max_hp;
    uint64_t exp;
    uint8_t  level;
};

struct S2C_AddObject {
    PacketHeader h;
    int32_t object_id;
    uint8_t object_type;
    int32_t visual_id;
    char    obj_name[MAX_NAME_LEN];
    Vec3i   pos;
    int16_t yaw;
    int32_t hp;
    int32_t max_hp;
    uint8_t level;
};

struct S2C_RemoveObject {
    PacketHeader h;
    int32_t object_id;
};

// 내 캐릭터의 권위 상태.
//
// last_processed_input이 화해(reconciliation)의 열쇠다.
// 클라는 이 번호 이하의 입력을 히스토리에서 버리고,
// 남은 입력들을 서버가 준 위치에서부터 다시 적용해 현재 위치를 만든다.
struct S2C_SelfState {
    PacketHeader h;
    uint32_t last_processed_input;
    uint32_t server_tick;
    Vec3i    pos;
    int16_t  yaw;
    int16_t  vel_x;     // cm/s
    int16_t  vel_y;
    int16_t  vel_z;
    uint8_t  flags;     // MoveFlag
};

// 남의 캐릭터/NPC 상태.
//
// 속도를 같이 보내는 이유: 위치만 보내면 클라는 값이 도착한 뒤에야 보간을
// 시작할 수 있어서 항상 한 틱 늦게 움직인다. 속도가 있으면 다음 위치를
// 미리 추정(dead reckoning)할 수 있다.
struct S2C_MoveObject {
    PacketHeader h;
    int32_t  object_id;
    uint32_t server_tick;
    Vec3i    pos;
    int16_t  yaw;
    int16_t  vel_x;
    int16_t  vel_y;
};

struct S2C_ChatMessage {
    PacketHeader h;
    int32_t object_id;
    char    message[MAX_CHAT_MSG_LEN];
};

struct S2C_StatusChange {
    PacketHeader h;
    int32_t  object_id;
    int32_t  hp;
    int32_t  max_hp;
    uint64_t exp;
    uint8_t  level;
};

#pragma pack(pop)

// ----------------------------------------------------------------------------
// 패킷 조립 헬퍼
// 항상 `T packet{};`로 값 초기화한 뒤 이 함수로 헤더를 채운다.
// 초기화를 빼먹으면 스택 쓰레기값이 그대로 네트워크로 나간다.
// ----------------------------------------------------------------------------
template <typename T>
constexpr void InitHeader(T& packet, PacketType type) {
    static_assert(sizeof(T) <= MAX_PACKET_SIZE, "패킷이 MAX_PACKET_SIZE를 넘는다");
    packet.h.size = static_cast<uint16_t>(sizeof(T));
    packet.h.type = static_cast<uint16_t>(type);
}

static_assert(sizeof(PacketHeader) == 4,  "헤더는 4바이트여야 한다");
static_assert(sizeof(Vec3i)        == 12, "Vec3i는 12바이트여야 한다");
