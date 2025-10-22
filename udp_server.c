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
} client_info;

typedef struct {    
    int LFR; 
    int LAF; 
    int frame_size; 
    struct recQ_slot { 
        int received; 
        char msg[BUFSIZE];
    } recQ[ARRAY_SIZE];
} RWS_info;


// Global client address info 
client_info global_client_info;

// Global file pointer for reading file data
FILE *global_read_fp = NULL;




void error(const char * msg) { 
    perror(msg); 
    exit(1); 
}

void init_RWS(RWS_info *receiver_window, int frame_size) { 
    receiver_window->LFR = -1; 
    receiver_window->LAF = -1;
    receiver_window->frame_size = frame_size; 
    for (int i = 0; i < ARRAY_SIZE; i++) { 
        receiver_window->recQ[i].received = 0; 
        memset(receiver_window->recQ[i].msg, 0, BUFSIZE);
    } 
}

void ack_sender(char *msg, char *ACK_type, int acknum) { 
    char send_buf[BUFSIZE];

    // Format: "<acknum> | <msg>"
    if (acknum < 0) snprintf(send_buf, sizeof(send_buf), "NA | %s", msg); 
    else snprintf(send_buf, sizeof(send_buf), "%d | %s | %s", acknum, ACK_type, msg);

    printf("SENDING: %s\n", send_buf);
    printf("TO: %s\n", inet_ntoa(global_client_info.clientaddr.sin_addr));
    printf("Length: %d\n", (int)strlen(send_buf)); 

    int n = sendto(
        global_client_info.sockfd, 
        send_buf, 
        strlen(send_buf), 
        0, 
        (struct sockaddr *) &global_client_info.clientaddr, 
        global_client_info.clientlen
    );

    if (n < 0) {
        perror("Failed to send response");
    }

}

void handle_ls(int frame_num) {
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
        free(dir_buff);
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

    ack_sender(response, "ACK:LS", frame_num);
    return; 
}

void handle_exit(int frame_num) { 
    printf("Client requested exit.\n");
    ack_sender("Goodbye!\n", "ACK:EXIT", frame_num); 
    exit(0);
}

void handle_delete(char *buf, int frame_num) {
    buf += 7; 
    buf[strcspn(buf, "\r\n ")] = '\0';

    printf("Requested to delete file: %s\n", buf); 

    if (remove(buf) == 0) {
        printf("File %s deleted successfully.\n", buf);
        ack_sender("File deleted successfully.", "ACK:DELETE", frame_num);
    } else {
        perror("File deletion failed");
        ack_sender("File deletion failed.", "ACK:DELETE", frame_num);
    }
}

void handle_put(char *buf, int frame_num) { 
    buf += 4; 
    buf[strcspn(buf, "\r\n ")] = '\0';

    printf("Requested to put file: %s\n", buf); 

    char response[BUFSIZE];
    snprintf(response, sizeof(response), "gimmefile:%s", buf);

    ack_sender(response, "ACK:PUT", frame_num); 

    (void)buf;
}


void handle_get(char *buf, int frame_num) { 
    if (buf == NULL) return;
    buf += 4; 

    buf[strcspn(buf, "\r\n")] = '\0'; 

    printf("Requested to get file: %s\n", buf); 

    char response[BUFSIZE]; 
    snprintf(response, sizeof(response), "putfile:%s", buf); 
    ack_sender(response, "ACK:GET", frame_num);

    (void)buf; 
}


int handle_frame_num(int frame_num, RWS_info *receiver_window) { 
    if(frame_num == (receiver_window->LFR + 1) % ARRAY_SIZE) { 
        receiver_window->LFR = (receiver_window->LFR + 1) % ARRAY_SIZE; 
        return 0; 
    }
    else { 
        printf("Out of order frame received: %d, expected %d\n", frame_num, (receiver_window->LFR + 1) % ARRAY_SIZE);
        return -1; 
    }
}


// --------CLIENT PUT HANDLER---------
// ...existing code...
void put_helper(char *filename, char *data_buf, int data_len, int frame_num){
    // open file for appending 
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        perror("fopen");
        return;
    }

    size_t written = fwrite(data_buf, 1, data_len, fp);
    fclose(fp);
    printf("Wrote %zu bytes to file: %s\n", written, filename);

    char gimme_buf[BUFSIZE];
    snprintf(gimme_buf, sizeof(gimme_buf), "gotfile:%s", filename);

    ack_sender(gimme_buf, "ACK:PUT", frame_num);
}



int client_input_handler(char *buf, int buf_len, RWS_info *receiver_window) { 
    printf("CLIENT BUFFER: %s\n", buf);
    if(buf == NULL || buf_len <= 0) return -1;
    // Input format: "<acknum> | <ack_type> | <msg>"
    char *first_delim = strchr(buf, '|');
    if (first_delim == NULL) {
        fprintf(stderr, "Malformed message (no first '|'): %s\n", buf);
        return -1;
    }

    // Null-terminate frame number and parse it
    *first_delim = '\0';
    int frame_num = -1;
    if (sscanf(buf, "%d", &frame_num) != 1) {
        fprintf(stderr, "Malformed frame number: %s\n", buf);
        return -1;
    }

    // Find second '|'
    char *second_delim = strchr(first_delim + 1, '|');
    if (second_delim == NULL) {
        fprintf(stderr, "Malformed message (no second '|'): %s\n", buf);
        return -1;
    }

    // Extract ack_type
    *second_delim = '\0';
    char *ack_type = first_delim + 1;

    // Trim leading/trailing spaces in ack_type
    while (isspace((unsigned char)*ack_type)) ack_type++;  // remove leading spaces
    char *end = ack_type + strlen(ack_type) - 1;
    while (end > ack_type && isspace((unsigned char)*end)) *end-- = '\0';

    // Extract msg (the remainder after second '|')
    char *msg = second_delim + 1;
    while (isspace((unsigned char)*msg)) msg++; // remove leading spaces

    // Now you have:
    printf("Frame number: %d\n", frame_num);
    printf("Ack type: '%s'\n", ack_type);
    printf("Message: '%s'\n", msg);

    if(strncmp(ack_type, "putfile:", 8) == 0) { 
        put_helper(ack_type + 8, msg, buf_len - (msg - buf), frame_num);
        return frame_num;
    }

    if(strncmp(ack_type, "putfile_end", 11) == 0) {
        printf("Finished receiving file data for frame %d\n", frame_num);
        ack_sender("File transfer complete.", "ACK:PUT_END", frame_num);
        return frame_num;
    }

    // Point cmd_start to the second '|' so later code can skip past ack_type
    char *cmd_start = second_delim;

    printf("Received frame number: %d\n", frame_num);
    printf("Recieved full message: %s\n", buf);
    // Check if frame number is expected
    if (handle_frame_num(frame_num, receiver_window) < 0) { 
        return -1; 
    }

    // Move to command part (skip '|' and spaces)
    cmd_start++;
    while (*cmd_start == ' ') cmd_start++;
    cmd_start[strcspn(cmd_start, "\n")] = '\0'; // remove newline
    printf("Command buffer: %s\n", cmd_start);  

    if(strcmp(cmd_start, "ls") == 0) { 
        handle_ls(frame_num); 
    } 
    else if (strcmp(cmd_start, "exit") == 0) {
        handle_exit(frame_num); 
    } 
    else if (strncmp(cmd_start, "get", 3) == 0) {
        handle_get(cmd_start, frame_num);
    } 
    else if (strncmp(cmd_start, "put", 3) == 0) {
        handle_put(cmd_start, frame_num);
    } 
    else if (strncmp(cmd_start, "delete", 6) == 0) {
        handle_delete(cmd_start, frame_num);
    } 
    else {
        fprintf(stderr, "Invalid request: %s\n", cmd_start);
        ack_sender("Invalid command", "ACK:INVALID", frame_num);
    }

    return frame_num; 
}

int main(int argc, char ** argv) { 
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
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) 
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
        
        if (n < 0) error("ERROR in recvfrom");

        hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
                              sizeof(clientaddr.sin_addr.s_addr), AF_INET);

        if (hostp == NULL)
            error("ERROR on gethostbyaddr");
        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL)
            error("ERROR on inet_ntoa");

        printf("server received datagram from %s:%d\n", 
               hostaddrp, ntohs(clientaddr.sin_port));

        global_client_info.sockfd = sockfd;
        global_client_info.clientaddr = clientaddr;
        global_client_info.clientlen = clientlen;

        if (n > 0) { 
            client_input_handler(buf, n, &receiver_window); 
        }
    }
}
