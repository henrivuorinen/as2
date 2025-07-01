/**
 * ELEC-C7310 Assignment #2: ThreadBank Client
 * Client program that connects to the bank server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/bank_socket"
#define BUFFER_SIZE 256

int main(int argc, char **argv) {
    int sock;
    struct sockaddr_un addr;
    char buffer[BUFFER_SIZE];

    // Set line buffering for stdin and stdout
    setvbuf(stdin, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Create socket
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }

    // Set up address
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Connect to server
    printf("Connecting to the bank, please wait.\n");
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sock);
        return 1;
    }

    // Wait for "ready" message
    ssize_t bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        printf("%s", buffer); // Should print "ready\n"
    } else {
        printf("Error receiving ready message\n");
        close(sock);
        return 1;
    }

    // Main loop
    int quit = 0;
    while (!quit) {
        // Read command from stdin
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL) {
            break;
        }

        // Check for quit command
        if (strncmp(buffer, "q", 1) == 0) {
            quit = 1;
        }

        // Send command to server
        if (send(sock, buffer, strlen(buffer), 0) == -1) {
            perror("send");
            break;
        }

        // If quitting, don't wait for response
        if (quit) {
            break;
        }

        // Receive response from server
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            printf("%s", buffer);
        } else if (bytes_received == 0) {
            printf("Server disconnected\n");
            break;
        } else {
            perror("recv");
            break;
        }
    }

    close(sock);
    return 0;
}
