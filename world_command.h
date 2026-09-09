#pragma once
// ============================================================================
// world_command.h — 섹터 경계를 넘는 변경을 모으는 커맨드 버퍼
//
// [왜 필요한가]
//
// 섹터 병렬화(체커보드)가 성립하려면 규칙 하나가 지켜져야 한다.
//
//     섹터 태스크는 자기 섹터가 소유한 객체만 변경한다.
//
// 왜 이 규칙으로 충분한지 따져 보면 이렇다.
//   - 같은 웨이브에서 도는 섹터들은 간격이 2 이상이다.
//   - 사이에 섹터가 통째로 하나 끼므로 서로의 객체는 최소 SECTOR_SIZE 떨어져 있다.
//   - VIEW_RANGE <= SECTOR_SIZE 이므로 서로를 볼 수조차 없다.
//   - 활성 섹터가 아닌 섹터의 객체는 이번 웨이브에 아무도 쓰지 않는다.
//     따라서 읽는 것은 안전하다.
//
// 즉 "읽기는 자유, 쓰기는 자기 것만"이다.
// 남의 객체를 바꿔야 하면 여기에 커맨드로 적어두고,
// 웨이브 사이의 짧은 직렬 구간에서 한꺼번에 적용한다.
//
// [지금 당장의 이득]
//
// 아직 병렬 실행을 하지 않아도 이득이 있다.
// 기존에는 투사체 폭발 하나가 순회 도중에 여러 객체의 HP를 깎았고,
// 그 순서가 그리드 순회 순서에 따라 달라졌다. 커맨드로 모아서
// 정해진 순서로 적용하면 결과가 결정론적이 된다.
//
// [주의]
//
// 패킷 전송은 커맨드로 만들지 않았다. 전송 버퍼는 월드 상태가 아니고
// 세션마다 자체 락이 있어서, 두 섹터가 같은 세션에 보내도 안전하다.
// 다만 그 경우 락 경합이 생기므로, 나중에 문제가 되면 이쪽도 옮긴다.
// ============================================================================

#include <cstdint>
#include <vector>

#include "protocol.h"

enum class CommandType : uint8_t {
    Damage,      // 피해. 사망 처리까지 여기서 결정된다
    Migrate,     // 섹터 소속 변경 (객체가 섹터 경계를 넘음)
    GridRemove,  // 그리드에서 제거 (사망, 접속 종료)
    GridAdd,     // 그리드에 추가 (부활, 접속)
};

struct WorldCommand {
    CommandType type = CommandType::Damage;
    int32_t     target_id = -1;
    int32_t     source_id = -1;   // 가해자. 없으면 -1
    int32_t     value = 0;    // 피해량
    Vec3i       from{};            // Migrate 이전 위치
    Vec3i       to{};              // Migrate 이후 위치 / GridAdd 위치
};

// ----------------------------------------------------------------------------
// 파티션 하나가 쓰는 출력함
//
// 파티션(= 나중의 섹터 그룹)마다 하나씩 두므로 기록 중에는 락이 없다.
// ----------------------------------------------------------------------------
class CommandOutbox {
public:
    void Damage(int32_t target_id, int32_t source_id, int32_t amount) {
        WorldCommand c{};
        c.type = CommandType::Damage;
        c.target_id = target_id;
        c.source_id = source_id;
        c.value = amount;
        m_commands.push_back(c);
    }

    void Migrate(int32_t object_id, const Vec3i& from, const Vec3i& to) {
        WorldCommand c{};
        c.type = CommandType::Migrate;
        c.target_id = object_id;
        c.from = from;
        c.to = to;
        m_commands.push_back(c);
    }

    void GridRemove(int32_t object_id, const Vec3i& at) {
        WorldCommand c{};
        c.type = CommandType::GridRemove;
        c.target_id = object_id;
        c.from = at;
        m_commands.push_back(c);
    }

    void GridAdd(int32_t object_id, const Vec3i& at) {
        WorldCommand c{};
        c.type = CommandType::GridAdd;
        c.target_id = object_id;
        c.to = at;
        m_commands.push_back(c);
    }

    const std::vector<WorldCommand>& Commands() const { return m_commands; }
    void Clear() { m_commands.clear(); }
    bool Empty() const { return m_commands.empty(); }

private:
    std::vector<WorldCommand> m_commands;
};

// ----------------------------------------------------------------------------
// 전체 버스
//
// 적용 순서는 파티션 번호 -> 기록 순서로 고정한다.
// 이 순서가 고정되어야 같은 입력에 같은 결과가 나온다.
// 스레드 완료 순서에 맡기면 재현이 불가능해져서 디버깅이 어려워진다.
// ----------------------------------------------------------------------------
class CommandBus {
public:
    void Initialize(size_t partition_count) {
        m_outboxes.resize(partition_count);
    }

    CommandOutbox& For(size_t partition) { return m_outboxes[partition]; }

    size_t PartitionCount() const { return m_outboxes.size(); }

    // 직렬 구간에서만 부른다.
    template <typename F>
    void ForEachCommand(F&& f) const {
        for (const CommandOutbox& box : m_outboxes) {
            for (const WorldCommand& c : box.Commands()) f(c);
        }
    }

    void Clear() {
        for (CommandOutbox& box : m_outboxes) box.Clear();
    }

    size_t Count() const {
        size_t n = 0;
        for (const CommandOutbox& box : m_outboxes) n += box.Commands().size();
        return n;
    }

private:
    std::vector<CommandOutbox> m_outboxes;
};
