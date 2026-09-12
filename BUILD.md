# 빌드

## 요구 사항

- Visual Studio 2022
- C++20 이상 (`/std:c++20`). 코루틴이 필요하다
- [stdexec](https://github.com/NVIDIA/stdexec) — include 경로에 추가
- 링크: `WS2_32.lib`, `MSWSock.lib` (소스에 `#pragma comment`로 걸려 있다)

실행 디렉터리에 `skills.csv`, `monsters.csv`가 있어야 한다.
`navdata.bin`은 없으면 평지로 동작한다.

## 필수 설정: `/utf-8`

**이 설정을 빼면 빌드가 깨진다.** 반드시 넣어야 한다.

```
프로젝트 속성 → C/C++ → 명령줄 → 추가 옵션:  /utf-8
```

또는 `.vcxproj`에 직접:

```xml
<ClCompile>
  <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
</ClCompile>
```

### 왜 필요한가

소스가 UTF-8인데 MSVC는 한국어 Windows에서 기본적으로 CP949로 읽는다.
UTF-8 한글 한 글자는 3바이트인데 CP949는 2바이트씩 묶어 읽으므로
정렬이 어긋나고, 마지막 바이트가 리드 바이트로 해석되면서
**바로 뒤의 문자를 삼켜버린다.**

```cpp
static_assert(COND, "... 충분하다");
//                            ^^ '다'의 마지막 바이트가 닫는 따옴표를 삼킨다
```

그러면 문자열이 안 닫히고 그 뒤 코드가 통째로 문자열로 먹힌다.
증상은 이렇게 나타난다.

```
error C2001: 문자열 리터럴 내 줄 바꿈
error C2143: '(': 일치하는 토큰을 찾을 수 없습니다
```

여기서 끝나지 않는 것이 문제다. `protocol.h`가 깨지면 `Vec3i`,
`MAX_PACKET_SIZE` 같은 타입이 정의되지 않은 채로 템플릿이
인스턴스화되면서, **엉뚱한 곳에서 수백 줄짜리 stdexec 오류가 튀어나온다.**

```
"stdexec::__call_result_t<...>"에 멤버 "await_ready"이(가) 없음
```

`co_await`이나 sender 정의가 잘못된 것처럼 보이지만 원인은 인코딩이다.
**stdexec 오류를 만나면 먼저 그 위에 C2001이 있는지 확인할 것.**

## 이중 안전장치

모든 `.h` / `.cpp`에 UTF-8 BOM을 넣어 두었다.
BOM이 있으면 `/utf-8` 없이도 MSVC가 UTF-8로 인식한다.

다만 다른 편집기나 도구로 다시 저장하면서 BOM이 날아가는 일이 흔하다.
그래서 `/utf-8`도 같이 넣는 것을 권한다. 둘 다 있어도 문제없다.

컴파일 타임 메시지(`static_assert`)와 런타임 로그 문자열은
전부 ASCII로 바꿔 두었다. 만약 BOM과 `/utf-8`이 모두 없는 상태로
빌드하더라도 최소한 파싱은 통과하게 하기 위해서다.
주석의 한글은 그대로 두었다. 주석은 삼켜져도 구문이 깨지지 않는다.

## 콘솔 출력

`main()`에서 `SetConsoleOutputCP(CP_UTF8)`을 호출한다.
소스가 UTF-8이라 문자열 리터럴도 UTF-8이고,
콘솔 코드 페이지를 맞추지 않으면 한글 로그가 깨진다.

## CSV 파일

`skills.csv`, `monsters.csv`는 **BOM 없이** 저장해야 한다.
런타임 파서가 첫 글자를 보고 주석/헤더를 판별하는데
BOM이 있으면 그 판별이 어긋난다.

메모장에서 편집하면 BOM이 붙을 수 있으니 주의할 것.

## OS 비의존 부분 검증

`tools_portable_check.cpp`는 Windows API를 쓰지 않는 헤더만 모아
컴파일하고 실행하는 작은 프로그램이다.

```bash
g++ -std=c++20 -I. -Wall -Wextra -o pcheck tools_portable_check.cpp
./pcheck
```

MSVC 전체 빌드는 stdexec와 Winsock이 있어야 하지만,
프로토콜 / 수치 테이블 / 이동 시뮬레이션 / 경로 탐색은
OS와 무관해서 이렇게 따로 확인할 수 있다.

Visual Studio 프로젝트에는 넣지 말 것. `main`이 두 개가 된다.

확인 항목:

- `skills.csv`, `monsters.csv`가 실제로 파싱되는지 (컬럼 수 불일치 검출)
- 1초 이동 거리가 `WALK_SPEED`와 맞는지 (정수 절삭 손실 검출)
- 조작된 입력이 정제되는지
- 시선 검사와 그리드 웨이브 번호

예상 출력:

```
skills.csv  : OK (2)
monsters.csv: OK (3)
1초 이동 거리 : 594 cm (WALK_SPEED=600)
```

594는 정상이다. `TICK_MS`가 33이라 30틱이 990ms이고, 딱 1% 차이다.
정확히 맞추려면 `TICK_RATE`를 1000의 약수로 바꾸면 된다.
