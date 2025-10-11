/* 
 * udpserver.c - A simple UDP echo server 
 * usage: udpserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h> // for handle_ls

#define BUFSIZE 1024
#define RWS 4
#define ARRAY_SIZE 25

typedef struct {
    int sockfd;
    struct sockaddr_in clientaddr;
    int clientlen;
} client_res_info;

typedef struct {
   int LFR; 
   int LAF; 
   int frame_ptr; 
   int frame_size;  
   struct recQ_slot {
    int received; 
    char msg[BUFSIZE];
   } recQ[ARRAY_SIZE];
} RWS_info; 

/* Function declarations */
void handle_ls(client_res_info response_info, char *buf, int ack_num);
void handle_exit(client_res_info response_info, char *buf, int ack_num);
void handle_get(client_res_info response_info, char *buf, int ack_num);
void handle_put(client_res_info response_info, char *buf, int ack_num);
void handle_delete(client_res_info response_info, char *buf, int ack_num);
void error(const char *msg);
int sender(client_res_info response_info, char *response, char *return_code, int acknum);
int input_transfer(client_res_info response_info, char *buf, int acknum);

void error(const char *msg) {
    perror(msg);
    exit(1);
}

void init_RWS(RWS_info *receiver_window, int frame_size) { 
    receiver_window->LFR = -1; 
    receiver_window->LAF = -1;
    receiver_window->frame_ptr = 0; 
    receiver_window->frame_size = frame_size; 
    for (int i = 0; i < ARRAY_SIZE; i++) { 
        receiver_window->recQ[i].received = 0; 
        memset(receiver_window->recQ[i].msg, 0, BUFSIZE);
    } 
}
int window_handler(int ack_num, RWS_info *receiver_window) { 
    if(ack_num == (receiver_window->LFR + 1) % RWS) { 
        receiver_window->LFR = (receiver_window->LFR + 1) % ARRAY_SIZE; 
        return 0; 
    }
    else { 

        printf("Out of order ACK received: %d\n, expected %d", ack_num, (receiver_window->LFR + 1) % ARRAY_SIZE);
        return -1; 
    }
}

int sender(client_res_info response_info, char *response, char *return_code, int acknum) { 
    char send_buf[BUFSIZE];


    // Format: "<acknum> | <response>"
    if (acknum < 0) snprintf(send_buf, sizeof(send_buf), "NA | %s", response); 
    else snprintf(send_buf, sizeof(send_buf), "%d | %s", acknum, response);

    printf("SENDING: %s\n", send_buf);
    printf("TO: %s\n", inet_ntoa(response_info.clientaddr.sin_addr));
    printf("Length: %d\n", (int)strlen(send_buf)); 

    int n = sendto(
        response_info.sockfd, 
        send_buf, 
        strlen(send_buf), 
        0, 
        (struct sockaddr *) &response_info.clientaddr, 
        response_info.clientlen
    );

    if (n < 0) {
        perror("Failed to send response");
        return -1;
    }

    (void)return_code; // suppress unused warning
    return n;
}

int input_transfer(client_res_info response_info, char *buf, int acknum) {
    char command[25];
    // 2. Extract base command (first token)
    sscanf(buf, "%24s", command);

    if (strcmp(command, "ls") == 0) {
        handle_ls(response_info, buf, acknum);
    } 
    else if (strcmp(command, "exit") == 0) {
        handle_exit(response_info, buf, acknum);
    } 
    else if (strcmp(command, "get") == 0) {
        handle_get(response_info, buf, acknum);
    } 
    else if (strcmp(command, "put") == 0) {
        handle_put(response_info, buf, acknum);
    } 
    else if (strcmp(command, "delete") == 0) {
        handle_delete(response_info, buf, acknum);
    } 
    else {
        fprintf(stderr, "Invalid request: %s\n", buf);
        sender(response_info, "Invalid command", NULL, acknum); 
    }

    return acknum; // useful for caller
}


void handle_get(client_res_info response_info, char *buf, int ack_num) {
    if (buf == NULL) return;
    buf += 4; // skip "get "
    buf[strcspn(buf, "\r\n ")] = '\0'; 
    printf("Requested file: %s\n", buf);
    FILE *file_ptr = fopen(buf, "rb");
    if (!file_ptr) {
        printf("FILE WAS NOT FOUND"); 
        sender(response_info, "File not found", NULL, ack_num); 
        perror("File not found");
        return;
    }

    sender(response_info, "Starting file transfer", NULL, ack_num); 
    fseek(file_ptr, 0, SEEK_END);
    long file_size = ftell(file_ptr);
    fseek(file_ptr, 0, SEEK_SET);

    printf("Sending file: %s (%ld bytes)\n", buf, file_size);

    char file_buffer[BUFSIZE];
    size_t bytes_read;
    int frame_num = 0;

    while ((bytes_read = fread(file_buffer, 1, BUFSIZE - 50, file_ptr)) > 0) {
        // Frame the packet: "<ack_num> | <data>"
        char packet[BUFSIZE];
        memset(packet, 0, sizeof(packet));
        
        int len = snprintf(packet, sizeof(packet), "%s | ", buf);

        // Make sure we don’t overflow
        memcpy(packet + len, file_buffer, bytes_read);
        len += bytes_read;

        // Send the framed message
        int n = sender(response_info, packet, NULL, ack_num); 

        if (n < 0) {
            perror("sendto failed");
            break;
        }

        printf("Sent frame %d (%zu bytes)\n", ack_num + frame_num, bytes_read);
        frame_num++;

        // Optionally wait for ACK here before sending next frame (if reliable)
    }

    fclose(file_ptr);

    // Optionally send end-of-file marker
    char end_msg[64];
    snprintf(end_msg, sizeof(end_msg), "%d | END", ack_num);
    sender(response_info, end_msg, NULL, ack_num); 

    printf("File transfer complete.\n");
}


void handle_put(client_res_info response_info, char *buf, int ack_num) {
    if (buf != NULL) buf += 4; 
    else return; 






    (void)response_info;
    (void)buf;
    // TODO: implement file upload logic

}

void handle_delete(client_res_info response_info, char *buf, int ack_num) {
    (void)response_info;
    if (buf != NULL) buf += 7;
    else return;

    int status = remove(buf);

    if (status == 0) {
        printf("File '%s' deleted successfully.\n", buf);
    } else {
        printf("Error deleting file '%s'.\n", buf);
        perror("Error details"); 
    }
}

void handle_ls(client_res_info response_info, char *buf, int ack_num) {
    (void)buf;
    char *dir_buff = NULL; 
    dir_buff = (char *)malloc(BUFSIZE);
    if (dir_buff == NULL) {
        perror("malloc failed");
        return;
    }

    // Get the current working directory
    if (getcwd(dir_buff, BUFSIZE) != NULL) {
        printf("Current working directory: %s\n", dir_buff);
    } else {
        perror("getcwd failed");
    }

    DIR *dir = opendir(".");
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    char response[BUFSIZE] = "";
    while ((entry = readdir(dir)) != NULL) {
        strcat(response, entry->d_name);
        strcat(response, ",");
    }
    closedir(dir);
    free(dir_buff); 
    sender(response_info, response, NULL, ack_num);
    return; 
}

void handle_exit(client_res_info response_info, char *buf, int ack_num) {
    (void)response_info;
    (void)buf;
    printf("Client requested exit.\n");
    sender(response_info, "Goodbye!\n", NULL, ack_num); 
    exit(0); 
    // In UDP, we don't have a persistent connection to close.
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int sockfd;
    int portno;
    int clientlen;
    struct sockaddr_in serveraddr;
    struct sockaddr_in clientaddr;
    struct hostent *hostp;
    char buf[BUFSIZE];
    char *hostaddrp;
    int optval;
    int n;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
               (const void *)&optval , sizeof(int));

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
             sizeof(serveraddr)) < 0) 
        error("ERROR on binding");


    RWS_info receiver_window; 
    init_RWS(&receiver_window, BUFSIZE); 


    printf("Server listening on port %d\n", portno);

    clientlen = sizeof(clientaddr);
    while (1) {
        bzero(buf, BUFSIZE);
        n = recvfrom(sockfd, buf, BUFSIZE, 0,
                     (struct sockaddr *) &clientaddr, (socklen_t *) &clientlen);
        if (n < 0)
            error("ERROR in recvfrom");

        hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
                              sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        if (hostp == NULL)
            error("ERROR on gethostbyaddr");
        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
            error("ERROR on inet_ntoa");
        printf("server received datagram from %s (%s)\n", 
               hostp->h_name, hostaddrp);
        printf("server received %d/%d bytes: %s\n", (int)strlen(buf), n, buf);
        
        client_res_info client_info;
        client_info.sockfd = sockfd;
        client_info.clientaddr = clientaddr;
        client_info.clientlen = clientlen;

        if (n > 0) {
            int acknum = -1;
            char *cmd_start = strstr(buf, "|");

            if (cmd_start == NULL) {
                fprintf(stderr, "Malformed message (no '|'): %s\n", buf);
                return -1;
            }
            *cmd_start = '\0';

            // Extract acknum
            if (sscanf(buf, "%d", &acknum) != 1) {
                fprintf(stderr, "Malformed ACK number: %s\n", buf);
                return -1;
            }

            // Move to command part (skip '|' and spaces)
            cmd_start++;
            while (*cmd_start == ' ') cmd_start++;
            cmd_start[strcspn(cmd_start, "\n")] = '\0'; // remove newline

            printf("ACKNUM: %d\n", acknum);
            printf("Command buffer: %s\n", cmd_start);

            // Pass *only* the command part to input_transfer
            if (window_handler(acknum, &receiver_window) ==0) input_transfer(client_info, cmd_start, acknum);
            else printf("Ignoring out-of-order ACK: %d\n", acknum);
        }
        else {
            fprintf(stderr, "Received NULL buffer.\n");
        }

        if (n < 0) 
            error("ERROR in sendto");
    }
}
