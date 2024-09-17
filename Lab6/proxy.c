/*
 * Main proxy driver: runs a web proxy which stands between a client
 * and a server, receiving HTTP requests from the client, creating a new
 * thread to service THAT client. In the thread routine, the proxy parses
 * the client's HTTP request, then forwards it to the server, perhaps
 * caching the response were any later clients to send that same HTTP request
 * to the proxy. This proxy implements an LRU cache eviction policy though
 * the use of a queue, where each access moves an element to the end of the
 * queue (making it the least likely to be evicted next)
 *
 * Arden Diakhate-Palme <aqd@andrew.cmu.edu>
 */

#include "csapp.h"
#include "http_parser.h"
#include "cache.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

typedef struct sockaddr SA;

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20191101 Firefox/63.0.1";

void proxy(int clientfd);
int sendRequest(const char *host, const char *port, const char *uri,
                parser_t *parse, rio_t *rio);
void forward(int clientfd, int serverfd, const char *req_uri);
void *thread(void *vargp);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

//handles a broken pipe SIGPIPE signal, returning without any operations
//is essentially equivalent to ignoring the signal
void sigpipe_handler(int sig) {
    return;
}

//global volatile (read directly from memory) variables for the cache
volatile obj_t cache_head;
volatile obj_t cache_tail;
volatile size_t cache_size;
volatile size_t curr_id;
pthread_rwlock_t rwlock;

/*
 * main opens a listening file descriptor before entering a server loop,
 * wherein each new connection accepted results in the creation of a new
 * thread to serve that client.
 *
 * Args: command line arguments designate the port with which to associate the
 * server's socket.
 */
int main(int argc, char **argv) {
    int listenfd, *connfdp;
    struct sockaddr_in clientaddr;
    socklen_t clientlen;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage, %s <port>\n", argv[0]);
        exit(0);
    }

    signal(SIGPIPE, sigpipe_handler);

    // create listening fd socket
    if ((listenfd = open_listenfd(argv[1])) < 0) {
        fprintf(stderr, "Error with open_listenfd");
        exit(1);
    }

    // server loop
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = (int *)malloc(sizeof(int));
        *connfdp = accept(listenfd, (SA *)&clientaddr, &clientlen);
        pthread_create(&tid, NULL, thread, (void *)connfdp);
    }

    return 0;
}

//thread routine initializier: initializes the read-write lock once
void init_proxy_thread(void) {
    pthread_rwlock_init(&rwlock, NULL);
}
/* thread routine: initializes relevant locks, becomes a detached thread
 * (such that pthread_join() doesnt have to be called to reap it), calls
 * proxy to run the web proxy in the context of the new thread
 */
void *thread(void *vargp) {
    int connfd = *((int *)(vargp));
    pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_proxy_thread);

    pthread_detach(pthread_self());
    free(vargp);
    proxy(connfd);
    return NULL;
}

/* the proxy function runs in the context of the newly created process
 * to service a particular client, completes some of the parsing
 * and error checking of the client's HTTP request, before checking
 * if the response is already cache: if so the cached data to the client
 * if not: send the request, read the reponse.
 *
 * Args: the client's socket file descriptor (which will be used to read
 * and parse the sent HTTP request)
 */
void proxy(int clientfd) {
    rio_t rio;
    char buf[MAXLINE];
    int serverfd;

    rio_readinitb(&rio, clientfd);
    rio_readlineb(&rio, buf, MAXLINE);

    parser_t *parse = parser_new();
    parser_state state = parser_parse_line(parse, buf);
    if (state == REQUEST) {
        const char *val[30];
        char *tmp = " ";
        val[0] = tmp;

        const char *tmp1 = " ";
        parser_retrieve(parse, METHOD, &tmp1);
        if (strcmp(tmp1, "GET") != 0)
            clienterror(clientfd, "not_implemented", "501", "Not Implemented",
                        "");

        parser_retrieve(parse, HOST, val);
        const char *req_host = *val;
        parser_retrieve(parse, PORT, val);
        const char *req_port = *val;
        parser_retrieve(parse, URI, val);
        const char *req_uri = *val;

        pthread_rwlock_rdlock(&rwlock);
        obj_t tmp2 = cacheFind(req_uri);
        pthread_rwlock_unlock(&rwlock);

        if (tmp2 == NULL) {
            fprintf(stderr, "cache: not found.\n");
            serverfd = sendRequest(req_host, req_port, req_uri, parse, &rio);
            if (serverfd > 0)
                forward(clientfd, serverfd, req_uri);
            close(serverfd);
        } else {
            fprintf(stderr, "cache: found\n");
            if (rio_writen(clientfd, tmp2->value, tmp2->len) < 0) {
                fprintf(stderr, "error in rio_writen to cli: [%d]%s\n", errno,
                        strerror(errno));
            }

            pthread_rwlock_wrlock(&rwlock);
            replace_tail(tmp2);
            pthread_rwlock_unlock(&rwlock);
        }

        close(clientfd);
    }
}

/* sendRequests() opens a socket to the server which was initially requested
 * by the client (in the client's HTTP request line). Once the socket is open,
 * this function, parses the request headers and formats the client's HTTP
 * request line, before sending that information to the server
 *
 * Argument [0]: the hostname of the server to connect to
 * Argument [1]: the port on which to connect to the server
 * Argument [2]: the uri of the client's http request line (requested from the server)
 * Argument [3]: the parser object to parse the headers of the client's HTTP request
 *      (which has already parsed one line)
 * Argument [4]: a pointer to the robust I/O structure which has been initialized
 *      to read from the client's socket file descriptor
 */
int sendRequest(const char *host, const char *port, const char *uri,
                parser_t *parse, rio_t *rio) {
    char buf[MAXLINE] = "GET ";
    int serverfd = -1;

    // create the proxy's HTML request line to send to web server
    size_t i = 0;
    size_t cnt = 0;
    while (uri[i] != '\0') {
        if (uri[i] == '/')
            cnt++;
        if (uri[i] == '/' && cnt > 2)
            break;
        i++;
    }
    char *tmp = (char *)uri + i;
    (void)strcat(buf, tmp);
    char *version = " HTTP/1.0\r\n";
    (void)strcat(buf, version);

    if ((serverfd = open_clientfd(host, port)) < 0) {
        if (errno != ECONNREFUSED) {
            fprintf(stderr, "error in open_clientfd: %s\n", strerror(errno));
            return -1;
        }
    }

    // send request line and all request headers
    char *CRLF = "\r\n";
    char host_hdr[MAXLINE] = "Host: ";
    (void)strcat(host_hdr, host);
    (void)strcat(host_hdr, CRLF);

    char userAgent_hdr[MAXLINE] = "User-Agent: ";
    (void)strcat(userAgent_hdr, header_user_agent);
    (void)strcat(userAgent_hdr, CRLF);

    char connection_hdr[MAXLINE] = "Connection: close\r\n";
    char proxyConnection_hdr[MAXLINE] = "Proxy-Connection: close\r\n";

    rio_writen(serverfd, buf, strlen(buf));
    if (rio_writen(serverfd, userAgent_hdr, strlen(userAgent_hdr)) < 0) {
        if (errno == 32)
            return -1;
    }

    rio_writen(serverfd, connection_hdr, strlen(connection_hdr));
    rio_writen(serverfd, proxyConnection_hdr, strlen(proxyConnection_hdr));

    /* send any other request headers */
    char hdr_buf[MAXBUF];
    ssize_t bytes_read;
    parser_state state;
    while ((bytes_read = rio_readlineb(rio, hdr_buf, MAXLINE)) > 0) {
        if ((state = parser_parse_line(parse, hdr_buf)) != ERROR) {
            if (state == REQUEST)
                exit(67);

            if (state == HEADER) {
                char hdr_str[MAXLINE];
                header_t *hdr;
                size_t count = 0;
                while ((hdr = parser_retrieve_next_header(parse)) != NULL) {
                    if (strcmp(hdr->name, "User-Agent") == 0) {
                        count++;
                        continue;
                    }
                    if (strcmp(hdr->name, "Connection") == 0) {
                        count++;
                        continue;
                    }
                    if (strcmp(hdr->name, "Proxy-Connection") == 0) {
                        count++;
                        continue;
                    }
                    sprintf(hdr_str, "%s: %s\r\n", hdr->name, hdr->value);
                    rio_writen(serverfd, hdr_str, bytes_read);
                    count++;
                }
                if (count == 0) {
                    char *CRLF = "\r\n";
                    rio_writen(serverfd, CRLF, strlen(CRLF));
                    return serverfd;
                }
            }
        }
    }
    return -1; // should never reach
}


/* The forward function forwards an http response from the server to the client,
 * possibly caching the response if it fits within a preset web_object size
 * constraints.
 *
 * Argument [0]: the client socket file descriptor to forwards server response to
 * Argument [1]: the server socket file descriptor from which to read http response
 * Argument [2]: the uri of the HTTP request that was forwarded to the server
 * by the client (for caching purposes)
 *
 */
void forward(int clientfd, int serverfd, const char *req_uri) {
    rio_t server_rio;
    rio_readinitb(&server_rio, serverfd);

    size_t obj_size = 0;
    ssize_t bytes_read;
    char web_obj_buf[MAX_OBJECT_SIZE];
    char newbuf[MAXBUF];
    bool fitsInCache = true;

    while ((bytes_read = rio_readnb(&server_rio, newbuf, MAXBUF)) > 0) {
        /*
        size_t i= 0;
        while(newbuf[i] != '4')
            i++;
        if(newbuf[i+1] == '0' && newbuf[i+2] == '4'){
            fprintf(stderr,": %s\n",req_uri);
            fitsInCache = false;

        }
        */

        if (obj_size + bytes_read > MAX_OBJECT_SIZE) {
            fprintf(stderr, "web object is too large\n");
            fitsInCache = false;
        } else {
            memcpy((void *)((char *)web_obj_buf + obj_size), newbuf,
                   bytes_read);
            obj_size += bytes_read;
        }

        int tmp_err;
        if ((tmp_err = rio_writen(clientfd, newbuf, bytes_read)) < 0) {
            fprintf(stderr, "error in rio_writen: [%d] %s\n", errno,
                    strerror(errno));
            if (errno == 32) {
                break;
            }
        }
    }

    if (fitsInCache) {
        char *tmp_uri = malloc(MAXLINE * sizeof(char));
        strcpy(tmp_uri, req_uri);

        char *web_obj = (char *)malloc(obj_size * sizeof(char));
        memcpy(web_obj, web_obj_buf, obj_size);

        pthread_rwlock_wrlock(&rwlock);
        cacheAdd(tmp_uri, web_obj, obj_size);
        pthread_rwlock_unlock(&rwlock);
    }
}

/* BEGIN Cache function definitions */

//Caches a server response by inserting a key-value pair correpsonding to
//alloc'd memory into a FIFO queue
void cacheAdd(char *key, char *value, size_t len) {

    obj_t tmp;
    if ((tmp = cacheFind(key)) != NULL)
        return;

    obj_t web_obj = (obj_t)malloc(sizeof(struct cache_obj));

    web_obj->key = key;
    web_obj->value = value;
    web_obj->len = len;

    //evict a line if this new added response reaches past the maximum capacity
    cache_size += len;
    if (cache_size > MAX_CACHE_SIZE) {
        evict(len);
    }

    //queue not yet init'd so init it by setting head & tail to same node
    if (cache_head == NULL) {
        web_obj->next = NULL;
        cache_head = web_obj;
        cache_tail = web_obj;
        return;
    }

    web_obj->next = NULL;
    cache_tail->next = web_obj;
    cache_tail = web_obj;
    return;
}

//searched the FIFO queue of key-value pairs to find the element with the
//corresponding key
obj_t cacheFind(const char *key) {
    obj_t tmp = cache_head;
    while (tmp != NULL) {
        if (strcmp((const char *)tmp->key, key) == 0) {
            return tmp;
        }
        tmp = tmp->next;
    }
    return NULL;
}


//evicts a cache line (i.e an http server response) from the cache by
//dequeuing an element from the head of the queue
void evict(size_t obj_size) {
    size_t aggreg_size = 0; //agreggate size of freed space in cache
    while (aggreg_size + 1 < obj_size) {

        obj_t tmp = cache_head;
        cache_head = cache_head->next;

        aggreg_size += tmp->len;
        cache_size -= tmp->len;

        free(tmp->key);
        free(tmp->value);
        free(tmp);
    }
}

//replaces the tail of the cache with the requested element (the obj
// argument [0]) while maintaining the integrity of the cache queue
void replace_tail(obj_t obj) {
    if (obj == cache_head) {
        cache_head = cache_head->next;
        obj->next = NULL;
        cache_tail->next = obj;
        cache_tail = obj;
        return;
    }

    //search the entire queue from the head O(n)
    obj_t tmp = cache_head;
    while (tmp->next != obj) {
        tmp = tmp->next;
    }
    tmp->next = obj->next;
    obj->next = NULL;
    cache_tail->next = obj;
    cache_tail = obj;
}
/* END Cache function definitions */

// from CSAPP:e3 textbook: function for parsing and sending apporpriately-formatted
// errors back to the client
// Arguments include the message to send to back to the client (long and
// short forms) as well as the error number in string form
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg) {
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-Length: %zu\r\n\r\n", strlen(cause));
    rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor="
                 "ffffff"
                 ">\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    rio_writen(fd, buf, strlen(buf));
}

