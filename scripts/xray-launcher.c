// Native POSIX lifetime supervisor, bundled with the macOS application.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int signalNumber) { (void)signalNumber; stopping = 1; }
static void pauseBriefly(void) {
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
}
static int exitStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    char* end;
    errno = 0;
    long parentNumber = strtol(argv[1], &end, 10);
    if (errno || *end || parentNumber <= 1 || parentNumber > INT_MAX) return 2;
    pid_t parent = (pid_t)parentNumber;
    if (getppid() != parent) return 2;
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    pid_t child = fork();
    if (child < 0) { perror("Cannot fork Xray"); return 1; }
    if (child == 0) {
        action.sa_handler = SIG_DFL;
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        execv(argv[2], &argv[2]);
        perror("Cannot execute Xray");
        _exit(127);
    }
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return exitStatus(status);
        if (result < 0 && errno != EINTR) return 1;
        if (stopping || getppid() != parent || (kill(parent, 0) < 0 && errno == ESRCH)) break;
        pauseBriefly();
    }
    kill(child, SIGTERM);
    for (int attempt = 0; attempt < 20; attempt++) {
        if (waitpid(child, &status, WNOHANG) == child) return 0;
        pauseBriefly();
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}
