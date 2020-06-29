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

#define N_POOL_MAX 20

///
static bool parse_mqueue_request_string(char *req, char *ret[2]); 
static void * handle_client_request(char *request_string); 
static void handle_sigint(int num); 
static void * threadpool_worker(void *arg); 
/* static void threadpool_toggleavail(void);; */ 

static char _mq_name[256]; // will include prefixed "/"
static long _mqueue_max_msg_count,
            _mqueue_max_msg_size,
            _part_delim_alloc_size;

static struct mq_attr _mq_attributes;

static pthread_t _threadpool[N_POOL_MAX];

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
    sigprocmask(SIG_BLOCK, &set, NULL);
    //
    
    _mq_name[0] = '/';
    strcpy(_mq_name + 1, argv[1]);

    _mqueue_max_msg_count  = atol(argv[2]);
    _mqueue_max_msg_size   = atol(argv[3]);
    _part_delim_alloc_size = ((_mqueue_max_msg_size - 2) / 2) + 1;

    fprintf(stdout, "mserve started w/ parameters: \n\tMAX_MSG: %ld\n\tMAX_MSG_SIZE: %ld\n\tthread pool count: %d\n-----------------------------\n", _mqueue_max_msg_count, _mqueue_max_msg_size, N_POOL_MAX);

    _mq_attributes = (struct mq_attr) {
        0, 
        _mqueue_max_msg_count,
        _mqueue_max_msg_size,
        0
    };

    // initialize thread pool with a certain amount of threads at the start
    for (size_t i = 0; i < N_POOL_MAX; i++) {
        int ret = pthread_create(_threadpool + i, NULL, threadpool_worker, NULL);
        if(ret) {
            fprintf(stderr, PROGNAME ": error spawning new worker thread: error code %d\n", ret);
            continue;
        }
    }

    fprintf(stdout, "mserve: polling for requests...\n-----------------------------\n");

    // sync. wait for sigint
    sigset_t sigint_set;
    sigemptyset(&sigint_set);
    sigaddset(&sigint_set, SIGINT);
    
    int ret = sigwaitinfo(&sigint_set, NULL);
    if(ret == -1) {
        perror(PROGNAME ": failed waiting on SIGINT");
        return EXIT_FAILURE;
    }

    handle_sigint(SIGINT);

    return EXIT_SUCCESS;
}

static void * threadpool_worker(void *arg) {
    // poll the mqueue until a new request becomes available
    char *request_string = malloc(_mqueue_max_msg_size); // buffer for the string request message from the mqueue
    if(!request_string) {
        fprintf(stderr, PROGNAME ": failed allocating memory for mqueue message buffer: %s\n", strerror(errno));
        return NULL;
    }

    // open the request mqueue before polling for requests
    mqd_t request_mqueue = mq_open(_mq_name, O_RDONLY | O_CREAT, MQUEUE_NODE_PERMISSIONS, &_mq_attributes);
    if(request_mqueue == -1) {
        // failed creating or opening mqueue
        perror(PROGNAME ": failed creating mqueue");
        return NULL;
    }

    while (1) {
        ssize_t last_request_len; // ret value of mq_receive - the length of the received message or -1 in case of error
        last_request_len = mq_receive(request_mqueue, request_string, _mqueue_max_msg_size, NULL);
        if(last_request_len == -1) {
            perror(PROGNAME ": failed receiving request from mqueue");
            continue;
        }

        // received new request, take care of it
        void *res = handle_client_request(request_string);
    }

    return NULL;
}

/* static void threadpool_toggleavail(void) { */
/*     pthread_t *last_available = NULL, */
/*                me = pthread_self(); */
/*     bool found_myself = false; */

/*     for (size_t i = 0; i < N_POOL_MAX; i++) { */
/*         if(_threadpool_available[i] == 0) { */
/*             last_available = &_threadpool_available[i]; */
/*             continue; */
/*         } */
            
/*         if(pthread_equal(_threadpool_available[i], me)) { */
/*             // found myself in the pool of available threads, toggle */
/*             _threadpool_available[i] = 0; */
/*             return; */
/*         } */
/*     } */

/*     // haven't found ourselves in the available pool, which means that we were previously unavailable and we'll take the first empty available spot */
/*     *last_available = me; */            
/* } */

static void * handle_client_request(char *request_string) {
    // _request_string is a pointer to a string in the main event loop of main. that string is stored on the stack and is thus "voided" once a new request is received
    // if the new worker thread is not fast enough to parse it before a new request comes in, that can result in problems, thus I just create a local copy on the stack of the new worker thread and safely parse it from there

    char request_pathname[_part_delim_alloc_size],
         request_fifo_name[_part_delim_alloc_size];

    bool parse_ret = parse_mqueue_request_string(request_string, (char *[]) {request_pathname, request_fifo_name});
    if(!parse_ret) {
        fprintf(stderr, PROGNAME ": error parsing mqueue request string: %s\n", request_string);
        return NULL;
    }
    
    fprintf(stdout, PROGNAME ": received new request: (%s, %s) -- handling\n", request_pathname, request_fifo_name);

    // open requested file for reading
    int file_fd = open(request_pathname, O_RDONLY);
    if(file_fd == -1) {
        perror(PROGNAME ": error opening requsted file");
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
        return NULL;
    }

    void *buf = malloc(DEFAULT_BUFSIZE); // 128 KiB, 2^17 bytes
    if(!buf) {
        perror(PROGNAME ": failed allocating buffer");
        close(file_fd);
        close(fifo_fd);
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
