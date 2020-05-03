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

void work(void);

int main(void) {
    pid_t children[10];
    for(size_t i = 0; i < 10; i++) {
        pid_t pid = fork();
        if(pid == -1) {
            perror("parent: error forking");
            return EXIT_FAILURE;
        }
        if(pid == 0) {
            raise(SIGSTOP); // child stops itself
            work(); // after resuming it goes on to execute work()
            return EXIT_SUCCESS; // and finally, it successfully terminates
        } else {
            fprintf(stdout, "parent: spawned child (%d)\n", pid);
            children[i] = pid;
        }
    }

    // parent spawned all 10 children who are now stopped - begin resuming them one by one
    for(size_t i = 0; i < 10; i++) {
        fprintf(stdout, "parent: signaling child (%d) to continue...\n", children[i]);
        if(kill(children[i], SIGCONT) == -1) {
            fprintf(stderr, "parent: error signalling child (%d) to continue: %s\n", children[i], strerror(errno));
        }
    }

    return EXIT_SUCCESS; // exit from parent once all children have been resumed
}

void work(void) {
    pid_t mypid = getpid();
    srand(mypid);
    int32_t sleep_time = (rand() % 10) + 1;
    fprintf(stdout, "(%d): began sleeping for %d seconds\n", mypid, sleep_time);
    sleep(sleep_time);
    fprintf(stdout, "(%d): done sleeping after %d seconds\n", mypid, sleep_time);
}
