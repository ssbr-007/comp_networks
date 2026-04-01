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
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(s, (sockaddr*)&a, sizeof(a)) < 0) {
        std::cerr << "Connection failed\n";
        close(s);
        return 1;
    }
    
    std::cout << "Connected\n";
    
    // Отправляем HELLO
    send_packet(s, 1, "user");
    
    // Получаем WELCOME
    Packet p;
    if (!recv_packet(s, p) || p.cmd != 2) {
        std::cerr << "Expected WELCOME\n";
        close(s);
        return 1;
    }
    std::cout << p.txt << "\n";
    
    std::string line;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, line);
        
        if (line == "/quit") {
            send_packet(s, 6);
            break;
        }
        else if (line == "/ping") {
            send_packet(s, 4);
            if (recv_packet(s, p) && p.cmd == 5) {
                std::cout << "PONG\n";
            }
        }
        else if (!line.empty()) {
            send_packet(s, 3, line.c_str());
        }
    }
    
    close(s);
    std::cout << "Disconnected\n";
    return 0;
}