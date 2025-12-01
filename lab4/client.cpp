#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <limits>

using namespace std;

struct CommandPacket {
    uint32_t length;
    char command[256];
};

struct MatrixUploadInfo {
    uint32_t matrix_size;
    uint32_t num_configs;
    uint32_t matrix_bytes;
};

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
    pkt.length = htonl(static_cast<uint32_t>(cmd.size()));
    memcpy(pkt.command, cmd.data(), cmd.size());
    return sendAll(s, reinterpret_cast<char*>(&pkt), sizeof(pkt)) == sizeof(pkt);
}

bool receiveCommand(int s, string& outCmd) {
    CommandPacket pkt{};
    if (recvAll(s, reinterpret_cast<char*>(&pkt), sizeof(pkt)) != sizeof(pkt)) return false;
    uint32_t len = ntohl(pkt.length);
    if (len > 256) return false;
    outCmd.assign(pkt.command, pkt.command + len);
    return true;
}

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(12345);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sockfd, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }
    cout << "[client] Connected\n";
    sendCommand(sockfd, "HELLO");
    string reply;
    if (receiveCommand(sockfd, reply))
        cout << "[server] " << reply << "\n";

    int n;
    cout << "Enter matrix size n: ";
    cin >> n;
    cout << "Enter thread configs (e.g. 1 2 4 8). Empty = {1,2,4,8,16}: ";
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    string line;
    getline(cin, line);
    vector<int> cfg;
    {
        stringstream ss(line);
        int t;
        while (ss >> t) if (t > 0) cfg.push_back(t);
    }
    if (cfg.empty()) cfg = {1, 2, 4, 8, 16};

    vector<vector<int>> matrix(n, vector<int>(n));
    for (auto& row : matrix)
        for (int& v : row)
            v = rand() % 100;

    sendCommand(sockfd, "UPLOAD_MATRIX");
    MatrixUploadInfo hdr{};
    hdr.matrix_size = htonl(n);
    hdr.num_configs = htonl(static_cast<uint32_t>(cfg.size()));
    hdr.matrix_bytes = htonl(n * n * static_cast<int>(sizeof(int)));
    sendAll(sockfd, reinterpret_cast<char*>(&hdr), sizeof(hdr));

    vector<int32_t> cfgNet(cfg.size());
    for (size_t i = 0; i < cfg.size(); ++i)
        cfgNet[i] = htonl(cfg[i]);
    sendAll(sockfd, reinterpret_cast<char*>(cfgNet.data()), cfgNet.size() * sizeof(int32_t));

    vector<int32_t> flat;
    flat.reserve(n * n);
    for (auto& row : matrix)
        for (int v : row)
            flat.push_back(htonl(v));
    sendAll(sockfd, reinterpret_cast<char*>(flat.data()), flat.size() * sizeof(int32_t));

    if (receiveCommand(sockfd, reply))
        cout << "[server] " << reply << "\n";

    sendCommand(sockfd, "START_TRANSPOSE");

    atomic<bool> done{false};
    atomic<bool> resultReady{false};
    string finalResult;

    thread listener([&] {
        string msg;
        while (receiveCommand(sockfd, msg)) {
            if (msg.rfind("INFO:", 0) == 0) {
                cout << "[server] " << msg << "\n";
            } else if (msg == "TRANSPOSE_STARTED") {
                cout << "[server] " << msg << "\n";
            } else if (msg == "TRANSPOSE_COMPLETED") {
                cout << "[server] " << msg << "\n";
                done = true;
            } else if (msg.rfind("RESULT:", 0) == 0) {
                finalResult = msg;
                resultReady = true;
                break;
            } else {
                cout << "[server] " << msg << "\n";
            }
        }
    });

    cout << "\nPress Enter to request STATUS, until finished\n";
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    while (!done) {
        cin.get();
        sendCommand(sockfd, "REQUEST_STATUS");
    }

    sendCommand(sockfd, "REQUEST_RESULTS");
    while (!resultReady)
        this_thread::sleep_for(chrono::milliseconds(10));

    listener.join();

    cout << "\n===== RESULT =====\n";
    cout << finalResult << "\n";

    sendCommand(sockfd, "QUIT");
    close(sockfd);
    return 0;
}
