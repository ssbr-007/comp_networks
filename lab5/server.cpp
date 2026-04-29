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
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <json/json.h>  // Требуется установка libjsoncpp-dev

#define MAX_NAME     32
#define MAX_PAYLOAD  256
#define MAX_TIME_STR 32
#define THREAD_POOL_SIZE 10
#define MAX_CLIENTS 100

// Типы сообщений (расширенные)
enum MessageType {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,
    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,
    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14
};

// Расширенный формат сообщения
struct MessageEx {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    time_t   timestamp;
    char     payload[MAX_PAYLOAD];
};

// Структура для хранения в файле
struct StoredMessage {
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

// Офлайн-сообщение
struct OfflineMsg {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
};

// Структура клиента
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

// Хранилище офлайн-сообщений
std::map<std::string, std::queue<OfflineMsg>> offline_messages;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

// Генератор ID
uint32_t next_msg_id = 1;
pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

// История в файле
std::string history_file = "chat_history.json";

// ============ Вспомогательные функции ============

std::string get_current_time_str() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", t);
    return std::string(buffer);
}

std::string message_type_to_string(uint8_t type) {
    switch(type) {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        default: return "MSG_UNKNOWN";
    }
}

// Сохранение сообщения в JSON-файл
void save_message_to_history(const StoredMessage& msg) {
    Json::Value history;
    
    // Читаем существующую историю
    std::ifstream infile(history_file);
    if (infile.is_open()) {
        Json::Reader reader;
        reader.parse(infile, history);
        infile.close();
    }
    
    // Добавляем новое сообщение
    Json::Value entry;
    entry["msg_id"] = (Json::UInt64)msg.msg_id;
    entry["timestamp"] = (Json::UInt64)msg.timestamp;
    entry["sender"] = msg.sender;
    entry["receiver"] = msg.receiver;
    entry["type"] = msg.type;
    entry["text"] = msg.text;
    entry["delivered"] = msg.delivered;
    entry["is_offline"] = msg.is_offline;
    
    history.append(entry);
    
    // Записываем обратно
    std::ofstream outfile(history_file);
    Json::StyledWriter writer;
    outfile << writer.write(history);
    outfile.close();
    
    // Логирование TCP/IP
    std::cout << "[Application] saved message to history file\n";
}

// Чтение истории из файла
std::vector<StoredMessage> read_history(int count = 50) {
    std::vector<StoredMessage> messages;
    
    std::ifstream infile(history_file);
    if (!infile.is_open()) {
        return messages;
    }
    
    Json::Value history;
    Json::Reader reader;
    if (!reader.parse(infile, history)) {
        return messages;
    }
    infile.close();
    
    int start = std::max(0, (int)history.size() - count);
    for (int i = start; i < (int)history.size(); i++) {
        StoredMessage msg;
        msg.msg_id = history[i]["msg_id"].asUInt();
        msg.timestamp = history[i]["timestamp"].asUInt64();
        msg.sender = history[i]["sender"].asString();
        msg.receiver = history[i]["receiver"].asString();
        msg.type = history[i]["type"].asString();
        msg.text = history[i]["text"].asString();
        msg.delivered = history[i]["delivered"].asBool();
        msg.is_offline = history[i]["is_offline"].asBool();
        messages.push_back(msg);
    }
    
    return messages;
}

// Форматирование сообщения для вывода
std::string format_message(const StoredMessage& msg) {
    std::string time_str = std::ctime(&msg.timestamp);
    time_str.pop_back(); // убираем \n
    
    std::stringstream ss;
    if (msg.type == "MSG_PRIVATE") {
        ss << "[" << time_str << "][id=" << msg.msg_id << "]";
        if (msg.is_offline) ss << "[OFFLINE]";
        ss << "[" << msg.sender << " -> " << msg.receiver << "]: " << msg.text;
    } else {
        ss << "[" << time_str << "][id=" << msg.msg_id << "][" << msg.sender << "]: " << msg.text;
    }
    return ss.str();
}

// Логирование по модели TCP/IP
void log_tcpip_receive(int fd, const std::string& src_ip, const std::string& dst_ip, 
                       uint8_t cmd, int bytes) {
    std::cout << "\n[Network Access] frame arrived from network interface\n";
    std::cout << "[Internet] simulated IP hdr: src=" << src_ip << " dst=" << dst_ip << " proto=6\n";
    std::cout << "[Transport] simulated TCP hdr: recv() " << bytes << " bytes via TCP\n";
    std::cout << "[Application] deserialize MessageEx -> cmd=" << (int)cmd << "\n";
}

void log_tcpip_send(int fd, const std::string& dst_ip, uint8_t cmd, int bytes) {
    std::cout << "\n[Application] prepare MessageEx cmd=" << (int)cmd << "\n";
    std::cout << "[Transport] send() " << bytes << " bytes via TCP\n";
    std::cout << "[Internet] destination ip = " << dst_ip << "\n";
    std::cout << "[Network Access] frame sent to network interface\n";
}

// ============ Работа с MessageEx ============

void send_message_ex(int fd, uint8_t type, const char* sender, const char* receiver, 
                      const char* payload, uint32_t msg_id = 0) {
    MessageEx msg;
    
    pthread_mutex_lock(&id_mutex);
    msg.msg_id = (msg_id == 0) ? next_msg_id++ : msg_id;
    pthread_mutex_unlock(&id_mutex);
    
    msg.type = type;
    msg.timestamp = time(nullptr);
    strncpy(msg.sender, sender, MAX_NAME - 1);
    msg.sender[MAX_NAME - 1] = '\0';
    strncpy(msg.receiver, receiver, MAX_NAME - 1);
    msg.receiver[MAX_NAME - 1] = '\0';
    strncpy(msg.payload, payload, MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = htonl(sizeof(MessageEx) - sizeof(uint32_t));
    
    // Отправка
    write(fd, &msg.length, 4);
    write(fd, ((char*)&msg) + 4, ntohl(msg.length));
    
    // Логирование
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(fd, (sockaddr*)&addr, &len);
    log_tcpip_send(fd, inet_ntoa(addr.sin_addr), type, ntohl(msg.length));
}

bool recv_message_ex(int fd, MessageEx& msg) {
    if (read(fd, &msg.length, 4) != 4) return false;
    msg.length = ntohl(msg.length);
    if (msg.length > sizeof(MessageEx) - 4) return false;
    if (read(fd, ((char*)&msg) + 4, msg.length) != msg.length) return false;
    
    // Получение IP клиента
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(fd, (sockaddr*)&addr, &len);
    log_tcpip_receive(fd, inet_ntoa(addr.sin_addr), "127.0.0.1", msg.type, msg.length);
    
    return true;
}

// ============ Работа с клиентами ============

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

std::vector<std::string> get_online_users() {
    std::vector<std::string> users;
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.authenticated) {
            users.push_back(client.nickname);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return users;
}

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

void broadcast(uint8_t cmd, const char* sender, const char* payload, int sender_fd = -1) {
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) {
        if (client.authenticated && client.fd != sender_fd) {
            send_message_ex(client.fd, cmd, sender, "", payload);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Доставка офлайн-сообщений
void deliver_offline_messages(int client_fd, const std::string& nickname) {
    pthread_mutex_lock(&offline_mutex);
    auto it = offline_messages.find(nickname);
    if (it != offline_messages.end()) {
        std::queue<OfflineMsg>& queue = it->second;
        while (!queue.empty()) {
            OfflineMsg& msg = queue.front();
            
            // Отправляем как приватное с пометкой OFFLINE
            char formatted[MAX_PAYLOAD];
            snprintf(formatted, sizeof(formatted), "[OFFLINE] %s", msg.text);
            send_message_ex(client_fd, MSG_PRIVATE, msg.sender, msg.receiver, formatted, msg.msg_id);
            
            std::cout << "[Application] delivered offline message from " << msg.sender 
                      << " to " << msg.receiver << "\n";
            
            // Обновляем статус в истории
            std::vector<StoredMessage> history = read_history(10000);
            for (auto& hmsg : history) {
                if (hmsg.msg_id == msg.msg_id) {
                    hmsg.delivered = true;
                    break;
                }
            }
            
            queue.pop();
        }
        offline_messages.erase(it);
    }
    pthread_mutex_unlock(&offline_mutex);
}

// Сохранение офлайн-сообщения
void store_offline_message(const std::string& sender, const std::string& receiver, 
                           const std::string& text, uint32_t msg_id, time_t timestamp) {
    pthread_mutex_lock(&offline_mutex);
    
    OfflineMsg msg;
    strncpy(msg.sender, sender.c_str(), MAX_NAME - 1);
    strncpy(msg.receiver, receiver.c_str(), MAX_NAME - 1);
    strncpy(msg.text, text.c_str(), MAX_PAYLOAD - 1);
    msg.timestamp = timestamp;
    msg.msg_id = msg_id;
    
    offline_messages[receiver].push(msg);
    
    std::cout << "[Application] store message in offline queue for user: " << receiver << "\n";
    
    pthread_mutex_unlock(&offline_mutex);
}

// Добавление клиента
void add_client(int fd, const std::string& nickname, const std::string& address, int port) {
    pthread_mutex_lock(&clients_mutex);
    Client client = {fd, nickname, address, port, true};
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "\n[Application] User [" << nickname << "] connected from " << address << ":" << port << "\n";
    std::cout << "[Application] SYN → ACK → READY\n";
    std::cout << "[Application] coffee powered TCP/IP stack initialized\n";
    std::cout << "[Application] packets never sleep\n";
    
    // Доставляем офлайн-сообщения
    deliver_offline_messages(fd, nickname);
    std::cout << "[Application] no offline messages for " << nickname << "\n";
    
    // Системное сообщение
    char msg[MAX_PAYLOAD];
    snprintf(msg, sizeof(msg), "%s joined the chat", nickname.c_str());
    broadcast(MSG_SERVER_INFO, "Server", msg, fd);
}

// Удаление клиента
void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    std::string nickname;
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->fd == fd) {
            nickname = it->nickname;
            clients.erase(it);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (!nickname.empty()) {
        std::cout << "[Application] User [" << nickname << "] disconnected\n";
        
        char msg[MAX_PAYLOAD];
        snprintf(msg, sizeof(msg), "%s left the chat", nickname.c_str());
        broadcast(MSG_SERVER_INFO, "Server", msg);
    }
}

// ============ Обработка команд ============

void handle_history_request(int client_fd, const std::string& nickname, int count) {
    std::cout << "[Application] handle MSG_HISTORY from " << nickname << " (count=" << count << ")\n";
    
    auto messages = read_history(count);
    std::stringstream response;
    
    for (const auto& msg : messages) {
        response << format_message(msg) << "\n";
    }
    
    if (messages.empty()) {
        response << "No messages in history\n";
    }
    
    send_message_ex(client_fd, MSG_HISTORY_DATA, "Server", nickname.c_str(), response.str().c_str());
}

void handle_list_request(int client_fd, const std::string& nickname) {
    std::cout << "[Application] handle MSG_LIST from " << nickname << "\n";
    
    auto users = get_online_users();
    std::stringstream response;
    response << "=== Online users (" << users.size() << ") ===\n";
    for (const auto& user : users) {
        response << "  " << user << "\n";
    }
    
    send_message_ex(client_fd, MSG_SERVER_INFO, "Server", nickname.c_str(), response.str().c_str());
}

void handle_private_message(const MessageEx& msg, int sender_fd, const std::string& sender) {
    std::cout << "[Application] handle MSG_PRIVATE from " << sender 
              << " to " << msg.receiver << "\n";
    
    int target_fd = find_client_by_nickname(msg.receiver);
    
    StoredMessage stored;
    stored.msg_id = msg.msg_id;
    stored.timestamp = msg.timestamp;
    stored.sender = sender;
    stored.receiver = msg.receiver;
    stored.type = message_type_to_string(msg.type);
    stored.text = msg.payload;
    stored.delivered = (target_fd != -1);
    stored.is_offline = (target_fd == -1);
    
    save_message_to_history(stored);
    
    if (target_fd != -1) {
        // Получатель онлайн
        char formatted[MAX_PAYLOAD];
        snprintf(formatted, sizeof(formatted), "[PRIVATE][%s]: %s", sender.c_str(), msg.payload);
        send_message_ex(target_fd, MSG_PRIVATE, sender.c_str(), msg.receiver, formatted, msg.msg_id);
        
        // Подтверждение отправителю
        char confirm[MAX_PAYLOAD];
        snprintf(confirm, sizeof(confirm), "[PRIVATE] to %s: %s", msg.receiver, msg.payload);
        send_message_ex(sender_fd, MSG_SERVER_INFO, "Server", sender.c_str(), confirm);
        
        std::cout << "[Application] message delivered immediately\n";
    } else {
        // Получатель офлайн
        store_offline_message(sender, msg.receiver, msg.payload, msg.msg_id, msg.timestamp);
        std::cout << "[Application] receiver " << msg.receiver << " is offline\n";
        std::cout << "[Application] if it works — don't touch it\n";
        std::cout << "[Application] append record to history file delivered=false\n";
        
        send_message_ex(sender_fd, MSG_SERVER_INFO, "Server", sender.c_str(), 
                       ("Message saved for " + std::string(msg.receiver) + " (offline)").c_str());
    }
}

// ============ Поток обработки клиента ============

void* handle_client(void* arg) {
    int client_fd = *(int*)arg;
    delete (int*)arg;
    
    // Получаем адрес клиента
    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_fd, (sockaddr*)&addr, &addr_len);
    std::string client_address = inet_ntoa(addr.sin_addr);
    int client_port = ntohs(addr.sin_port);
    
    std::cout << "\n=== New connection from " << client_address << ":" << client_port << " ===\n";
    
    // Приветственное сообщение
    send_message_ex(client_fd, MSG_WELCOME, "Server", "", "Welcome to TCP Chat Server! Enter your nickname:");
    
    // Получаем аутентификацию
    MessageEx auth_msg;
    if (!recv_message_ex(client_fd, auth_msg) || auth_msg.type != MSG_AUTH) {
        std::cout << "[Application] authentication failed: no MSG_AUTH received\n";
        close(client_fd);
        return nullptr;
    }
    
    std::string nickname(auth_msg.payload);
    
    // Проверка ника
    if (nickname.empty() || !is_nickname_unique(nickname)) {
        std::cout << "[Application] authentication failed for nickname: " << nickname << "\n";
        send_message_ex(client_fd, MSG_ERROR, "Server", "", "Nickname invalid or already taken");
        close(client_fd);
        return nullptr;
    }
    
    std::cout << "[Application] authentication success: " << nickname << "\n";
    send_message_ex(client_fd, MSG_AUTH, "Server", "", "Authentication successful");
    
    // Добавляем клиента
    add_client(client_fd, nickname, client_address, client_port);
    
    // Основной цикл
    MessageEx msg;
    while (server_running && recv_message_ex(client_fd, msg)) {
        std::cout << "[Application] handle cmd=" << (int)msg.type << " from " << nickname << "\n";
        
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "[Application] handle MSG_TEXT\n";
                broadcast(MSG_TEXT, nickname.c_str(), msg.payload, client_fd);
                
                // Сохраняем в историю
                {
                    StoredMessage stored;
                    stored.msg_id = msg.msg_id;
                    stored.timestamp = msg.timestamp;
                    stored.sender = nickname;
                    stored.receiver = "";
                    stored.type = "MSG_TEXT";
                    stored.text = msg.payload;
                    stored.delivered = true;
                    stored.is_offline = false;
                    save_message_to_history(stored);
                }
                break;
                
            case MSG_PRIVATE:
                handle_private_message(msg, client_fd, nickname);
                break;
                
            case MSG_LIST:
                handle_list_request(client_fd, nickname);
                break;
                
            case MSG_HISTORY: {
                int count = 50;
                if (strlen(msg.payload) > 0) {
                    count = atoi(msg.payload);
                    if (count <= 0) count = 50;
                    if (count > 500) count = 500;
                }
                handle_history_request(client_fd, nickname, count);
                break;
            }
                
            case MSG_PING:
                std::cout << "[Application] handle MSG_PING\n";
                send_message_ex(client_fd, MSG_PONG, "Server", "", "PONG");
                break;
                
            case MSG_BYE:
                std::cout << "[Application] handle MSG_BYE\n";
                remove_client(client_fd);
                close(client_fd);
                return nullptr;
                
            default:
                std::cout << "[Application] unknown command: " << (int)msg.type << "\n";
                break;
        }
    }
    
    remove_client(client_fd);
    close(client_fd);
    return nullptr;
}

// ============ Пул потоков ============

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
        
        int* fd_copy = new int(client_fd);
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, fd_copy);
        pthread_detach(thread);
    }
    return nullptr;
}

// ============ Main ============

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
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }
    
    std::cout << "========================================\n";
    std::cout << "TCP Chat Server v2.0 (MessageEx)\n";
    std::cout << "Port: 8080\n";
    std::cout << "Thread pool size: " << THREAD_POOL_SIZE << "\n";
    std::cout << "History file: " << history_file << "\n";
    std::cout << "========================================\n\n";
    
    // Создаем пул потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }
    
    // Основной цикл
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
        
        pthread_mutex_lock(&queue_mutex);
        connection_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
        
        std::cout << "[Network Access] new connection accepted\n";
    }
    
    close(server_fd);
    return 0;
}