#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <climits>
#include <iomanip>
#include <algorithm>
using namespace std;
inline bool isOdd(int x) {
    return x % 2 != 0;
}

struct Result {
    long long sum = 0;
    int minOdd = INT_MAX;
};

Result sequential(const int* a, size_t n) {
    Result r;
    for (size_t i = 0; i < n; ++i)
        if (isOdd(a[i])) {
            r.sum += a[i];
            r.minOdd = min(r.minOdd, a[i]);
        }
    return r;
}

Result parallel_mutex(const int* a, size_t n, int threadsCount) {
    if (threadsCount < 1) threadsCount = 1;

    mutex m;
    Result global;
    thread* threads = new thread[threadsCount];

    size_t chunk = (n + threadsCount - 1) / threadsCount;

    for (int t = 0; t < threadsCount; ++t) {
        size_t L = static_cast<size_t>(t) * chunk;
        size_t R = min(n, L + chunk);
        if (L >= n) {
            threads[t] = thread([](){});
            continue;
        }

        threads[t] = thread([&, L, R]() {
            Result local;
            for (size_t i = L; i < R; ++i)
                if (isOdd(a[i])) {
                    local.sum += a[i];
                    local.minOdd = min(local.minOdd, a[i]);
                }
            lock_guard<mutex> lock(m);
            global.sum += local.sum;
            global.minOdd = min(global.minOdd, local.minOdd);
        });
    }

    for (int t = 0; t < threadsCount; ++t)
        threads[t].join();

    delete[] threads;
    return global;
}

Result parallel_atomic(const int* a, size_t n, int threadsCount) {
    if (threadsCount < 1) threadsCount = 1;

    atomic<long long> sum(0);
    atomic<int> minOdd(INT_MAX);
    thread* threads = new thread[threadsCount];

    size_t chunk = (n + threadsCount - 1) / threadsCount;

    for (int t = 0; t < threadsCount; ++t) {
        size_t L = static_cast<size_t>(t) * chunk;
        size_t R = min(n, L + chunk);
        if (L >= n) {
            threads[t] = thread([](){});
            continue;
        }

        threads[t] = thread([&, L, R]() {
            long long localSum = 0;
            int localMin = INT_MAX;

            for (size_t i = L; i < R; ++i)
                if (isOdd(a[i])) {
                    localSum += a[i];
                    localMin = min(localMin, a[i]);
                }

            sum.fetch_add(localSum, memory_order_relaxed);

            int current = minOdd.load(memory_order_relaxed);
            while (localMin < current &&
                   !minOdd.compare_exchange_weak(current, localMin, memory_order_relaxed)) {
            }
        });
    }

    for (int t = 0; t < threadsCount; ++t)
        threads[t].join();

    delete[] threads;

    Result r;
    r.sum = sum.load(memory_order_relaxed);
    r.minOdd = minOdd.load(memory_order_relaxed);
    return r;
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    size_t sizes[] = {100'000, 1'000'000, 100'000'000};
    int threadsList[] = {1, 2, 4, 8, 16, 32, 64};

    mt19937 rng(42);
    uniform_int_distribution<int> dist(-1'000'000, 1'000'000);

    cout << left
         << setw(12) << "Size"
         << setw(10) << "Threads"
         << setw(10) << "Mode"
         << setw(14) << "Time(s)"
         << setw(20) << "Sum"
         << setw(10) << "MinOdd"
         << "\n";
    cout << string(76, '-') << "\n";

    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        size_t n = sizes[s];
        int* a = new int[n];

        for (size_t i = 0; i < n; ++i)
            a[i] = dist(rng);

        auto t0 = chrono::high_resolution_clock::now();
        Result r1 = sequential(a, n);
        auto t1 = chrono::high_resolution_clock::now();
        double timeSeq = chrono::duration<double>(t1 - t0).count();

        cout << left
             << setw(12) << n
             << setw(10) << "-"
             << setw(10) << "Seq"
             << setw(14) << fixed << setprecision(6) << timeSeq
             << setw(20) << r1.sum
             << setw(10) << r1.minOdd
             << "\n";

        for (int T : threadsList) {
            t0 = chrono::high_resolution_clock::now();
            Result r2 = parallel_mutex(a, n, T);
            t1 = chrono::high_resolution_clock::now();
            double timeMutex = chrono::duration<double>(t1 - t0).count();

            cout << left
                 << setw(12) << n
                 << setw(10) << T
                 << setw(10) << "Mutex"
                 << setw(14) << fixed << setprecision(6) << timeMutex
                 << setw(20) << r2.sum
                 << setw(10) << r2.minOdd
                 << "\n";
        }

        for (int T : threadsList) {
            t0 = chrono::high_resolution_clock::now();
            Result r3 = parallel_atomic(a, n, T);
            t1 = chrono::high_resolution_clock::now();
            double timeAtomic = chrono::duration<double>(t1 - t0).count();

            cout << left
                 << setw(12) << n
                 << setw(10) << T
                 << setw(10) << "Atomic"
                 << setw(14) << fixed << setprecision(6) << timeAtomic
                 << setw(20) << r3.sum
                 << setw(10) << r3.minOdd
                 << "\n";
        }

        cout << string(76, '-') << "\n";

        delete[] a;
    }

    return 0;
}
