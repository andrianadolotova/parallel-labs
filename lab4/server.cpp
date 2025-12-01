#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <iostream>
#include <vector>
#include <thread>
#include <unordered_map>
#include <chrono>
#include <string>
#include <cstring>

using namespace std;
using namespace chrono;

struct CommandPacket {
    uint32_t length;
    char command[256];
};

struct MatrixUploadInfo {
    uint32_t matrix_size;
    uint32_t num_configs;
    uint32_t matrix_bytes;
};

struct ClientTask {
    vector<vector<int>> baseMatrix;
    vector<int> threadConfigs;
    vector<double> times;
    size_t currentIndex = 0;
    bool isProcessing = false;
};

unordered_map<int, ClientTask> g_clients;

int recvAll(int s, char* buffer, int length) {
    int received = 0;
    while (received < length) {
        int n = recv(s, buffer + received, length - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    return received;
}

int sendAll(int s, const char* data, int length) {
    int sent = 0;
    while (sent < length) {
        int n = send(s, data + sent, length - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

bool sendCommand(int s, const string& cmd) {
    if (cmd.size() > 256) return false;
    CommandPacket pkt{};
    pkt.length = htonl((uint32_t)cmd.size());
    memcpy(pkt.command, cmd.data(), cmd.size());
    return sendAll(s, (char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

bool receiveCommand(int s, string& outCmd) {
    CommandPacket pkt{};
    if (recvAll(s, (char*)&pkt, sizeof(pkt)) != sizeof(pkt)) return false;
    uint32_t len = ntohl(pkt.length);
    if (len > 256) return false;
    outCmd.assign(pkt.command, pkt.command + len);
    return true;
}

void transpose_part(vector<vector<int>>& a, int n, int start_i, int end_i) {
    for (int i = start_i; i < end_i; i++)
        for (int j = i + 1; j < n; j++)
            swap(a[i][j], a[j][i]);
}

void transpose_multi(vector<vector<int>>& a, int threads_num) {
    int n = (int)a.size();
    if (n == 0) return;
    if (threads_num <= 1) {
        transpose_part(a, n, 0, n);
        return;
    }
    vector<thread> threads;
    threads.reserve(threads_num);
    int base = n / threads_num;
    int extra = n % threads_num;
    int current = 0;
    for (int t = 0; t < threads_num; t++) {
        int count = base + (t < extra);
        int start_i = current;
        int end_i = current + count;
        current = end_i;
        if (start_i >= end_i) break;
        threads.emplace_back(transpose_part, ref(a), n, start_i, end_i);
    }
    for (auto& th : threads) th.join();
}

void processingThreadFunc(int cs) {
    ClientTask& ct = g_clients[cs];
    int n = (int)ct.baseMatrix.size();
    if (n == 0 || ct.threadConfigs.empty()) {
        sendCommand(cs, "ERROR: NO DATA");
        ct.isProcessing = false;
        return;
    }
    ct.times.clear();
    ct.currentIndex = 0;
    ct.isProcessing = true;
    for (size_t i = 0; i < ct.threadConfigs.size(); ++i) {
        ct.currentIndex = i;
        int threads_num = ct.threadConfigs[i];
        vector<vector<int>> work = ct.baseMatrix;
        auto start = high_resolution_clock::now();
        transpose_multi(work, threads_num);
        auto end = high_resolution_clock::now();
        double sec = duration<double>(end - start).count();
        ct.times.push_back(sec);
        string info = "INFO: threads=" + to_string(threads_num) + ", time=" + to_string(sec) + " s";
        sendCommand(cs, info);
    }
    ct.isProcessing = false;
    sendCommand(cs, "TRANSPOSE_COMPLETED");
}

void serveClient(int cs) {
    cout << "[server] client connected: " << cs << "\n";
    g_clients[cs] = ClientTask{};
    try {
        string cmd;
        while (receiveCommand(cs, cmd)) {
            if (cmd == "HELLO") {
                sendCommand(cs, "WELCOME");
            } else if (cmd == "UPLOAD_MATRIX") {
                MatrixUploadInfo info{};
                if (recvAll(cs, (char*)&info, sizeof(info)) != sizeof(info))
                    break;
                int n = (int)ntohl(info.matrix_size);
                int cfgCount = (int)ntohl(info.num_configs);
                int bytes = (int)ntohl(info.matrix_bytes);
                if (bytes != n * n * 4) break;
                ClientTask& ct = g_clients[cs];
                ct.baseMatrix.assign(n, vector<int>(n));
                ct.threadConfigs.resize(cfgCount);
                vector<int32_t> cfgNet(cfgCount);
                if (recvAll(cs, (char*)cfgNet.data(), cfgCount * 4) != cfgCount * 4)
                    break;
                for (int i = 0; i < cfgCount; i++) {
                    ct.threadConfigs[i] = ntohl(cfgNet[i]);
                    if (ct.threadConfigs[i] <= 0) ct.threadConfigs[i] = 1;
                }
                int total = n * n;
                vector<int32_t> flat(total);
                if (recvAll(cs, (char*)flat.data(), total * 4) != total * 4)
                    break;
                int idx = 0;
                for (int i = 0; i < n; i++)
                    for (int j = 0; j < n; j++)
                        ct.baseMatrix[i][j] = ntohl(flat[idx++]);
                sendCommand(cs, "MATRIX_RECEIVED");
            } else if (cmd == "START_TRANSPOSE") {
                ClientTask& ct = g_clients[cs];
                if (ct.baseMatrix.empty() || ct.threadConfigs.empty()) {
                    sendCommand(cs, "ERROR: NO DATA");
                    continue;
                }
                if (ct.isProcessing) {
                    sendCommand(cs, "ERROR: ALREADY");
                    continue;
                }
                sendCommand(cs, "TRANSPOSE_STARTED");
                thread(processingThreadFunc, cs).detach();
            } else if (cmd == "REQUEST_STATUS") {
                ClientTask& ct = g_clients[cs];
                if (!ct.isProcessing)
                    sendCommand(cs, "STATUS: FINISHED");
                else {
                    string s = "STATUS: " + to_string(ct.currentIndex + 1) +
                               "/" + to_string(ct.threadConfigs.size());
                    sendCommand(cs, s);
                }
            } else if (cmd == "REQUEST_RESULTS") {
                ClientTask& ct = g_clients[cs];
                if (ct.times.empty()) {
                    sendCommand(cs, "ERROR: NO RESULTS");
                    continue;
                }
                string report = "RESULT:\n";
                report += "Matrix " + to_string(ct.baseMatrix.size()) + "x" + to_string(ct.baseMatrix.size()) + "\n";
                for (size_t i = 0; i < ct.threadConfigs.size(); i++)
                    report += to_string(ct.threadConfigs[i]) + " threads: " +
                              to_string(ct.times[i]) + " s\n";
                sendCommand(cs, report);
            } else if (cmd == "QUIT") {
                sendCommand(cs, "BYE");
                break;
            } else {
                sendCommand(cs, "ERROR");
            }
        }
    } catch (...) {}
    close(cs);
    g_clients.erase(cs);
    cout << "[server] client disconnected: " << cs << "\n";
}

int main() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) return 1;
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(serverSocket, (sockaddr*)&addr, sizeof(addr)) < 0) return 1;
    if (listen(serverSocket, SOMAXCONN) < 0) return 1;

    cout << "[server] listening on port 12345\n";

    while (true) {
        int clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket < 0) continue;
        thread(serveClient, clientSocket).detach();
    }

    close(serverSocket);
    return 0;
}
