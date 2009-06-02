/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <netdb.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "mtpd.h"

int the_socket = -1;

extern struct protocol l2tp;
static struct protocol *protocols[] = {&l2tp, NULL};
static struct protocol *the_protocol;

static int pppd_argc;
static char **pppd_argv;
static pid_t pppd_pid;

/* We redirect signals to a pipe in order to prevent race conditions. */
static int signals[2];

static void interrupt(int signal)
{
    write(signals[1], &signal, sizeof(int));
}

static int initialize(int argc, char **argv)
{
    int timeout = 0;
    int i;

    for (i = 2; i < argc; ++i) {
        if (!argv[i][0]) {
            pppd_argc = argc - i - 1;
            pppd_argv = &argv[i + 1];
            argc = i;
            break;
        }
    }

    if (argc >= 2) {
        for (i = 0; protocols[i]; ++i) {
            if (!strcmp(argv[1], protocols[i]->name)) {
                log_print(INFO, "Using protocol %s", protocols[i]->name);
                the_protocol = protocols[i];
                timeout = the_protocol->connect(argc - 2, &argv[2]);
                break;
            }
        }
    }

    if (!the_protocol || timeout == -USAGE_ERROR) {
        printf("Usage: %s <protocol-args> '' <pppd-args>, "
               "where protocol-args are one of:\n", argv[0]);
        for (i = 0; protocols[i]; ++i) {
            printf("       %s %s\n", protocols[i]->name, protocols[i]->usage);
        }
        exit(USAGE_ERROR);
    }
    return timeout;
}

static void stop_pppd()
{
    if (pppd_pid) {
        log_print(INFO, "Sending signal to pppd (pid = %d)", pppd_pid);
        kill(pppd_pid, SIGTERM);
        sleep(5);
        pppd_pid = 0;
    }
}

int main(int argc, char **argv)
{
    struct pollfd pollfds[2];
    int timeout;
    int error = 0;

    srandom(time(NULL));

    if (pipe(signals) == -1) {
        log_print(FATAL, "Pipe() %s", strerror(errno));
        exit(SYSTEM_ERROR);
    }

    signal(SIGHUP, interrupt);
    signal(SIGINT, interrupt);
    signal(SIGTERM, interrupt);
    signal(SIGCHLD, interrupt);
    signal(SIGPIPE, SIG_IGN);
    atexit(stop_pppd);

    timeout = initialize(argc, argv);
    pollfds[0].fd = signals[0];
    pollfds[0].events = POLLIN;
    pollfds[1].fd = the_socket;
    pollfds[1].events = POLLIN;

    while (timeout >= 0) {
        if (poll(pollfds, 2, timeout ? timeout : -1) == -1 && errno != EINTR) {
            log_print(FATAL, "Poll() %s", strerror(errno));
            exit(SYSTEM_ERROR);
        }
        if (pollfds[0].revents) {
            break;
        }
        timeout = pollfds[1].revents ?
            the_protocol->process() : the_protocol->timeout();
    }

    if (timeout < 0) {
        error = -timeout;
    } else {
        int signal;
        read(signals[0], &signal, sizeof(int));
        log_print(INFO, "Received signal %d", signal);
        if (signal == SIGCHLD && waitpid(pppd_pid, &error, WNOHANG) == pppd_pid
            && WIFEXITED(error)) {
            error = WEXITSTATUS(error);
            log_print(INFO, "Pppd is terminated (status = %d)", error);
            error += PPPD_EXITED;
            pppd_pid = 0;
        } else {
            error = USER_REQUESTED;
        }
    }

    stop_pppd();
    the_protocol->shutdown();

    log_print(INFO, "Mtpd is terminated (status = %d)", error);
    return error;
}

void log_print(int level, char *format, ...)
{
    if (level >= 0 && level <= LOG_MAX) {
        char *levels = "DIWEF";
        va_list ap;
        fprintf(stderr, "%c: ", levels[level]);
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

void create_socket(int family, int type, char *server, char *port)
{
    struct addrinfo hints = {
        .ai_flags = AI_NUMERICSERV,
        .ai_family = family,
        .ai_socktype = type,
    };
    struct addrinfo *records;
    struct addrinfo *r;
    int error;

    log_print(INFO, "Connecting to %s port %s", server, port);

    error = getaddrinfo(server, port, &hints, &records);
    if (error) {
        log_print(FATAL, "Getaddrinfo() %s", (error == EAI_SYSTEM) ?
                  strerror(errno) : gai_strerror(error));
        exit(NETWORK_ERROR);
    }

    for (r = records; r; r = r->ai_next) {
        the_socket = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (the_socket != -1
            && connect(the_socket, r->ai_addr, r->ai_addrlen) == 0) {
            break;
        }
    }

    freeaddrinfo(records);

    if (the_socket == -1) {
        log_print(FATAL, "Connect() %s", strerror(errno));
        exit(NETWORK_ERROR);
    }

    fcntl(the_socket, F_SETFD, FD_CLOEXEC);
    log_print(INFO, "Connection established (socket = %d)", the_socket);
}

void start_pppd(int pppox)
{
    if (pppd_pid) {
        log_print(WARNING, "Pppd is already started (pid = %d)", pppd_pid);
        close(pppox);
        return;
    }

    log_print(INFO, "Starting pppd (pppox = %d)", pppox);

    pppd_pid = fork();
    if (pppd_pid < 0) {
        log_print(FATAL, "Fork() %s", strerror(errno));
        exit(SYSTEM_ERROR);
    }

    if (!pppd_pid) {
        char number[16];
        char *args[1024] = {"", "nodetach", "pppox", number};
        int i;

        sprintf(number, "%d", pppox);
        for (i = 0; i < pppd_argc; ++i) {
            args[4 + i] = pppd_argv[i];
        }

        execvp("pppd", args);
        log_print(FATAL, "Exec() %s", strerror(errno));
        exit(1); /* Pretending a fatal error in pppd. */
    }

    log_print(INFO, "Pppd started (pid = %d)", pppd_pid);
    close(pppox);
}
