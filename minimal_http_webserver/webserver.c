#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

const char *PORT =  "3334";
#define LISTEN_BACKLOCK 20
#define MAX_MESSAGE_SIZE 1024

typedef struct {
    struct addrinfo *hints, *res;
    int sockfd;
}AI_HOLDER;

typedef struct {
    int caller;
    char message[MAX_MESSAGE_SIZE];
}MESSAGE;

typedef struct {
    MESSAGE *message_thread_shared;
    AI_HOLDER *ai_holder;
}PTHREAD_ARGS;

typedef struct MESSAGE_QUEUE {
    MESSAGE *message;
    struct MESSAGE_QUEUE *next;
}MESSAGE_QUEUE;

MESSAGE_QUEUE *queue = NULL;
pthread_mutex_t lock;

int prepare_connection(AI_HOLDER *ai_holder){
    if(ai_holder == NULL){
        fprintf(stderr, "Struct AI_HOLDER in prepare_connection was unexpectedly NULL!\n");
        return -3;
    }
    int status;
    if((status = getaddrinfo(NULL, PORT, (ai_holder)->hints, &(ai_holder)->res)) != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -2;
    }

    if((status = socket((ai_holder)->res->ai_family, (ai_holder)->res->ai_socktype, (ai_holder)->res->ai_protocol)) == -1){
        perror("Failed to create socket!");
        // free memory allocated by getaddrinfo()
        freeaddrinfo((ai_holder)->res);
        return -1;
    }
    (ai_holder)->sockfd = status;

    int yes = 1;
    if(setsockopt((ai_holder)->sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1){
        perror("Unable to set socket options with setsockopt!\n");
        close((ai_holder)->sockfd);
        freeaddrinfo((ai_holder)->res);
        return -5;
    }

    if(bind((ai_holder)->sockfd, (ai_holder)->res->ai_addr, (ai_holder)->res->ai_addrlen) == -1){
        perror("Could not bind socket to port!\n");
        // close the socket
        close((ai_holder)->sockfd);
        // and again free memory to terminate potential memory leaks
        freeaddrinfo((ai_holder)->res);
        return -4;
    }

    if(listen((ai_holder)->sockfd, LISTEN_BACKLOCK) == -1){
        perror("Failed to create listener on socket!\n");
        close((ai_holder)->sockfd);
        freeaddrinfo((ai_holder)->res);
        return -6;
    }

    return 0;
}

int prepare_addrinfo(AI_HOLDER *ai_holder){
    struct addrinfo *hints = (struct addrinfo*)malloc(sizeof(struct addrinfo));
    if(hints == NULL){
        fprintf(stderr, "Could not allocate memory for hints!\n");
        return -1;
    }
    memset(hints, 0, sizeof(*hints));
    hints->ai_family = AF_UNSPEC;
    hints->ai_socktype = SOCK_STREAM;
    hints->ai_flags = AI_PASSIVE;

    ai_holder->hints = hints;
    return 0;
}

// safely free an allocated memory pointer
void free_safe(void **toFree){
    if(toFree != NULL && *toFree != NULL){
        free(*toFree);
        *toFree = NULL;
    }
}

void add_message(MESSAGE *message){
    MESSAGE_QUEUE *new_node = (MESSAGE_QUEUE*)malloc(sizeof(MESSAGE_QUEUE));
    if(new_node == NULL){
        fprintf(stderr, "Memory allocation failed for message queue node.\n");
        return;
    }
    new_node->message = message;
    new_node->next = NULL;

    pthread_mutex_lock(&lock);

    if(queue == NULL)
        queue = new_node;
    else{
        MESSAGE_QUEUE *iterator = queue;

        while(iterator->next != NULL)
            iterator = iterator->next;
        
        iterator->next = new_node;
    }
    pthread_mutex_unlock(&lock);
}

MESSAGE *poll_message(){
    pthread_mutex_lock(&lock);
    if(queue != NULL){
        MESSAGE_QUEUE *poll = queue;
        queue = queue->next;
        MESSAGE *msg = poll->message;
        free(poll);
        pthread_mutex_unlock(&lock);
        return msg;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

char *read_content_from_file(char *filePath){
    printf("Trying to read content from: %s\n", filePath);
    FILE *file = fopen(filePath, "r");
    size_t contentSize = 1;
    char ch;

    if(file == NULL){
        printf("Failed to open File!\n");
        return NULL;
    }

    char *content = (char *) malloc(sizeof(char) * contentSize);
    if(content == NULL){
        perror("Failed to allocate content");
        fclose(file);
        return NULL;
    }
    content[0] = '\0';

    while((ch = fgetc(file)) != EOF){
        contentSize++;
        content = (char*) realloc(content, contentSize);
        if(content == NULL) {
            perror("Failed to reallocate content");
            fclose(file);
            return NULL;
        }
        content[contentSize-2] = ch;
        content[contentSize-1] = '\0';
    }

    fclose(file);
    return content;
}

void sendResponse(const int caller, const char *http_response, const char *content) {
    printf("Sending response:\n%s%s\n", http_response, content);
    send(caller, http_response, strlen(http_response), 0);
    send(caller, content, strlen(content), 0);
    send(caller, "\r\n", 2, 0);
}

void handleGET(const int caller, const char *token, const char *message) {
    char *http_header = strtok(NULL, " ");
    if(http_header == NULL || (!strstr(http_header, "HTTP/1.0\r\n") && !strstr(http_header, "HTTP/1.1\r\n") && !strstr(http_header, "HTTP/2.0\r\n"))){
        const char r[] = "HTTP/1.1 400 BAD REQUEST\r\n\r\n";
        send(caller, r, strlen(r), 1);
        return;
    }

    // Standard response
    printf("Filetoken: %s\n", token);
    const char response[] ="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";

    if(strcmp(token, "/") == 0) {
        char *content = read_content_from_file("content/homepage.html");
        if(content == NULL) {
            const char r[] = "HTTP/1.1 404 NOT FOUND\r\n\r\n";
            send(caller, r, sizeof(r), 1);
        }
        else {
            sendResponse(caller, response, content);
            free(content);
        }
    }
    else {
        char *content;
        char file[1024];
        snprintf(file, sizeof(file), "content%s", token);
        if((content = read_content_from_file(file)) != NULL) {
            sendResponse(caller, response, content);
            free(content);
        }
        else {
            const char r[] = "HTTP/1.1 400 BAD REQUEST\r\n\r\n";
            send(caller, r, strlen(r), 1);
        }
    }
}

void *await_calls(void *args){
    PTHREAD_ARGS *pargs = (PTHREAD_ARGS*)args;

    while(1){
        int nfd;
        struct sockaddr_storage client_addr;
        socklen_t addr_size = sizeof(client_addr);

        if((nfd = accept(pargs->ai_holder->sockfd, (struct sockaddr*)&client_addr, &addr_size)) == -1){
            perror("Could not accept new client!\n");
            continue;
        }

        char message[MAX_MESSAGE_SIZE];
        int bytes_received = recv(nfd, message, sizeof(message) - 1, 0);
        if(bytes_received == -1){
            perror("Error, recv returned -1");
            close(nfd);
            continue;
        }
        if(bytes_received == 0){
            close(nfd);
            continue;
        }

        message[bytes_received] = '\0';

        MESSAGE *m = (MESSAGE*)malloc(sizeof(MESSAGE));
        if(m == NULL){
            fprintf(stderr, "Memory allocation failed for MESSAGE.\n");
            close(nfd);
            continue;
        }
        strcpy(m->message, message);
        m->caller = nfd;

        add_message(m);
        
        // stop this thread do not read inputs anymore
        if(strstr(m->message, "[STOP]"))
            return NULL;
    }
}

void *handle_message(void *args) {
    while(1){
        MESSAGE *m = NULL;
        while((m = poll_message()) != NULL){
            printf("Caller: %d\nMessage: %s\n\n", m->caller, m->message);

            if(strstr(m->message, "[STOP]")){
                close(m->caller);
                free(m);
                return NULL;
            }
            // handle HTTP GET requests here
            if(strstr(m->message, "GET")) {
                strtok(m->message, " ");
                handleGET(m->caller, strtok(NULL, " "), m->message);
            }

            close(m->caller);
            free(m);
        }
    }
}

int init_mutex(){
    if (pthread_mutex_init(&lock, NULL)) {
        fprintf(stderr, "Failed to initialize Mutex!\n");
        return 1;
    }
    return 0;
}

void cleanup() {
    pthread_mutex_destroy(&lock);
    while (queue != NULL) {
        MESSAGE *m = poll_message();
        if (m != NULL) {
            close(m->caller);
            free(m);
        }
    }
}

int main(){
    printf("Starting Server...\n");
    AI_HOLDER ai_holder;
    PTHREAD_ARGS pargs;
    pargs.ai_holder = &ai_holder;

    if(prepare_addrinfo(&ai_holder) != 0){
        return -1;
    }
    if(prepare_connection(&ai_holder) != 0){
        free_safe((void**)&ai_holder.hints);
        return -2;
    }

    printf("Server initialized!\n");
    // create mutex on lock (global var)
    init_mutex();

    // all things set up, begin with the handling of incoming calls
    pthread_t receive_message_thread, handle_message_thread;

    pthread_create(&receive_message_thread, NULL, await_calls, &pargs);
    pthread_create(&handle_message_thread, NULL, handle_message, &pargs);

    // wait on receive thread -> stop was sent to server
    pthread_join(receive_message_thread, NULL);
    // wait on handle thread 
    pthread_join(handle_message_thread, NULL);

    cleanup();
    close(ai_holder.sockfd);
    free_safe((void**)&ai_holder.hints);
    freeaddrinfo(ai_holder.res);
    return 0;
}