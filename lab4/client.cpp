#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <signal.h>

#define MAX_PAYLOAD 1024
#define RECONNECT_DELAY 2

// Типы сообщений (расширенные)
enum MessageType {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10
};

struct Packet {
    uint32_t sz;
    uint8_t cmd;
    char txt[MAX_PAYLOAD];
};

volatile bool connected = false;
volatile bool running = true;
volatile bool authenticated = false;
int sockfd = -1;
std::string my_nickname;
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
            if (p.cmd == MSG_TEXT) {
                // Обычное сообщение
                std::cout << "\r\033[K" << p.txt << "\n> " << std::flush;
            }
            else if (p.cmd == MSG_PRIVATE) {
                // Личное сообщение
                std::cout << "\r\033[K\033[33m" << p.txt << "\033[0m\n> " << std::flush;
            }
            else if (p.cmd == MSG_PONG) {
                std::cout << "\r\033[K\033[36mPONG from server\033[0m\n> " << std::flush;
            }
            else if (p.cmd == MSG_WELCOME) {
                std::cout << "\r\033[K\033[32m" << p.txt << "\033[0m\n> " << std::flush;
                authenticated = true;
            }
            else if (p.cmd == MSG_SERVER_INFO) {
                // Системное сообщение
                std::cout << "\r\033[K\033[35m[SERVER]: " << p.txt << "\033[0m\n> " << std::flush;
            }
            else if (p.cmd == MSG_ERROR) {
                // Сообщение об ошибке
                std::cout << "\r\033[K\033[31m[ERROR]: " << p.txt << "\033[0m\n> " << std::flush;
                if (strstr(p.txt, "Authentication") != nullptr) {
                    authenticated = false;
                    connected = false;
                }
            }
        }
        else {
            std::cout << "\r\033[K\033[31mConnection lost. Reconnecting...\033[0m\n";
            connected = false;
            authenticated = false;
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
    
    // Запрашиваем никнейм
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, my_nickname);
    if (my_nickname.empty()) my_nickname = "user";
    
    // Отправляем MSG_AUTH для аутентификации
    send_packet(fd, MSG_AUTH, my_nickname.c_str());
    
    // Ждем ответа (WELCOME или ERROR)
    Packet p;
    if (!recv_packet(fd, p)) {
        close(fd);
        return false;
    }
    
    if (p.cmd == MSG_ERROR) {
        std::cout << "\033[31m" << p.txt << "\033[0m\n";
        close(fd);
        return false;
    }
    
    if (p.cmd != MSG_WELCOME) {
        std::cout << "\033[31mUnexpected response from server\033[0m\n";
        close(fd);
        return false;
    }
    
    std::cout << "\033[32m" << p.txt << "\033[0m\n";
    connected = true;
    authenticated = true;
    return true;
}

void reconnect() {
    while (!connected && running) {
        std::cout << "\033[33mAttempting to reconnect...\033[0m\n";
        sleep(RECONNECT_DELAY);
        
        if (connect_to_server()) {
            std::cout << "\033[32mReconnected successfully!\033[0m\n";
            
            // Запускаем поток приема
            pthread_t recv_thread;
            pthread_create(&recv_thread, NULL, receive_thread, NULL);
            pthread_detach(recv_thread);
            break;
        }
    }
}

void print_help() {
    std::cout << "\n\033[36m=== Commands ===\033[0m\n";
    std::cout << "  /w <nick> <message>  - Send private message\n";
    std::cout << "  /ping                - Send ping to server\n";
    std::cout << "  /quit                - Disconnect from server\n";
    std::cout << "  /help                - Show this help\n";
    std::cout << "================\n\n";
}

int main() {
    std::cout << "\033[36m=== TCP Chat Client ===\033[0m\n";
    print_help();
    
    // Игнорируем SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    // Подключаемся к серверу
    if (!connect_to_server()) {
        std::cout << "\033[31mFailed to connect. Will retry...\033[0m\n";
        reconnect();
    }
    
    if (!connected) {
        std::cout << "\033[31mUnable to connect to server. Exiting.\033[0m\n";
        return 1;
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
            std::cout << "\033[33mNot connected. Reconnecting...\033[0m\n";
            reconnect();
            if (!connected) continue;
        }
        
        if (input == "/quit") {
            send_packet(sockfd, MSG_BYE);
            running = false;
            break;
        }
        else if (input == "/ping") {
            send_packet(sockfd, MSG_PING);
        }
        else if (input == "/help") {
            print_help();
        }
        else if (input.substr(0, 3) == "/w ") {
            // Формат: /w nickname message
            std::string rest = input.substr(3);
            size_t space_pos = rest.find(' ');
            if (space_pos != std::string::npos) {
                std::string target = rest.substr(0, space_pos);
                std::string message = rest.substr(space_pos + 1);
                
                // Формат для MSG_PRIVATE: target_nick:message
                std::string payload = target + ":" + message;
                send_packet(sockfd, MSG_PRIVATE, payload.c_str());
            } else {
                std::cout << "\033[31mUsage: /w <nickname> <message>\033[0m\n";
            }
        }
        else if (!input.empty() && input[0] != '/') {
            send_packet(sockfd, MSG_TEXT, input.c_str());
        }
    }
    
    // Закрываем сокет
    pthread_mutex_lock(&socket_mutex);
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    pthread_mutex_unlock(&socket_mutex);
    
    std::cout << "\033[36mDisconnected\033[0m\n";
    return 0;
}