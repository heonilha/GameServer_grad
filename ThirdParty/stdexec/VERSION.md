# stdexec (저장소 포함 사본)

- 원본: https://github.com/NVIDIA/stdexec
- 라이선스: Apache License 2.0 with LLVM Exception (`LICENSE.txt`)
- 포함 범위: `include/` 만 (헤더 전용 라이브러리라 빌드 산출물이 없다)

## 버전

받은 버전이 기록돼 있지 않아, 2026-09-17에 upstream 이력과 대조해 찾았다.
이 `include/` 트리의 git tree 해시가 upstream `main`의 아래 커밋들과 **완전히 같다.**

| 커밋 | 날짜 |
|---|---|
| `f0e8ae6fdc6c188389b146bb854b80d399724b04` | 2026-07-27 (이 트리가 처음 나온 커밋) |
| `f96062202bca370e79c109f982de2b23d23de2f2` | 2026-08-07 (이 트리가 유지된 마지막 커밋) |

기준 버전은 `f96062202bca370e79c109f982de2b23d23de2f2`로 본다.
`include` tree 해시: `aea7ff9c518b3a240f1e67b1309196d6cd8efe4b`

## 갱신할 때

1. upstream에서 원하는 커밋의 `include/`와 `LICENSE.txt`로 통째로 교체한다 (부분 수정 금지)
2. 위 표의 커밋 해시를 갱신한다
3. 서버를 전체 빌드하고 `pcheck`를 돌린다

`.gitattributes`의 `ThirdParty/** -text` 때문에 줄바꿈이 변환되지 않는다.
