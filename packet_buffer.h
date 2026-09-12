#pragma once
// ============================================================================
// packet_buffer.h — 세션별 수신 조립 버퍼
//
// 기존 코드의 문제:
//   std::vector<unsigned char> leftover;
//   leftover.insert(leftover.end(), r.data.begin(), r.data.end());
//   ...
//   leftover.erase(leftover.begin(), leftover.begin() + offset);
//
//   erase(begin, ...)는 남은 원소를 전부 앞으로 당긴다. recv 한 번마다
//   O(n) 이동이 생기고, 앞서 vector를 새로 만들어 복사하는 비용까지 겹쳤다.
//
// 이 클래스는 고정 배열 + head/tail 커서로 그 비용을 없앤다.
// 유효 데이터는 항상 [m_head, m_tail) 구간에 연속으로 놓인다.
// 링버퍼처럼 wrap-around를 두지 않는 이유는, 패킷이 경계에서 두 조각으로
// 갈라지면 파싱 코드가 훨씬 복잡해지기 때문이다. 대신 앞쪽이 비면 남은
// 조각(항상 패킷 하나 미만)만 앞으로 당기므로 비용이 사실상 없다.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <array>

#include "protocol.h"

class PacketBuffer {
public:
    // 최대 패킷 여러 개가 한 번에 들어와도 담기게 넉넉히 잡는다.
    static constexpr size_t CAPACITY = MAX_PACKET_SIZE * 16;

    // ---- 수신 ----

    // WSARecv가 직접 써 넣을 위치. 여기에 바로 받으므로 복사가 없다.
    char* WritePtr() { return reinterpret_cast<char*>(m_data.data()) + m_tail; }

    // 지금 쓸 수 있는 바이트 수. 0이면 Compact()를 부르고 다시 확인한다.
    size_t Writable() const { return CAPACITY - m_tail; }

    // 실제로 받은 바이트 수를 반영한다.
    void Commit(size_t bytes) { m_tail += bytes; }

    // ---- 파싱 ----

    // 완전한 패킷이 하나 있으면 true를 반환하고 out에 시작 주소를 담는다.
    // 반환된 포인터는 Consume()이나 Compact() 호출 전까지만 유효하다.
    bool PeekPacket(const uint8_t*& out, uint16_t& out_size) const {
        const size_t available = m_tail - m_head;
        if (available < sizeof(PacketHeader)) return false;

        const uint8_t* p = m_data.data() + m_head;
        const uint16_t size = reinterpret_cast<const PacketHeader*>(p)->size;

        // 크기가 헤더보다 작거나 상한을 넘으면 스트림이 깨진 것이다.
        // 호출자가 세션을 끊도록 여기서는 false를 돌려준다.
        if (size < sizeof(PacketHeader) || size > MAX_PACKET_SIZE) {
            out_size = 0;
            return false;
        }
        if (available < size) return false;   // 아직 덜 왔다

        out = p;
        out_size = size;
        return true;
    }

    // PeekPacket이 false를 준 게 "덜 온 것"인지 "깨진 것"인지 구분한다.
    bool IsCorrupted() const {
        if (m_tail - m_head < sizeof(PacketHeader)) return false;
        const uint16_t size =
            reinterpret_cast<const PacketHeader*>(m_data.data() + m_head)->size;
        return size < sizeof(PacketHeader) || size > MAX_PACKET_SIZE;
    }

    // 처리를 마친 패킷만큼 앞으로 나아간다. 여기서는 메모리를 안 옮긴다.
    void Consume(uint16_t size) {
        m_head += size;
        if (m_head == m_tail) {         // 전부 처리했으면 커서만 리셋
            m_head = 0;
            m_tail = 0;
        }
    }

    // 남은 조각을 앞으로 당겨 쓰기 공간을 확보한다.
    // 옮기는 양은 항상 패킷 하나 미만이라 비용이 거의 없다.
    void Compact() {
        if (m_head == 0) return;
        const size_t remain = m_tail - m_head;
        if (remain > 0) {
            std::memmove(m_data.data(), m_data.data() + m_head, remain);
        }
        m_head = 0;
        m_tail = remain;
    }

private:
    std::array<uint8_t, CAPACITY> m_data{};
    size_t m_head = 0;   // 아직 처리하지 않은 데이터의 시작
    size_t m_tail = 0;   // 받은 데이터의 끝
};
