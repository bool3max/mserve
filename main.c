// mserve - an attemp to build a simple mqueue request based threaded file server, for practice of mqueues, pipes, fifos, as well as pthreads and other low level concepts

#include <stdlib.h>
#include <stdio.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>

#define MQUEUE_NODE_PERMISSIONS 0660
#define FIFO_STORAGE_PATH "/home/bogdan/.mserve/fifos/"
#define PROGNAME "mserve"
#define REQUEST_STRING_DELIM ";" // a string literal since I pass it to strtok
#define DEFAULT_BUFSIZE 131072 // 128k (KiB), 2^17

#define N_POOL_MAX 100
#define SIGWORKPENDING (SIGRTMIN + 1) // sent to worker threads by the parent when there is work to be done

///
static bool parse_mqueue_request_string(char *req, char *ret[2]); 
static void * handle_client_request(char *request_string); 
static void handle_sigint(int num); 
static void * threadpool_worker(void *arg); 
static void threadpool_toggleavail(void);; 
static char * grq_consume(void);
static bool grq_push(char *req);

static const char *_mq_name; // so that the SIGINT handler can unlink the queue -- includes prefixed "/"
static long _mqueue_max_msg_count,
            _mqueue_max_msg_size,
            _part_delim_alloc_size;

static pthread_t _threadpool[N_POOL_MAX], // thread pool ids
                 _threadpool_available[N_POOL_MAX]; // ids of all currently available threads (those ready to do work)
static sigset_t _worker_set; // signal set for which worker threads wait on

static char *_request_queue[N_POOL_MAX]; // all currently pending requests - the main thread pushes a new one here as soon as a new one becomes available


int main(int argc, char **argv) {
    /*
     * argv[1] : mqueue name, without leading slash
     * argv[2] : max number of messages in queue
     * argv[3] : max message size
     */ 

    if(argc < 4) {
        fprintf(stderr, "less than 3 arguments were provided\n");
        return EXIT_FAILURE;
    }

    // set up SIGINT handler
    if(sigaction(SIGINT, & (struct sigaction) {.sa_handler = handle_sigint}, NULL) != 0) {
        perror(PROGNAME ": failed setting a SIGINT handler");
        return EXIT_FAILURE;
    }

    // block SIGPIPE (prevent it from terminating the process) -- instead write(2) fails with the EPIPE error
    // I see no reason for SIGPIPE to happen though, unless a client process crashes or is deliberately killed after creating and opening the pipe but before the transfer is finished
    // then - the server would attemp to write to a pipe that has no one connected to the read end, i.e. no file descriptor on the system reffering to the read end of the pipe
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    sigaddset(&set, SIGWORKPENDING);
    sigprocmask(SIG_BLOCK, &set, NULL);
    //
    
    // initialize set on which worker threads will wait upon
    sigemptyset(&_worker_set);
    sigaddset(&_worker_set, SIGWORKPENDING);
    //

    char mq_name_feed[strlen(argv[1]) + 2];
    mq_name_feed[0] = '/';
    strcpy(mq_name_feed + 1, argv[1]);

    _mq_name = mq_name_feed;

    _mqueue_max_msg_count  = atol(argv[2]);
    _mqueue_max_msg_size   = atol(argv[3]);
    _part_delim_alloc_size = ((_mqueue_max_msg_size - 2) / 2) + 1;

    fprintf(stdout, "mserve started w/ parameters: \n\tMAX_MSG: %ld\n\tMAX_MSG_SIZE: %ld\n\tthread pool count: %d\n-----------------------------\n", _mqueue_max_msg_count, _mqueue_max_msg_size, N_POOL_MAX);

    const struct mq_attr mq_attributes = {
        0, 
        _mqueue_max_msg_count,
        _mqueue_max_msg_size,
        0
    };

    // open the request mqueue before polling for requests
    mqd_t request_mqueue = mq_open(mq_name_feed, O_RDONLY | O_CREAT, MQUEUE_NODE_PERMISSIONS, &mq_attributes);
    if(request_mqueue == -1) {
        // failed creating or opening mqueue
        perror(PROGNAME ": failed creating mqueue");
        return EXIT_FAILURE;
    }

    // initialize thread pool with a certain amount of threads at the start
    for (size_t i = 0; i < N_POOL_MAX; i++) {
        int ret = pthread_create(_threadpool + i, NULL, threadpool_worker, NULL);
        if(ret) {
            fprintf(stderr, PROGNAME ": error spawning new worker thread: error code %d\n", ret);
            continue;
        }

        // add thread to array of available threads -- since all are avaiable at start
        _threadpool_available[i] = _threadpool[i];
    }

    fprintf(stdout, "mserve: polling for requests...\n-----------------------------\n");

    while(1) {
        char *request_string = malloc(_mqueue_max_msg_size); // buffer for the string request message from the mqueue
        if(!request_string) {
            fprintf(stderr, PROGNAME ": failed allocating memory for mqueue message buffer: %s\n", strerror(errno));
            continue;
        }

        ssize_t last_request_len; // ret value of mq_receive - the length of the received message or -1 in case of error
        last_request_len = mq_receive(request_mqueue, request_string, _mqueue_max_msg_size, NULL);
        if(last_request_len == -1) {
            perror(PROGNAME ": failed receiving request from mqueue");
            continue;
        }

        // successfully received new request from mqueue - push it to grq

        if(!grq_push(request_string)) {
            fprintf(stderr, PROGNAME ": failed pushing request %s to grq - no space in queue\n", request_string);
            free(request_string);
            continue;
        }

        // request is now in grq - find first available worker and signal it to begin processing a request from the grq

        pthread_t current_worker = 0;
        for(size_t i = 0; i < N_POOL_MAX; i++) {
            // find the first thread available for the job 
            if(_threadpool_available[i] != 0) {
                current_worker = _threadpool_available[i];
                break;
            }
        }

        if(current_worker == 0) {
            // couldn't find an available thread
            fprintf(stderr, PROGNAME ": couldn't find available worker thread for request: %s\n", request_string);
            continue;
        }

        int ret = pthread_kill(current_worker, SIGWORKPENDING);
        if(ret != 0) {
            perror(PROGNAME ": failed signalling worker thread to process request");
            free(request_string);
        }
    }

    return EXIT_SUCCESS;
}

static bool grq_push(char *req) {
    // push a new request to the global request queue - returns false if the queue is full

    char **first_avail = NULL; // first available spot
    
    for(size_t i = 0; i < N_POOL_MAX; i++) {
        if(_request_queue[i] == NULL) {
            first_avail = _request_queue + i;
            break;
        }
    }

    if(!first_avail) return false;

    *first_avail = req; 
    return true;
}

static char * grq_consume(void) {
    // allocate and return address of new buffer representing the first string in the grq. this buffer must be freed by the caller once it is done with.
    char *buf = NULL;
    for(size_t i = 0; i < N_POOL_MAX; i++) {
        if(_request_queue[i] != NULL) {
            buf = strndup(_request_queue[i], _mqueue_max_msg_size);
            _request_queue[i] = NULL;
            break;
        }
    }

    return buf;
}

static void * threadpool_worker(void *arg) {
    // sleep until we receive a signal to process a new request from the main thread    
    int sig; 
    while((sig = sigwaitinfo(&_worker_set, NULL)) != -1) {
        // synchronously wait for the main thread to signal us to process a request

        threadpool_toggleavail();
        
        char *request_string = grq_consume(); // grq_consume() can return NULL, but in practice I don't think that would ever happen since a worker gets signalled whenever a new request arrives into the grq
        handle_client_request(request_string); 

        free(request_string);

        threadpool_toggleavail();
    }

    perror("worker thread: failed waiting on set");

    return NULL;
}

static void threadpool_toggleavail(void) {
    pthread_t *last_available = NULL,
               me = pthread_self();
    bool found_myself = false;

    for (size_t i = 0; i < N_POOL_MAX; i++) {
        if(_threadpool_available[i] == 0) {
            last_available = &_threadpool_available[i];
            continue;
        }
            
        if(pthread_equal(_threadpool_available[i], me)) {
            // found myself in the pool of available threads, toggle
            _threadpool_available[i] = 0;
            return;
        }
    }

    // haven't found ourselves in the available pool, which means that we were previously unavailable and we'll take the first empty available spot
    *last_available = me;            
}

static void * handle_client_request(char *request_string) {
    // _request_string is a pointer to a string in the main event loop of main. that string is stored on the stack and is thus "voided" once a new request is received
    // if the new worker thread is not fast enough to parse it before a new request comes in, that can result in problems, thus I just create a local copy on the stack of the new worker thread and safely parse it from there

    char request_pathname[_part_delim_alloc_size],
         request_fifo_name[_part_delim_alloc_size];

    bool parse_ret = parse_mqueue_request_string(request_string, (char *[]) {request_pathname, request_fifo_name});
    if(!parse_ret) {
        fprintf(stderr, PROGNAME ": error parsing mqueue request string: %s\n", request_string);
        free(request_string);
        return NULL;
    }
    
    fprintf(stdout, PROGNAME ": received new request: (%s, %s) -- handling\n", request_pathname, request_fifo_name);

    // open requested file for reading
    int file_fd = open(request_pathname, O_RDONLY);
    if(file_fd == -1) {
        perror(PROGNAME ": error opening requsted file");
        free(request_string);
        return NULL;
    }

    // construct full FIFO path
    char full_fifo_path[strlen(FIFO_STORAGE_PATH) + strlen(request_fifo_name) + 1];
    strcpy(full_fifo_path, FIFO_STORAGE_PATH);
    strncat(full_fifo_path, request_fifo_name, 11);

    // open FIFO for writing only -- this may block until client also opens the FIFO
    int fifo_fd = open(full_fifo_path, O_WRONLY);
    if(fifo_fd == -1) {
        perror(PROGNAME ": failed opening FIFO for writing");
        close(file_fd);
        free(request_string);
        return NULL;
    }

    void *buf = malloc(DEFAULT_BUFSIZE); // 128 KiB, 2^17 bytes
    if(!buf) {
        perror(PROGNAME ": failed allocating buffer");
        close(file_fd);
        close(fifo_fd);
        free(request_string);
        return NULL;
    }

    size_t bread;
    while((bread = read(file_fd, buf, DEFAULT_BUFSIZE)) > 0) {
        // read call complete, "bread" bytes are now in "buf"
        ssize_t bwritten = write(fifo_fd, buf, bread);

        if(bwritten == -1) {
            // failed writing the batch of bytes received
            fprintf(stderr, PROGNAME ": failed writing to FIFO - request aborted (%s): %s\n", full_fifo_path, strerror(errno));
            free(buf);
            free(request_string);
            close(file_fd);
            close(fifo_fd);
            return NULL;
        }

        /* fprintf(stdout, PROGNAME ": read %lu bytes of data and wrote %ld bytes of data\n", bread, bwritten); */
    }

    fprintf(stdout, PROGNAME ": done processing request (%s, %s) last bread: %lu\n", request_pathname, request_fifo_name, bread);

    close(file_fd);
    close(fifo_fd);
    free(buf);

    return NULL;
}

static bool parse_mqueue_request_string(char *req, char *ret[2]) {
    // parse the null terminated string pointed to by "req" and store the pathname of the requested file at *ret[0] and the request FIFO name at *ret[1]
    // the return mechanism is similar to that of pipe(2)
    // on failure, false is returned

    char *internal;
    char *str_ret = strtok_r(req, REQUEST_STRING_DELIM, &internal);
    strcpy(ret[0], str_ret);

    str_ret = strtok_r(NULL, REQUEST_STRING_DELIM, &internal);
    if(str_ret) {
        strcpy(ret[1], str_ret);
        return true;
    } else return false;
}

static void handle_sigint(int num) {
    // do cleanup -- close all FDs (done auto by exit()...), unlink opened mqueue...
    fprintf(stdout, "RECEIVED SIGINT(%d) SIGNAL, ABORTING...\n", num);
    mq_unlink(_mq_name);
    exit(3); // flushes all streams and closes all open FDs (FIFOs, other open files...)
}
