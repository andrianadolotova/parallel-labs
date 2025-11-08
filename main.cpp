#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>

using namespace std;

bool is_transposed_ok(int** orig, int** trans, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (orig[i][j] != trans[j][i]) {
                return false;
            }
        }
    }
    return true;
}

void transpose_part(int** a, int n, int start_i, int end_i) {
    for (int i = start_i; i < end_i; i++) {
        for (int j = i + 1; j < n; j++) {
            std::swap(a[i][j], a[j][i]);
        }
    }
}
void transpose_multi(int** a, int n, int threads_num) {
    vector<thread> threads;
    threads.reserve(threads_num);
    int base = n / threads_num;
    int extra = n % threads_num;
    int current = 0;
    for (int t = 0; t < threads_num; t++) {
        int count = base + (t < extra ? 1 : 0);
        int start_i = current;
        int end_i = current + count;
        current = end_i;
        if (start_i >= end_i)
            break;
        threads.emplace_back(transpose_part, a, n, start_i, end_i);
    }
    for (auto& th : threads) {
        th.join();
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    int matrix_sizes[] = {500, 1000, 2000, 5000, 10000, 20000};
    int matrix_sizes_count = sizeof(matrix_sizes) / sizeof(matrix_sizes[0]);

    int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    int thread_counts_count = sizeof(thread_counts) / sizeof(thread_counts[0]);

    cout << fixed << setprecision(5);
    cout << "-----------------------------------------------------------\n";
    cout << " MatrixSize | Threads |   Time (s) |   Check\n";
    cout << "-----------------------------------------------------------\n";

    for (int idx = 0; idx < matrix_sizes_count; idx++) {
        int n = matrix_sizes[idx];

        int** a = new int*[n];
        int** orig = new int*[n];
        for (int i = 0; i < n; i++) {
            a[i] = new int[n];
            orig[i] = new int[n];
        }

        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                int val = rand() % 100;
                a[i][j] = val;
                orig[i][j] = val;
            }
        }

        for (int t = 0; t < thread_counts_count; t++) {
            int threads_num = thread_counts[t];

            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    a[i][j] = orig[i][j];
                }
            }

            auto start = chrono::high_resolution_clock::now();
            transpose_multi(a, n, threads_num);
            auto end = chrono::high_resolution_clock::now();

            chrono::duration<double> dur = end - start;

            bool ok = is_transposed_ok(orig, a, n);

            cout << setw(11) << n << " | "
                 << setw(7) << threads_num << " | "
                 << setw(10) << dur.count() << " | "
                 << (ok ? "   OK" : " ERROR") << "\n";
        }

        cout << "-----------------------------------------------------------\n";

        for (int i = 0; i < n; i++) {
            delete[] a[i];
            delete[] orig[i];
        }
        delete[] a;
        delete[] orig;
    }

    return 0;
}
