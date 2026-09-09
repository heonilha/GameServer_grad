#pragma once
// ============================================================================
// tick_metrics.h — 틱 성능 계측
//
// 이 파일이 있는 이유는 성능 튜닝 때문만이 아니다.
//
// 기획서와 졸업 심사에서 "왜 이 구조를 택했는가"에 답하려면 숫자가 필요하다.
// "단일 스레드 시뮬레이션으로 충분하다"는 주장은 근거 없이는 그냥 의견이지만,
// 동접별 틱 소요 시간 그래프가 있으면 근거가 된다.
//
// 수집 항목:
//   - 페이즈별 소요 시간 (어디가 병목인지)
//   - 틱 초과 횟수 (33ms 예산을 넘긴 횟수)
//   - 전송 패킷 수와 바이트 수 (대역폭이 먼저 무너지는지 확인)
//
// CSV로 뽑으면 그대로 그래프가 된다.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class TickMetrics {
public:
    struct Sample {
        uint32_t tick = 0;
        int32_t  player_count = 0;
        int64_t  simulate_us = 0;
        int64_t  view_us = 0;
        int64_t  snapshot_us = 0;
        int64_t  total_us = 0;
        int64_t  packets = 0;
        int64_t  bytes = 0;
        int64_t  commands = 0;
    };

    // ---- 틱 단위 수집 ----

    void BeginTick(uint32_t tick, int32_t player_count) {
        m_current = Sample{};
        m_current.tick = tick;
        m_current.player_count = player_count;
        m_packets.store(0, std::memory_order_relaxed);
        m_bytes.store(0, std::memory_order_relaxed);
        m_tick_start = Clock::now();
        m_phase_start = m_tick_start;
    }

    void EndPhaseSimulate() { m_current.simulate_us = TakePhase(); }
    void EndPhaseView() { m_current.view_us = TakePhase(); }
    void EndPhaseSnapshot() { m_current.snapshot_us = TakePhase(); }

    // 병렬 페이즈에서 여러 스레드가 동시에 부른다.
    void AddPackets(int64_t count, int64_t bytes) {
        m_packets.fetch_add(count, std::memory_order_relaxed);
        m_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    void SetCommandCount(int64_t n) { m_current.commands = n; }

    void EndTick(int64_t budget_us) {
        m_current.total_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - m_tick_start).count();
        m_current.packets = m_packets.load(std::memory_order_relaxed);
        m_current.bytes = m_bytes.load(std::memory_order_relaxed);

        if (m_current.total_us > budget_us) ++m_overrun_count;

        m_history.push_back(m_current);
        if (m_history.size() > MAX_HISTORY) {
            m_history.erase(m_history.begin(),
                m_history.begin() + MAX_HISTORY / 4);
        }
    }

    // ---- 요약 ----

    // 최근 구간의 백분위수. 평균은 순간적인 스파이크를 감춘다.
    // 실제로 문제가 되는 것은 p99이므로 같이 본다.
    struct Summary {
        int64_t samples = 0;
        int64_t avg_us = 0;
        int64_t p50_us = 0;
        int64_t p99_us = 0;
        int64_t max_us = 0;
        int64_t avg_packets = 0;
        int64_t avg_bytes = 0;
        int64_t avg_commands = 0;
        int32_t players = 0;
    };

    Summary Summarize(size_t window = 300) const {
        Summary s{};
        if (m_history.empty()) return s;

        const size_t count = std::min(window, m_history.size());
        const size_t begin = m_history.size() - count;

        std::vector<int64_t> totals;
        totals.reserve(count);

        int64_t sum = 0, packets = 0, bytes = 0, commands = 0;
        for (size_t i = begin; i < m_history.size(); ++i) {
            totals.push_back(m_history[i].total_us);
            sum += m_history[i].total_us;
            packets += m_history[i].packets;
            bytes += m_history[i].bytes;
            commands += m_history[i].commands;
        }
        std::sort(totals.begin(), totals.end());

        s.samples = static_cast<int64_t>(count);
        s.avg_us = sum / static_cast<int64_t>(count);
        s.p50_us = totals[count / 2];
        s.p99_us = totals[static_cast<size_t>(count * 99 / 100)];
        s.max_us = totals.back();
        s.avg_packets = packets / static_cast<int64_t>(count);
        s.avg_bytes = bytes / static_cast<int64_t>(count);
        s.avg_commands = commands / static_cast<int64_t>(count);
        s.players = m_history.back().player_count;
        return s;
    }

    uint64_t OverrunCount() const { return m_overrun_count; }

    // ---- 내보내기 ----

    // 부하 테스트 결과를 CSV로. 엑셀이나 파이썬에서 바로 그래프로 만든다.
    bool WriteCsv(const std::string& path) const {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "w") != 0 || fp == nullptr) return false;

        std::fprintf(fp,
            "tick,players,simulate_us,view_us,snapshot_us,total_us,packets,bytes,commands\n");
        for (const Sample& s : m_history) {
            std::fprintf(fp, "%u,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                s.tick, s.player_count,
                static_cast<long long>(s.simulate_us),
                static_cast<long long>(s.view_us),
                static_cast<long long>(s.snapshot_us),
                static_cast<long long>(s.total_us),
                static_cast<long long>(s.packets),
                static_cast<long long>(s.bytes),
                static_cast<long long>(s.commands));
        }
        std::fclose(fp);
        return true;
    }

private:
    using Clock = std::chrono::steady_clock;

    int64_t TakePhase() {
        const auto now = Clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_phase_start).count();
        m_phase_start = now;
        return us;
    }

    static constexpr size_t MAX_HISTORY = 20000;   // 30Hz 기준 약 11분

    Sample m_current{};
    Clock::time_point m_tick_start{};
    Clock::time_point m_phase_start{};

    std::atomic<int64_t> m_packets{ 0 };
    std::atomic<int64_t> m_bytes{ 0 };

    std::vector<Sample> m_history;
    uint64_t m_overrun_count = 0;
};
