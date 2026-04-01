#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <string>

#define MAX_PAYLOAD 1024
#define THREAD_POOL_SIZE 10
#define MAX_CLIENTS 100

// Структура сообщения
struct Packet {
    uint32_t sz;
    uint8_t cmd;
    char txt[MAX_PAYLOAD];
};

// Структура клиента
struct Client {
    int fd;
    std::string name;
    std::string address;
    int port;
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
    Packet p;
    p.cmd = cmd;
    strncpy(p.txt, msg, MAX_PAYLOAD - 1);
    p.txt[MAX_PAYLOAD - 1] = '\0';
    p.sz = htonl(1 + strlen(p.txt) + 1);
    write(fd, &p.sz, 4);
    write(fd, &p.cmd, 1 + strlen(p.txt) + 1);
}

bool recv_packet(int fd, Packet& p) {
    if (read(fd, &p.sz, 4) != 4) return false;
    p.sz = ntohl(p.sz);
    if (p.sz > MAX_PAYLOAD + 1) return false;
    if (read(fd, &p.cmd, p.sz) != p.sz) return false;
    p.txt[p.sz - 1] = '\0';
    return true;
}

// Широковещательная рассылка
void broadcast(uint8_t cmd, const char* msg, int sender_fd = -1) {
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        if (client.fd != sender_fd) {
            send_packet(client.fd, cmd, msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Добавление клиента в список
void add_client(int fd, const std::string& name, const std::string& address, int port) {
    pthread_mutex_lock(&clients_mutex);
    Client client = {fd, name, address, port};
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "Client connected: " << address << ":" << port 
              << " (" << name << ")\n";
    
    // Уведомляем всех о новом клиенте
    char msg[MAX_PAYLOAD];
    snprintf(msg, sizeof(msg), "User %s joined the chat", name.c_str());
    broadcast(3, msg, fd);
}

// Удаление клиента из списка
void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    std::string name;
    std::string address;
    int port;
    
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->fd == fd) {
            name = it->name;
            address = it->address;
            port = it->port;
            clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "Client disconnected: " << address << ":" << port 
              << " (" << name << ")\n";
    
    // Уведомляем всех об отключении
    char msg[MAX_PAYLOAD];
    snprintf(msg, sizeof(msg), "User %s left the chat", name.c_str());
    broadcast(3, msg);
}

// Обработка клиента в отдельном потоке
void* handle_client(void* arg) {
    int client_fd = *(int*)arg;
    delete (int*)arg;
    
    Packet p;
    
    // Получаем HELLO
    if (!recv_packet(client_fd, p) || p.cmd != 1) {
        std::cerr << "Expected HELLO from client " << client_fd << "\n";
        close(client_fd);
        return NULL;
    }
    
    std::string client_name = p.txt;
    
    // Получаем адрес клиента
    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_fd, (sockaddr*)&addr, &addr_len);
    std::string client_address = inet_ntoa(addr.sin_addr);
    int client_port = ntohs(addr.sin_port);
    
    // Отправляем WELCOME
    char welcome[MAX_PAYLOAD];
    snprintf(welcome, sizeof(welcome), "Welcome %s:%d", 
             client_address.c_str(), client_port);
    send_packet(client_fd, 2, welcome);
    
    // Добавляем клиента в список
    add_client(client_fd, client_name, client_address, client_port);
    
    // Цикл обработки сообщений
    while (true) {
        if (!recv_packet(client_fd, p)) {
            // Клиент отключился
            break;
        }
        
        if (p.cmd == 3) {  // MSG_TEXT
            std::cout << "[" << client_address << ":" << client_port 
                      << "]: " << p.txt << "\n";
            
            // Формируем сообщение для рассылки
            char broadcast_msg[MAX_PAYLOAD];
            snprintf(broadcast_msg, sizeof(broadcast_msg), 
                     "[%s]: %s", client_name.c_str(), p.txt);
            
            // Рассылаем всем клиентам
            broadcast(3, broadcast_msg, client_fd);
        }
        else if (p.cmd == 4) {  // MSG_PING
            std::cout << "PING from " << client_address << ":" << client_port << "\n";
            send_packet(client_fd, 5);  // MSG_PONG
        }
        else if (p.cmd == 6) {  // MSG_BYE
            std::cout << "BYE from " << client_address << ":" << client_port << "\n";
            break;
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
        pthread_detach(thread);  // Отсоединяем поток, чтобы он сам освободился
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
    
    std::cout << "Server ready on port 8080\n";
    std::cout << "Thread pool size: " << THREAD_POOL_SIZE << "\n";
    
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
    
    // Очистка (никогда не достигнется при обычной работе)
    close(server_fd);
    return 0;
}