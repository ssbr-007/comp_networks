#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <string>
#include <map>

#define MAX_PAYLOAD 1024
#define THREAD_POOL_SIZE 10
#define MAX_CLIENTS 100

// Типы сообщений (расширенные)
enum MessageType {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,        // аутентификация
    MSG_PRIVATE = 8,     // личное сообщение
    MSG_ERROR = 9,       // ошибка
    MSG_SERVER_INFO = 10 // системные сообщения
};

// Структура сообщения
struct Packet {
    uint32_t sz;
    uint8_t cmd;
    char txt[MAX_PAYLOAD];
};

// Структура клиента (расширенная)
struct Client {
    int fd;
    std::string nickname;
    std::string address;
    int port;
    bool authenticated;
};

// Глобальные данные
std::vector<Client> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
std::queue<int> connection_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
bool server_running = true;

// Функции для работы с пакетами
void send_packet(int fd, uint8_t cmd, const char* msg = "") {
    // OSI Layer 7 - Application (подготовка ответа)
    std::cout << "[Layer 7 - Application] prepare response (cmd=" << (int)cmd << ")\n";
    
    Packet p;
    p.cmd = cmd;
    strncpy(p.txt, msg, MAX_PAYLOAD - 1);
    p.txt[MAX_PAYLOAD - 1] = '\0';
    p.sz = htonl(1 + strlen(p.txt) + 1);
    
    // OSI Layer 6 - Presentation (сериализация)
    std::cout << "[Layer 6 - Presentation] serialize Message (size=" << ntohl(p.sz) << ")\n";
    
    // OSI Layer 4 - Transport (отправка)
    std::cout << "[Layer 4 - Transport] send() to fd=" << fd << "\n";
    
    write(fd, &p.sz, 4);
    write(fd, &p.cmd, 1 + strlen(p.txt) + 1);
}

bool recv_packet(int fd, Packet& p) {
    // OSI Layer 4 - Transport (приём)
    std::cout << "[Layer 4 - Transport] recv() from fd=" << fd << "\n";
    
    if (read(fd, &p.sz, 4) != 4) return false;
    p.sz = ntohl(p.sz);
    if (p.sz > MAX_PAYLOAD + 1) return false;
    if (read(fd, &p.cmd, p.sz) != p.sz) return false;
    p.txt[p.sz - 1] = '\0';
    
    // OSI Layer 6 - Presentation (десериализация)
    std::cout << "[Layer 6 - Presentation] deserialize Message (cmd=" << (int)p.cmd 
              << ", size=" << p.sz << ")\n";
    
    return true;
}

// Поиск клиента по нику
int find_client_by_nickname(const std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.nickname == nickname && client.authenticated) {
            int fd = client.fd;
            pthread_mutex_unlock(&clients_mutex);
            return fd;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

// Проверка уникальности ника
bool is_nickname_unique(const std::string& nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.nickname == nickname) {
            pthread_mutex_unlock(&clients_mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return true;
}

// Широковещательная рассылка
void broadcast(uint8_t cmd, const char* msg, int sender_fd = -1) {
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        if (client.authenticated && client.fd != sender_fd) {
            send_packet(client.fd, cmd, msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Отправка личного сообщения
bool send_private_message(int sender_fd, const std::string& sender_nick, 
                          const std::string& target_nick, const std::string& message) {
    int target_fd = find_client_by_nickname(target_nick);
    if (target_fd == -1) {
        return false;
    }
    
    char formatted_msg[MAX_PAYLOAD];
    snprintf(formatted_msg, sizeof(formatted_msg), "[PRIVATE][%s]: %s", 
             sender_nick.c_str(), message.c_str());
    send_packet(target_fd, MSG_PRIVATE, formatted_msg);
    return true;
}

// Добавление клиента в список
void add_client(int fd, const std::string& nickname, const std::string& address, int port) {
    pthread_mutex_lock(&clients_mutex);
    Client client = {fd, nickname, address, port, true};
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "User [" << nickname << "] connected from " << address << ":" << port << "\n";
    
    // OSI Layer 7 - системное сообщение
    char msg[MAX_PAYLOAD];
    snprintf(msg, sizeof(msg), "User %s connected", nickname.c_str());
    broadcast(MSG_SERVER_INFO, msg, fd);
}

// Удаление клиента из списка
void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    std::string nickname;
    std::string address;
    int port;
    
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->fd == fd) {
            nickname = it->nickname;
            address = it->address;
            port = it->port;
            clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (!nickname.empty()) {
        std::cout << "User [" << nickname << "] disconnected from " << address << ":" << port << "\n";
        
        // OSI Layer 7 - системное сообщение
        char msg[MAX_PAYLOAD];
        snprintf(msg, sizeof(msg), "User %s disconnected", nickname.c_str());
        broadcast(MSG_SERVER_INFO, msg);
    }
}

// Обработка аутентификации
bool handle_authentication(int client_fd, const std::string& nickname) {
    // OSI Layer 5 - Session (управление сессией)
    std::cout << "[Layer 5 - Session] authentication attempt for nickname: " << nickname << "\n";
    
    // Проверка: ник не пустой
    if (nickname.empty()) {
        std::cout << "[Layer 5 - Session] authentication failed: empty nickname\n";
        send_packet(client_fd, MSG_ERROR, "Error: Nickname cannot be empty");
        return false;
    }
    
    // Проверка: ник уникален
    if (!is_nickname_unique(nickname)) {
        std::cout << "[Layer 5 - Session] authentication failed: nickname '" << nickname << "' already exists\n";
        send_packet(client_fd, MSG_ERROR, "Error: Nickname already taken");
        return false;
    }
    
    // OSI Layer 5 - Session (аутентификация успешна)
    std::cout << "[Layer 5 - Session] authentication success for nickname: " << nickname << "\n";
    return true;
}

// Обработка клиента в отдельном потоке
void* handle_client(void* arg) {
    int client_fd = *(int*)arg;
    delete (int*)arg;
    
    Packet p;
    
    // Получаем адрес клиента
    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_fd, (sockaddr*)&addr, &addr_len);
    std::string client_address = inet_ntoa(addr.sin_addr);
    int client_port = ntohs(addr.sin_port);
    
    // ============ ЭТАП 1: Ожидание MSG_AUTH ============
    std::cout << "\n=== New connection from " << client_address << ":" << client_port << " ===\n";
    
    if (!recv_packet(client_fd, p)) {
        std::cerr << "Failed to receive auth packet\n";
        close(client_fd);
        return NULL;
    }
    
    // OSI Layer 7 - Application (обработка MSG_AUTH)
    std::cout << "[Layer 7 - Application] handle MSG_AUTH\n";
    
    if (p.cmd != MSG_AUTH) {
        std::cout << "[Layer 5 - Session] authentication failed: expected MSG_AUTH, got " << (int)p.cmd << "\n";
        send_packet(client_fd, MSG_ERROR, "Error: Authentication required");
        close(client_fd);
        return NULL;
    }
    
    std::string nickname(p.txt);
    
    // Аутентификация
    if (!handle_authentication(client_fd, nickname)) {
        close(client_fd);
        return NULL;
    }
    
    // Отправляем подтверждение аутентификации
    char welcome_msg[MAX_PAYLOAD];
    snprintf(welcome_msg, sizeof(welcome_msg), "Welcome, %s! You are now connected.", nickname.c_str());
    send_packet(client_fd, MSG_WELCOME, welcome_msg);
    
    // Добавляем клиента в список
    add_client(client_fd, nickname, client_address, client_port);
    
    // ============ ЭТАП 2: Основной цикл обработки ============
    while (true) {
        if (!recv_packet(client_fd, p)) {
            // Клиент отключился
            break;
        }
        
        // OSI Layer 7 - Application (обработка команд)
        if (p.cmd == MSG_TEXT) {
            std::cout << "[Layer 7 - Application] handle MSG_TEXT from " << nickname << "\n";
            
            // Формируем сообщение для рассылки
            char broadcast_msg[MAX_PAYLOAD];
            snprintf(broadcast_msg, sizeof(broadcast_msg), "[%s]: %s", 
                     nickname.c_str(), p.txt);
            
            // OSI Layer 7 - широковещательная рассылка
            broadcast(MSG_TEXT, broadcast_msg, client_fd);
        }
        else if (p.cmd == MSG_PRIVATE) {
            std::cout << "[Layer 7 - Application] handle MSG_PRIVATE from " << nickname << "\n";
            
            // Формат: target_nick:message
            std::string payload(p.txt);
            size_t colon_pos = payload.find(':');
            
            if (colon_pos != std::string::npos) {
                std::string target_nick = payload.substr(0, colon_pos);
                std::string message = payload.substr(colon_pos + 1);
                
                if (send_private_message(client_fd, nickname, target_nick, message)) {
                    // Подтверждение отправителю
                    char confirm_msg[MAX_PAYLOAD];
                    snprintf(confirm_msg, sizeof(confirm_msg), "[PRIVATE] to %s: %s", 
                             target_nick.c_str(), message.c_str());
                    send_packet(client_fd, MSG_SERVER_INFO, confirm_msg);
                } else {
                    send_packet(client_fd, MSG_ERROR, "Error: User not found");
                }
            } else {
                send_packet(client_fd, MSG_ERROR, "Error: Invalid private message format");
            }
        }
        else if (p.cmd == MSG_PING) {
            std::cout << "[Layer 7 - Application] handle MSG_PING from " << nickname << "\n";
            send_packet(client_fd, MSG_PONG);
        }
        else if (p.cmd == MSG_BYE) {
            std::cout << "[Layer 7 - Application] handle MSG_BYE from " << nickname << "\n";
            break;
        }
        else {
            std::cout << "[Layer 7 - Application] unknown command: " << (int)p.cmd << "\n";
        }
    }
    
    // Удаляем клиента из списка и закрываем соединение
    remove_client(client_fd);
    close(client_fd);
    
    return NULL;
}

// Рабочий поток из пула
void* worker_thread(void* arg) {
    while (server_running) {
        pthread_mutex_lock(&queue_mutex);
        while (connection_queue.empty() && server_running) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        int client_fd = connection_queue.front();
        connection_queue.pop();
        pthread_mutex_unlock(&queue_mutex);
        
        // Создаем копию дескриптора для потока
        int* fd_copy = new int(client_fd);
        
        // Создаем поток для обработки клиента
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, fd_copy);
        pthread_detach(thread);
    }
    return NULL;
}

int main() {
    // Создаем сокет
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }
    
    // Настройка адреса
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    // Разрешаем переиспользование адреса
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Привязываем сокет
    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return 1;
    }
    
    // Начинаем слушать
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }
    
    std::cout << "========================================\n";
    std::cout << "Server ready on port 8080\n";
    std::cout << "Thread pool size: " << THREAD_POOL_SIZE << "\n";
    std::cout << "========================================\n\n";
    
    // Создаем пул потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }
    
    // Основной цикл принятия подключений
    while (server_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (server_running) {
                std::cerr << "Accept failed\n";
            }
            continue;
        }
        
        // Добавляем клиента в очередь
        pthread_mutex_lock(&queue_mutex);
        connection_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
        
        std::cout << "New connection accepted, queued for processing\n";
    }
    
    // Очистка
    close(server_fd);
    return 0;
}