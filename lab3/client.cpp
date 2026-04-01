#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <signal.h>

#define MAX_PAYLOAD 1024
#define RECONNECT_DELAY 2  // секунды

struct Packet {
    uint32_t sz;
    uint8_t cmd;
    char txt[MAX_PAYLOAD];
};

volatile bool connected = false;
volatile bool running = true;
int sockfd = -1;
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

void send_packet(int fd, uint8_t cmd, const char* msg = "") {
    Packet p;
    p.cmd = cmd;
    strncpy(p.txt, msg, MAX_PAYLOAD - 1);
    p.txt[MAX_PAYLOAD - 1] = '\0';
    p.sz = htonl(1 + strlen(p.txt) + 1);
    
    pthread_mutex_lock(&socket_mutex);
    write(fd, &p.sz, 4);
    write(fd, &p.cmd, 1 + strlen(p.txt) + 1);
    pthread_mutex_unlock(&socket_mutex);
}

bool recv_packet(int fd, Packet& p) {
    if (read(fd, &p.sz, 4) != 4) return false;
    p.sz = ntohl(p.sz);
    if (p.sz > MAX_PAYLOAD + 1) return false;
    if (read(fd, &p.cmd, p.sz) != p.sz) return false;
    p.txt[p.sz - 1] = '\0';
    return true;
}

// Поток для приема сообщений
void* receive_thread(void* arg) {
    Packet p;
    
    while (running && connected) {
        pthread_mutex_lock(&socket_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&socket_mutex);
        
        if (fd < 0) break;
        
        if (recv_packet(fd, p)) {
            if (p.cmd == 3) {  // MSG_TEXT
                std::cout << "\r\033[K" << p.txt << "\n> " << std::flush;
            }
            else if (p.cmd == 5) {  // MSG_PONG
                std::cout << "\r\033[KPONG\n> " << std::flush;
            }
            else if (p.cmd == 2) {  // MSG_WELCOME
                std::cout << "\r\033[K" << p.txt << "\n> " << std::flush;
            }
        }
        else {
            // Соединение разорвано
            std::cout << "\r\033[KConnection lost. Reconnecting...\n";
            connected = false;
            break;
        }
    }
    return NULL;
}

bool connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    
    pthread_mutex_lock(&socket_mutex);
    sockfd = fd;
    pthread_mutex_unlock(&socket_mutex);
    
    // Отправляем HELLO
    std::string nickname;
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "user";
    
    send_packet(fd, 1, nickname.c_str());
    
    // Получаем WELCOME
    Packet p;
    if (!recv_packet(fd, p) || p.cmd != 2) {
        close(fd);
        return false;
    }
    
    std::cout << p.txt << "\n";
    
    connected = true;
    return true;
}

void reconnect() {
    while (!connected && running) {
        std::cout << "Attempting to reconnect...\n";
        sleep(RECONNECT_DELAY);
        
        if (connect_to_server()) {
            std::cout << "Reconnected successfully!\n";
            
            // Запускаем поток приема
            pthread_t recv_thread;
            pthread_create(&recv_thread, NULL, receive_thread, NULL);
            pthread_detach(recv_thread);
            break;
        }
    }
}

int main() {
    std::cout << "=== TCP Chat Client ===\n";
    
    // Игнорируем SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    // Подключаемся к серверу
    if (!connect_to_server()) {
        std::cout << "Failed to connect. Will retry...\n";
        reconnect();
    }
    
    // Запускаем поток приема
    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, receive_thread, NULL);
    pthread_detach(recv_thread);
    
    // Основной цикл ввода
    std::string input;
    while (running) {
        std::cout << "> ";
        std::getline(std::cin, input);
        
        if (!connected) {
            std::cout << "Not connected. Reconnecting...\n";
            reconnect();
            if (!connected) continue;
        }
        
        if (input == "/quit") {
            send_packet(sockfd, 6);
            running = false;
            break;
        }
        else if (input == "/ping") {
            send_packet(sockfd, 4);
        }
        else if (!input.empty()) {
            send_packet(sockfd, 3, input.c_str());
        }
    }
    
    // Закрываем сокет
    pthread_mutex_lock(&socket_mutex);
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    pthread_mutex_unlock(&socket_mutex);
    
    std::cout << "Disconnected\n";
    return 0;
}