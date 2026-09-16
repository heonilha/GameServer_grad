#pragma once
// ============================================================================
// movement.h — 이동 시뮬레이션
//
// [가장 중요한 파일]
//
// 이 파일은 protocol.h와 마찬가지로 서버와 클라이언트가 그대로 공유한다.
// 클라이언트 예측(prediction)이 성립하려면 양쪽이 "같은 입력에 같은 결과"를
// 내야 하는데, 그러려면 시뮬레이션 코드가 문자 그대로 같아야 하기 때문이다.
//
// 언리얼 쪽에서 CharacterMovementComponent를 쓰지 않고 이 함수로 직접
// 위치를 계산해야 한다. 언리얼 이동 컴포넌트는 내부에 서버가 알 수 없는
// 상태(스텝업, 스윕 결과, 물리 씬)를 잔뜩 갖고 있어서 재현이 불가능하다.
//
// ---------------------------------------------------------------------------
// 왜 전부 정수 연산인가
//
// float 연산은 컴파일러(서버 MSVC / 클라 UE-Clang), 최적화 옵션, SIMD 사용
// 여부에 따라 마지막 비트가 달라질 수 있다. 한 틱의 오차는 무시할 만하지만
// 매 틱 누적되면 예측 위치가 서버와 계속 어긋나고, 스냅샷이 도착할 때마다
// 캐릭터가 미세하게 떤다.
//
// 단위를 cm와 ms로 고정하고 정수만 쓰면 이 문제가 원천적으로 사라진다.
// 중간 계산은 오버플로를 막기 위해 int64로 한다.
// ============================================================================

#include <cstdint>

#include "protocol.h"
#include "nav_grid.h"

// 이 높이 차이 이하는 지면에 붙은 것으로 본다 (cm).
// 경사면을 걸을 때 grounded 판정이 깜빡이는 것을 막는 값.
inline constexpr int32_t STEP_TOLERANCE = 45;

// ----------------------------------------------------------------------------
// 시뮬레이션 상태 — 서버와 클라가 똑같이 들고 있어야 하는 값
// ----------------------------------------------------------------------------
struct MoveState {
    Vec3i   pos{};
    int32_t vel_x = 0;      // cm/s
    int32_t vel_y = 0;
    int32_t vel_z = 0;
    uint16_t yaw = 0;
    bool    grounded = true;

    // 이동량 계산에서 버려지는 소수부를 1/1000 cm 단위로 들고 있는다.
    //
    // 없으면 매 틱 잘린 만큼이 그대로 손실된다.
    //   600 cm/s * 33 ms / 1000 = 19.8 -> 19 cm
    //   30틱이면 570 cm. 설계값 600의 95%밖에 안 나온다.
    //
    // 잔차를 다음 틱으로 넘기면 설계값과 일치한다.
    // 서버와 클라이언트가 같은 코드를 돌리므로 예측도 그대로 맞는다.
    int32_t frac_x = 0;
    int32_t frac_y = 0;
    int32_t frac_z = 0;
};

// 한 번의 입력. C2S_Input에서 헤더를 뺀 것과 같다.
struct MoveInput {
    uint32_t sequence = 0;
    int16_t  move_x = 0;    // -1000 ~ 1000
    int16_t  move_y = 0;
    uint16_t yaw = 0;
    uint8_t  buttons = 0;
};

namespace movement_detail {

    inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    // 정수 제곱근 (뉴턴법). 입력 방향 정규화에 쓴다.
    // std::sqrt를 쓰지 않는 이유는 부동소수점을 배제하기 위해서다.
    inline int64_t ISqrt(int64_t n) {
        if (n <= 0) return 0;
        int64_t x = n;
        int64_t y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + n / x) / 2;
        }
        return x;
    }

} // namespace movement_detail

// ----------------------------------------------------------------------------
// 입력 정제
//
// 클라이언트가 보낸 값을 그대로 믿지 않는다. move_x/move_y에 10000을 넣어
// 열 배 속도로 달리는 것이 가장 흔한 조작이다.
// 크기가 1000을 넘으면 방향은 유지하고 크기만 1000으로 깎는다.
// ----------------------------------------------------------------------------
inline MoveInput SanitizeInput(const MoveInput& raw) {
    using namespace movement_detail;

    MoveInput in = raw;
    in.move_x = static_cast<int16_t>(Clamp(in.move_x, -1000, 1000));
    in.move_y = static_cast<int16_t>(Clamp(in.move_y, -1000, 1000));
    in.yaw = static_cast<uint16_t>(Clamp(in.yaw, 0, 35999));

    const int64_t mag_sq =
        static_cast<int64_t>(in.move_x) * in.move_x +
        static_cast<int64_t>(in.move_y) * in.move_y;

    if (mag_sq > 1000LL * 1000LL) {
        const int64_t mag = ISqrt(mag_sq);
        if (mag > 0) {
            in.move_x = static_cast<int16_t>(in.move_x * 1000 / mag);
            in.move_y = static_cast<int16_t>(in.move_y * 1000 / mag);
        }
    }
    return in;
}

// ----------------------------------------------------------------------------
// 한 틱 시뮬레이션
//
// 서버는 틱 루프에서, 클라는 예측과 재적용(replay)에서 이 함수를 부른다.
// 두 곳의 호출 결과가 같아야 하므로, 여기에 시간·난수·전역 상태를
// 들여오면 안 된다. 인자만 보고 결정되는 순수 함수여야 한다.
// ----------------------------------------------------------------------------
inline void SimulateStep(MoveState& state,
    const MoveInput& raw_input,
    const NavGrid& nav)
{
    using namespace movement_detail;

    const MoveInput input = SanitizeInput(raw_input);
    state.yaw = input.yaw;

    // ---- 수평 속도 ----
    const int32_t speed = (input.buttons & BTN_SPRINT) ? SPRINT_SPEED : WALK_SPEED;
    state.vel_x = static_cast<int32_t>(
        static_cast<int64_t>(input.move_x) * speed / 1000);
    state.vel_y = static_cast<int32_t>(
        static_cast<int64_t>(input.move_y) * speed / 1000);

    // ---- 점프와 중력 ----
    if (state.grounded && (input.buttons & BTN_JUMP)) {
        state.vel_z = JUMP_VELOCITY;
        state.grounded = false;
    }
    if (!state.grounded) {
        state.vel_z -= GRAVITY * TICK_MS / 1000;
    }

    // ---- 이동량 (cm/s * ms / 1000 = cm) ----
    //
    // 나머지를 버리지 않고 다음 틱으로 넘긴다.
    // C++의 정수 나눗셈은 0 방향으로 자르고 %의 부호는 피제수를 따르므로,
    // 음수 방향 이동에서도 같은 방식으로 동작한다. 결정론이 유지된다.
    auto step = [](int32_t velocity, int32_t& frac) -> int32_t {
        const int64_t total =
            static_cast<int64_t>(velocity) * TICK_MS + frac;
        frac = static_cast<int32_t>(total % 1000);
        return static_cast<int32_t>(total / 1000);
    };

    const int32_t dx = step(state.vel_x, state.frac_x);
    const int32_t dy = step(state.vel_y, state.frac_y);
    const int32_t dz = step(state.vel_z, state.frac_z);

    // ---- 수평 이동 + 벽 슬라이딩 ----
    //
    // 대각선 이동이 벽에 막혔을 때 완전히 멈추면 조작감이 나쁘다.
    // X와 Y를 따로 시도해서 가능한 축으로만 미끄러지게 한다.
    const int32_t target_x = state.pos.x + dx;
    const int32_t target_y = state.pos.y + dy;

    if (nav.IsWalkable(target_x, target_y)) {
        state.pos.x = target_x;
        state.pos.y = target_y;
    }
    else {
        if (dx != 0 && nav.IsWalkable(target_x, state.pos.y)) {
            state.pos.x = target_x;
            state.vel_y = 0;
            state.frac_y = 0;
        }
        else if (dy != 0 && nav.IsWalkable(state.pos.x, target_y)) {
            state.pos.y = target_y;
            state.vel_x = 0;
            state.frac_x = 0;
        }
        else {
            state.vel_x = 0;
            state.vel_y = 0;
            state.frac_x = 0;
            state.frac_y = 0;
        }
    }

    // 월드 경계
    state.pos.x = Clamp(state.pos.x, WORLD_MIN_CM, WORLD_MAX_CM - 1);
    state.pos.y = Clamp(state.pos.y, WORLD_MIN_CM, WORLD_MAX_CM - 1);

    // ---- 수직 이동과 착지 ----
    const int32_t ground = nav.SampleHeight(state.pos.x, state.pos.y);
    state.pos.z += dz;

    if (state.pos.z <= ground) {
        state.pos.z = ground;
        state.vel_z = 0;
        state.frac_z = 0;
        state.grounded = true;
    }
    else if (state.vel_z <= 0 && state.pos.z - ground < STEP_TOLERANCE) {
        // 완만한 경사를 내려갈 때 매 틱 공중 판정이 되는 걸 막는다.
        state.pos.z = ground;
        state.vel_z = 0;
        state.frac_z = 0;
        state.grounded = true;
    }
    else {
        state.grounded = false;
    }
}
