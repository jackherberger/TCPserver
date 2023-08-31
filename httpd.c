#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <getopt.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define DEFAULT_BACKLOG 100
#define MAXLEN 1000
#define BUFFER_SIZE 1024
int up = 1;

void sig_handler(int signum){
	if (signum == SIGQUIT){
		up = 0;
		exit(0);
	}
}

void error_handler(int err, int newsock){
	char* response;
	if (err == 404){
		response = "404 Not Found";
		// report 404 - not found
	}
	else if (err == 403){
		response = "403 Permission Denied";
		// report 403 - permision denied
	}
	else if (err == 501){
		response = "501 Not Implemented";
		//report 501 - not implemented
	}
	else if (err == 500){
		response = "500 Internal Error";
		// report 500 - internal error
	}
	else if (err == 400){
		response = "400 Bad Request";
		// report 400 - bad request
	}

	char* response_send = malloc(strlen(response) + 30);
	sprintf(response_send, "HTTP/1.1 %s\r\n\r\n", response);
	send(newsock, response_send, strlen(response_send), 0);
	shutdown(newsock, SHUT_RDWR);
	free(response_send);
}

char* make_header(const char* fpath){
	struct stat fileStat;
	if (stat(fpath, &fileStat) < 0){
		//error_handler(404, newsock);
		return NULL;
	}

	char* header = malloc(1024);
	sprintf(header, "HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: %ld\r\n"
			"\r\n",
			fileStat.st_size);
	return header;
}

void child(int newsock, char *pname){
	int len;
	char buffer[BUFFER_SIZE] = {0};
	char request_type[5] = {0};
	char temp[48] = {0};
	char http[12] = {0};
	int kv_flag = 0;

	len = recv(newsock, buffer, sizeof(buffer), 0);
	if (len < 0){ // Internal error, code 500
		error_handler(500, newsock);
printf("Error reading from socket.\n");
		exit(1);
	}
	sscanf(buffer, "%s %s %s", request_type, temp, http); // Splice her up
	puts(buffer);	
	const char* fname = temp;
	if (fname[0] == '/'){
		fname++;
	}
	if (strncmp(fname, "kv/", 3) == 0){ // Check for kv
		kv_flag = 1; 		    // If so, set flag
		fname = fname + 3;	    // Move fname up 3 spots
	}
	printf("Request: %s\n", request_type);
	printf("Fname: %s\n", fname);
	printf("Http: %s\n", http);

	if (strcmp(request_type, "GET") == 0){
		/*-------------------- GET ------------------------*/
		if (kv_flag){ // KV get
			int fd = open(pname, O_WRONLY); // Open 'get_FIFO' from kvstore
			if (fd == -1){ // Internal error, code 500
				error_handler(500, newsock);
				printf("Couldn't open kvstore FIFO.\n");
				exit(1);
			}
			pid_t clientPID = getpid();
			char command[BUFFER_SIZE] = {0}; // Set up command
			snprintf(command, sizeof(command), "get %d %s\n", clientPID, fname);
			write(fd, command, strlen(command) + 1); // Write command to FIFO
			sleep(1); //Hold up, let it read command
			int new_fifo = open("get_fifo", O_RDONLY); // Read from new FIFO
			
			if (new_fifo == -1){ // Internal error, code 500
				error_handler(500, newsock);
				printf("Error opening 'get' FIFO.\n");
				exit(1);
			}

			char line[BUFFER_SIZE];
			ssize_t bytes_read = read(new_fifo, line, sizeof(line) - 1);
			if (bytes_read == -1){ // Internal error, code 500
				error_handler(500, newsock);
				exit(1);
			}
			char check[] = "Not a valid key.";
			line[bytes_read] = '\0';
			if (strstr(line, check) != NULL){
				// Not found error, code 404
				error_handler(404, newsock);
				exit(0);
			}
			char* value = line + (strlen(fname) + 1);
			char* header = malloc(BUFFER_SIZE);
			sprintf(header, "HTTP/1.1 200 OK\r\n"
					"Content-Type: text/html\r\n"
					"Content-Length: %ld\r\n"
					"\r\n",
					strlen(value));
			
			char* response = malloc(strlen(header) + strlen(value) + 1);
			strcpy(response, header);
			strcat(response, value);
			send(newsock, response, strlen(response), 0);
			
			// Clean up
			free(header);
			free(response);	
			close(fd);
			unlink("get_fifo");
			exit(0);
		}
		else{
			FILE* get_file = fopen(fname, "r");
			if (get_file == NULL) { // Not found error, code 404
				error_handler(404, newsock);
				printf("File not found.\n");
				exit(1);
			}
			char *buffer = (char*)malloc(BUFFER_SIZE * sizeof(char));
			if (buffer == NULL) {
				printf("Failed to allocate memory.\n");
				fclose(get_file);
				exit(1);
			}
			
			size_t buffer_size = BUFFER_SIZE;
			size_t buffer_len = 0;
			char *line;

			while ((line = fgets(buffer + buffer_len, buffer_size - buffer_len, get_file)) != NULL){
				buffer_len += strlen(line);
				if (buffer_len >= buffer_size - 1){
					buffer_size = buffer_size * 2;
					char *temp = realloc(buffer, buffer_size * sizeof(char));
					if (temp == NULL) {
						printf("Failed to reallocate memory for buf.\n");
						free(buffer);
						fclose(get_file);
						exit(1);
					}
					buffer = temp;
				}
			}
			puts(buffer);
			char* header = make_header(fname);
			if (header == NULL){
				error_handler(404, newsock);
				exit(1);
			}
			char* response = malloc(strlen(header) + strlen(buffer) + 1);
			if (response == NULL) {
				printf("Failed to reallocate memory for response.\n");
				free(buffer);
				fclose(get_file);
				exit(1);
			}

			// Compile together
			strcpy(response, header);
			strcat(response, buffer);
			// Send
			send(newsock, response, strlen(response), 0);
			// Clean
			free(response);
			free(header);
			fclose(get_file);
			free(buffer);
			exit(0);
		}
	}
	else if (strcmp(request_type, "PUT") == 0){
		/* ------------ PUT --------------------------------- */
		char command[BUFFER_SIZE];
		int fd = open(pname, O_WRONLY);
		char *value = strstr(buffer, "\r\n\r\n") + 4;

		snprintf(command, sizeof(command), "set %s \"%s\"\n", fname, value); 
		write(fd, command, strlen(command) + 1);

		char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
		send(newsock, response, strlen(response), 0);
		close(fd);
		exit(1);
	}



	else if (strcmp(request_type, "HEAD") == 0){
		/*------------- HEAD --------------------------- */
		if (kv_flag){
			int fd = open(pname, O_WRONLY);
			if (fd == -1){ // Internal error, code 500
				error_handler(500, newsock);
				printf("Couldn't open kvstore FIFO.\n");
				exit(1);
			}
			pid_t clientPID = getpid();
			char command[BUFFER_SIZE] = {0};
			snprintf(command, sizeof(command), "get %d %s\n", clientPID, fname);
			write(fd, command, strlen(command) + 1);
			sleep(1);
			int new_fifo = open("get_fifo", O_RDONLY);
			if (new_fifo == -1){
				printf("Error opening 'get' FIFO.\n");
				exit(1);
			}

			char line[BUFFER_SIZE];
			ssize_t bytes_read = read(new_fifo, line, sizeof(line) - 1);
			if (bytes_read == -1){
				printf("Error reading 'get' FIFO.\n");
			}
			char check[] = "Not a valid key.";
			printf("HERE\n");
			line[bytes_read] = '\0';
			printf("Line read: %s\n", line);
			fflush(stdout);
			if (strstr(line, check) != NULL){
				// Not found error, code 404
				error_handler(404, newsock);
				exit(1);
			}

			char* value = line + (strlen(fname) + 1);
			char* header = malloc(BUFFER_SIZE);
			sprintf(header, "HTTP/1.1 200 OK\r\n"
					"Content-Type: text/html\r\n"
					"Content-Length: %ld\r\n"
					"\r\n",
					strlen(value));
			printf("Header %s\n", header);
			send(newsock, header, strlen(header), 0);
			
			// Clean up
			free(header);
			close(fd);
			unlink("get_fifo");
			printf("Made here\n");
			exit(0);
		}
		else {
			FILE* get_file = fopen(fname, "r");
			if (get_file == NULL) {
				error_handler(404, newsock);
				printf("File not found.\n");
				exit(1);
			}
			size_t contents_size = 0;
			char* header = make_header(fname);
			send(newsock, header, strlen(header), 0);
			
			// Clean up
			free(header);
			fclose(get_file);
			exit(0);
		}
	}
	else { // Bad request OR not implemented, codes 501 or 400
		error_handler(501, newsock);
		exit(1);
		// RETURN ERROR
	}
	exit(0);
}

int main(int argc, char *argv[]) {

	// Take care of command line args
	char* fifo = argv[1];
	char* port_arg = argv[2];
    	int port = atoi(port_arg);
	
	// Initialize neccesary variables
	int mlen, sock_fd, newsock;
   	struct sockaddr_in sa, newsockinfo, peerinfo;
    	socklen_t len;
    
    	char localaddr[INET_ADDRSTRLEN], peeraddr[INET_ADDRSTRLEN], buff[MAXLEN+1];
    	
	// Create TCP socket
   	if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		printf("Socket creation failed.\n");
		exit(1);
	}

	// Set up server address and port
    	sa.sin_family = AF_INET;
    	sa.sin_port = htons(port);
    	sa.sin_addr.s_addr = htonl(INADDR_ANY);
    	
	// Bind the socket to address and port
    	if (bind(sock_fd, (struct sockaddr *) &sa, sizeof(sa)) < 0){
		printf("Binding failed.\n");
		exit(1);
	}

    	// Listen for incoming connections
   	if (listen(sock_fd, DEFAULT_BACKLOG) < 0){
		printf("Listening failed.\n");
		exit(1);
	}
	printf("Server listening on port: %d\n", port);
	
	signal(SIGQUIT, sig_handler);
	while (up) {
		len = sizeof(newsockinfo);
		// Accept incoming connections
		if ((newsock = accept(sock_fd, (struct sockaddr *) &peerinfo, &len)) < 0){
			printf("Accept failed.\n");
			exit(1);
		}
		// Create child
		pid_t pid = fork();
		
		if (pid < 0){
			printf("Child failed.\n");
			exit(1);
		}
		else if (pid == 0){
			len = sizeof(newsockinfo);
			getsockname(newsock, (struct sockaddr *) &newsockinfo, &len);
	
			child(newsock, fifo);
			
			printf("Error in child.\n");
			exit(1);
		}
		else {
			int status;
			waitpid(pid, &status, 0);
//			close(newsock);
		}
		
	}
    	return 0;
}
