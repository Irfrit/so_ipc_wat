#ifndef COMMON_H
#define COMMON_H

#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

/* Ścieżki FIFO */
#define FIFO_P1 "/tmp/fifo_p1"
#define FIFO_P2 "/tmp/fifo_p2"
#define FIFO_P3 "/tmp/fifo_p3"
#define FIFO_MAIN "/tmp/fifo_main"
#define FIFO_READY "/tmp/fifo_ready"
#define FIFO_CMD "/tmp/fifo_cmd"  /* FIFO komend do kontroli P3 */

/* Klucz kolejki komunikatów */
#define MSG_QUEUE_KEY 1234

/* Typ wiadomości */
#define MSG_TYPE 1

/* Maksymalna długość linii */
#define MAX_LINE_LEN 256

/* Specjalna wartość oznaczająca koniec danych */
#define END_OF_DATA -1

/* Struktura wiadomości dla komunikacji P3 -> P2 */
struct message {
    long mtype;
    char mtext[MAX_LINE_LEN];
    int end_flag;  /* 1 = koniec danych, 0 = normalne dane */
};

/* Struktura powiadomienia o sygnale dla FIFO */
struct signal_info {
    int msg_type;  /* 1 = sygnał */
    int signal_num;
    pid_t sender_pid;
};

/* Struktura powiadomienia o gotowości */
struct readiness_msg {
    pid_t pid;
    char process_name[8];  /* "P1", "P2" lub "P3" */
};

/* Struktura komendy do kontroli P3 */
struct p3_command {
    int cmd_type;  /* 0 = czytaj ze stdin, 1 = czytaj z pliku */
    char filename[256];  /* nazwa pliku jeśli cmd_type == 1 */
};

#define CMD_TYPE_STDIN 0
#define CMD_TYPE_FILE 1

/* Struktura powiadomienia o zakończeniu */
struct completion_msg {
    int msg_type;  /* 2 = zakończenie */
    pid_t pid;
    int cmd_type;  /* Która komenda została zakończona */
};

/* Struktura żądania zamknięcia */
struct shutdown_msg {
    int msg_type;  /* 3 = żądanie zamknięcia */
    pid_t pid;
    int reason;  /* Sygnał który spowodował zamknięcie (np. SIGTERM) */
};

#define MSG_TYPE_SIGNAL 1
#define MSG_TYPE_COMPLETION 2
#define MSG_TYPE_SHUTDOWN 3

#endif /* COMMON_H */
