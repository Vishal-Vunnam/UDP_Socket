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

#define BUFSIZE 1024

/*
 * error - wrapper for perror
 */

typedef struct {
    int sockfd;
    struct sockaddr_in clientaddr;
    int clientlen;
} client_res_info;

void error(char *msg) {
  perror(msg);
  exit(1);
}

int sender(client_res_info response_info, char *response, char *return_code) { 
    int n = sendto(
        response_info.sockfd, 
        response, 
        strlen(response), 
        0, 
        (struct sockaddr *) &response_info.clientaddr, 
        response_info.clientlen
    );

    if (n < 0) {
        error("Failed to send response");
        return -1;
    }

    // Optionally handle return_code if needed later
    // (e.g., strcpy(return_code, "200 OK");)

    return n;
}

int input_transfer(client_res_info response_info, char *buf) {
    char command[25];

    buf[strcspn(buf, "\n")] = '\0';

    if (strcmp(buf, "ls") == 0) {
        handle_ls(response_info, buf);
    } 
    else if (strcmp(buf, "exit") == 0) {
        handle_exit(response_info, buf);
    } 
    else {
        sscanf(buf, "%24s", command);

        if (strcmp(command, "get") == 0) {
            handle_get(response_info, buf);
        } 
        else if (strcmp(command, "put") == 0) {
            handle_put(response_info, buf);
        } 
        else if (strcmp(command, "delete") == 0) {
            handle_delete(response_info, buf);
        } 
        else {
            error("Request type was invalid");
        }
    }

    return 0;
}

void handle_get(client_res_info response_info, char *buf) {
    // Example: parse "get filename"
    printf("filename", filename);
    if (filename != NULL)
        filename += 4; // skip "get "
    else
        return;

    // TODO: implement file retrieval logic
}

void handle_put(client_res_info response_info, char *buf) {
    // TODO: implement file upload logic
}

void handle_delete(client_res_info response_info, char *buf) {
    // TODO: implement delete logic
}

void handle_ls(client_res_info response_info, char *buf) {
    // TODO: implement directory listing logic
}

void handle_exit(client_res_info response_info, char *buf) {
    // TODO: implement client disconnect logic
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int sockfd; /* socket */
  int portno; /* port to listen on */
  int clientlen; /* byte size of client's address */
  struct sockaddr_in serveraddr; /* server's addr */
  struct sockaddr_in clientaddr; /* client addr */
  struct hostent *hostp; /* client host info */
  char buf[BUFSIZE]; /* message buf */
  char *hostaddrp; /* dotted decimal host addr string */
  int optval; /* flag value for setsockopt */
  int n; /* message byte size */

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

  printf("Server listening on port %d", portno);


  clientlen = sizeof(clientaddr);
  while (1) {
    /*
     * recvfrom: receive a UDP datagram from a client
     */
    bzero(buf, BUFSIZE);
    n = recvfrom(sockfd, buf, BUFSIZE, 0,
		 (struct sockaddr *) &clientaddr, &clientlen);
    if (n < 0)
      error("ERROR in recvfrom");

    /* 
     * gethostbyaddr: determine who sent the datagram
     */
    hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
			  sizeof(clientaddr.sin_addr.s_addr), AF_INET);
    if (hostp == NULL)
      error("ERROR on gethostbyaddr");
    hostaddrp = inet_ntoa(clientaddr.sin_addr);
    if (hostaddrp == NULL)
      error("ERROR on inet_ntoa\n");
    printf("server received datagram from %s (%s)\n", 
	   hostp->h_name, hostaddrp);
    printf("server received %d/%d bytes: %s\n", strlen(buf), n, buf);
    
    /* 
     * sendto: echo the input back to the client 
     */
    client_res_info client_info;  // ✅ Declare variable

    client_info.sockfd = sockfd;
    client_info.clientaddr = clientaddr;
    client_info.clientlen = clientlen;

    input_transfer(client_info, buf); 
    n = sendto(sockfd, buf, strlen(buf), 0, 
	       (struct sockaddr *) &clientaddr, clientlen);
    if (n < 0) 
      error("ERROR in sendto");
  }
}