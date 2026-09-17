#pragma once
// ============================================================================
// csv_util.h — CSV 파싱과 문자열 복사 도우미
//
// strtok_s와 strncpy를 쓰지 않는 이유
//
//   strtok_s는 MSVC 확장이라 std:: 네임스페이스에 없다.
//   <cstring>을 포함해도 std::strtok_s는 존재하지 않으므로
//   "네임스페이스 std에 strtok_s 멤버가 없습니다" 오류가 난다.
//   전역 ::strtok_s로 부르면 되지만 그러면 MSVC에서만 빌드된다.
//
//   strncpy는 표준이지만 MSVC가 C4996(안전하지 않음) 경고를 낸다.
//   경고를 오류로 취급하는 설정(/WX)이면 빌드가 멈춘다.
//   게다가 대상이 꽉 차면 널 종료를 붙여주지 않아 실수하기 쉽다.
//
// 둘 다 직접 쓰는 편이 짧고, 의존성도 없고, 동작이 명확하다.
// ============================================================================

#include <cstddef>
#include <cstring>

#include "file_util.h"

// ----------------------------------------------------------------------------
// CSV 한 줄을 쉼표로 쪼갠다.
//
// 원본 문자열을 제자리에서 자른다(쉼표를 '\0'으로 바꾼다).
// 따라서 line은 수정 가능한 버퍼여야 하고, 반환된 포인터들은
// line이 살아있는 동안만 유효하다.
//
// 따옴표로 감싼 필드는 지원하지 않는다. 수치 테이블에는 쉼표가
// 들어간 값이 없으므로 필요하지 않다. 필요해지면 그때 확장한다.
//
// 반환: 실제로 채운 필드 수
// ----------------------------------------------------------------------------
inline int SplitCsvLine(char* line, const char** fields, int max_fields)
{
    if (line == nullptr || max_fields <= 0) return 0;

    int count = 0;
    char* cursor = line;
    fields[count++] = cursor;

    for (; *cursor != '\0'; ++cursor) {
        if (*cursor == '\r' || *cursor == '\n') {
            *cursor = '\0';           // 줄 끝. 여기서 멈춘다
            break;
        }
        if (*cursor == ',' && count < max_fields) {
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    return count;
}

// ----------------------------------------------------------------------------
// 고정 크기 버퍼에 안전하게 복사한다. 항상 널로 끝난다.
// ----------------------------------------------------------------------------
inline void CopyFixed(char* dest, size_t dest_size, const char* src)
{
    if (dest == nullptr || dest_size == 0) return;
    if (src == nullptr) { dest[0] = '\0'; return; }

    size_t i = 0;
    for (; i + 1 < dest_size && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}
