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
#include <ctype.h>
#include <errno.h>

#define BUFSIZE 1024
#define RWS 4
#define ARRAY_SIZE 10

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

static int 



/* Function declarations */
void handle_ls(client_res_info response_info, char *buf, int ack_num);
void handle_exit(client_res_info response_info, char *buf, int ack_num);
void handle_get(client_res_info response_info, char *buf, int ack_num);
void handle_put(client_res_info response_info, char *buf, int ack_num);
void handle_delete(client_res_info response_info, char *buf, int ack_num);
void error(const char *msg);
int sender(client_res_info response_info, char *response, char *return_code, int acknum);
int input_transfer(client_res_info response_info, char *buf, int acknum);
void put_helper(char * buf, int buf_len, client_res_info response_info); 



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
    if(ack_num == (receiver_window->LFR + 1) % ARRAY_SIZE) { 
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


void get_handler(char *buf, ssize_t buf_len, server_res_info server_info) {
    if (!buf || buf_len <= 0) return;

    // Find first '|' (end of header section)
    char *sep = memchr(buf, '|', buf_len);
    if (!sep) {
        fprintf(stderr, "Invalid packet (missing '|')\n");
        return;
    }

    // Extract filename
    char filename[256];
    memset(filename, 0, sizeof(filename));

    char *prefix = strstr(buf, "putfile:");
    size_t name_len = 0;
    if (prefix) {
        // between "putfile:" and first '|'
        name_len = sep - (prefix + 8);
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, prefix + 8, name_len);
    } else {
        // no "putfile:" prefix, assume filename starts at beginning
        name_len = sep - buf;
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, buf, name_len);
    }

    // Trim trailing spaces
    char *end = filename + strlen(filename) - 1;
    while (end > filename && isspace((unsigned char)*end)) *end-- = '\0';

    // Extract frame number (after "frame:")
    int frame_num = -1;
    char *frame_ptr = strstr(buf, "frame:");
    if (frame_ptr) {
        frame_num = atoi(frame_ptr + 6);  // skip "frame:"
    } else {
        fprintf(stderr, "Warning: frame number missing in packet for %s\n", filename);
    }

    if (fre)

    // Send ACK back to client
    char ack_msg[128];
    snprintf(ack_msg, sizeof(ack_msg), "ACK:%s|frame:%d", filename, frame_num);

    ssize_t n = sendto(
        server_info.sockfd,
        ack_msg,
        strlen(ack_msg),
        0,
        (struct sockaddr *)&server_info.serveraddr,
        server_info.serverlen
    );

    if (n < 0) perror("Failed to send ACK");
    else printf("[ACK SENT] %s (frame %d)\n", filename, frame_num);

    // Get file data (everything after the *second* '|')
    char *data_start = strstr(sep + 1, "|");
    if (!data_start) {
        fprintf(stderr, "Invalid packet (missing data separator '|')\n");
        return;
    }
    data_start++; // skip past second '|'

    size_t data_len = buf + buf_len - data_start;
    printf("[WRITE] %zu bytes to %s (frame %d)\n", data_len, filename, frame_num);

    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        perror("fopen");
        return;
    }

    fwrite(data_start, 1, data_len, fp);
    fclose(fp);
}




void handle_put(client_res_info response_info, char *buf, int ack_num) {
    if (buf != NULL) buf += 4; 
    else return; 

    buf[strcspn(buf, "\r\n ")] = '\0';

    printf("Requested to put file: %s\n", buf); 

    char response[BUFSIZE];
    snprintf(response, sizeof(response), "gimmefile:%s", buf);
    sender(response_info, response, NULL, ack_num); 


    (void)response_info;
    (void)buf;
    // TODO: implement file upload logic

}


void put_helper(char *buf, int buf_len, client_res_info response_info) {
    if (buf == NULL || buf_len <= 0) return;

    // Expected format: "putfile:filename | data..."
    char *original_buf = buf;  // Save original pointer
    
    // Skip "putfile:" (8 characters)
    buf += 8;
    buf_len -= 8;  // Adjust length too!

    // Find the separator between filename and data
    char *sep = strstr(buf, "|");
    if (!sep) {
        fprintf(stderr, "Malformed putfile command (missing '|'): %s\n", buf);
        return;
    }

    // Extract filename
    char filename[128];
    memset(filename, 0, sizeof(filename));

    // Copy part before '|'
    int filename_len = sep - buf;
    strncpy(filename, buf, filename_len);
    filename[filename_len] = '\0';  // Ensure null termination

    // Trim trailing spaces from filename
    char *end = filename + strlen(filename) - 1;
    while (end > filename && isspace((unsigned char)*end)) *end-- = '\0';

    char ack_msg[64]; 
    snprintf(ack_msg, sizeof(ack_msg), "GOTIT(SERVER):%s", filename);
    int n = sendto(
        response_info.sockfd,
        ack_msg,
        strlen(ack_msg),
        0,
        (struct sockaddr *)&response_info.clientaddr,
        response_info.clientlen
    );
    if (n < 0) { 
        perror("Failed to send ACK"); 
        return; 
    }
    else printf("Send ACK for putfile:%s\n", filename); 


    // Move sep past '|' to get payload
    sep++;
    
    // Calculate data length: total remaining bytes after '|'
    size_t data_len = buf_len - (sep - buf);
    
    if (data_len <= 0) {
        fprintf(stderr, "No payload found in command.\n");
        return;
    }

    printf("Writing %zu bytes to file: %s\n", data_len, filename);

    // Open file for appending
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        perror("fopen");
        return;
    }

    // Write data to file
    size_t written = fwrite(sep, 1, data_len, fp);
    fclose(fp);

    if (written != data_len) {
        fprintf(stderr, "Warning: wrote %zu bytes but expected %zu\n", written, data_len);
    }

    printf("Successfully wrote %zu bytes to file: %s\n", written, filename);
}


void handle_delete(client_res_info response_info, char *buf, int ack_num) {
    (void)response_info;
    if (buf != NULL) buf += 7;
    else return;
    buf[strcspn(buf, "\r\n ")] = '\0'; 
    int status = remove(buf);

    if (status == 0) {
        printf("File '%s' deleted successfully.\n", buf);
        sender(response_info, "File deleted successfully", NULL, ack_num); 
    } else {
        printf("Error deleting file '%s'.\n", buf);
        sender(response_info, "File deletion failed", NULL, ack_num); 
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
        // printf("server received %d/%d bytes: %s\n", (int)strlen(buf), n, buf);
        
        client_res_info client_info;
        client_info.sockfd = sockfd;
        client_info.clientaddr = clientaddr;
        client_info.clientlen = clientlen;

        if (n > 0) {
            int acknum = -1;


            if (strncmp(buf, "putfile:", 8) == 0) {
                // printf("PUTFILE handled separately. THIS IS BUF %s\n", buf);
                put_helper(buf, n, client_info); 
                continue; 
            }

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
            if (window_handler(acknum, &receiver_window) == 0) input_transfer(client_info, cmd_start, acknum);
            // else if (strncmp(cmd_start, "putfile:", 8) == 0) put_helper(buf, n, client_info); 
            else printf("Ignoring out-of-order ACK: %d\n", acknum);
        }
        else {
            fprintf(stderr, "Received NULL buffer.\n");
        }

        if (n < 0) 
            error("ERROR in sendto");
    }
}
