#include "common.h"

#define INPUT_BUFFER_SIZE 10
#define SOURCE_TYPE_SIZE 10

int msgid = -1;
int pipefd[2];
int cmd_fifo_fd = -1;  /* Command FIFO kept open for entire program lifetime */
pid_t pid_p1 = -1;
pid_t pid_p2 = -1;
pid_t pid_p3 = -1;

/* Track if processes are currently running */
int processes_running = 0;

/* Track if PIDs have been displayed */
int pids_displayed = 0;

/* Store last P2 PID for termination instructions */
pid_t last_pid_p2 = -1;

/* FIFO for signal monitoring in menu mode */
int fifo_main_menu_fd = -1;

/* Flag to indicate child termination */
volatile sig_atomic_t child_terminated = 0;

void sigchld_handler(int sig) {
    (void)sig;
    child_terminated = 1;
}

void reset_process_state() {
    pid_p1 = -1;
    pid_p2 = -1;
    pid_p3 = -1;
    processes_running = 0;
    last_pid_p2 = -1;
}

void cleanup_resources() {
    fprintf(stderr, "[MAIN] Cleaning up resources\n");
    
    /* Close command FIFO */
    if (cmd_fifo_fd >= 0) {
        close(cmd_fifo_fd);
        cmd_fifo_fd = -1;
    }
    
    /* Remove message queue */
    if (msgid >= 0) {
        msgctl(msgid, IPC_RMID, NULL);
        fprintf(stderr, "[MAIN] Message queue removed\n");
        msgid = -1;
    }
    
    /* Remove FIFOs */
    unlink(FIFO_P1);
    unlink(FIFO_P2);
    unlink(FIFO_P3);
    unlink(FIFO_MAIN);
    unlink(FIFO_READY);
    unlink(FIFO_CMD);
    fprintf(stderr, "[MAIN] FIFOs removed\n");
}

void wait_for_all_children() {
    int status;
    pid_t result;
    int count = 0;
    int saved_errno = 0;
    
    /* Wait for all remaining child processes to terminate */
    /* Note: Some children may have already been reaped by earlier waitpid calls */
    while (1) {
        result = waitpid(-1, &status, 0);
        if (result > 0) {
            fprintf(stderr, "[MAIN] Child process %d terminated\n", result);
            count++;
        } else {
            /* Save errno immediately when waitpid fails */
            saved_errno = errno;
            break;
        }
    }
    
    /* When waitpid returns -1 with errno==ECHILD, all children have been reaped */
    if (result == -1 && saved_errno == ECHILD) {
        fprintf(stderr, "[MAIN] All children have terminated (%d reaped here)\n", count);
    } else {
        fprintf(stderr, "[MAIN] Done waiting for children (%d reaped here, errno=%d)\n", count, saved_errno);
    }
}

void shutdown_system() {
    fprintf(stderr, "[MAIN] Shutting down system\n");
    
    /* Close command FIFO to signal P3 to exit */
    if (cmd_fifo_fd >= 0) {
        close(cmd_fifo_fd);
        cmd_fifo_fd = -1;
    }
    
    /* Send SIGUSR1 to P3 to wake it up so it can detect closed FIFO */
    if (pid_p3 > 0) {
        kill(pid_p3, SIGUSR1);
    }
    
    /* Remove message queue to unblock P2 if it's stuck in msgrcv waiting for messages from P3 */
    if (msgid >= 0) {
        msgctl(msgid, IPC_RMID, NULL);
        fprintf(stderr, "[MAIN] Message queue removed\n");
        msgid = -1;
    }
    
    /* Wait for all children to terminate */
    wait_for_all_children();
    
    /* Cleanup remaining resources */
    cleanup_resources();
    
    fprintf(stderr, "[MAIN] Main process terminating\n");
}

/* Check FIFO_MAIN for signals and forward to P3 */
void check_and_forward_signals() {
    if (fifo_main_menu_fd < 0) {
        return; /* FIFO not open */
    }
    
    /* Non-blocking check for signals */
    struct signal_info sig_info;
    ssize_t n = read(fifo_main_menu_fd, &sig_info, sizeof(sig_info));
    
    if (n == sizeof(sig_info) && sig_info.msg_type == MSG_TYPE_SIGNAL) {
        fprintf(stderr, "[MAIN] Received signal %d from P2, forwarding to P3\n", sig_info.signal_num);
        
        /* Forward to P3 */
        if (pid_p3 > 0) {
            int fifo_p3 = open(FIFO_P3, O_WRONLY | O_NONBLOCK);
            if (fifo_p3 >= 0) {
                ssize_t written = write(fifo_p3, &sig_info, sizeof(sig_info));
                if (written == sizeof(sig_info)) {
                    fprintf(stderr, "[MAIN] Forwarded signal %d to P3\n", sig_info.signal_num);
                    /* Send SIGUSR1 to P3 to notify it to read FIFO */
                    kill(pid_p3, SIGUSR1);
                } else {
                    perror("[MAIN] write to FIFO_P3");
                }
                close(fifo_p3);
            } else {
                fprintf(stderr, "[MAIN] Cannot open FIFO_P3 (P3 may have terminated)\n");
            }
        }
    }
}


int display_menu_and_get_choice() {
    int choice = 0;
    char input[INPUT_BUFFER_SIZE];
    
    /* Check if child terminated before displaying menu */
    if (child_terminated) {
        fprintf(stderr, "[MAIN] Child termination detected, initiating shutdown\n");
        return -2; /* Special value to indicate shutdown */
    }
    
    fprintf(stderr, "\n=== System komunikacji międzyprocesowej ===\n");
    fprintf(stderr, "1. Wyświetl PID-y procesów\n");
    fprintf(stderr, "2. Uruchom z stdin\n");
    fprintf(stderr, "3. Uruchom z pliku\n");
    if (processes_running && last_pid_p2 > 0) {
        fprintf(stderr, "\nAby zakończyć system wyślij SIGTERM do procesu P2 (PID: %d)\n", last_pid_p2);
        fprintf(stderr, "Przykład: kill -SIGTERM %d\n", last_pid_p2);
    }
    fprintf(stderr, "Wybór (1/2/3): ");
    
    while (1) {
        /* Check if child terminated during input wait */
        if (child_terminated) {
            fprintf(stderr, "\n[MAIN] Child termination detected during menu\n");
            return -2; /* Special value to indicate shutdown */
        }
        
        /* Check for signals from P2 before waiting for input */
        check_and_forward_signals();
        
        /* Use select to wait for stdin AND FIFO_MAIN with timeout */
        fd_set read_fds;
        struct timeval timeout;
        int max_fd = STDIN_FILENO;
        
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        
        /* Also monitor FIFO_MAIN for signals if it's open */
        if (fifo_main_menu_fd >= 0) {
            FD_SET(fifo_main_menu_fd, &read_fds);
            if (fifo_main_menu_fd > max_fd) {
                max_fd = fifo_main_menu_fd;
            }
        }
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; /* 100ms timeout for better responsiveness */
        
        int select_result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) continue; /* Interrupted by signal, retry */
            perror("[MAIN] select");
            return -2;
        } else if (select_result == 0) {
            /* Timeout - check for signals and continue */
            continue;
        }
        
        /* Check if FIFO has data (signal from P2) */
        if (fifo_main_menu_fd >= 0 && FD_ISSET(fifo_main_menu_fd, &read_fds)) {
            check_and_forward_signals();
            /* Don't read stdin yet, loop again */
            continue;
        }
        
        /* stdin has data ready */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (fgets(input, sizeof(input), stdin) == NULL) {
                /* EOF detected (Ctrl+D) */
                if (isatty(STDIN_FILENO)) {
                    /* Interactive terminal - clear EOF and redisplay menu */
                    clearerr(stdin);
                    fprintf(stderr, "\n[MAIN] EOF detected, powrót do menu\n");
                    return -1;  /* Signal to redisplay menu */
                } else {
                    /* Non-interactive (pipe/file) - clear EOF and redisplay menu */
                    /* The ONLY way to terminate is SIGTERM to P2 */
                    clearerr(stdin);
                    fprintf(stderr, "\n[MAIN] EOF detected, powrót do menu\n");
                    fprintf(stderr, "[MAIN] Aby zakończyć system, wyślij SIGTERM do procesu P2\n");
                    return -1;  /* Signal to redisplay menu */
                }
            }
            
            /* Remove newline if present */
            input[strcspn(input, "\n")] = 0;
            
            /* Validate input */
            if (strlen(input) == 1 && (input[0] >= '1' && input[0] <= '3')) {
                choice = input[0] - '0';
                break;
            } else {
                fprintf(stderr, "[MAIN] Nieprawidłowy wybór. Proszę wpisać 1, 2 lub 3.\n");
                fprintf(stderr, "Wybór (1/2/3): ");
            }
        }
    }
    
    return choice;
}

int get_filename(char *filename, size_t max_len) {
    fprintf(stderr, "Podaj nazwę pliku: ");
    
    if (fgets(filename, max_len, stdin) == NULL) {
        /* EOF detected (Ctrl+D) - return to menu */
        fprintf(stderr, "\n[MAIN] Operacja anulowana (EOF)\n");
        return -1;
    }
    
    /* Remove newline if present */
    filename[strcspn(filename, "\n")] = 0;
    
    /* Check if filename is empty */
    if (strlen(filename) == 0) {
        fprintf(stderr, "[MAIN] Nie podano nazwy pliku\n");
        return -1;
    }
    
    /* Check if file exists */
    if (access(filename, R_OK) != 0) {
        fprintf(stderr, "[MAIN] Nie można otworzyć pliku '%s'\n", filename);
        return -1;
    }
    
    return 0;
}

int create_fifos() {
    /* Remove old FIFOs if they exist */
    unlink(FIFO_P1);
    unlink(FIFO_P2);
    unlink(FIFO_P3);
    unlink(FIFO_MAIN);
    unlink(FIFO_READY);
    unlink(FIFO_CMD);
    
    /* Create new FIFOs */
    if (mkfifo(FIFO_P1, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_P1");
        return -1;
    }
    if (mkfifo(FIFO_P2, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_P2");
        return -1;
    }
    if (mkfifo(FIFO_P3, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_P3");
        return -1;
    }
    if (mkfifo(FIFO_MAIN, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_MAIN");
        return -1;
    }
    if (mkfifo(FIFO_READY, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_READY");
        return -1;
    }
    if (mkfifo(FIFO_CMD, 0666) < 0) {
        perror("[MAIN] mkfifo FIFO_CMD");
        return -1;
    }
    
    fprintf(stderr, "[MAIN] FIFO utworzone pomyślnie\n");
    return 0;
}

int create_message_queue() {
    /* Utwórz plik klucza dla ftok */
    const char *keyfile = "/tmp/msgq_key";
    FILE *f = fopen(keyfile, "w");
    if (f) {
        fprintf(f, "key");
        fclose(f);
    }
    
    key_t key = ftok(keyfile, MSG_QUEUE_KEY);
    if (key == (key_t)-1) {
        perror("[MAIN] ftok");
        return -1;
    }
    
    msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid < 0) {
        perror("[MAIN] msgget");
        return -1;
    }
    
    fprintf(stderr, "[MAIN] Kolejka komunikatów utworzona (ID: %d, Klucz: %d)\n", msgid, key);
    return 0;
}

void wyswietl_wszystkie_pidy() {
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "        IDENTYFIKATORY PROCESÓW (PID)\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "PM  (Proces Główny):    %d\n", getpid());
    fprintf(stderr, "P1  (Proces Wyjścia):   %d\n", pid_p1);
    fprintf(stderr, "P2  (Proces Liczący):   %d\n", pid_p2);
    fprintf(stderr, "P3  (Proces Czytający): %d\n", pid_p3);
    fprintf(stderr, "========================================\n\n");
}

void wyswietl_instrukcje_sygnalow() {
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "    INSTRUKCJE TESTOWANIA SYGNAŁÓW\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Proces P2 (PID: %d) obsługuje sygnały zewnętrzne:\n\n", pid_p2);
    
    fprintf(stderr, "Aby ZATRZYMAĆ P2 (wstrzymać przetwarzanie):\n");
    fprintf(stderr, "  kill -SIGTSTP %d\n", pid_p2);
    fprintf(stderr, "  lub: kill -20 %d\n\n", pid_p2);
    
    fprintf(stderr, "Aby WZNOWIĆ P2 (kontynuować przetwarzanie):\n");
    fprintf(stderr, "  kill -SIGCONT %d\n", pid_p2);
    fprintf(stderr, "  lub: kill -18 %d\n\n", pid_p2);
    
    fprintf(stderr, "Aby ZAKOŃCZYĆ P2 (po przetworzeniu wszystkich danych):\n");
    fprintf(stderr, "  kill -SIGTERM %d\n", pid_p2);
    fprintf(stderr, "  lub: kill -15 %d\n\n", pid_p2);
    
    fprintf(stderr, "UWAGA:\n");
    fprintf(stderr, "- P1 i P3 BLOKUJĄ sygnały zewnętrzne (TSTP/TERM/CONT)\n");
    fprintf(stderr, "- Reagują tylko na powiadomienia przez FIFO od PM\n");
    fprintf(stderr, "- P2 przetwarza sygnały i przekazuje info do PM przez FIFO\n");
    fprintf(stderr, "========================================\n\n");
}

void start_processes() {
    char msgid_str[32];
    char pipefd_str[32];
    char parent_pid_str[32];
    pid_t main_pid = getpid();
    int ready_fd;
    struct readiness_msg ready_msg;
    int ready_count = 0;
    
    snprintf(msgid_str, sizeof(msgid_str), "%d", msgid);
    snprintf(pipefd_str, sizeof(pipefd_str), "%d", pipefd[1]);
    snprintf(parent_pid_str, sizeof(parent_pid_str), "%d", main_pid);
    
    /* Utwórz P1 */
    pid_p1 = fork();
    if (pid_p1 < 0) {
        perror("[MAIN] fork P1");
        cleanup_resources();
        exit(EXIT_FAILURE);
    } else if (pid_p1 == 0) {
        /* Proces potomny P1 */
        close(pipefd[1]); /* Zamknij koniec zapisu */
        
        snprintf(pipefd_str, sizeof(pipefd_str), "%d", pipefd[0]);
        execl("./p1", "p1", pipefd_str, NULL);
        perror("[MAIN] execl p1");
        exit(EXIT_FAILURE);
    }
    close(pipefd[0]); /* Rodzic zamyka koniec odczytu */
    fprintf(stderr, "[MAIN] Uruchomiono P1 (PID: %d)\n", pid_p1);
    
    /* Utwórz P2 */
    pid_p2 = fork();
    if (pid_p2 < 0) {
        perror("[MAIN] fork P2");
        cleanup_resources();
        exit(EXIT_FAILURE);
    } else if (pid_p2 == 0) {
        /* Proces potomny P2 */
        snprintf(pipefd_str, sizeof(pipefd_str), "%d", pipefd[1]);
        execl("./p2", "p2", msgid_str, pipefd_str, parent_pid_str, NULL);
        perror("[MAIN] execl p2");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[MAIN] Uruchomiono P2 (PID: %d)\n", pid_p2);
    
    /* Zapisz PID P2 dla instrukcji zakończenia */
    last_pid_p2 = pid_p2;
    
    /* Utwórz P3 - teraz przekazuje tylko msgid, bez źródła/nazwy pliku */
    pid_p3 = fork();
    if (pid_p3 < 0) {
        perror("[MAIN] fork P3");
        cleanup_resources();
        exit(EXIT_FAILURE);
    } else if (pid_p3 == 0) {
        /* Proces potomny P3 */
        close(pipefd[1]); /* Zamknij koniec zapisu */
        
        execl("./p3", "p3", msgid_str, NULL);
        perror("[MAIN] execl p3");
        exit(EXIT_FAILURE);
    }
    close(pipefd[1]); /* Rodzic zamyka koniec zapisu */
    fprintf(stderr, "[MAIN] Uruchomiono P3 (PID: %d)\n", pid_p3);
    
    /* Oznacz procesy jako działające */
    processes_running = 1;
    
    /* Teraz otwórz FIFO gotowości - procesy potomne działają i otworzą do zapisu */
    ready_fd = open(FIFO_READY, O_RDONLY);
    if (ready_fd < 0) {
        perror("[MAIN] open FIFO_READY");
        cleanup_resources();
        exit(EXIT_FAILURE);
    }
    
    /* Czekaj aż wszystkie 3 procesy zasygnalizują gotowość */
    fprintf(stderr, "[MAIN] Czekam na potwierdzenie gotowości procesów...\n");
    
    while (ready_count < 3) {
        ssize_t n = read(ready_fd, &ready_msg, sizeof(ready_msg));
        if (n == sizeof(ready_msg)) {
            fprintf(stderr, "[MAIN] Proces %s (PID: %d) gotowy\n", ready_msg.process_name, ready_msg.pid);
            ready_count++;
        } else if (n == 0) {
            /* EOF - wszyscy zapisujący zamknęli FIFO (nie powinno się zdarzyć zanim wszystkie 3 procesy zakończą) */
            fprintf(stderr, "[MAIN] Nieoczekiwane EOF na FIFO_READY\n");
            break;
        } else if (n < 0) {
            if (errno == EINTR) {
                continue; /* Przerwane przez sygnał, ponawiaj */
            }
            perror("[MAIN] read FIFO_READY");
            break;
        }
    }
    
    close(ready_fd);
    
    /* Zweryfikuj czy wszystkie procesy są gotowe */
    if (ready_count < 3) {
        fprintf(stderr, "[MAIN] Tylko %d procesy potwierdziły gotowość (oczekiwano 3)\n", ready_count);
        cleanup_resources();
        exit(EXIT_FAILURE);
    }
    
    fprintf(stderr, "[MAIN] Wszystkie procesy gotowe\n");
    
    /* Nie wyświetlaj PID automatycznie - użytkownik może zażądać przez opcję menu 1 */
    pids_displayed = 1;  /* Oznacz jako gotowe do wyświetlenia, ale nie wyświetlaj automatycznie */
}

void monitor_signals() {
    int fifo_main;
    int children_terminated = 0;
    fd_set read_fds;
    struct timeval timeout;
    
    fprintf(stderr, "[MAIN] Otwieranie FIFO_MAIN do monitorowania\n");
    
    /* Otwórz FIFO do odczytu sygnałów od P2 */
    fifo_main = open(FIFO_MAIN, O_RDONLY | O_NONBLOCK);
    if (fifo_main < 0) {
        perror("[MAIN] open FIFO_MAIN");
        return;
    }
    
    fprintf(stderr, "[MAIN] Monitorowanie sygnałów od P2\n");
    
    while (children_terminated < 3) {
        /* Najpierw sprawdź czy jakiekolwiek dziecko zakończyło się */
        int status;
        pid_t result;
        while ((result = waitpid(-1, &status, WNOHANG)) > 0) {
            fprintf(stderr, "[MAIN] Proces potomny %d zakończony\n", result);
            children_terminated++;
            if (children_terminated >= 3) {
                break;
            }
        }
        
        if (children_terminated >= 3) {
            break;
        }
        
        /* Konfiguruj select() z timeoutem 100ms dla efektywności */
        FD_ZERO(&read_fds);
        FD_SET(fifo_main, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; /* 100ms */
        
        int select_result = select(fifo_main + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result > 0 && FD_ISSET(fifo_main, &read_fds)) {
            /* Dane dostępne w FIFO - może być sygnał lub powiadomienie o zamknięciu */
            union {
                struct signal_info sig;
                struct shutdown_msg shutdown;
                char buffer[256];
            } msg_union;
            
            ssize_t n = read(fifo_main, &msg_union, sizeof(msg_union));
            
            if (n >= (ssize_t)sizeof(int)) {
                /* Sprawdź typ wiadomości (pierwsze pole w obu strukturach) */
                int msg_type = msg_union.sig.msg_type;
                
                if (msg_type == MSG_TYPE_SIGNAL) {
                    fprintf(stderr, "[MAIN] Otrzymano powiadomienie o sygnale: %d od PID %d\n",
                            msg_union.sig.signal_num, msg_union.sig.sender_pid);
                    
                    /* Zapisz sygnał do FIFO P3 - tylko jeśli P3 nadal żyje */
                    if (pid_p3 > 0) {
                        int fifo_p3 = open(FIFO_P3, O_WRONLY | O_NONBLOCK);
                        if (fifo_p3 >= 0) {
                            ssize_t written = write(fifo_p3, &msg_union.sig, sizeof(struct signal_info));
                            if (written < 0) {
                                perror("[MAIN] write to FIFO_P3");
                            } else {
                                fprintf(stderr, "[MAIN] Powiadomiono P3 przez FIFO\n");
                                
                                /* Wyślij SIGUSR1 do P3 aby powiadomić go o odczycie FIFO */
                                kill(pid_p3, SIGUSR1);
                                
                                /* P3 przekaże sygnał do P2 - NIE powiadamiaj P2 bezpośrednio */
                                /* Łańcuch sygnałów: P2 → MAIN → P3 → P2 → P1 */
                            }
                            close(fifo_p3);
                        } else {
                            fprintf(stderr, "[MAIN] Nie można otworzyć FIFO_P3 (P3 prawdopodobnie zakończony)\n");
                        }
                    }
                } else if (msg_type == MSG_TYPE_SHUTDOWN) {
                    fprintf(stderr, "[MAIN] Otrzymano powiadomienie o zamknięciu od P2\n");
                    /* Nie przerywaj - pozwól normalnej detekcji zakończenia dzieci to obsłużyć */
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
    }
    
    fprintf(stderr, "[MAIN] Wszystkie %d procesy potomne zakończone\n", children_terminated);
    close(fifo_main);
}

int send_command_to_p3(const char *source_type, const char *filename) {
    struct p3_command cmd;
    
    /* Przygotuj komendę */
    if (strcmp(source_type, "stdin") == 0) {
        cmd.cmd_type = CMD_TYPE_STDIN;
        cmd.filename[0] = '\0';
    } else if (strcmp(source_type, "file") == 0) {
        cmd.cmd_type = CMD_TYPE_FILE;
        strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
        cmd.filename[sizeof(cmd.filename) - 1] = '\0';
    } else {
        fprintf(stderr, "[MAIN] Nieprawidłowy typ źródła: %s\n", source_type);
        return -1;
    }
    
    /* Użyj globalnego FIFO komend (pozostawionego otwartego) */
    if (cmd_fifo_fd < 0) {
        /* Otwórz FIFO komend jeśli jeszcze nie jest otwarte */
        cmd_fifo_fd = open(FIFO_CMD, O_WRONLY);
        if (cmd_fifo_fd < 0) {
            perror("[MAIN] open FIFO_CMD");
            return -1;
        }
    }
    
    /* Wyślij komendę do P3 */
    ssize_t written = write(cmd_fifo_fd, &cmd, sizeof(cmd));
    
    if (written != sizeof(cmd)) {
        perror("[MAIN] write FIFO_CMD");
        return -1;
    }
    
    fprintf(stderr, "[MAIN] Komenda wysłana do P3\n");
    return 0;
}

int run_program(const char *source_type, const char *filename) {
    int fifo_main;
    union {
        struct completion_msg comp;
        struct signal_info sig;
        struct shutdown_msg shutdown;
        char buffer[256];
    } msg_union;
    fd_set read_fds;
    int shutdown_requested = 0;
    
    /* Wyślij komendę do P3 aby rozpocząć przetwarzanie */
    if (send_command_to_p3(source_type, filename) < 0) {
        fprintf(stderr, "[MAIN] Nie udało się wysłać komendy do P3\n");
        return -1;
    }
    
    /* Czekaj na powiadomienie o zakończeniu od P3 zamiast sleep */
    fprintf(stderr, "[MAIN] Czekam na zakończenie przetwarzania...\n");
    
    /* Otwórz FIFO do odczytu powiadomienia o zakończeniu */
    fifo_main = open(FIFO_MAIN, O_RDONLY | O_NONBLOCK);
    if (fifo_main < 0) {
        perror("[MAIN] open FIFO_MAIN");
        return -1;
    }
    
    /* Czekaj na wiadomość o zakończeniu - bez timeoutu, czekaj w nieskończoność */
    while (1) {
        /* Sprawdź czy jakiekolwiek dziecko zakończyło się (bez blokowania) */
        int status;
        pid_t child_result = waitpid(-1, &status, WNOHANG);
        if (child_result > 0) {
            fprintf(stderr, "[MAIN] Proces potomny %d zakończony podczas przetwarzania\n", child_result);
            /* Jeśli P2 zakończył się, wywołaj zamknięcie */
            if (child_result == pid_p2) {
                fprintf(stderr, "[MAIN] P2 zakończony - inicjowanie zamknięcia\n");
                shutdown_requested = 1;
                break;
            }
        }
        
        FD_ZERO(&read_fds);
        FD_SET(fifo_main, &read_fds);
        
        /* Użyj timeoutu 500ms aby okresowo sprawdzać procesy potomne (zmniejsza użycie CPU) */
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000; /* 500ms */
        int select_result = select(fifo_main + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result > 0 && FD_ISSET(fifo_main, &read_fds)) {
            /* Dane dostępne w FIFO - odczytaj i sprawdź typ wiadomości */
            ssize_t n = read(fifo_main, &msg_union, sizeof(msg_union));
            
            if (n >= (ssize_t)sizeof(int)) {
                /* Dostęp do msg_type przez którykolwiek element unii (oba mają to jako pierwsze pole) */
                int msg_type = msg_union.comp.msg_type;
                
                if (msg_type == MSG_TYPE_COMPLETION) {
                    fprintf(stderr, "[MAIN] Otrzymano powiadomienie o zakończeniu od P3 (PID: %d)\n", 
                            msg_union.comp.pid);
                    break;
                } else if (msg_type == MSG_TYPE_SIGNAL) {
                    /* Powiadomienie o sygnale od P2 - przekaż do P3 */
                    fprintf(stderr, "[MAIN] Otrzymano powiadomienie o sygnale podczas przetwarzania: %d od PID %d\n",
                            msg_union.sig.signal_num, msg_union.sig.sender_pid);
                    
                    /* Przekaż do P3 */
                    int fifo_p3 = open(FIFO_P3, O_WRONLY | O_NONBLOCK);
                    if (fifo_p3 >= 0) {
                        ssize_t written = write(fifo_p3, &msg_union.sig, sizeof(struct signal_info));
                        if (written < 0) {
                            perror("[MAIN] write to FIFO_P3");
                        } else {
                            fprintf(stderr, "[MAIN] Przekazano sygnał %d do P3\n", msg_union.sig.signal_num);
                            /* Wyślij SIGUSR1 do P3 aby powiadomić go o odczycie FIFO */
                            if (pid_p3 > 0) {
                                kill(pid_p3, SIGUSR1);
                            }
                        }
                        close(fifo_p3);
                    }
                    
                    /* P3 przekaże sygnał do P2 - NIE powiadamiaj P2 bezpośrednio tutaj */
                    /* Łańcuch sygnałów: P2 (zewnętrzny) → MAIN → P3 → P2 (FIFO) → P1 */
                    
                    /* Kontynuuj czekanie na zakończenie */
                    continue;
                } else if (msg_type == MSG_TYPE_SHUTDOWN) {
                    /* P2 żąda zamknięcia systemu */
                    fprintf(stderr, "[MAIN] Otrzymano żądanie zamknięcia od P2 (PID: %d, powód: sygnał %d)\n",
                            msg_union.shutdown.pid, msg_union.shutdown.reason);
                    shutdown_requested = 1;
                    break;
                } else {
                    fprintf(stderr, "[MAIN] Nieznany typ wiadomości: %d\n", msg_type);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("[MAIN] read FIFO_MAIN");
                break;
            }
        } else if (select_result < 0 && errno != EINTR) {
            perror("[MAIN] select");
            break;
        }
        /* Jeśli timeout lub EINTR, po prostu kontynuuj pętlę i sprawdź dzieci ponownie */
    }
    
    close(fifo_main);
    
    if (shutdown_requested) {
        fprintf(stderr, "[MAIN] Przetwarzanie zakończone - zażądano zamknięcia\n");
        return 1;  /* Sygnalizuj że zażądano zamknięcia */
    }
    
    fprintf(stderr, "[MAIN] Przetwarzanie zakończone\n");
    return 0;
}

int main(int argc, char *argv[]) {
    char source_type[SOURCE_TYPE_SIZE] = "stdin";
    char filename[MAX_LINE_LEN] = "";
    struct sigaction sa;
    
    fprintf(stderr, "[MAIN] Uruchamianie procesu głównego (PID: %d)\n", getpid());
    
    /* Zablokuj wszystkie sygnały zewnętrzne - MAIN powinien reagować tylko przez menu lub gdy dzieci zakończą się */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGCONT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    fprintf(stderr, "[MAIN] Sygnały zewnętrzne zablokowane (SIGINT, SIGTERM, SIGTSTP, SIGCONT)\n");
    
    /* Skonfiguruj handler SIGCHLD aby wykrywać zakończenie dzieci */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; /* Nie powiadamiaj o stop/kontynuacji */
    sigaction(SIGCHLD, &sa, NULL);
    fprintf(stderr, "[MAIN] Handler SIGCHLD zainstalowany\n");
    
    /* Zainicjalizuj zasoby RAZ przy starcie */
    /* Utwórz FIFO */
    if (create_fifos() < 0) {
        fprintf(stderr, "[MAIN] Nie udało się utworzyć FIFO\n");
        exit(EXIT_FAILURE);
    }
    
    /* Utwórz kolejkę komunikatów */
    if (create_message_queue() < 0) {
        fprintf(stderr, "[MAIN] Nie udało się utworzyć kolejki komunikatów\n");
        cleanup_resources();
        exit(EXIT_FAILURE);
    }
    
    /* Utwórz pipe dla P2 -> P1 */
    if (pipe(pipefd) < 0) {
        perror("[MAIN] pipe");
        cleanup_resources();
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[MAIN] Pipe utworzony dla komunikacji P2->P1\n");
    
    /* Uruchom wszystkie procesy RAZ przy starcie - będą działać przez cały czas życia programu */
    start_processes();
    
    /* Otwórz FIFO_MAIN do odczytu żeby P2 mógł zapisywać powiadomienia o sygnałach w dowolnym momencie */
    /* To musi być zrobione PO uruchomieniu procesów żeby P2 istniał */
    int fifo_main_fd = open(FIFO_MAIN, O_RDONLY | O_NONBLOCK);
    if (fifo_main_fd < 0) {
        perror("[MAIN] open FIFO_MAIN for menu mode");
        shutdown_system();
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[MAIN] Otwarto FIFO_MAIN do monitorowania sygnałów w trybie menu\n");
    
    /* Sprawdź czy uruchomiono z argumentami linii poleceń (kompatybilność wsteczna) */
    if (argc >= 2) {
        /* Podano argumenty - kompatybilność wsteczna, bez pętli menu */
        if (strcmp(argv[1], "stdin") == 0) {
            strcpy(source_type, "stdin");
        } else if (strcmp(argv[1], "file") == 0 && argc >= 3) {
            strcpy(source_type, "file");
            strncpy(filename, argv[2], sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
            
            /* Zwaliduj czy plik istnieje */
            if (access(filename, R_OK) != 0) {
                fprintf(stderr, "[MAIN] Nie można otworzyć pliku '%s'\n", filename);
                exit(EXIT_FAILURE);
            }
        } else {
            fprintf(stderr, "[MAIN] Usage: %s [stdin | file <filename>]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        
        /* Uruchom raz - procesy będą czekać na SIGTERM */
        int result = run_program(source_type, filename);
        if (result == 1) {
            /* Zażądano zamknięcia przez P2 */
            fprintf(stderr, "[MAIN] Zażądano zamknięcia przez P2\n");
            shutdown_system();
            return 0;
        }
        fprintf(stderr, "[MAIN] Przetwarzanie zakończone. Aby zamknąć program, wyślij SIGTERM do P2 (PID: %d)\n", pid_p2);
        
        /* Czekaj aż dzieci zakończą się - monitoruj żądanie zamknięcia */
        fprintf(stderr, "[MAIN] Czekam na SIGTERM do P2...\n");
        
        /* NIE zamykaj FIFO komend tutaj - pozostaw P3 żywego aby otrzymywał propagację sygnałów */
        /* FIFO komend zostanie zamknięte w shutdown_system() */
        
        /* Kontynuuj monitorowanie sygnałów nawet po zakończeniu przetwarzania */
        monitor_signals();
        
        shutdown_system();
        return 0;
    }
    
    /* Brak argumentów - tryb interaktywnego menu */
    /* JEDYNYM sposobem wyjścia jest SIGTERM do P2 */
    while (1) {
        /* Sprawdź czy jakikolwiek proces potomny zakończył się (np. P2 po SIGTERM podczas czytania pliku) */
        if (child_terminated) {
            int status;
            pid_t result = waitpid(-1, &status, WNOHANG);
            if (result > 0) {
                fprintf(stderr, "[MAIN] Child process %d terminated\n", result);
                shutdown_system();
                return 0;
            }
        }
        
        int choice = display_menu_and_get_choice();
        
        if (choice == -2) {
            /* Dziecko zakończone podczas menu */
            shutdown_system();
            return 0;
        } else if (choice == -1) {
            /* Wykryto EOF na stdin - monitoruj sygnały czekając na zakończenie dzieci */
            fprintf(stderr, "[MAIN] EOF na stdin, czekam na zakończenie dzieci...\n");
            monitor_signals();
            shutdown_system();
            return 0;
        } else if (choice == 1) {
            /* Wyświetl PID */
            if (processes_running) {
                wyswietl_wszystkie_pidy();
                wyswietl_instrukcje_sygnalow();
            } else {
                fprintf(stderr, "\n[MAIN] Procesy nie są jeszcze uruchomione.\n");
                fprintf(stderr, "[MAIN] Wybierz opcję 2 lub 3, aby uruchomić system.\n");
            }
        } else if (choice == 2) {
            /* Uruchom ze stdin */
            strcpy(source_type, "stdin");
            fprintf(stderr, "[MAIN] Wybrano: Standardowe wejście\n");
            int result = run_program(source_type, NULL);
            if (result == 1) {
                /* Zażądano zamknięcia */
                shutdown_system();
                return 0;
            }
        } else if (choice == 3) {
            /* Uruchom z pliku */
            strcpy(source_type, "file");
            if (get_filename(filename, sizeof(filename)) < 0) {
                /* Błąd pobierania nazwy pliku lub EOF - wróć do menu */
                continue;
            }
            fprintf(stderr, "[MAIN] Wybrano: Plik '%s'\n", filename);
            int result = run_program(source_type, filename);
            if (result == 1) {
                /* Zażądano zamknięcia */
                shutdown_system();
                return 0;
            }
        }
    }
    
    fprintf(stderr, "[MAIN] Proces główny kończy działanie\n");
    return 0;
}
