// spam mserve with a bunch of requests all at once and benchmark its sequential performance
// works by spawning tons of new child processes all at once, each of which pushes a new request to mserve's mqueue and voids the received file data

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <mqueue.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>

#define MSERVE_MQUEUE_NAME "/mserve"
#define MSERVE_MQUEUE_MAXMSG 128
#define MSERVE_MQUEUE_MAXMSG_SIZE 2048
#define MSERVE_FIFO_PATH "/home/bogdan/.mserve/fifos/"
#define REQUESTED_FILE_PATH "/home/bogdan/.mserve/example_random"
#define BUFSIZE 131072

void mserve_request(const mqd_t);

#define N_REQUESTS 20

int main(void) {
    mqd_t mqd = mq_open(MSERVE_MQUEUE_NAME, O_WRONLY);
    if(mqd == -1) {
        perror("client: error opening mqueue");
        return EXIT_FAILURE;
    }

    // open mqueue descriptors persist through forks
    pid_t children[N_REQUESTS];
    for(size_t i = 0; i < N_REQUESTS; i++) {
        pid_t pid = fork();
        if(pid == -1) {
            perror("parent: error forking");
            mq_close(mqd);
            return EXIT_FAILURE;
        }

        if(pid == 0) {
            mserve_request(mqd);
            mq_close(mqd);
            return EXIT_SUCCESS;
        } else {
            fprintf(stdout, "parent: spawned new child (%d)\n", pid);
            children[i] = pid; // save child PID to array
        }
    }

    // spawned all processes
    fprintf(stdout, "parent: successfully spawned all 10 children - began benchmarking\n");
    struct timespec time_start,
                    time_end;
    clock_gettime(CLOCK_REALTIME, &time_start);
    for(size_t i = 0; i < N_REQUESTS; i++) {
        // wait on the children one by one -- if in fact we start waiting on a child that has already terminated by the time we started waiting
        // the call will simply return immediately resulting in no lost waiting time
        int wstatus;
        if(waitpid(children[i], &wstatus, 0) == -1) {
            // error
            fprintf(stderr, "parent: error waiting for child: (%d)\n", children[i]);
        }

        if(!(WIFEXITED(wstatus))) {
            fprintf(stderr, "parent: child (%d) wait() call occurred on non-exit condition\n", children[i]);
        }
    }

    clock_gettime(CLOCK_REALTIME, &time_end);

    fprintf(stdout, "parent: all children successfully terminated - time passed: %ldms\n", ((time_end.tv_sec - time_start.tv_sec) * 1000) + ((time_end.tv_nsec - time_start.tv_nsec) / 1000000));

    mq_close(mqd);
    return EXIT_SUCCESS;
}

void mserve_request(const mqd_t mqd) {
    // send a request to mserve -- create a new FIFO, void received data, and close FIFO once done
    pid_t PID = getpid();

    // create path to new fifo with random name
    char fifo_name[11];
    char fifo_path[128];
    srand(PID); // seed based on PID
    sprintf(fifo_name, "%d", rand());
    sprintf(fifo_path, MSERVE_FIFO_PATH "%s", fifo_name);

    // create new FIFO
    if(mkfifo(fifo_path, 0660) == -1) {
        fprintf(stderr, "child: (%d): error creating FIFO: %s: %s\n", PID, fifo_path, strerror(errno));
        return;
    }

    // construct request string
    char req[128];
    sprintf(req, REQUESTED_FILE_PATH ";%s", fifo_name);

    fprintf(stdout, "child: (%d) full request string: %s\n", PID, req);

    // push a new request to mserve's queue
    if(mq_send(mqd, req, sizeof(req), 0) == -1) {
        fprintf(stderr, "(%d): error pushing new request to mqueue: %s\n", PID, strerror(errno));
        unlink(fifo_path);
        return;
    }

    int fifo = open(fifo_path, O_RDONLY); // may block until the server also attempts to open the FIFO
    if(fifo == -1) {
        fprintf(stderr, "(%d): error opening FIFO: %s: %s\n", PID, fifo_path, strerror(errno));
        unlink(fifo_path);
        return;
    }

    // request is sent, FIFO is open on both ends (client's open() call blocks until server properly tries to open as well), begin reading what the server sends */
    
    void *buffer = malloc(BUFSIZE);
    if(!buffer) {
        fprintf(stderr, "(%d): error allocating memory: %s\n", PID, strerror(errno));
        unlink(fifo_path);
        close(fifo);
        return;
    }

    size_t bread;
    while((bread = read(fifo, buffer, BUFSIZE)) > 0) {
        /* fprintf(stdout, "(%d): read %lu bytes to buffer, voiding\n", PID, bread); */ 
    }

    if(bread == -1) {
        fprintf(stdout, "(%d): error reading from FIFO: %s\n", PID, strerror(errno));
    } else {
        fprintf(stdout, "(%d): successfully transferred all data from server, last bread: %lu\n", PID, bread);
    }

    close(fifo);
    unlink(fifo_path);
    free(buffer);
    
    return; 
}
