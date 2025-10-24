#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

#define BUFSIZE 1024
#define SWS 4
#define RWS 4
#define ARRAY_SIZE 10
#define ACK_TIMEOUT 3


void error(char *msg) { 
    perror(msg); 
    exit(0); 
}


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

typedef struct {    
    int LFR; 
    int LAF; 
    int frame_size; 
    struct recQ_slot { 
        int received; 
        char msg[BUFSIZE];
    } recQ[ARRAY_SIZE];
} RWS_info;

FILE *client_write_fp = NULL;
char *client_write_filename = NULL;
bool client_receiving = false;
bool client_sending = true;

typedef struct { 
    int sockfd; 
    struct sockaddr_in serveraddr; 
    int serverlen; 
} server_res_info; 


void init_SWS(SWS_info *sender_window, int frame_size);
void reset_timeout(struct timeval *timeout);
int command_valid(char *cmd);
int handle_user_input(char *buf, int buf_len, SWS_info *sender_window);
int send_next_file_chunk(SWS_info *sender_window, const char *filename);
int send_frame(char *s_msg, char *msg_type, int msg_len, SWS_info *sender_window);
int check_ack_num(int ack_num, SWS_info *sender_window);
int sender_ack_handler(char* buf, int buf_len, SWS_info *sender_window, RWS_info *receiver_window);
void handle_timeout(SWS_info *sender_window, int sockfd, struct sockaddr_in serveraddr, int serverlen);

// Global server response info
server_res_info global_server_info; 

// Global file pointer for reading file data
FILE *global_read_fp = NULL;
char *global_filename = NULL;

// Global file pointer for writing file data
FILE *global_write_fp = NULL;



void init_SWS(SWS_info *sender_window, int frame_size) { 
  sender_window->LAR = -1; 
  sender_window->LFS = -1; 
  sender_window->frame_ptr = 0; 
  sender_window->frame_size = frame_size; 
  for (int i = 0; i < ARRAY_SIZE; i++) { 
    sender_window->sendQ[i].acked = 1; 
    memset(sender_window->sendQ[i].msg, 0, BUFSIZE);
    sender_window->sendQ[i].msg_len = 0;
  }
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
// Command to reset select timeout
void reset_timeout(struct timeval *timeout) { 
  timeout->tv_sec = 1; 
  timeout->tv_usec = 0; 
}


// --------SENDING ALGORITHMS--------
int command_valid(char *cmd) {
    if (!cmd) return 0;
    while (isspace((unsigned char)*cmd)) cmd++;
    if (strncmp(cmd, "get ", 4) == 0 || strcmp(cmd, "get") == 0)
        return 1;
    if (strncmp(cmd, "put ", 4) == 0 || strcmp(cmd, "put") == 0)
        return 1;
    if (strncmp(cmd, "delete ", 7) == 0 || strcmp(cmd, "delete") == 0)
        return 1; 
    if (strncmp(cmd, "ls", 2) == 0 || strcmp(cmd, "ls") == 0)
        return 1; 
    if (strncmp(cmd, "exit", 4) == 0 || strcmp(cmd, "exit") == 0) 
        return 1; 
    return 0;
}



int send_frame(char *s_msg, char* msg_type, int msg_len, SWS_info *sender_window) {
    int seq_num = (sender_window->LFS + 1) % ARRAY_SIZE; 

    char msg_with_seq[BUFSIZE];
    int header_len = snprintf(msg_with_seq, BUFSIZE, "%d | %s | ", seq_num, msg_type);
    
    // Copy binary data after header
    memcpy(msg_with_seq + header_len, s_msg, msg_len);
    int total_len = header_len + msg_len;

    if (sender_window->LFS - sender_window->LAR >= SWS) { 
        printf("Window full, cannot send frame %d now.\n", seq_num);
        return 0; 
    }

    int n = sendto(global_server_info.sockfd, msg_with_seq, total_len, 0, 
                   (struct sockaddr *)&global_server_info.serveraddr, 
                   global_server_info.serverlen);
    if (n < 0) {
        perror("ERROR sending frame");
        return -1;
    }

    sender_window->LFS = seq_num;
    sender_window->sendQ[seq_num].acked = 0;
    sender_window->sendQ[seq_num].send_time = time(NULL);
    memcpy(sender_window->sendQ[seq_num].msg, msg_with_seq, total_len);
    sender_window->sendQ[seq_num].msg_len = total_len;

    return n;
}


int handle_user_input(char *buf, int buf_len, SWS_info *sender_window) { 
    if (!command_valid(buf)) { 
        fprintf(stderr, "Invalid command: %s\n", buf);
        return -1;
    }

    // // ✅ Set flag if it's a PUT command
    // if (strncmp(buf, "put ", 4) == 0 }}) {
    //     client_sending = true;
    // }

    int n = send_frame(buf, "COMMAND", buf_len, sender_window); 
    if (n < 0) {
        fprintf(stderr, "Failed to send frame: %s\n", buf);
        return -1;
    }

    return 0;
}

int send_next_file_chunk(SWS_info *sender_window, const char *filename) { 
    if (!global_read_fp) { 
        global_read_fp = fopen(filename, "rb"); 
        global_filename = strdup(filename);
        if (!global_read_fp) { 
            perror("fopen");
            return -1; 
        }
    }

    while(sender_window->LFS - sender_window->LAR < SWS) {
        char file_buf[BUFSIZE - 128]; // Leave room for header
        size_t bytes_read = fread(file_buf, 1, sizeof(file_buf), global_read_fp); 
        
        if (bytes_read > 0) { 
            char msg_type[124]; 
            snprintf(msg_type, sizeof(msg_type), "putfile:%s", filename);
            
            // ✅ Build header separately, then append binary data
            char msg[BUFSIZE];
            int seq_num = (sender_window->LFS + 1) % ARRAY_SIZE;
            int header_len = snprintf(msg, sizeof(msg), "%d | putfile:%s | ", 
                                     seq_num, filename);
            
            // Copy binary data after header
            memcpy(msg + header_len, file_buf, bytes_read);
            int total_len = header_len + bytes_read;

            // Send with actual byte count, not strlen
            if (sender_window->LFS - sender_window->LAR >= SWS) { 
                printf("Window full, cannot send frame %d now.\n", seq_num);
                return 0; 
            }

            int n = sendto(global_server_info.sockfd, msg, total_len, 0, 
                          (struct sockaddr *)&global_server_info.serveraddr, 
                          global_server_info.serverlen);
            
            if (n < 0) {
                fprintf(stderr, "Failed to send file chunk for %s\n", filename);
                return -1; 
            }

            // Update window state
            sender_window->LFS = seq_num;
            sender_window->sendQ[seq_num].acked = 0;
            sender_window->sendQ[seq_num].send_time = time(NULL);
            memcpy(sender_window->sendQ[seq_num].msg, msg, total_len);
            sender_window->sendQ[seq_num].msg_len = total_len;  // ✅ Store actual length

        } else {
            if (feof(global_read_fp)) {
                printf("Finished sending file: %s\n", filename);
            } else {
                perror("fread");
            }
            fclose(global_read_fp);
            free(global_filename);
            global_filename = NULL;
            send_frame("EOF", "putfile_end", 3, sender_window);  // Send actual length
            global_read_fp = NULL;
            return 0; 
        }
    }
    return 0;
}
// ---------RECEIVING ALGORITHMS----------
int check_ack_num(int ack_num, SWS_info *sender_window) { 
    if (ack_num < 0 || ack_num >= ARRAY_SIZE) {
        fprintf(stderr, "Invalid ACK number: %d\n", ack_num);
        return -1; 
    }
    printf("Checking ACK number: %d\n", ack_num);

    if (ack_num != (sender_window->LAR + 1) % ARRAY_SIZE) {
        fprintf(stderr, "ACK number %d not in expected range [%d, %d]\n", 
                ack_num, (sender_window->LAR + 1) % ARRAY_SIZE, sender_window->LFS);
        return -1; 
    }
    if (!sender_window->sendQ[ack_num].acked) { 
        sender_window->sendQ[ack_num].acked = 1; 
        printf("ACK received for frame %d\n", ack_num);

        while (sender_window->sendQ[(sender_window->LAR + 1) % ARRAY_SIZE].acked && 
               sender_window->LAR != sender_window->LFS) {
            sender_window->LAR = (sender_window->LAR + 1) % ARRAY_SIZE;
            printf("Sliding window: LAR now %d\n", sender_window->LAR);
        }
        return 0; 
    } else {

        if (ack_num > sender_window->LAR && ack_num <= sender_window->LFS) {
            printf("Duplicate ACK received for frame %d\n", ack_num);
            return -1; 
        }
        else { 
            fprintf(stderr, "ACK number %d out of current window [%d, %d]\n", ack_num, (sender_window->LAR + 1) % ARRAY_SIZE, sender_window->LFS);
            return -1;
        }
    }
}
int check_rec_ack_num(int ack_num, RWS_info *receiver_window) { 
    if (ack_num < 0 || ack_num >= ARRAY_SIZE) {
        fprintf(stderr, "Invalid received frame number: %d\n", ack_num);
        return -1; 
    }
    
    printf("Client checking received frame: %d (LFR=%d)\n", ack_num, receiver_window->LFR);
    
    if (ack_num == (receiver_window->LFR + 1) % ARRAY_SIZE) { 
        // Expected frame - accept it
        receiver_window->LFR = (receiver_window->LFR + 1) % ARRAY_SIZE; 
        return 0; 
    }
    else if (ack_num <= receiver_window->LFR) { 
        // Duplicate frame (already received)
        printf("Duplicate frame received: %d (already have up to %d)\n", ack_num, receiver_window->LFR);
        
        // Send ACK for duplicate
        char ack_response[BUFSIZE];
        snprintf(ack_response, sizeof(ack_response), "%d | ACK | duplicate", ack_num);
        sendto(global_server_info.sockfd, ack_response, strlen(ack_response), 0,
               (struct sockaddr *)&global_server_info.serveraddr, 
               global_server_info.serverlen);
        return 1;  // Duplicate, don't process
    }
    else {
        // Out of order (gap in sequence)
        printf("Out of order frame: %d, expected %d\n", ack_num, (receiver_window->LFR + 1) % ARRAY_SIZE);
        return -1;  // Drop it
    }
}
int sender_ack_handler(char* buf, int buf_len, SWS_info *sender_window, RWS_info *receiver_window) { 
    int acknum = -1; 
    char *sep = strstr(buf, "|");
    if (sep == NULL) {
        fprintf(stderr, "Malformed message (no '|'): %s\n", buf);
        return -1;
    }
    *sep = '\0';
    if (sscanf(buf, "%d", &acknum) != 1) {
        fprintf(stderr, "Malformed ACK number: %s\n", buf);
        return -1;
    }   
    printf("Received ACK for frame %d\n", acknum);
    
    // Only check ACK if client is currently sending (PUT operation)
    // else if (acknum != )



    if (!client_receiving) {
        if (check_ack_num(acknum, sender_window) < 0) { 
            return -1; 
        }
    }
    else { 
        if (check_rec_ack_num(acknum, receiver_window) != 0) { 
            return -1; 
        }
    }

    // move pointer to payload after first '|'
    char *payload = sep + 1;
    // ✅ Skip only single space after '|' if present
    if (*payload == ' ') payload++;

    if(strncmp(payload, "ACK:GET" , 7) == 0) {
        // Start receiving file data
        client_receiving = true;
        printf("Starting to receive file data from server.\n");
    }



    // If there's an ACK/type field (e.g. "ACK:GET | ..."), skip it and the next '|'
    char *ack_type_start = payload;
    if (strncmp(payload, "ACK", 3) == 0) {
        char *p = strchr(payload, '|');
        if (p) {
            payload = p + 1;
            if (*payload == ' ') payload++;  // ✅ Skip single space only
            ack_type_start = payload;
        }
    }


    // ===== HANDLE GET FILE DATA FROM SERVER =====
    if (strncmp(ack_type_start, "getfile:", 8) == 0) {
        client_receiving = true;
        
        // Extract filename from "getfile:filename"
        char filename[256];
        char *fname_start = ack_type_start + 8;
        
        // Find where filename ends (at the next '|' or space)
        char *fname_end = strchr(fname_start, '|');
        if (fname_end) {
            int fname_len = fname_end - fname_start;
            if (fname_len > 0 && fname_len < 256) {
                strncpy(filename, fname_start, fname_len);
                filename[fname_len] = '\0';
                
                // Trim any trailing spaces
                char *trim = filename + strlen(filename) - 1;
                while (trim > filename && isspace((unsigned char)*trim)) {
                    *trim = '\0';
                    trim--;
                }
            }
            
            // Find data after the '|'
            char *data_start = fname_end + 1;
            if (*data_start == ' ') data_start++;
            
            int data_len = buf_len - (data_start - buf);
            
            printf("Receiving file chunk for: %s (%d bytes)\n", filename, data_len);
            
            // Open file for writing if not already open
            if (!client_write_fp || !client_write_filename || 
                strcmp(client_write_filename, filename) != 0) {
                
                if (client_write_fp) {
                    fclose(client_write_fp);
                    free(client_write_filename);
                }
                
                client_write_fp = fopen(filename, "wb");
                client_write_filename = strdup(filename);
                
                if (!client_write_fp) {
                    perror("Failed to open file for writing");
                    return -1;
                }
            }
            
            // Write data to file
            if (client_write_fp && data_len > 0) {
                size_t written = fwrite(data_start, 1, data_len, client_write_fp);
                printf("Client wrote %zu bytes to %s\n", written, filename);
            }
            
            // Send ACK back to server
            char ack_response[BUFSIZE];
            snprintf(ack_response, sizeof(ack_response), "%d | ACK | received", acknum);
            int n = sendto(global_server_info.sockfd, ack_response, strlen(ack_response), 0,
                          (struct sockaddr *)&global_server_info.serveraddr, 
                          global_server_info.serverlen);
            if (n < 0) {
                perror("Failed to send ACK to server");
            }
        }
        return 0;
    }
    
    // ===== HANDLE GET FILE END =====
    if (strncmp(ack_type_start, "getfile_end", 11) == 0) {
        printf("File transfer complete!\n");
        
        if (client_write_fp) {
            fclose(client_write_fp);
            client_write_fp = NULL;
        }
        if (client_write_filename) {
            free(client_write_filename);
            client_write_filename = NULL;
        }
        
        client_receiving = false;
        
        // Send final ACK
        char ack_response[BUFSIZE];
        snprintf(ack_response, sizeof(ack_response), "%d | ACK | complete", acknum);
        sendto(global_server_info.sockfd, ack_response, strlen(ack_response), 0,
               (struct sockaddr *)&global_server_info.serveraddr, 
               global_server_info.serverlen);
        
        return 0;
    }

    // ===== HANDLE PUT OPERATION (EXISTING CODE) =====
    // Now strip whitespace ONLY for command parsing, not for binary data
    char *cmd = payload;
    while (isspace((unsigned char)*cmd)) cmd++;

    if (strncmp(cmd, "gimmefile:", 10) == 0) {
        client_sending = true;
        char filename[256];
        char *fname = cmd + 10;
        // skip any leading colons and whitespace for filename parsing
        while (*fname && (isspace((unsigned char)*fname) || *fname == ':')) fname++;
        if (sscanf(fname, "%255s", filename) == 1) {
            printf("Requested file: %s\n", filename);
            if (send_next_file_chunk(sender_window, filename) < 0) {
                fprintf(stderr, "Failed to send next file chunk for %s\n", filename);
            }
        } else {
            fprintf(stderr, "Malformed gimmefile payload: %s\n", payload);
        }
    }


    // IF READ FILE IS STILL OPEN, TRY TO SEND NEXT CHUNK
    if (global_read_fp) {
        if (send_next_file_chunk(sender_window, global_filename) < 0) {
            fprintf(stderr, "Failed to send next file chunk\n");
        }
    } else {
        // Done sending file
        client_sending = false;
    }
    
    return 0;
}



void handle_timeout(SWS_info *sender_window, int sockfd, struct sockaddr_in serveraddr, int serverlen) {
    for (int i = (sender_window->LAR + 1) % ARRAY_SIZE; 
         i != (sender_window->LFS + 1) % ARRAY_SIZE; 
         i = (i + 1) % ARRAY_SIZE) { 
        
        if (!sender_window->sendQ[i].acked) {
            printf("Resending frame %d\n", i);
            sender_window->sendQ[i].send_time = time(NULL);
            
            // ✅ Use stored msg_len instead of strlen
            int n = sendto(sockfd, 
                          sender_window->sendQ[i].msg, 
                          sender_window->sendQ[i].msg_len,  // Use actual length!
                          0, 
                          (struct sockaddr *)&serveraddr, 
                          serverlen);
            if (n < 0) {
                perror("ERROR resending frame");
            }
        }
    }
}


int main(int argc, char **argv) { 
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    SWS_info sender_window; 
    RWS_info receiver_window;
    char buf[BUFSIZE];

    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    serverlen = sizeof(serveraddr); 

    global_server_info.sockfd = sockfd; 
    global_server_info.serveraddr = serveraddr;
    global_server_info.serverlen = serverlen;

    init_SWS(&sender_window, BUFSIZE);
    init_RWS(&receiver_window, BUFSIZE);

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;
    struct timeval tv; 
    printf("Please enter msg: \n");
    fflush(stdout);

    while (1){ 
        if(sender_window.LFS > sender_window.LAR) { 
            int oldest_unacked = (sender_window.LAR + 1) % ARRAY_SIZE; 
            if (!sender_window.sendQ[oldest_unacked].acked &&
                sender_window.sendQ[oldest_unacked].send_time != 0 &&
                difftime(time(NULL), sender_window.sendQ[oldest_unacked].send_time) >= ACK_TIMEOUT)
            {
                printf("ACK timeout for frame %d, resending...\n", oldest_unacked);
                handle_timeout(&sender_window, sockfd, serveraddr, serverlen); 
            }
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        reset_timeout(&tv);

        int rv = select(maxfd + 1, &readfds, NULL, NULL, &tv); 
        if (rv > 0) { 
            if (FD_ISSET(sockfd, &readfds)){
                n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen); 
                if(n < 0) error("error in recvfrom");
                buf[n] = '\0';
                printf("------------------------ \n");
                printf("ECHO FROM SERVER: %s\n", buf); 
                printf("------------------------ \n");
                sender_ack_handler(buf, n, &sender_window, &receiver_window); 
                memset(buf, 0, BUFSIZE);
            }
            if (FD_ISSET(STDIN_FILENO, &readfds)) { 
                if (fgets(buf, BUFSIZE, stdin) != NULL) {
                    buf[strcspn(buf, "\n")] = '\0';
                    handle_user_input(buf, strlen(buf), &sender_window); 
                }
                printf("Please enter msg: \n ");
                fflush(stdout);
                memset(buf, 0, BUFSIZE);
            }
        }
        else if (rv == 0) { 
            // timeout
        }
        else { 
            perror("select error");
        }
    }
    return 0; 
}
