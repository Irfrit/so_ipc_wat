#include "common.h"

volatile sig_atomic_t signal_received = 0;
volatile sig_atomic_t is_suspended = 0;
int pipefd;

void handle_fifo_signal(int sig) {
    (void)sig; /* Nieużywane */
    /* Ten handler służy tylko do powiadomień o sygnałach przez FIFO */
    signal_received = 1;
}

void block_external_signals() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGCONT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    fprintf(stderr, "[P1] Sygnały zewnętrzne zablokowane (SIGINT, SIGTSTP, SIGCONT, SIGTERM)\n");
}

void setup_fifo_handler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_fifo_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    /* Użyj SIGUSR1 do powiadomień FIFO */
    sigaction(SIGUSR1, &sa, NULL);
}

int main(int argc, char *argv[]) {
    int count;
    int running = 1;
    int fifo_fd;
    struct signal_info sig_info;
    struct readiness_msg ready_msg;
    int ready_fifo;
    
    if (argc < 2) {
        fprintf(stderr, "[P1] Usage: %s <pipefd>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    pipefd = atoi(argv[1]);
    
    fprintf(stderr, "[P1] Uruchamianie procesu P1 (PID: %d)\n", getpid());
    
    /* Zablokuj sygnały zewnętrzne - P1 nie powinien na nie reagować */
    block_external_signals();
    
    /* Skonfiguruj handler dla powiadomień FIFO */
    setup_fifo_handler();
    
    /* Wyślij sygnał gotowości do procesu głównego PRZED otwarciem innych FIFO */
    ready_fifo = open(FIFO_READY, O_WRONLY);
    if (ready_fifo >= 0) {
        ready_msg.pid = getpid();
        strcpy(ready_msg.process_name, "P1");
        ssize_t n = write(ready_fifo, &ready_msg, sizeof(ready_msg));
        if (n != sizeof(ready_msg)) {
            perror("[P1] write FIFO_READY");
        } else {
            fprintf(stderr, "[P1] Wysłano potwierdzenie gotowości\n");
        }
        /* Pozostaw FIFO otwarte - zostanie zamknięte automatycznie po zakończeniu procesu */
    } else {
        perror("[P1] open FIFO_READY");
        fprintf(stderr, "[P1] Nie udało się wysłać potwierdzenia gotowości\n");
    }
    
    /* Teraz otwórz FIFO do odczytu powiadomień o sygnałach */
    fifo_fd = open(FIFO_P1, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("[P1] open FIFO_P1");
        exit(EXIT_FAILURE);
    }
    
    while (running) {
        /* Sprawdź czy jesteśmy zawieszeni - czekaj aż otrzymamy SIGCONT */
        while (is_suspended && running) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; /* 100ms */
            select(0, NULL, NULL, NULL, &tv);
        }
        
        ssize_t result = read(pipefd, &count, sizeof(count));
        
        if (result < 0) {
            if (errno == EINTR) {
                /* Przerwane przez sygnał - kontynuuj */
                fprintf(stderr, "[P1] Odczyt przerwany przez sygnał\n");
                continue;
            } else {
                perror("[P1] read");
                break;
            }
        } else if (result == 0) {
            /* Pipe zamknięty */
            fprintf(stderr, "[P1] Pipe zamknięty przez P2\n");
            break;
        }
        
        /* Sprawdź czy to znacznik końca danych */
        if (count == END_OF_DATA) {
            fprintf(stderr, "[P1] Otrzymano znacznik końca danych\n");
            /* Nie kończ - czekaj na następną porcję danych */
            fprintf(stderr, "[P1] Oczekiwanie na więcej danych...\n");
            continue;
        }
        
        /* Wyprowadź liczbę na stdout */
        printf("%d\n", count);
        fflush(stdout);
        fprintf(stderr, "[P1] Wyprowadzono liczbę: %d\n", count);
        
        /* Sprawdź powiadomienia o sygnałach przez FIFO */
        if (signal_received) {
            signal_received = 0;
            ssize_t n = read(fifo_fd, &sig_info, sizeof(sig_info));
            if (n > 0) {
                fprintf(stderr, "[P1] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                        sig_info.signal_num, sig_info.sender_pid);
                
                /* Obsłuż zawieszenie/wznowienie */
                if (sig_info.signal_num == SIGTSTP) {
                    fprintf(stderr, "[P1] Przechodzę w stan zawieszenia\n");
                    is_suspended = 1;
                } else if (sig_info.signal_num == SIGCONT) {
                    fprintf(stderr, "[P1] Wznawianie ze stanu zawieszenia\n");
                    is_suspended = 0;
                } else if (sig_info.signal_num == SIGTERM) {
                    fprintf(stderr, "[P1] Otrzymano powiadomienie SIGTERM - zakończenie\n");
                    running = 0;
                    break;
                }
            }
        }
    }
    
    /* Sprzątanie */
    close(fifo_fd);
    close(pipefd);
    
    fprintf(stderr, "[P1] Proces P1 kończy działanie\n");
    return 0;
}
