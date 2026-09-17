# 졸업작품 MMORPG 서버

오픈월드 MMORPG의 자체 제작 게임 서버.
C++26 execution(P2300, stdexec) 기반 sender/receiver 모델 위에
Windows IOCP를 통합했다.

## 처음 받았을 때

**Visual Studio 2026**("C++를 사용한 데스크톱 개발" 워크로드)만 있으면 된다.
라이브러리 설치나 프로젝트 속성 입력은 필요 없다.

1. 저장소를 클론한다
2. `GameServer_grad/GameServer_grad.slnx`를 연다
3. 구성을 **Debug / x64**로 두고 **F5**
4. 콘솔에 `Game server started on port 3500`이 나오면 준비 완료

- 처음 실행하면 Windows 방화벽 창이 뜬다. 다른 PC에서 접속하려면 **개인 네트워크**를 허용한다.
  같은 PC(`127.0.0.1`)에서만 테스트하면 상관없다.
- 클라이언트 접속 방법은 클라이언트 저장소(`heonilha/Test`)의 `HANDOFF_CLIENT_V52.md` 참고.
- `Failed to load monsters.csv`로 바로 꺼지면 작업 디렉터리 문제다. `BUILD.md` 참고.

## 빌드

`BUILD.md` 참고. **`/utf-8` 컴파일 옵션이 필수다**(프로젝트 파일에 이미 들어 있다).
빼면 한글 주석이 구문을 깨뜨리고, 그 여파로 stdexec에서
관계없어 보이는 템플릿 오류가 쏟아진다.

## 구조

| 파일 | 역할 |
|---|---|
| `protocol.h` | **클라이언트와 공유.** 패킷 정의와 상수 |
| `movement.h` | **클라이언트와 공유.** 결정론적 정수 이동 시뮬레이션 |
| `skills.csv` / `monsters.csv` | **클라이언트와 공유.** 수치 테이블 |
| `net_core.h` | IOCP를 stdexec sender로 감싼 계층 |
| `packet_buffer.h` | 세션별 수신 조립 버퍼 |
| `world_object.h` | Session / NpcEntity / 관리자 |
| `world_grid.h` | 시야 처리용 섹터 격자 |
| `nav_grid.h` | 이동가능 비트맵과 높이맵 |
| `game_tick.h` | 고정 주기 시뮬레이션 루프 |
| `tick_worker_pool.h` | 읽기 전용 페이즈 병렬화 |
| `tick_metrics.h` | 성능 계측과 CSV 출력 |
| `combat.h` / `combat_tick.h` | 전투 판정 |
| `pathfinding.h` | 시선 검사 + 지역 A* |
| `monster_ai.h` | 몬스터 상태 기계 |
| `server_main.cpp` | 패킷 핸들러와 서버 기동 |

### 공유 파일 규칙

`protocol.h`, `movement.h`, `skills.csv`, `monsters.csv`는
**서버 저장소에 원본을 두고 클라이언트가 복사해 간다.**
복사본 두 개를 손으로 맞추면 반드시 어긋난다.

`movement.h`가 공유여야 하는 이유는 클라이언트 예측이 성립하려면
서버와 클라가 같은 입력에 같은 결과를 내야 하기 때문이다.
그래서 클라에서 `CharacterMovementComponent`를 쓰면 안 된다.
언리얼 이동 컴포넌트는 서버가 알 수 없는 내부 상태를 갖고 있다.

## 틱 구조

```
30Hz 고정 틱
  페이즈 1  시뮬레이션 + 그리드 갱신   월드 변경   단일 스레드
  페이즈 A  몬스터 AI (10Hz)          월드 변경   단일 스레드
  페이즈 B  전투 판정                 월드 변경   단일 스레드
  페이즈 2  시야 갱신 (10Hz)          자기 것만   병렬
  페이즈 3  스냅샷 전송 (15Hz)        읽기만      병렬
```

## 계측

5초마다 콘솔에 요약이 찍히고, Ctrl+C로 종료하면
`tick_metrics.csv`가 생성된다.

```
[stat] players=1200 tick avg=4.31ms p99=11.80ms max=18.20ms | pkt/tick=8400 out=6.15MB/s | overrun=0
```

평균이 아니라 **p99**를 봐야 한다. 평균은 스파이크를 감추는데,
플레이어가 렉을 체감하는 것은 p99 구간이다.

## 발전 과정

`docs/CHANGELOG.md` 참고.
