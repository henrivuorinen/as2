#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define MAX_ACCOUNTS 100
#define MAX_DESKS 5
#define MAX_QUEUE_SIZE 10
#define SOCKET_PATH "/tmp/bank_socket"
#define ACCOUNT_FILE "accounts.dat"
#define LOG_FILE "bank.log"
#define BUFFER_SIZE 256

typedef struct {
    int account_id;
    double balance;
    pthread_rwlock_t lock;
} Account;

typedef struct {
    int client_fd;
    int desk_id;
} ClientRequest;

typedef struct {
    ClientRequest queue[MAX_QUEUE_SIZE];
    int front, rear, count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} RequestQueue;

// Global variables
Account accounts[MAX_ACCOUNTS];
RequestQueue desk_queues[MAX_DESKS];
pthread_t desk_threads[MAX_DESKS];
int server_socket = -1;
volatile int shutdown_flag = 0;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *log_file = NULL;

// Function prototypes
void init_accounts(void);
void save_accounts(void);
void load_accounts(void);
void init_queues(void);
void *desk_worker(void *arg);
void handle_client(int client_fd, int desk_id);
void process_command(int client_fd, const char *command, int desk_id);
void log_transaction(const char *message);
void signal_handler(int sig);
int find_shortest_queue(void);
void enqueue_client(int queue_id, int client_fd);
ClientRequest dequeue_client(int queue_id);

void init_accounts(void) {
    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        accounts[i].account_id = i;
        accounts[i].balance = 1000.0; // Start with 1000 euros for simplicity
        pthread_rwlock_init(&accounts[i].lock, NULL);
    }
}

void save_accounts(void) {
    FILE *file = fopen(ACCOUNT_FILE, "wb");
    if (file) {
        for (int i = 0; i < MAX_ACCOUNTS; i++) {
            fwrite(&accounts[i].account_id, sizeof(int), 1, file);
            fwrite(&accounts[i].balance, sizeof(double), 1, file);
        }
        fclose(file);
    }
}

void load_accounts(void) {
    FILE *file = fopen(ACCOUNT_FILE, "rb");
    if (file) {
        for (int i = 0; i < MAX_ACCOUNTS; i++) {
            if (fread(&accounts[i].account_id, sizeof(int), 1, file) != 1) break;
            if (fread(&accounts[i].balance, sizeof(double), 1, file) != 1) break;
        }
        fclose(file);
    }
}

void init_queues(void) {
    for (int i = 0; i < MAX_DESKS; i++) {
        desk_queues[i].front = desk_queues[i].rear = desk_queues[i].count = 0;
        pthread_mutex_init(&desk_queues[i].mutex, NULL);
        pthread_cond_init(&desk_queues[i].not_empty, NULL);
        pthread_cond_init(&desk_queues[i].not_full, NULL);
    }
}

void log_transaction(const char *message) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';
        fprintf(log_file, "[%s] %s\n", time_str, message);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}

int find_shortest_queue(void) {
    int shortest = 0;
    int min_count = desk_queues[0].count;

    for (int i = 1; i < MAX_DESKS; i++) {
        if (desk_queues[i].count < min_count) {
            min_count = desk_queues[i].count;
            shortest = i;
        }
    }
    return shortest;
}

void enqueue_client(int queue_id, int client_fd) {
    pthread_mutex_lock(&desk_queues[queue_id].mutex);

    while (desk_queues[queue_id].count >= MAX_QUEUE_SIZE) {
        pthread_cond_wait(&desk_queues[queue_id].not_full, &desk_queues[queue_id].mutex);
    }

    desk_queues[queue_id].queue[desk_queues[queue_id].rear].client_fd = client_fd;
    desk_queues[queue_id].queue[desk_queues[queue_id].rear].desk_id = queue_id;
    desk_queues[queue_id].rear = (desk_queues[queue_id].rear + 1) % MAX_QUEUE_SIZE;
    desk_queues[queue_id].count++;

    pthread_cond_signal(&desk_queues[queue_id].not_empty);
    pthread_mutex_unlock(&desk_queues[queue_id].mutex);
}

ClientRequest dequeue_client(int queue_id) {
    ClientRequest request = {-1, -1};

    pthread_mutex_lock(&desk_queues[queue_id].mutex);

    while (desk_queues[queue_id].count == 0 && !shutdown_flag) {
        pthread_cond_wait(&desk_queues[queue_id].not_empty, &desk_queues[queue_id].mutex);
    }

    if (desk_queues[queue_id].count > 0) {
        request = desk_queues[queue_id].queue[desk_queues[queue_id].front];
        desk_queues[queue_id].front = (desk_queues[queue_id].front + 1) % MAX_QUEUE_SIZE;
        desk_queues[queue_id].count--;
        pthread_cond_signal(&desk_queues[queue_id].not_full);
    }

    pthread_mutex_unlock(&desk_queues[queue_id].mutex);
    return request;
}

void *desk_worker(void *arg) {
    int desk_id = *(int *)arg;

    while (!shutdown_flag) {
        ClientRequest request = dequeue_client(desk_id);
        if (request.client_fd != -1) {
            handle_client(request.client_fd, desk_id);
        }
    }
    return NULL;
}

void handle_client(int client_fd, int desk_id) {
    char buffer[BUFFER_SIZE];

    // Send "ready" message
    send(client_fd, "ready\n", 6, 0);

    while (!shutdown_flag) {
        ssize_t bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) break;

        buffer[bytes_received] = '\0';

        // Remove newline
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';

        if (strcmp(buffer, "q") == 0) {
            break;
        }

        process_command(client_fd, buffer, desk_id);
    }

    close(client_fd);
}

void process_command(int client_fd, const char *command, int desk_id) {
    char response[BUFFER_SIZE];
    char log_msg[BUFFER_SIZE];

    if (command[0] == 'l') { // List balance
        int account_id;
        if (sscanf(command, "l %d", &account_id) == 1 && account_id >= 0 && account_id < MAX_ACCOUNTS) {
            pthread_rwlock_rdlock(&accounts[account_id].lock);
            snprintf(response, BUFFER_SIZE, "ok: %.2f\n", accounts[account_id].balance);
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Balance inquiry for account %d: %.2f",
                    desk_id, account_id, accounts[account_id].balance);
            pthread_rwlock_unlock(&accounts[account_id].lock);
        } else {
            snprintf(response, BUFFER_SIZE, "fail: Invalid account\n");
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Invalid balance inquiry", desk_id);
        }
    }
    else if (command[0] == 'w') { // Withdraw
        int account_id, amount;
        if (sscanf(command, "w %d %d", &account_id, &amount) == 2 &&
            account_id >= 0 && account_id < MAX_ACCOUNTS && amount > 0) {
            pthread_rwlock_wrlock(&accounts[account_id].lock);
            if (accounts[account_id].balance >= amount) {
                accounts[account_id].balance -= amount;
                snprintf(response, BUFFER_SIZE, "ok: Withdrew %d euros\n", amount);
                snprintf(log_msg, BUFFER_SIZE, "Desk %d: Withdrew %d from account %d, new balance: %.2f",
                        desk_id, amount, account_id, accounts[account_id].balance);
            } else {
                snprintf(response, BUFFER_SIZE, "fail: Insufficient funds\n");
                snprintf(log_msg, BUFFER_SIZE, "Desk %d: Failed withdrawal of %d from account %d (insufficient funds)",
                        desk_id, amount, account_id);
            }
            pthread_rwlock_unlock(&accounts[account_id].lock);
        } else {
            snprintf(response, BUFFER_SIZE, "fail: Invalid parameters\n");
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Invalid withdrawal command", desk_id);
        }
    }
    else if (command[0] == 'd') { // Deposit
        int account_id, amount;
        if (sscanf(command, "d %d %d", &account_id, &amount) == 2 &&
            account_id >= 0 && account_id < MAX_ACCOUNTS && amount > 0) {
            pthread_rwlock_wrlock(&accounts[account_id].lock);
            accounts[account_id].balance += amount;
            snprintf(response, BUFFER_SIZE, "ok: Deposited %d euros\n", amount);
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Deposited %d to account %d, new balance: %.2f",
                    desk_id, amount, account_id, accounts[account_id].balance);
            pthread_rwlock_unlock(&accounts[account_id].lock);
        } else {
            snprintf(response, BUFFER_SIZE, "fail: Invalid parameters\n");
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Invalid deposit command", desk_id);
        }
    }
    else if (command[0] == 't') { // Transfer
        int from_account, to_account, amount;
        if (sscanf(command, "t %d %d %d", &from_account, &to_account, &amount) == 3 &&
            from_account >= 0 && from_account < MAX_ACCOUNTS &&
            to_account >= 0 && to_account < MAX_ACCOUNTS && amount > 0) {

            if (from_account == to_account) {
                snprintf(response, BUFFER_SIZE, "fail: Cannot transfer to same account\n");
                snprintf(log_msg, BUFFER_SIZE, "Desk %d: Failed transfer (same account)", desk_id);
            } else {
                // Lock accounts in order to prevent deadlock
                int first = (from_account < to_account) ? from_account : to_account;
                int second = (from_account < to_account) ? to_account : from_account;

                pthread_rwlock_wrlock(&accounts[first].lock);
                pthread_rwlock_wrlock(&accounts[second].lock);

                if (accounts[from_account].balance >= amount) {
                    accounts[from_account].balance -= amount;
                    accounts[to_account].balance += amount;
                    snprintf(response, BUFFER_SIZE, "ok: Transferred %d euros\n", amount);
                    snprintf(log_msg, BUFFER_SIZE, "Desk %d: Transferred %d from account %d to account %d",
                            desk_id, amount, from_account, to_account);
                } else {
                    snprintf(response, BUFFER_SIZE, "fail: Insufficient funds\n");
                    snprintf(log_msg, BUFFER_SIZE, "Desk %d: Failed transfer of %d from account %d (insufficient funds)",
                            desk_id, amount, from_account);
                }

                pthread_rwlock_unlock(&accounts[second].lock);
                pthread_rwlock_unlock(&accounts[first].lock);
            }
        } else {
            snprintf(response, BUFFER_SIZE, "fail: Invalid parameters\n");
            snprintf(log_msg, BUFFER_SIZE, "Desk %d: Invalid transfer command", desk_id);
        }
    }
    else {
        snprintf(response, BUFFER_SIZE, "fail: Unknown command\n");
        snprintf(log_msg, BUFFER_SIZE, "Desk %d: Unknown command", desk_id);
    }

    send(client_fd, response, strlen(response), 0);
    log_transaction(log_msg);
}

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        shutdown_flag = 1;

        // Wake up all waiting threads
        for (int i = 0; i < MAX_DESKS; i++) {
            pthread_cond_broadcast(&desk_queues[i].not_empty);
        }

        if (server_socket != -1) {
            close(server_socket);
        }
    }
}

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open log file
    log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        perror("Failed to open log file");
        return 1;
    }

    // Initialize accounts and load from file
    init_accounts();
    load_accounts();
    init_queues();

    // Create server socket
    server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCKET_PATH);

    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    if (listen(server_socket, 5) == -1) {
        perror("listen");
        return 1;
    }

    // Create desk threads
    int desk_ids[MAX_DESKS];
    for (int i = 0; i < MAX_DESKS; i++) {
        desk_ids[i] = i;
        pthread_create(&desk_threads[i], NULL, desk_worker, &desk_ids[i]);
    }

    log_transaction("Bank server started");
    printf("Bank server started. Listening on %s\n", SOCKET_PATH);

    // Accept client connections
    while (!shutdown_flag) {
        int client_fd = accept(server_socket, NULL, NULL);
        if (client_fd == -1) {
            if (!shutdown_flag) {
                perror("accept");
            }
            continue;
        }

        // Find shortest queue and enqueue client
        int shortest_queue = find_shortest_queue();
        enqueue_client(shortest_queue, client_fd);
    }

    // Cleanup
    printf("Shutting down server...\n");
    log_transaction("Bank server shutting down");

    // Wait all threads to finish
    for (int i = 0; i < MAX_DESKS; i++) {
        pthread_join(desk_threads[i], NULL);
    }

    save_accounts();

    if (log_file) {
        fclose(log_file);
    }

    unlink(SOCKET_PATH);

    printf("Server shutdown complete.\n");
    return 0;
}
