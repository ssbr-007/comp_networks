#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <signal.h>
#include <ctime>
#include <sstream>
#include <iomanip>

#define MAX_NAME     32
#define MAX_PAYLOAD  256
#define RECONNECT_DELAY 2

// Типы сообщений
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

volatile bool connected = false;
volatile bool running = true;
volatile bool authenticated = false;
int sockfd = -1;
std::string my_nickname;
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

// Получение текущего времени
std::string get_time_str(time_t t) {
    struct tm* tm = localtime(&t);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);
    return std::string(buffer);
}

// Отправка MessageEx
void send_message_ex(int fd, uint8_t type, const char* receiver, const char* payload) {
    MessageEx msg;
    msg.type = type;
    msg.msg_id = 0;  // Сервер назначит ID
    msg.timestamp = time(nullptr);
    strncpy(msg.sender, my_nickname.c_str(), MAX_NAME - 1);
    msg.sender[MAX_NAME - 1] = '\0';
    strncpy(msg.receiver, receiver, MAX_NAME - 1);
    msg.receiver[MAX_NAME - 1] = '\0';
    strncpy(msg.payload, payload, MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = htonl(sizeof(MessageEx) - sizeof(uint32_t));
    
    pthread_mutex_lock(&socket_mutex);
    write(fd, &msg.length, 4);
    write(fd, ((char*)&msg) + 4, ntohl(msg.length));
    pthread_mutex_unlock(&socket_mutex);
}

// Прием MessageEx
bool recv_message_ex(int fd, MessageEx& msg) {
    if (read(fd, &msg.length, 4) != 4) return false;
    msg.length = ntohl(msg.length);
    if (msg.length > sizeof(MessageEx) - 4) return false;
    if (read(fd, ((char*)&msg) + 4, msg.length) != msg.length) return false;
    return true;
}

// Вывод сообщения с форматированием
void display_message(const MessageEx& msg) {
    std::string time_str = get_time_str(msg.timestamp);
    
    if (msg.type == MSG_TEXT) {
        std::cout << "\r\033[K[" << time_str << "][id=" << msg.msg_id << "][" 
                  << msg.sender << "]: " << msg.payload << "\n> " << std::flush;
    }
    else if (msg.type == MSG_PRIVATE) {
        std::cout << "\r\033[K\033[33m[" << time_str << "][id=" << msg.msg_id << "]";
        if (std::string(msg.payload).find("[OFFLINE]") == 0) {
            std::cout << "[OFFLINE]";
        }
        std::cout << "[" << msg.sender << " -> " << msg.receiver << "]: " << msg.payload 
                  << "\033[0m\n> " << std::flush;
    }
    else if (msg.type == MSG_SERVER_INFO) {
        std::cout << "\r\033[K\033[35m[SERVER]: " << msg.payload << "\033[0m\n> " << std::flush;
    }
    else if (msg.type == MSG_HISTORY_DATA) {
        std::cout << "\r\033[K\033[36m=== History ===\033[0m\n";
        std::cout << msg.payload << "\n";
        std::cout << "\033[36m================\033[0m\n> " << std::flush;
    }
    else if (msg.type == MSG_ERROR) {
        std::cout << "\r\033[K\033[31m[ERROR]: " << msg.payload << "\033[0m\n> " << std::flush;
    }
    else if (msg.type == MSG_PONG) {
        std::cout << "\r\033[K\033[36m[SERVER]: PONG\033[0m\n> " << std::flush;
    }
    else if (msg.type == MSG_WELCOME) {
        std::cout << "\r\033[K\033[32m" << msg.payload << "\033[0m\n> " << std::flush;
    }
}

// Поток приема сообщений
void* receive_thread(void* arg) {
    MessageEx msg;
    
    while (running && connected) {
        pthread_mutex_lock(&socket_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&socket_mutex);
        
        if (fd < 0) break;
        
        if (recv_message_ex(fd, msg)) {
            if (msg.type == MSG_AUTH) {
                authenticated = true;
                std::cout << "\r\033[K\033[32mAuthenticated!\033[0m\n> " << std::flush;
            }
            else {
                display_message(msg);
            }
        }
        else {
            std::cout << "\r\033[K\033[31mConnection lost\033[0m\n";
            connected = false;
            authenticated = false;
            break;
        }
    }
    return nullptr;
}

// Подключение к серверу
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
    
    // Получаем приветствие
    MessageEx welcome;
    if (!recv_message_ex(fd, welcome)) {
        close(fd);
        return false;
    }
    std::cout << welcome.payload << "\n";
    
    // Ввод ника и аутентификация
    std::cout << "> ";
    std::getline(std::cin, my_nickname);
    if (my_nickname.empty()) my_nickname = "user";
    
    send_message_ex(fd, MSG_AUTH, "", my_nickname.c_str());
    
    // Ждем ответ
    MessageEx response;
    if (!recv_message_ex(fd, response)) {
        close(fd);
        return false;
    }
    
    if (response.type == MSG_ERROR) {
        std::cout << "\033[31m" << response.payload << "\033[0m\n";
        close(fd);
        return false;
    }
    
    connected = true;
    return true;
}

// Вывод справки
void print_help() {
    std::cout << "\n\033[36m=== Available commands ===\033[0m\n";
    std::cout << "  /help                 - Show this help\n";
    std::cout << "  /list                 - Show online users\n";
    std::cout << "  /history              - Show last 50 messages\n";
    std::cout << "  /history N            - Show last N messages\n";
    std::cout << "  /quit                 - Disconnect\n";
    std::cout << "  /w <nick> <message>   - Send private message\n";
    std::cout << "  /ping                 - Test connection\n";
    std::cout << "\033[36mTip: packets never sleep\033[0m\n";
    std::cout << "==========================\n\n";
}

int main() {
    std::cout << "\033[36m=== TCP Chat Client v2.0 ===\033[0m\n";
    print_help();
    
    signal(SIGPIPE, SIG_IGN);
    
    if (!connect_to_server()) {
        std::cout << "\033[31mFailed to connect to server\033[0m\n";
        return 1;
    }
    
    // Запускаем поток приема
    pthread_t recv_thread;
    pthread_create(&recv_thread, nullptr, receive_thread, nullptr);
    pthread_detach(recv_thread);
    
    // Основной цикл ввода
    std::string input;
    while (running) {
        std::cout << "> ";
        std::getline(std::cin, input);
        
        if (!connected) {
            std::cout << "\033[33mNot connected\033[0m\n";
            continue;
        }
        
        if (input == "/quit") {
            send_message_ex(sockfd, MSG_BYE, "", "");
            running = false;
            break;
        }
        else if (input == "/ping") {
            send_message_ex(sockfd, MSG_PING, "", "");
        }
        else if (input == "/help") {
            print_help();
        }
        else if (input == "/list") {
            send_message_ex(sockfd, MSG_LIST, "", "");
        }
        else if (input.substr(0, 8) == "/history") {
            std::string param = input.length() > 8 ? input.substr(9) : "";
            send_message_ex(sockfd, MSG_HISTORY, "", param.c_str());
        }
        else if (input.substr(0, 3) == "/w ") {
            std::string rest = input.substr(3);
            size_t space_pos = rest.find(' ');
            if (space_pos != std::string::npos) {
                std::string target = rest.substr(0, space_pos);
                std::string message = rest.substr(space_pos + 1);
                send_message_ex(sockfd, MSG_PRIVATE, target.c_str(), message.c_str());
            } else {
                std::cout << "\033[31mUsage: /w <nickname> <message>\033[0m\n";
            }
        }
        else if (!input.empty() && input[0] != '/') {
            send_message_ex(sockfd, MSG_TEXT, "", input.c_str());
        }
    }
    
    pthread_mutex_lock(&socket_mutex);
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    pthread_mutex_unlock(&socket_mutex);
    
    std::cout << "\033[36mDisconnected\033[0m\n";
    return 0;
}