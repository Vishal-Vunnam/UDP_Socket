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
#include <stdbool.h>
#include <time.h> 

#define BUFSIZE 1024
#define RWS 4
#define SWS 4
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

typedef struct {
   int LAR; 
   int LFS; 
   int frame_ptr; 
   int frame_size;  
   struct sendQ_slot {
      char msg[BUFSIZE]; 
      int msg_len; 
      int acked; 
      time_t send_time; 
   } sendQ[ARRAY_SIZE];
} SWS_info; 

int server_send_file_chunk(SWS_info *sender_window, const char *filename, int current_frame);

bool server_sending = false;
SWS_info server_sender_window;  
char *server_filename = NULL;
FILE *server_read_fp = NULL;


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

void init_server_SWS(SWS_info *sender_window, int frame_size) { 
  sender_window->LAR = -1; 
  sender_window->LFS = -1; 
  sender_window->frame_size = frame_size; 
  for (int i = 0; i < ARRAY_SIZE; i++) { 
    sender_window->sendQ[i].acked = 1; 
    memset(sender_window->sendQ[i].msg, 0, BUFSIZE);
    sender_window->sendQ[i].msg_len = 0;
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
    buf[strcspn(buf, "\r\n ")] = '\0'; 

    // ack_sender("Preparing to send file.", "ACK:GET_PREPARE", frame_num);

    printf("Requested to get file: %s\n", buf); 

    // ACK the command
    ack_sender("Starting file transfer", "ACK:GET", frame_num);
    
    // Set flag and prepare to send
    server_sending = true;
    // init_server_SWS(&server_sender_window, BUFSIZE);
    // server_sender_window.LAR = frame_num;  // Start from current frame
    
    // Start sending file chunks
    server_send_file_chunk(&server_sender_window, buf, frame_num);
}


int handle_frame_num(int frame_num, RWS_info *receiver_window) { 
    if(frame_num == (receiver_window->LFR + 1) % ARRAY_SIZE) { 
        receiver_window->LFR = (receiver_window->LFR + 1) % ARRAY_SIZE; 
        return 0; 
    }
    else if (frame_num >= (receiver_window->LFR + 1 - RWS) % ARRAY_SIZE && frame_num <= (receiver_window->LFR) % ARRAY_SIZE) { 
        printf("Duplicate frame received: %d\n", frame_num);
        ack_sender("Duplicate frame", "ACK:DUPLICATE", frame_num);
        return 1; 
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

    ack_sender(gimme_buf, "ACK:PUT_Confirm", frame_num);
}

int server_send_file_chunk(SWS_info *sender_window, const char *filename, int current_frame) {
    if (!server_read_fp) { 
        server_read_fp = fopen(filename, "rb"); 
        server_filename = strdup(filename);
        if (!server_read_fp) { 
            perror("fopen");
            return -1; 
        }
    }

    // Send up to window size
    while(sender_window->LFS - sender_window->LAR < SWS) {
        char file_buf[BUFSIZE - 128];
        size_t bytes_read = fread(file_buf, 1, sizeof(file_buf), server_read_fp); 
        
        if (bytes_read > 0) { 
            char msg[BUFSIZE];
            // ✅ FIX: Use sender_window->LFS, not current_frame
            int seq_num = (sender_window->LFS + 1) % ARRAY_SIZE;
            int header_len = snprintf(msg, sizeof(msg), "%d | getfile:%s | ", 
                                     seq_num, filename);
            
            memcpy(msg + header_len, file_buf, bytes_read);
            int total_len = header_len + bytes_read;

            if (sender_window->LFS - sender_window->LAR >= SWS) { 
                printf("Server window full, cannot send frame %d now.\n", seq_num);
                return 0; 
            }

            int n = sendto(global_client_info.sockfd, msg, total_len, 0, 
                          (struct sockaddr *)&global_client_info.clientaddr, 
                          global_client_info.clientlen);
            
            if (n < 0) {
                perror("ERROR sending file chunk from server");
                return -1; 
            }

            printf("Server sent frame %d with %zu bytes\n", seq_num, bytes_read);
            
            // Update server's sending window
            sender_window->LFS = seq_num;
            sender_window->sendQ[seq_num].acked = 0;
            sender_window->sendQ[seq_num].send_time = time(NULL);
            memcpy(sender_window->sendQ[seq_num].msg, msg, total_len);
            sender_window->sendQ[seq_num].msg_len = total_len;

        } else {
            if (feof(server_read_fp)) {
                printf("Server finished sending file: %s\n", filename);
            } else {
                perror("fread");
            }
            
            // Send EOF marker
            int seq_num = (sender_window->LFS + 1) % ARRAY_SIZE;
            char msg[BUFSIZE];
            int msg_len = snprintf(msg, sizeof(msg), "%d | getfile_end | EOF", seq_num);
            
            sendto(global_client_info.sockfd, msg, msg_len, 0,
                  (struct sockaddr *)&global_client_info.clientaddr, 
                  global_client_info.clientlen);
            
            // ✅ Update window for EOF frame too
            sender_window->LFS = seq_num;
            sender_window->sendQ[seq_num].acked = 0;
            sender_window->sendQ[seq_num].send_time = time(NULL);
            memcpy(sender_window->sendQ[seq_num].msg, msg, msg_len);
            sender_window->sendQ[seq_num].msg_len = msg_len;
            
            fclose(server_read_fp);
            free(server_filename);
            server_read_fp = NULL;
            server_filename = NULL;
            // ✅ Don't set server_sending = false yet, wait for EOF ACK
            
            return 0; 
        }
    }
    return 0;
}

#define ACK_TIMEOUT 3

void server_handle_timeout(SWS_info *sender_window) {
    printf("=== SERVER TIMEOUT: Resending all unacked frames ===\n");
    printf("Server Window: LAR=%d, LFS=%d\n", sender_window->LAR, sender_window->LFS);
    
    for (int i = (sender_window->LAR + 1) % ARRAY_SIZE; 
         i != (sender_window->LFS + 1) % ARRAY_SIZE; 
         i = (i + 1) % ARRAY_SIZE) { 
        
        if (!sender_window->sendQ[i].acked) {
            printf("Server resending frame %d (length: %d)\n", i, sender_window->sendQ[i].msg_len);
            
            int n = sendto(global_client_info.sockfd, 
                          sender_window->sendQ[i].msg, 
                          sender_window->sendQ[i].msg_len,
                          0, 
                          (struct sockaddr *)&global_client_info.clientaddr, 
                          global_client_info.clientlen);
            if (n < 0) {
                perror("ERROR: Server resending frame");
            } else {
                sender_window->sendQ[i].send_time = time(NULL);
            }
        }
        
        // Stop when we've checked LFS
        if (i == sender_window->LFS) break;
    }
}

int server_check_ack(int ack_num, SWS_info *sender_window) {
    if (ack_num < 0 || ack_num >= ARRAY_SIZE) {
        fprintf(stderr, "Server: Invalid ACK number: %d\n", ack_num);
        return -1; 
    }
    
    printf("Server checking ACK: %d\n", ack_num);
    
    // if (!sender_window->sendQ[ack_num].acked) { 
        sender_window->sendQ[ack_num].acked = 1; 
        printf("Server: ACK received for frame %d\n", ack_num);

        // Slide window forward
        // while (sender_window->sendQ[(sender_window->LAR + 1) % ARRAY_SIZE].acked && 
        //        sender_window->LAR != sender_window->LFS) {
        //     sender_window->LAR = (sender_window->LAR + 1) % ARRAY_SIZE;
        //     printf("Server window sliding: LAR now %d\n", sender_window->LAR);
        // }

        if (ack_num == (sender_window->LAR + 1) % ARRAY_SIZE) {
            sender_window->LAR = ack_num; 
        }
        else { 
            printf("Server: ACK %d received out of order, LAR=%d\n", ack_num, sender_window->LAR);
            return -1; 
        }

        return 0; 
    // } else {
    //     printf("Server: Duplicate ACK for frame %d\n", ack_num);
    //     return 0;  // Not an error
    // }
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

    // Trim leading/trailing spaces in ack_type ONLY
    while (isspace((unsigned char)*ack_type)) ack_type++;
    char *end = ack_type + strlen(ack_type) - 1;
    while (end > ack_type && isspace((unsigned char)*end)) *end-- = '\0';

    // Extract msg - DON'T strip whitespace for binary data!
    char *msg = second_delim + 1;
    // ✅ Skip only the single space after '|' if present
    if (*msg == ' ') msg++;
    
    // Calculate actual data length
    int data_len = buf_len - (msg - buf);

    printf("Frame number: %d\n", frame_num);
    printf("Ack type: '%s'\n", ack_type);
    printf("Data length: %d bytes\n", data_len);

    // Check if frame number is expected
    if(!server_sending){
        if (handle_frame_num(frame_num, receiver_window) < 0) { 
            return -1; 
        }
    }

    // Handle binary file data
    if(strncmp(ack_type, "putfile:", 8) == 0) { 
        put_helper(ack_type + 8, msg, data_len, frame_num);
        return frame_num;
    }

    if(strncmp(ack_type, "putfile_end", 11) == 0) {
        printf("Finished receiving file data for frame %d\n", frame_num);
        ack_sender("File transfer complete.", "ACK:PUT_END", frame_num);
        return frame_num;
    }

    if (server_sending) {
        // Check if this is an ACK message
        if (strncmp(ack_type, "ACK", 3) == 0) {
            // Client ACKed our data frame
            if (server_check_ack(frame_num, &server_sender_window) == 0) {
                // Continue sending if file still open
                if (server_read_fp) {
                    server_send_file_chunk(&server_sender_window, server_filename, frame_num);
                }
                if (strncmp(msg, "complete", 8) == 0) { 
                    server_sending = false;
                }
            }
            return frame_num;
        }
    }

    // For text commands, NOW we can strip whitespace
    while (isspace((unsigned char)*msg)) msg++;
    msg[strcspn(msg, "\n")] = '\0';
    printf("Command buffer: %s\n", msg);  

    if(strcmp(msg, "ls") == 0) { 
        handle_ls(frame_num); 
    } 
    else if (strcmp(msg, "exit") == 0) {
        handle_exit(frame_num); 
    } 
    else if (strncmp(msg, "get", 3) == 0) {
        handle_get(msg, frame_num);
    } 
    else if (strncmp(msg, "put", 3) == 0) {
        handle_put(msg, frame_num);
    } 
    else if (strncmp(msg, "delete", 6) == 0) {
        handle_delete(msg, frame_num);
    } 
    else {
        fprintf(stderr, "Invalid request: %s\n", msg);
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
    init_server_SWS(&server_sender_window, BUFSIZE);

    printf("Server listening on port %d\n", portno);
    clientlen = sizeof(clientaddr);
    
    // ✅ ADD: File descriptor set and timeout for select()
    fd_set readfds;
    struct timeval tv;
    
    while (1) {
        // ✅ CHECK TIMEOUT when server is sending
        if (server_sending && server_sender_window.LFS > server_sender_window.LAR) {
            int oldest_unacked = (server_sender_window.LAR + 1) % ARRAY_SIZE;
            if (!server_sender_window.sendQ[oldest_unacked].acked &&
                server_sender_window.sendQ[oldest_unacked].send_time != 0 &&
                difftime(time(NULL), server_sender_window.sendQ[oldest_unacked].send_time) >= 3)
            {
                printf("Server: ACK timeout for frame %d, resending...\n", oldest_unacked);
                server_handle_timeout(&server_sender_window);
            }
        }
        
        // ✅ Setup select() instead of blocking recvfrom()
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        
        if (rv > 0 && FD_ISSET(sockfd, &readfds)) {
            // Data available
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
        } else if (rv == 0) {
            // Timeout - just continue loop to check for retransmission needs
        } else {
            perror("select error");
        }
    }
}