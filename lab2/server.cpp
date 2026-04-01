#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>

struct Packet {
    uint32_t sz;
    uint8_t cmd;
    char txt[1024];
};

void send_packet(int fd, uint8_t cmd, const char* msg = "") {
    Packet p;
    p.cmd = cmd;
    strcpy(p.txt, msg);
    p.sz = htonl(1 + strlen(msg) + 1);
    write(fd, &p.sz, 4);
    write(fd, &p.cmd, 1 + strlen(msg) + 1);
}

bool recv_packet(int fd, Packet& p) {
    if (read(fd, &p.sz, 4) != 4) return false;
    p.sz = ntohl(p.sz);
    if (read(fd, &p.cmd, p.sz) != p.sz) return false;
    return true;
}

int main() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }
    
    sockaddr_in a;
    a.sin_family = AF_INET;
    a.sin_port = htons(8080);
    a.sin_addr.s_addr = INADDR_ANY;
    
    // Исправленная строка - объявляем переменную opt
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(s, (sockaddr*)&a, sizeof(a)) < 0) {
        std::cerr << "Bind failed\n";
        close(s);
        return 1;
    }
    
    if (listen(s, 1) < 0) {
        std::cerr << "Listen failed\n";
        close(s);
        return 1;
    }
    
    std::cout << "Server ready on port 8080\n";
    
    sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(s, (sockaddr*)&client_addr, &len);
    
    if (client_fd < 0) {
        std::cerr << "Accept failed\n";
        close(s);
        return 1;
    }
    
    std::cout << "Client connected\n";
    
    Packet p;
    
    // Получаем HELLO
    if (!recv_packet(client_fd, p) || p.cmd != 1) {
        std::cerr << "Expected HELLO\n";
        close(client_fd);
        close(s);
        return 1;
    }
    
    std::cout << "[" << inet_ntoa(client_addr.sin_addr) << ":" 
              << ntohs(client_addr.sin_port) << "]: " << p.txt << "\n";
    
    // Отправляем WELCOME
    char welcome[100];
    sprintf(welcome, "Welcome %s:%d", inet_ntoa(client_addr.sin_addr), 
            ntohs(client_addr.sin_port));
    send_packet(client_fd, 2, welcome);
    
    // Цикл обработки сообщений
    while (recv_packet(client_fd, p)) {
        if (p.cmd == 3) {  // MSG_TEXT
            std::cout << "[" << inet_ntoa(client_addr.sin_addr) << ":" 
                      << ntohs(client_addr.sin_port) << "]: " << p.txt << "\n";
        }
        else if (p.cmd == 4) {  // MSG_PING
            std::cout << "PING received\n";
            send_packet(client_fd, 5);  // MSG_PONG
        }
        else if (p.cmd == 6) {  // MSG_BYE
            std::cout << "Client disconnected\n";
            break;
        }
        else {
            std::cout << "Unknown message type: " << (int)p.cmd << "\n";
        }
    }
    
    close(client_fd);
    close(s);
    return 0;
}