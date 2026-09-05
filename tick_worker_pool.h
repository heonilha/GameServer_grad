#pragma once
// ============================================================================
// tick_worker_pool.h — 틱 루프의 읽기 전용 페이즈를 나눠 돌리는 워커 풀
//
// 왜 페이즈를 나누는가
//
// 시뮬레이션 전체를 병렬화하면 "A가 B를 보는 시점"과 "B가 움직이는 시점"이
// 겹쳐서, 같은 틱인데 플레이어마다 다른 월드를 보게 된다.
// 그래서 쓰기 페이즈와 읽기 페이즈를 나눈다.
//
//   페이즈 1  시뮬레이션 + 그리드 갱신   월드를 변경   -> 단일 스레드
//   페이즈 2  시야 목록 갱신             자기 것만 변경 -> 병렬
//   페이즈 3  스냅샷 조립 + 전송         읽기만        -> 병렬
//
// 비용의 대부분은 3번인데 읽기 전용이라 안전하게 쪼개진다.
// 2번은 각 세션이 자기 시야 목록만 건드리도록 설계했으므로 역시 안전하다.
// (다른 세션의 시야 목록을 대신 고치는 코드가 있으면 이 전제가 깨진다)
//
// 표준 라이브러리만 쓴다. 틱마다 스레드를 만들지 않고 상주시켜서
// 생성 비용을 없앤다.
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class TickWorkerPool {
public:
    // worker_count는 "추가" 스레드 수다. 호출한 틱 스레드도 일을 나눠 맡으므로
    // 실제 분할 수는 worker_count + 1이 된다.
    explicit TickWorkerPool(unsigned int worker_count) {
        m_partitions = worker_count + 1;
        m_threads.reserve(worker_count);
        for (unsigned int i = 0; i < worker_count; ++i) {
            m_threads.emplace_back([this, i] { WorkerLoop(i + 1); });
        }
    }

    ~TickWorkerPool() {
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
        }
        m_start_cv.notify_all();
        for (auto& t : m_threads) {
            if (t.joinable()) t.join();
        }
    }

    TickWorkerPool(const TickWorkerPool&) = delete;
    TickWorkerPool& operator=(const TickWorkerPool&) = delete;

    unsigned int PartitionCount() const { return m_partitions; }

    // fn(partition_index, partition_count)를 모든 파티션에 대해 실행하고
    // 전부 끝날 때까지 기다린다.
    //
    // 호출자(틱 스레드)가 파티션 0을 직접 처리한다. 스레드 하나를 놀리지
    // 않기 위해서이기도 하고, 워커가 0개일 때 자동으로 단일 스레드로
    // 동작하게 만들기 위해서이기도 하다.
    void RunParallel(const std::function<void(unsigned int, unsigned int)>& fn) {
        if (m_threads.empty()) {
            fn(0, 1);
            return;
        }

        {
            std::lock_guard lock(m_mutex);
            m_task = &fn;
            m_pending = static_cast<int>(m_threads.size());
            ++m_generation;
        }
        m_start_cv.notify_all();

        fn(0, m_partitions);   // 호출자 몫

        std::unique_lock lock(m_mutex);
        m_done_cv.wait(lock, [this] { return m_pending == 0; });
        m_task = nullptr;
    }

private:
    void WorkerLoop(unsigned int partition_index) {
        uint64_t last_generation = 0;
        for (;;) {
            const std::function<void(unsigned int, unsigned int)>* task = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_start_cv.wait(lock, [&] {
                    return m_shutdown || m_generation != last_generation;
                    });
                if (m_shutdown) return;
                last_generation = m_generation;
                task = m_task;
            }

            if (task) (*task)(partition_index, m_partitions);

            {
                std::lock_guard lock(m_mutex);
                --m_pending;
            }
            m_done_cv.notify_one();
        }
    }

    std::vector<std::thread> m_threads;
    unsigned int m_partitions = 1;

    std::mutex m_mutex;
    std::condition_variable m_start_cv;
    std::condition_variable m_done_cv;

    const std::function<void(unsigned int, unsigned int)>* m_task = nullptr;
    uint64_t m_generation = 0;
    int m_pending = 0;
    bool m_shutdown = false;
};
