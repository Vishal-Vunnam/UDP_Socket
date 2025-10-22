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

#define BUFSIZE 1024
#define SWS 4
#define ARRAY_SIZE 10
#define ACK_TIMEOUT 10


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
int sender_ack_handler(char* buf, int buf_len, SWS_info *sender_window);
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
    snprintf(msg_with_seq, BUFSIZE, "%d | %s | %s", seq_num, msg_type, s_msg);

    if (sender_window->LFS - sender_window->LAR >= SWS) { 
        printf("Window full, cannot send frame %d now.\n", seq_num);
        return 0; 
    }

    int n = sendto(global_server_info.sockfd, msg_with_seq, strlen(msg_with_seq), 0, 
                   (struct sockaddr *)&global_server_info.serveraddr, global_server_info.serverlen);
    if (n < 0) {
        perror("ERROR sending frame");
        return -1;
    }

    sender_window->LFS = seq_num;
    sender_window->sendQ[seq_num].acked = 0;
    sender_window->sendQ[seq_num].send_time = time(NULL);
    strncpy(sender_window->sendQ[seq_num].msg, msg_with_seq, BUFSIZE);

    return n;
}

int handle_user_input(char *buf, int buf_len, SWS_info *sender_window) { 
    if (!command_valid(buf)) { 
        fprintf(stderr, "Invalid command: %s\n", buf);
        return -1;
    }


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
        printf("spinning here?");

        char file_buf[BUFSIZE - 64]; 
        size_t bytes_read = fread(file_buf, 1, sizeof(file_buf), global_read_fp); 
        printf("Read %zu bytes from file %s\n", bytes_read, filename);
        if (bytes_read > 0) { 
            char msg_type[124]; 
            snprintf(msg_type, sizeof(msg_type), "putfile:%s", filename);
            char msg[BUFSIZE]; 
            snprintf(msg, sizeof(msg), "%d | putfile:%s | %s", 
                     (sender_window->LFS + 1) % ARRAY_SIZE, filename, file_buf);


            printf("SENDING MESSAGE: %s\n", msg);
            
            // int n = sendto(
            //     global_server_info.sockfd, 
            //     msg, 
            //     strlen(msg), 
            //     0, 
            //     (struct sockaddr *) &global_server_info.serveraddr, 
            //     global_server_info.serverlen
            // );

            // sender_window->LFS = (sender_window->LFS + 1) % ARRAY_SIZE;
            // sender_window->sendQ[sender_window->LFS].acked = 0;
            // sender_window->sendQ[sender_window->LFS].send_time = time(NULL);
            // strncpy(sender_window->sendQ[sender_window->LFS].msg, msg, BUFSIZE);
            // sender_window->sendQ[sender_window->LFS].msg_len = strlen(msg); 

            int n = send_frame(file_buf, msg_type, bytes_read, sender_window);


            if (n < 0) {
                fprintf(stderr, "Failed to send file chunk for %s\n", filename);
                return -1; 
            }
        } else {
            if (feof(global_read_fp)) {
                printf("Finished sending file: %s\n", filename);
            } else {
                perror("fread");
            }
            fclose(global_read_fp);
            free(global_filename);
            global_filename = NULL;
            send_frame("EOF", "putfile_end", 12, sender_window);
            global_read_fp = NULL;
            return 0; 
        }
    }
}

// ---------RECEIVING ALGORITHMS----------
int check_ack_num(int ack_num, SWS_info *sender_window) { 
    if (ack_num < 0 || ack_num >= ARRAY_SIZE) {
        fprintf(stderr, "Invalid ACK number: %d\n", ack_num);
        return -1; 
    }
    printf("Checking ACK number: %d\n", ack_num);

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

int sender_ack_handler(char* buf, int buf_len, SWS_info *sender_window) { 
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
    if (check_ack_num(acknum, sender_window) < 0) { 
        return -1; 
    }

    // move pointer to payload after first '|'
    char *payload = sep + 1;
    while (isspace((unsigned char)*payload)) payload++;

    // If there's an ACK/type field (e.g. "ACK | gimmefile:..."), skip it and the next '|'
    if (strncmp(payload, "ACK", 3) == 0) {
        char *p = strchr(payload, '|');
        if (p) {
            payload = p + 1;
            while (isspace((unsigned char)*payload)) payload++;
        }
    }

    // Now payload should be the command, e.g. "gimmefile:foo1"
    if (strncmp(payload, "gimmefile:", 9) == 0) {
        char filename[256];
        char *fname = payload + 9;
        // skip any leading colons and whitespace so filename won't start with ':'
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
    }
    return 0;

}

void handle_timeout(SWS_info *sender_window, int sockfd, struct sockaddr_in serveraddr, int serverlen) {
    // placeholder to compile cleanly
    // (void)sender_window;
    // (void)sockfd;
    // (void)serveraddr;
    // (void)serverlen;
    for (int i = sender_window->LAR; i != sender_window->LFS; i = (i + 1) & ARRAY_SIZE) { 
        printf("Resending frame %d\n", i);
        sender_window->sendQ[i].send_time = time(NULL);
        int n = sendto(
            sockfd, 
            sender_window->sendQ[i].msg, 
            strlen(sender_window->sendQ[i].msg), 
            0, 
            (struct sockaddr *)&serveraddr, 
            serverlen
        );
        if (n < 0) {
            perror("ERROR resending frame");
        }

        sender_window->sendQ[i].send_time = time(NULL);
        sender_window->sendQ[i].acked = 0;
    }
}


int main(int argc, char **argv) { 
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    SWS_info sender_window; 
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
                sender_ack_handler(buf, n, &sender_window); 
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
