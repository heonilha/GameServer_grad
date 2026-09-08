#pragma once
// ============================================================================
// fixed_math.h — 전투 판정용 삼각함수 테이블
//
// yaw는 0~35999 (0.01도 단위)로 들어온다.
// 매번 std::cos를 부르지 않도록 3600개(0.1도 간격) 테이블을 만들어 둔다.
//
// [부동소수점을 써도 되는 이유]
// movement.h는 클라이언트가 같은 결과를 내야 해서 정수만 썼지만,
// 전투 판정은 서버만 한다. 클라는 서버가 알려준 결과를 받을 뿐이라
// 비트 단위로 일치할 필요가 없다.
// 테이블을 만들 때만 부동소수점을 쓰고, 판정 자체는 정수 비교로 한다.
// ============================================================================

#include <cstdint>
#include <array>
#include <cmath>

namespace fixed_math_detail {

    inline constexpr int32_t TABLE_SIZE = 3600;   // 0.1도 간격

    struct TrigTable {
        std::array<int32_t, TABLE_SIZE> cos_1e4{};
        std::array<int32_t, TABLE_SIZE> sin_1e4{};

        TrigTable() {
            constexpr double kPi = 3.14159265358979323846;
            for (int32_t i = 0; i < TABLE_SIZE; ++i) {
                const double rad = (i * 0.1) * kPi / 180.0;
                cos_1e4[i] = static_cast<int32_t>(std::lround(std::cos(rad) * 10000.0));
                sin_1e4[i] = static_cast<int32_t>(std::lround(std::sin(rad) * 10000.0));
            }
        }
    };

    inline const TrigTable& Table() {
        static const TrigTable table;   // 최초 호출 시 한 번만 만든다
        return table;
    }

    inline int32_t Index(int32_t yaw_centideg) {
        int32_t i = yaw_centideg / 10;          // 0.01도 -> 0.1도
        i %= TABLE_SIZE;
        if (i < 0) i += TABLE_SIZE;
        return i;
    }

} // namespace fixed_math_detail

// 반환값은 실제 값의 10000배다.
inline int32_t Cos1e4(int32_t yaw_centideg) {
    return fixed_math_detail::Table().cos_1e4[fixed_math_detail::Index(yaw_centideg)];
}

inline int32_t Sin1e4(int32_t yaw_centideg) {
    return fixed_math_detail::Table().sin_1e4[fixed_math_detail::Index(yaw_centideg)];
}
