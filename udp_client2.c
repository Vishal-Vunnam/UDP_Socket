/* 
 * udpclient.c - A simple UDP client
 * usage: udpclient <host> <port>
 */
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
/* 
 * error - wrapper for perror
 */
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
      int acked; 
      time_t send_time; 
   } sendQ[ARRAY_SIZE];
} SWS_info; 


typedef struct { 
    int sockfd; 
    struct sockaddr_in serveraddr; 
    int serverlen; 
} server_res_info; 

void init_SWS(SWS_info *sender_window, int frame_size) { 
  sender_window -> LAR = -1; 
  sender_window -> LFS = -1; 
  sender_window -> frame_ptr = 0; 
  sender_window -> frame_size = frame_size; 
  for (int i = 0; i < ARRAY_SIZE; i++) { 
    sender_window -> sendQ[i].acked = 1; 
    memset(sender_window->sendQ[i].msg, 0, BUFSIZE);
  }
}

static int expected_frame;

void get_handler(char *buf, ssize_t buf_len, server_res_info server_info) {
    if (!buf || buf_len <= 0) return;

    // Find first '|' (end of filename)
    char *sep1 = memchr(buf, '|', buf_len);
    if (!sep1) {
        fprintf(stderr, "Invalid packet (missing first '|')\n");
        return;
    }

    // Find second '|' (end of frame header)
    char *sep2 = memchr(sep1 + 1, '|', buf_len - (sep1 + 1 - buf));
    if (!sep2) {
        fprintf(stderr, "Invalid packet (missing second '|')\n");
        return;
    }

    // Extract filename safely
    char filename[256];
    memset(filename, 0, sizeof(filename));

    char *prefix = strstr(buf, "putfile:");
    if (prefix) {
        size_t name_len = sep1 - (prefix + 8); // skip "putfile:"
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, prefix + 8, name_len);
    } else {
        size_t name_len = sep1 - buf;
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, buf, name_len);
    }

    // Trim trailing whitespace
    char *end = filename + strlen(filename) - 1;
    while (end > filename && isspace((unsigned char)*end)) *end-- = '\0';

    // Extract frame number between sep1 and sep2
    int frame_num = -1;
    char frame_header[64];
    size_t frame_len = sep2 - (sep1 + 1);
    if (frame_len > 0 && frame_len < sizeof(frame_header)) {
        memcpy(frame_header, sep1 + 1, frame_len);
        frame_header[frame_len] = '\0';
        if (strncmp(frame_header, "frame:", 6) == 0) {
            frame_num = atoi(frame_header + 6);
        }
    }

    // Ignore out-of-order or duplicate frames
    if (frame_num != expected_frame) {
        printf("Ignoring out-of-order frame %d (expected %d)\n", frame_num, expected_frame);
        return;
    }

    // Send ACK immediately
    char ack_msg[64];
    snprintf(ack_msg, sizeof(ack_msg), "GOTIT(CLIENT):frame:%d", frame_num);
    ssize_t n = sendto(
        server_info.sockfd,
        ack_msg,
        strlen(ack_msg),
        0,
        (struct sockaddr *)&server_info.serveraddr,
        server_info.serverlen
    );
    if (n < 0)
        perror("Failed to send ACK");
    else
        printf("Sent ACK for frame %d (%s)\n", frame_num, filename);

    // Increment expected frame for next one
    expected_frame++;

    // File data starts right after sep2
    char *filedata = sep2 + 1;
    size_t data_len = buf_len - (filedata - buf);

    printf("Appending %zu bytes to file: %s\n", data_len, filename);

    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        perror("fopen");
        return;
    }

    fwrite(filedata, 1, data_len, fp);
    fclose(fp);
}


void put_handler(server_res_info server_info, char *filename, int ack_num) { 
    filename[strcspn(filename, "\r\n ")] = '\0';
    printf("sendingfile: %s\n", filename); 

    FILE *file_ptr = fopen(filename, "rb"); 


    if (!file_ptr) {
        printf("FILE WAS NOT FOUND\n"); 
        perror("File not found");
        return;
    }

    fseek(file_ptr, 0, SEEK_END); 
    long file_size = ftell(file_ptr); 
    fseek(file_ptr, 0, SEEK_SET);

    printf("Sending file: %s (%ld bytes)\n", filename, file_size);

    char file_buffer[BUFSIZE]; 
    char ack_buffer[BUFSIZE]; 
    char last_buffer[BUFSIZE];
    size_t bytes_read; 
    int fram_num = 0; 

    while((bytes_read = fread(file_buffer, 1, BUFSIZE - 100, file_ptr)) > 0) { 
        int n = recvfrom( 
            server_info.sockfd, 
            file_buffer, 
            sizeof(file_buffer), 
            0,
            (struct sockaddr *) &server_info.serveraddr, 
            (socklen_t *) &server_info.serverlen
        ); 
        if (n < 0) { 
            if (errno == EWOULDBLOCK || errno == EAGAIN) { 
                printf("Timeout waiting for ACK\n"); 
                // resend last packet
                if (fram_num > 0) { 
                    printf("Resending frame %d\n", fram_num - 1); 
                    int resend_n = sendto( 
                        server_info.sockfd, 
                        file_buffer, 
                        strlen(file_buffer), 
                        0, 
                        (struct sockaddr *) &server_info.serveraddr, 
                        server_info.serverlen
                    ); 
                    if (resend_n < 0) { 
                        perror("sendto failed"); 
                        break; 
                    } 
                } 
                continue; 
            } else { 
                perror("recvfrom failed"); 
                break; 
            } 
        }
        else if (n > 0) {
            if (n < BUFSIZE) ack_buffer[n] = '\0'; 
            if (strncasecmp(ack_buffer, "GOTIT(SERVER):", 14) == 0) { 
                printf("Recieved ACK from server: %s\n", ack_buffer); 
            }
            else {
                printf("Received unexpected message: %s\n", ack_buffer); 
                return; 
            }
        }
        
        printf("READ %zu bytes from file\n", bytes_read); 
        char packet[BUFSIZE]; 

        // Build the header: "putfile:filename | "
        int header_len = snprintf(packet, sizeof(packet), "putfile:%s |", filename); 
        
        // CRITICAL: Don't use snprintf for the data part - it stops at null bytes!
        // Just copy the binary data directly after the header
        memcpy(packet + header_len, file_buffer, bytes_read);
        int total_len = header_len + bytes_read;

        memcpy(last_buffer, packet, total_len); 

        // Send the complete packet with actual byte count
        n = sendto(
            server_info.sockfd, 
            packet, 
            total_len,  // Use actual length, not strlen()
            0, 
            (struct sockaddr *) &server_info.serveraddr, 
            server_info.serverlen
        );

        if (n < 0) { 
            perror("sendto failed"); 
            break; 
        } 

        printf("Sent frame %d (%zu bytes data + %d bytes header = %d total)\n", 
               fram_num, bytes_read, header_len, total_len);
        fram_num++;

        // usleep(1000); 
    }
    fclose(file_ptr);

    // Send end-of-file marker
    char end_msg[64];
    snprintf(end_msg, sizeof(end_msg), "%d|END", ack_num);
    int n = sendto(
        server_info.sockfd, 
        end_msg, 
        strlen(end_msg), 
        0, 
        (struct sockaddr *) &server_info.serveraddr, 
        server_info.serverlen
    );  

    printf("CLIENT: File Transfer complete: %d frames sent\n", fram_num);
}

void reset_timeout(struct timeval *timeout) { 
  timeout->tv_sec = 1; 
  timeout->tv_usec = 0; 
}

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

int handle_ACK(char* buf, SWS_info *sender_window, server_res_info sender_info, int n) { 
    int acknum = -1;
    char * putfile = NULL; 

    // IF the packet is a putfile command, handle differently
    if (strncmp(buf, "putfile:", 8) == 0) {
        char filename[256];
        if (sscanf(buf + 8, "%255[^|]", filename) == 1) {  // read until '|'
            printf("Received PUTFILE command. Filename: %s\n", filename);
            get_handler(buf, n, sender_info);
            return 0;
        } else {
            fprintf(stderr, "Malformed PUTFILE packet: %s\n", buf);
            return -1;
        }
    }

    // if (strncmp(buf, gimme))

    // 1. Try to extract the ack number (up to '|')
    if (sscanf(buf, "%d", &acknum) != 1) {
        fprintf(stderr, "Malformed ACK: %s\n", buf);
        return -1;
    }

    printf("Received ACK for frame: %d\n", acknum);




    if (acknum < 0 || acknum >= ARRAY_SIZE) { 
        fprintf(stderr, "ACK number out of range: %d\n", acknum);
        return -1;
    }
    if (acknum == (sender_window->LAR + 1) % ARRAY_SIZE) { 
        sender_window->sendQ[acknum].acked = 1; 
        sender_window->LAR = acknum; 
        sender_window->sendQ[acknum].send_time = 0; 

        printf("Sliding window forward. New LAR: %d\n", sender_window->LAR);



        // Check if server is requesting a file
        char *sep = strchr(buf, '|');
        if (sep != NULL) {
            sep++; // move past '|'
            while (isspace((unsigned char)*sep)) sep++; // skip spaces

            if (strncmp(sep, "gimmefile:", 10) == 0) {
                char filename[256];
                if (sscanf(sep + 10, "%255s", filename) == 1) {
                    printf("Received GIMMEFILE request for: %s\n", filename);
                    put_handler(sender_info, filename, acknum); 
                } else {
                    fprintf(stderr, "Malformed GIMMEFILE request: %s\n", sep);
                    return -1;
                }
            }
        }
        return 0;

    }
    else { 
        get_handler(buf, n, sender_info); 
        printf("Received out-of-order ACK: %d (expected %d)\n", acknum, (sender_window->LAR + 1) % ARRAY_SIZE);
        return -1;
    }
}

int handle_timeout(SWS_info *sender_window, int sockfd, struct sockaddr_in server_addr, int serverlen) { 
    printf("Timeout occurred, resending all frames in window\n");
    int start = (sender_window->LAR + 1) % ARRAY_SIZE;  // Fix: add modulo
    
    // Only resend frames between LAR+1 and LFS
    for (int i = 0; i < SWS && (sender_window->LAR + 1 + i) <= sender_window->LFS; i++) { 
        int frame_idx = (start + i) % ARRAY_SIZE;  // Fix: use proper indexing
        char *msg = sender_window->sendQ[frame_idx].msg;

        if (msg == NULL || msg[0] == '\0' || sender_window->sendQ[frame_idx].acked) {
            continue;  // skip empty or already acked entries
        }
        
        printf("Resending frame %d\n", frame_idx);
        sender_window->sendQ[frame_idx].send_time = time(NULL); 
        int n = sendto(sockfd, msg, strlen(msg), 0, (struct sockaddr *)&server_addr, serverlen);
        if (n < 0) { 
            error("ERROR in sendto"); 
            return -1; 
        }
    }

    return 0; 
}

int send_frame(char *s_msg, SWS_info *sender_window, int sockfd, struct sockaddr_in server_addr, int serverlen) { 
    // compute next sequence number
    int seq_num = (sender_window->LFS + 1) % ARRAY_SIZE; 
    // construct message buffer
    char msg_with_seq[BUFSIZE];
    snprintf(msg_with_seq, sizeof(msg_with_seq), "%d | %s ", seq_num,  s_msg);

    printf("preparing to send: %s\n", msg_with_seq);
    strcpy(sender_window->sendQ[sender_window->frame_ptr].msg, msg_with_seq);
    sender_window->sendQ[sender_window->frame_ptr].acked = 0; 
    sender_window->sendQ[sender_window->frame_ptr].send_time = time(NULL);


    
    if (sender_window->LFS - sender_window->LAR < SWS) { 
        sender_window->LFS = seq_num; 
        


        int n = sendto(sockfd, msg_with_seq, strlen(msg_with_seq), 0, 
                      (struct sockaddr *)&server_addr, serverlen);
        if (n < 0) { 
            perror("ERROR in sendto");
            return -1;
        }

        sender_window->frame_ptr = (sender_window->frame_ptr + 1) % ARRAY_SIZE;; 
        return n; 
    }
    return 0;
}


int main(int argc, char **argv) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    SWS_info sender_window; 
    char buf[BUFSIZE];

    /* check command line arguments */
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }


    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    serverlen = sizeof(serveraddr); 

    server_res_info server_info; 
    server_info.sockfd = sockfd; 
    server_info.serveraddr = serveraddr; 
    server_info.serverlen = serverlen;


    // Initialize Sender Window
    init_SWS(&sender_window, BUFSIZE); 


    /* get a message from the user */
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

    struct timeval tv; 
    printf("Please enter msg: \n");
    fflush(stdout);

    while (1){ 
    //   printf("Restarting loop \n"); 
    //   printf("LAR: %d, LFS: %d, frame_ptr: %d \n", sender_window.LAR, sender_window.LFS, sender_window.frame_ptr);
    //   printf("Window: \n");
    //   for (int i = 0; i < ARRAY_SIZE; i++) { 
    //     printf("Frame %d: %s, acked: %d \n", i, sender_window.sendQ[i].msg, sender_window.sendQ[i].acked); 
    //   }

      if (sender_window.LFS > sender_window.LAR) {
          // There are outstanding unacked frames
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
      int rv = select(maxfd+1, &readfds, NULL, NULL, &tv);
      if (rv > 0) {
          if (FD_ISSET(sockfd, &readfds)){
              int n = recvfrom(sockfd, buf, BUFSIZE, 0, &serveraddr, &serverlen);
              if (n < 0) error("recvfrom");
              buf[n] = '\0';  // <-- Important
              printf("------------------------ \n");
              printf("ECHO FROM SERVER: %s\n", buf); 
              printf("------------------------ \n"); 

              handle_ACK(buf, &sender_window, server_info, n); 
              memset(buf,0, BUFSIZE); 
          }
          if (FD_ISSET(STDIN_FILENO, &readfds)) {
              if (fgets(buf, BUFSIZE, stdin) != NULL) {
                  serverlen = sizeof(serveraddr);
                  buf[strcspn(buf, "\n")] = '\0';
                  send_frame(buf, &sender_window, sockfd, serveraddr, serverlen);
              }
              printf("Please enter msg: \n ");
              fflush(stdout);
              memset(buf, 0, BUFSIZE);
          }
      }  // ← Close the "if (rv > 0)" block HERE
      else if (rv == 0) {  // ← NOW this is at the right level
        //   if (sender_window.LFS > sender_window.LAR) {
        //       handle_timeout(&sender_window, sockfd, serveraddr, serverlen);
        //   }
      }
      else {  // rv < 0, actual error
          perror("select"); 
      }
    }
}
