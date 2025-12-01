#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <chrono>
#include <random>
#include <atomic>
#include <memory>

using namespace std;
using namespace chrono;

mutex coutMutex;

class IThreadPool {
public:
    virtual ~IThreadPool() = default;
    virtual bool addTask(const function<void()>& task) = 0;
    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void shutdown(bool immediate) = 0;
    virtual void printMetrics() {}
};

class NoQueueThreadPool : public IThreadPool {
public:
    explicit NoQueueThreadPool(size_t workerCount = 6)
        : m_workerCount(workerCount),
          m_running(false),
          m_accepting(false),
          m_shutdownRequested(false) {}

    ~NoQueueThreadPool() override {
        shutdown(true);
    }

    bool addTask(const function<void()>& task) override {
        lock_guard<mutex> lock(m_assignMutex);

        if (!m_running || !m_accepting || m_shutdownRequested) {
            return false;
        }

        for (auto& worker : m_workers) {
            lock_guard<mutex> wlock(worker->mtx);
            if (!worker->hasTask && !worker->stopping) {
                worker->task = task;
                worker->hasTask = true;
                worker->cv.notify_one();
                return true;
            }
        }

        return false;
    }

    void start() override {
        lock_guard<mutex> lock(m_controlMutex);
        if (m_running) return;

        m_workers.clear();
        m_workers.reserve(m_workerCount);

        for (size_t i = 0; i < m_workerCount; ++i) {
            auto w = make_unique<Worker>();
            w->id = i;
            w->stopping = false;
            w->hasTask = false;
            m_workers.push_back(move(w));
        }

        m_running = true;
        m_accepting = true;
        m_shutdownRequested = false;

        for (auto& worker : m_workers) {
            worker->threadObj = thread(&NoQueueThreadPool::workerLoop, this, worker.get());
        }
    }

    void pause() override {
        m_accepting = false;
    }

    void resume() override {
        if (m_running && !m_shutdownRequested) {
            m_accepting = true;
        }
    }

    void shutdown(bool /*immediate*/) override {
        lock_guard<mutex> lock(m_controlMutex);
        if (!m_running) return;

        m_accepting = false;
        m_shutdownRequested = true;

        for (auto& worker : m_workers) {
            {
                lock_guard<mutex> wlock(worker->mtx);
                worker->stopping = true;
                worker->cv.notify_all();
            }
        }

        for (auto& worker : m_workers) {
            if (worker->threadObj.joinable()) {
                worker->threadObj.join();
            }
        }

        m_workers.clear();
        m_running = false;
    }

private:
    struct Worker {
        size_t id = 0;
        thread threadObj;
        mutex mtx;
        condition_variable cv;
        bool hasTask = false;
        bool stopping = false;
        function<void()> task;
    };

    size_t m_workerCount;
    vector<unique_ptr<Worker>> m_workers;

    atomic<bool> m_running;
    atomic<bool> m_accepting;
    atomic<bool> m_shutdownRequested;

    mutex m_assignMutex;
    mutex m_controlMutex;

    void workerLoop(Worker* worker) {
        while (true) {
            function<void()> localTask;

            {
                unique_lock<mutex> lock(worker->mtx);
                worker->cv.wait(lock, [&] {
                    return worker->hasTask || worker->stopping;
                });

                if (worker->stopping && !worker->hasTask) {
                    break;
                }

                if (worker->hasTask) {
                    localTask = move(worker->task);
                    worker->hasTask = false;
                }
            }

            if (localTask) {
                localTask();
            }

        }
    }
};

class ThreadPoolDecorator : public IThreadPool {
public:
    explicit ThreadPoolDecorator(unique_ptr<IThreadPool> inner)
        : m_inner(move(inner)) {}

    bool addTask(const function<void()>& task) override {
        return m_inner->addTask(task);
    }

    void start() override {
        m_inner->start();
    }

    void pause() override {
        m_inner->pause();
    }

    void resume() override {
        m_inner->resume();
    }

    void shutdown(bool immediate) override {
        m_inner->shutdown(immediate);
    }

    void printMetrics() override {
        m_inner->printMetrics();
    }

protected:
    unique_ptr<IThreadPool> m_inner;
};

class LoggingMetricsThreadPool : public ThreadPoolDecorator {
public:
    using TaskCallback = function<void(size_t)>;

    explicit LoggingMetricsThreadPool(unique_ptr<IThreadPool> inner)
        : ThreadPoolDecorator(move(inner)),
          m_nextTaskId(0),
          m_submitted(0),
          m_accepted(0),
          m_rejected(0),
          m_completed(0),
          m_totalExecMs(0)
    {
        onTaskAccepted = [&](size_t id) {
            lock_guard<mutex> lock(coutMutex);
            cout << "[LOG] Task " << id << " accepted\n";
        };
        onTaskRejected = [&](size_t id) {
            lock_guard<mutex> lock(coutMutex);
            cout << "[LOG] Task " << id << " REJECTED (all workers busy)\n";
        };
        onTaskStarted = [&](size_t id) {
            lock_guard<mutex> lock(coutMutex);
            cout << "[LOG] Task " << id << " started\n";
        };
        onTaskCompleted = [&](size_t id) {
            lock_guard<mutex> lock(coutMutex);
            cout << "[LOG] Task " << id << " completed\n";
        };
    }

    bool addTask(const function<void()>& task) override {
        size_t id = ++m_nextTaskId;
        ++m_submitted;

        auto wrappedTask = [this, task, id]() {
            if (onTaskStarted) onTaskStarted(id);

            auto start = steady_clock::now();
            task();
            auto end = steady_clock::now();

            auto dur = duration_cast<milliseconds>(end - start).count();
            m_totalExecMs.fetch_add(dur);
            ++m_completed;

            if (onTaskCompleted) onTaskCompleted(id);
        };

        bool ok = m_inner->addTask(wrappedTask);
        if (ok) {
            ++m_accepted;
            if (onTaskAccepted) onTaskAccepted(id);
        } else {
            ++m_rejected;
            if (onTaskRejected) onTaskRejected(id);
        }
        return ok;
    }

    void printMetrics() override {
        lock_guard<mutex> lock(coutMutex);
        cout << "\n===== METRICS =====\n";
        cout << "Tasks submitted:  " << m_submitted.load() << "\n";
        cout << "Tasks accepted:   " << m_accepted.load() << "\n";
        cout << "Tasks rejected:   " << m_rejected.load() << "\n";
        cout << "Tasks completed:  " << m_completed.load() << "\n";

        if (m_completed > 0) {
            auto avg = m_totalExecMs.load() / m_completed.load();
            cout << "Average execution time: " << avg << " ms\n";
        } else {
            cout << "No completed tasks, cannot compute average.\n";
        }
        cout << "===================\n";
    }

    TaskCallback onTaskAccepted;
    TaskCallback onTaskRejected;
    TaskCallback onTaskStarted;
    TaskCallback onTaskCompleted;

private:
    atomic<size_t> m_nextTaskId;

    atomic<size_t> m_submitted;
    atomic<size_t> m_accepted;
    atomic<size_t> m_rejected;
    atomic<size_t> m_completed;
    atomic<long long> m_totalExecMs; // сума часу виконання задач
};

void simulatedTaskBody() {
    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<int> dis(8, 12);
    int secondsToWork = dis(gen);

    {
        lock_guard<mutex> lock(coutMutex);
        cout << "    [TASK] working for " << secondsToWork << " seconds\n";
    }

    this_thread::sleep_for(chrono::seconds(secondsToWork));

    {
        lock_guard<mutex> lock(coutMutex);
        cout << "    [TASK] work done\n";
    }
}

int main() {
    unique_ptr<IThreadPool> core = make_unique<NoQueueThreadPool>(6);
    unique_ptr<IThreadPool> pool = make_unique<LoggingMetricsThreadPool>(move(core));

    pool->start();

    const auto testDuration = chrono::seconds(30);
    auto endTime = chrono::steady_clock::now() + testDuration;

    atomic<bool> stopProducers{false};
    atomic<int> producerIdCounter{0};

    auto producerFunc = [&](int producerId) {
        mt19937 gen(random_device{}());
        uniform_int_distribution<int> delayMs(1000, 4000); // 1–4 секунди між задачами

        while (chrono::steady_clock::now() < endTime && !stopProducers.load()) {
            {
                 lock_guard<mutex> lock(coutMutex);
                cout << "[PRODUCER " << producerId << "] trying to add task\n";
            }

            pool->addTask([]() {
                simulatedTaskBody();
            });

            this_thread::sleep_for(chrono::milliseconds(delayMs(gen)));
        }

        {
            lock_guard<mutex> lock(coutMutex);
            cout << "[PRODUCER " << producerId << "] stopped\n";
        }
    };

    int numProducers = 3;
    vector<thread> producers;
    for (int i = 0; i < numProducers; ++i) {
        producers.emplace_back(producerFunc, ++producerIdCounter);
    }

    this_thread::sleep_for(testDuration);
    stopProducers = true;

    for (auto& t : producers) {
        if (t.joinable()) t.join();
    }
    pool->shutdown(false);

    pool->printMetrics();

    return 0;
}
