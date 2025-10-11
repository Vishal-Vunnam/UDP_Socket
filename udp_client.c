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

#define BUFSIZE 1024
#define SWS 4
#define ARRAY_SIZE 25
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

void get_handler(char *buf) {
    if (!buf) return;

    // Make a modifiable copy
    char line[8192];
    strncpy(line, buf, sizeof(line));
    line[sizeof(line) - 1] = '\0';

    // Split by '|'
    strtok(line, "|");               // skip ACK num
    char *filename = strtok(NULL, "|");
    char *filedata = strtok(NULL, "");

    if (!filename || !filedata) {
        fprintf(stderr, "Invalid format: %s\n", buf);
        return;
    }

    // Trim leading/trailing spaces
    while (isspace((unsigned char)*filename)) filename++;
    char *end = filename + strlen(filename) - 1;
    while (end > filename && isspace((unsigned char)*end)) *end-- = '\0';

    while (isspace((unsigned char)*filedata)) filedata++;
    end = filedata + strlen(filedata) - 1;
    while (end > filedata && isspace((unsigned char)*end)) *end-- = '\0';

    // Append data to file
    FILE *fp = fopen(filename, "ab");  // "ab" = append binary (works for text too)
    if (!fp) {
        perror("fopen");
        return;
    }

    fprintf(fp, "%s", filedata);
    fclose(fp);

    printf("Appended data to file: %s\n", filename);
}


void reset_timeout(struct timeval *timeout) { 
  timeout->tv_sec = 1; 
  timeout->tv_usec = 0; 
}

int handle_ACK(char* buf, SWS_info *sender_window) { 
    int acknum = -1;

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
        printf("Sliding window forward. New LAR: %d\n", sender_window->LAR);
        return 0;
    }
    else { 
        get_handler(buf); 
        printf("Received out-of-order ACK: %d (expected %d)\n", acknum, (sender_window->LAR + 1) % ARRAY_SIZE);
        return -1;
    }
    

}
int handle_timeout(SWS_info *sender_window, int sockfd, struct sockaddr_in server_addr, int serverlen) { 
  printf("Timeout occured, resending all frames in a periof");
    int start = sender_window->LAR + 1; 

    for (int i = start; i < start + SWS; i++) { 
        char *msg = sender_window->sendQ[i % SWS].msg;

        if (msg == NULL || msg[0] == '\0') {
            continue;  // skip empty entries
        }
        sender_window->sendQ[i % SWS].send_time = time(NULL); 
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
        sender_window->LFS++; 

        int n = sendto(sockfd, msg_with_seq, strlen(msg_with_seq), 0, 
                      (struct sockaddr *)&server_addr, serverlen);
        if (n < 0) { 
            perror("ERROR in sendto");
            return -1;
        }

        sender_window->frame_ptr = (sender_window->frame_ptr + 1) % SWS; 
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
      printf("Restarting loop \n"); 
      printf("LAR: %d, LFS: %d, frame_ptr: %d \n", sender_window.LAR, sender_window.LFS, sender_window.frame_ptr);
      printf("Window: \n");
      for (int i = 0; i < SWS; i++) { 
        printf("Frame %d: %s, acked: %d \n", i, sender_window.sendQ[i].msg, sender_window.sendQ[i].acked); 
      }

      if (sender_window.LFS > sender_window.LAR) {
          // There are outstanding unacked frames
          int oldest_unacked = (sender_window.LAR + 1) % ARRAY_SIZE;
          if (sender_window.sendQ[oldest_unacked].send_time != 0 && 
              difftime(time(NULL), sender_window.sendQ[oldest_unacked].send_time) >= ACK_TIMEOUT) {
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

              handle_ACK(buf, &sender_window); 
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
          if (sender_window.LFS > sender_window.LAR) {
              handle_timeout(&sender_window, sockfd, serveraddr, serverlen);
          }
      }
      else {  // rv < 0, actual error
          perror("select"); 
      }
    }
}
