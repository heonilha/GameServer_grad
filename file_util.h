#pragma once
// ============================================================================
// file_util.h — 파일 열기 한 겹
//
// fopen_s는 MSVC 확장이고 시그니처도 fopen과 다르다.
// 여기서 한 겹 덮어두면 두 가지가 좋아진다.
//
//   1. 호출부가 짧아진다 (출력 인자 + 반환 코드 검사 대신 포인터 하나)
//   2. MSVC 밖에서도 빌드된다.
//      서버는 Windows 전용이지만, 프로토콜/수치 테이블/경로 탐색처럼
//      OS와 무관한 부분은 다른 컴파일러로 따로 빌드해서 검증할 수 있다.
//      경고 수준이 다른 컴파일러를 한 번 통과시키는 것만으로도
//      MSVC가 놓치는 실수가 잡힌다.
// ============================================================================

#include <cstdio>

inline std::FILE* OpenFile(const char* path, const char* mode)
{
#ifdef _MSC_VER
    std::FILE* fp = nullptr;
    if (fopen_s(&fp, path, mode) != 0) return nullptr;
    return fp;
#else
    return std::fopen(path, mode);
#endif
}
