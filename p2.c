#include "common.h"

volatile sig_atomic_t sigtstp_received = 0;
volatile sig_atomic_t sigcont_received = 0;
volatile sig_atomic_t sigterm_received = 0;
volatile sig_atomic_t signal_from_fifo = 0;
volatile sig_atomic_t is_suspended = 0;
volatile sig_atomic_t end_of_data_received = 0;
volatile sig_atomic_t sigterm_from_fifo = 0;  /* Flag for SIGTERM received via FIFO */
volatile sig_atomic_t has_received_data = 0;  /* Flag to track if we've started processing */

int msgid;
int pipefd;
pid_t parent_pid;
pid_t my_pid;

void notify_parent_shutdown() {
    /* Nie wysyłaj sygnału - pozwól rodzicowi wykryć nasze zakończenie */
    /* Opcjonalnie wyślij przez FIFO jeśli rodzic nadal monitoruje */
    int fifo_main = open(FIFO_MAIN, O_WRONLY | O_NONBLOCK);
    if (fifo_main >= 0) {
        struct shutdown_msg shutdown_msg;
        shutdown_msg.msg_type = MSG_TYPE_SHUTDOWN;
        shutdown_msg.pid = getpid();
        shutdown_msg.reason = SIGTERM;
        ssize_t written = write(fifo_main, &shutdown_msg, sizeof(shutdown_msg));
        if (written == sizeof(shutdown_msg)) {
            fprintf(stderr, "[P2] Wysłano powiadomienie o zakończeniu do rodzica przez FIFO\n");
        }
        close(fifo_main);
    }
}

void signal_handler(int sig, siginfo_t *info, void *context) {
    (void)context; /* Nieużywane */
    
    /* Weryfikuj nadawcę - przetwarzaj tylko sygnały zewnętrzne
     * Odrzuć sygnały generowane przez jądro (si_pid == 0) jak Ctrl+C, Ctrl+Z z terminala
     * Akceptuj tylko sygnały z procesów zewnętrznych przez polecenie kill
     * Odrzuć sygnały od procesu rodzica lub siebie, aby uniknąć wewnętrznych pętli sygnałów
     */
    int is_external = (info->si_pid != 0) && 
                      (info->si_pid != parent_pid && info->si_pid != my_pid);
    
    if (is_external) {
        fprintf(stderr, "[P2] Otrzymano sygnał %d od zewnętrznego PID %d\n", sig, info->si_pid);
        
        /* Wyślij informację o sygnale do procesu rodzica przez FIFO */
        int fifo_main = open(FIFO_MAIN, O_WRONLY | O_NONBLOCK);
        if (fifo_main >= 0) {
            struct signal_info sig_info;
            sig_info.msg_type = MSG_TYPE_SIGNAL;
            sig_info.signal_num = sig;
            sig_info.sender_pid = info->si_pid;
            ssize_t written = write(fifo_main, &sig_info, sizeof(sig_info));
            if (written < 0) {
                perror("[P2] write to FIFO_MAIN");
            }
            close(fifo_main);
            fprintf(stderr, "[P2] Powiadomiono rodzica o sygnale %d przez FIFO\n", sig);
        }
        
        /* Ustaw odpowiednią flagę */
        switch (sig) {
            case SIGTSTP:
                sigtstp_received = 1;
                is_suspended = 1;
                fprintf(stderr, "[P2] Otrzymano SIGTSTP - przechodzę w stan zawieszenia\n");
                break;
            case SIGCONT:
                sigcont_received = 1;
                is_suspended = 0;
                fprintf(stderr, "[P2] Otrzymano SIGCONT - wznawianie ze stanu zawieszenia\n");
                break;
            case SIGTERM:
                sigterm_received = 1;
                fprintf(stderr, "[P2] SIGTERM odroczone - czekam na zakończenie czytania pliku\n");
                break;
        }
    } else {
        fprintf(stderr, "[P2] Zignorowano sygnał %d od wewnętrznego PID %d\n", sig, info->si_pid);
    }
}

void handle_fifo_signal(int sig) {
    (void)sig; /* Nieużywane */
    signal_from_fifo = 1;
}

void setup_signal_handlers() {
    struct sigaction sa;
    sigset_t mask;
    
    /* Zablokuj WSZYSTKIE sygnały najpierw */
    sigfillset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    fprintf(stderr, "[P2] Zablokowano wszystkie sygnały początkowo\n");
    
    /* Następnie odblokuj TYLKO SIGTERM, SIGTSTP, SIGCONT i SIGUSR1 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGCONT);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    fprintf(stderr, "[P2] Odblokowano TYLKO SIGTERM, SIGTSTP, SIGCONT, SIGUSR1\n");
    
    /* Skonfiguruj handler dla SIGTSTP i SIGCONT z SA_SIGINFO i SA_RESTART */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGCONT, &sa, NULL);
    
    /* Skonfiguruj handler dla SIGTERM z SA_SIGINFO ale BEZ SA_RESTART 
     * To zapewnia, że msgrcv() zostanie przerwane przez SIGTERM, umożliwiając łagodne zakończenie */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;  /* Brak SA_RESTART dla SIGTERM */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    
    /* Skonfiguruj handler dla powiadomień FIFO - BEZ SA_RESTART żeby msgrcv zostało przerwane */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_fifo_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* Brak SA_RESTART - chcemy przerwać msgrcv */
    sigaction(SIGUSR1, &sa, NULL);
    
    fprintf(stderr, "[P2] Konfiguracja handlerów sygnałów zakończona\n");
}

int main(int argc, char *argv[]) {
    struct message msg;
    int count;
    int running = 1;
    int fifo_fd;
    struct signal_info sig_info;
    struct readiness_msg ready_msg;
    int ready_fifo;
    
    if (argc < 4) {
        fprintf(stderr, "[P2] Usage: %s <msgid> <pipefd> <parent_pid>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    msgid = atoi(argv[1]);
    pipefd = atoi(argv[2]);
    parent_pid = atoi(argv[3]);
    my_pid = getpid();
    
    fprintf(stderr, "[P2] Uruchamianie procesu P2 (PID: %d, Rodzic PID: %d)\n", my_pid, parent_pid);
    
    /* Skonfiguruj handlery sygnałów */
    setup_signal_handlers();
    
    /* Wyślij sygnał gotowości do procesu głównego PRZED otwarciem innych FIFO */
    ready_fifo = open(FIFO_READY, O_WRONLY);
    if (ready_fifo >= 0) {
        ready_msg.pid = getpid();
        strcpy(ready_msg.process_name, "P2");
        ssize_t n = write(ready_fifo, &ready_msg, sizeof(ready_msg));
        if (n != sizeof(ready_msg)) {
            perror("[P2] write FIFO_READY");
        } else {
            fprintf(stderr, "[P2] Wysłano potwierdzenie gotowości\n");
        }
        /* Pozostaw FIFO otwarte - zostanie zamknięte automatycznie po zakończeniu procesu */
    } else {
        perror("[P2] open FIFO_READY");
        fprintf(stderr, "[P2] Nie udało się wysłać potwierdzenia gotowości\n");
    }
    
    /* Teraz otwórz FIFO do odczytu powiadomień o sygnałach */
    fifo_fd = open(FIFO_P2, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("[P2] open FIFO_P2");
        exit(EXIT_FAILURE);
    }
    
    while (running) {
        ssize_t result;
        
        /* Sprawdź powiadomienia o sygnałach przez FIFO PRZED blokowaniem na msgrcv */
        if (signal_from_fifo) {
            fprintf(stderr, "[P2] Sprawdzanie signal_from_fifo przed msgrcv (flaga ustawiona)\n");
            signal_from_fifo = 0;
            ssize_t n = read(fifo_fd, &sig_info, sizeof(sig_info));
            fprintf(stderr, "[P2] read() z FIFO zwróciło: %zd bytes, errno=%d\n", n, errno);
            if (n > 0) {
                fprintf(stderr, "[P2] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                        sig_info.signal_num, sig_info.sender_pid);
                
                if (sig_info.signal_num == SIGTERM) {
                    /* Jeśli obecnie przetwarzane są dane (nie na końcu danych), odrocz SIGTERM */
                    if (!end_of_data_received) {
                        fprintf(stderr, "[P2] SIGTERM przez FIFO otrzymany podczas przetwarzania - odroczenie\n");
                        sigterm_from_fifo = 1;
                        /* Nie kończ jeszcze - zostanie przetworzony po końcu danych */
                    } else {
                        fprintf(stderr, "[P2] SIGTERM przez FIFO - powiadamianie P1 i zakończenie\n");
                        
                        /* Powiadom P1 aby odczytał FIFO */
                        int fifo_p1 = open(FIFO_P1, O_WRONLY);
                        if (fifo_p1 >= 0) {
                            write(fifo_p1, &sig_info, sizeof(sig_info));
                            close(fifo_p1);
                            fprintf(stderr, "[P2] Powiadomiono P1 o SIGTERM przez FIFO\n");
                        }
                        
                        /* Zamknij pipe aby powiadomić P1 */
                        close(pipefd);
                        running = 0;
                        break;
                    }
                }
                
                /* Dla SIGTSTP/SIGCONT, przekaż do P1 */
                int fifo_p1 = open(FIFO_P1, O_WRONLY);
                if (fifo_p1 >= 0) {
                    write(fifo_p1, &sig_info, sizeof(sig_info));
                    close(fifo_p1);
                    fprintf(stderr, "[P2] Powiadomiono P1 o sygnale %d przez FIFO\n", sig_info.signal_num);
                }
            } else {
                fprintf(stderr, "[P2] signal_from_fifo ustawione ale brak danych w FIFO (n=%zd, errno=%d)\n", n, errno);
            }
        }
        
        /* Sprawdź czy jesteśmy zawieszeni - czekaj aż otrzymamy SIGCONT */
        while (is_suspended) {
            /* Czekaj na SIGCONT - użyj select() z timeoutem dla efektywności */
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; /* 100ms */
            select(0, NULL, NULL, NULL, &tv);
            
            /* Uwaga: SIGTERM NIE powinien powodować natychmiastowego wyjścia ze stanu zawieszenia
             * Zostanie obsłużony przez propagację FIFO gdy zostanie wywołane msgrcv
             * Zawieszenie i zakończenie są niezależnymi kwestiami */
        }
        
        /* Zawsze wykonuj blokujący odbiór - SIGTERM zostanie odroczony do końca danych */
        result = msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), MSG_TYPE, 0);
        
        if (result < 0) {
            if (errno == EINTR) {
                /* Przerwane przez sygnał podczas msgrcv */
                
                /* Sprawdź czy SIGTERM został otrzymany zewnętrznie i powinniśmy go teraz obsłużyć */
                /* Obsłuż SIGTERM gdy:
                 * 1. Przetwarzanie pliku jest zakończone (end_of_data_received == 1)
                 * 2. W trybie menu/bezczynności - żaden komunikat nie został jeszcze otrzymany (nigdy nie otrzymano danych)
                 * Musimy rozróżnić między "aktywnym przetwarzaniem" a "bezczynność/zakończone"
                 */
                if (sigterm_received) {
                    /* Jeśli otrzymaliśmy koniec danych, przejdź do oczekiwania FIFO i zakończenia */
                    /* LUB jeśli nie rozpoczęliśmy jeszcze przetwarzania (tryb bezczynności/menu) */
                    if (end_of_data_received || !has_received_data) {
                        fprintf(stderr, "[P2] msgrcv przerwane przez SIGTERM - oczekiwanie na propagację sygnału\n");
                        fprintf(stderr, "[P2] Czekam na powiadomienie SIGTERM przez FIFO od P3\n");
                        
                        /* Czekaj aż SIGTERM wróci przez łańcuch FIFO */
                        fd_set read_fds;
                        struct timeval timeout;
                        int wait_count = 0;
                        int max_wait = 100; /* 10 sekund */
                        
                        while (wait_count < max_wait && !sigterm_from_fifo) {
                            FD_ZERO(&read_fds);
                            FD_SET(fifo_fd, &read_fds);
                            timeout.tv_sec = 0;
                            timeout.tv_usec = 100000; /* 100ms */
                            
                            int sel_result = select(fifo_fd + 1, &read_fds, NULL, NULL, &timeout);
                            if (sel_result > 0) {
                                ssize_t n = read(fifo_fd, &sig_info, sizeof(sig_info));
                                if (n > 0 && sig_info.signal_num == SIGTERM) {
                                    fprintf(stderr, "[P2] Otrzymano SIGTERM przez FIFO - zamykanie\n");
                                    sigterm_from_fifo = 1;
                                    
                                    /* Notify P1 */
                                    int fifo_p1 = open(FIFO_P1, O_WRONLY);
                                    if (fifo_p1 >= 0) {
                                        write(fifo_p1, &sig_info, sizeof(sig_info));
                                        close(fifo_p1);
                                        fprintf(stderr, "[P2] Powiadomiono P1 o SIGTERM przez FIFO\n");
                                    }
                                    
                                    close(pipefd);
                                    running = 0;
                                    break;
                                }
                            }
                            wait_count++;
                        }
                        
                        if (!sigterm_from_fifo) {
                            fprintf(stderr, "[P2] Przekroczono limit czasu oczekiwania na SIGTERM przez FIFO, przechodzę do zakończenia\n");
                            close(pipefd);
                            running = 0;
                        }
                        break;
                    }
                    /* w przeciwnym razie: Nadal aktywnie przetwarza plik - odrocz SIGTERM */
                }
                
                /* Sprawdź czy otrzymaliśmy powiadomienie o sygnale przez FIFO */
                if (signal_from_fifo) {
                    signal_from_fifo = 0;
                    ssize_t n = read(fifo_fd, &sig_info, sizeof(sig_info));
                    if (n > 0 && sig_info.signal_num == SIGTERM) {
                        fprintf(stderr, "[P2] Otrzymano SIGTERM przez FIFO od P3 - zamykanie\n");
                        
                        /* Powiadom P1 aby odczytał FIFO */
                        int fifo_p1 = open(FIFO_P1, O_WRONLY);
                        if (fifo_p1 >= 0) {
                            write(fifo_p1, &sig_info, sizeof(sig_info));
                            close(fifo_p1);
                            fprintf(stderr, "[P2] Powiadomiono P1 o SIGTERM przez FIFO\n");
                        }
                        
                        /* Zamknij pipe aby powiadomić P1 */
                        close(pipefd);
                        running = 0;
                        break;
                    }
                }
                /* Dla innych sygnałów, kontynuuj czekanie - SIGTERM wróci przez łańcuch FIFO */
                fprintf(stderr, "[P2] msgrcv przerwane przez sygnał - kontynuacja czytania wiadomości\n");
                continue;
            } else if (errno == EIDRM) {
                /* Kolejka komunikatów usunięta - zakończenie systemu zainicjowane przez rodzica */
                fprintf(stderr, "[P2] Kolejka komunikatów usunięta, zakończenie\n");
                running = 0;
                break;
            } else {
                perror("[P2] msgrcv");
                break;
            }
        }
        
        /* Sprawdź znacznik końca danych */
        if (msg.end_flag == 1) {
            fprintf(stderr, "[P2] Otrzymano flagę końca danych\n");
            end_of_data_received = 1;
            
            /* Wyślij koniec danych do P1 */
            count = END_OF_DATA;
            if (write(pipefd, &count, sizeof(count)) < 0) {
                perror("[P2] write (end-of-data)");
            }
            
            /* Przetwórz wszelkie odroczone sygnały PO zakończeniu czytania pliku */
            if (sigtstp_received) {
                fprintf(stderr, "[P2] Przetwarzanie odroczonego SIGTSTP po czytaniu pliku\n");
                sigtstp_received = 0;
            }
            if (sigcont_received) {
                fprintf(stderr, "[P2] Przetwarzanie odroczonego SIGCONT po czytaniu pliku\n");
                sigcont_received = 0;
            }
            
            /* Jeśli SIGTERM został otrzymany podczas czytania pliku, czekaj na propagację FIFO */
            if (sigterm_received || sigterm_from_fifo) {
                fprintf(stderr, "[P2] SIGTERM był otrzymany podczas czytania pliku\n");
                
                /* Jeśli SIGTERM przyszedł już przez FIFO, możemy teraz zakończyć */
                if (sigterm_from_fifo) {
                    fprintf(stderr, "[P2] SIGTERM przez FIFO - powiadamianie P1 i zakończenie\n");
                    
                    /* Powiadom P1 aby odczytał FIFO */
                    struct signal_info term_info;
                    term_info.msg_type = MSG_TYPE_SIGNAL;
                    term_info.signal_num = SIGTERM;
                    term_info.sender_pid = 0;
                    
                    int fifo_p1 = open(FIFO_P1, O_WRONLY);
                    if (fifo_p1 >= 0) {
                        write(fifo_p1, &term_info, sizeof(term_info));
                        close(fifo_p1);
                        fprintf(stderr, "[P2] Powiadomiono P1 o SIGTERM przez FIFO\n");
                    }
                    
                    /* Zamknij pipe aby powiadomić P1 */
                    close(pipefd);
                    running = 0;
                    break;
                } else {
                    /* SIGTERM przyszedł zewnętrznie, czekaj na propagację FIFO */
                    fprintf(stderr, "[P2] Czekam na powiadomienie SIGTERM przez FIFO od P3 (propagacja sygnału)\n");
                    
                    /* Czekaj aż SIGTERM wróci przez łańcuch FIFO */
                    /* Użyj select() do odpytywania FIFO z timeoutem */
                    fd_set read_fds;
                    struct timeval timeout;
                    int wait_count = 0;
                    int max_wait = 100; /* Maksymalnie 10 sekund (100 * 100ms) */
                    
                    while (wait_count < max_wait && !sigterm_from_fifo) {
                        FD_ZERO(&read_fds);
                        FD_SET(fifo_fd, &read_fds);
                        timeout.tv_sec = 0;
                        timeout.tv_usec = 100000; /* 100ms */
                        
                        int sel_result = select(fifo_fd + 1, &read_fds, NULL, NULL, &timeout);
                        if (sel_result > 0) {
                            /* Dane dostępne w FIFO */
                            ssize_t n = read(fifo_fd, &sig_info, sizeof(sig_info));
                            if (n > 0 && sig_info.signal_num == SIGTERM) {
                                fprintf(stderr, "[P2] Otrzymano SIGTERM przez FIFO od P3 - zamykanie\n");
                                sigterm_from_fifo = 1;
                                
                                /* Powiadom P1 */
                                int fifo_p1 = open(FIFO_P1, O_WRONLY);
                                if (fifo_p1 >= 0) {
                                    write(fifo_p1, &sig_info, sizeof(sig_info));
                                    close(fifo_p1);
                                    fprintf(stderr, "[P2] Powiadomiono P1 o SIGTERM przez FIFO\n");
                                }
                                
                                /* Zamknij pipe aby powiadomić P1 */
                                close(pipefd);
                                running = 0;
                                break;
                            }
                        }
                        wait_count++;
                    }
                    
                    if (!sigterm_from_fifo) {
                        fprintf(stderr, "[P2] Przekroczono limit czasu oczekiwania na SIGTERM przez FIFO, przechodzę do zakończenia\n");
                        running = 0;
                        break;
                    }
                }
            }
            
            /* Czytanie pliku zakończone, czekaj na następny plik lub SIGTERM */
            fprintf(stderr, "[P2] Przetwarzanie zakończone, oczekiwanie na następne dane lub SIGTERM\n");
            
            /* NIE resetuj end_of_data_received tutaj - pozostaw ustawione żeby SIGTERM mógł zakończyć */
            
            /* Kontynuuj czekanie na więcej wiadomości (następna operacja na pliku) */
            continue;
        }
        
        /* Otrzymanie normalnej wiadomości (nie koniec danych) oznacza, że nowy plik jest przetwarzany */
        /* Zresetuj flagę końca danych przy rozpoczęciu nowego pliku */
        if (end_of_data_received && msg.end_flag == 0) {
            end_of_data_received = 0;
            has_received_data = 0;  /* Zresetuj dla nowego pliku */
            fprintf(stderr, "[P2] Rozpoczynanie przetwarzania nowego pliku\n");
        }
        
        /* Oznacz że rozpoczęliśmy przetwarzanie danych */
        has_received_data = 1;
        
        /* Policz znaki w linii */
        count = strlen(msg.mtext);
        fprintf(stderr, "[P2] Otrzymano linię: '%s' (długość: %d)\n", msg.mtext, count);
        
        /* Wyślij liczbę do P1 przez pipe */
        if (write(pipefd, &count, sizeof(count)) < 0) {
            perror("[P2] write");
            break;
        }
    }
    
    /* Sprzątanie */
    close(fifo_fd);
    close(pipefd);
    
    fprintf(stderr, "[P2] Proces P2 kończy działanie\n");
    return 0;
}
