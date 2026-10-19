#include "common.h"

volatile sig_atomic_t signal_received = 0;
volatile sig_atomic_t is_suspended = 0;
volatile sig_atomic_t should_terminate = 0;
int msgid;
char *input_source;
FILE *input_file = NULL;

void cleanup() {
    if (input_file && input_file != stdin) {
        fclose(input_file);
    }
}

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
    fprintf(stderr, "[P3] Sygnały zewnętrzne zablokowane (SIGINT, SIGTSTP, SIGCONT, SIGTERM)\n");
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
    struct message msg;
    int fifo_fd;
    int cmd_fifo_fd;
    struct signal_info sig_info;
    struct readiness_msg ready_msg;
    int ready_fifo;
    struct p3_command cmd;
    struct completion_msg comp_msg;
    
    fprintf(stderr, "[P3] Uruchamianie procesu P3 (PID: %d)\n", getpid());
    
    if (argc < 2) {
        fprintf(stderr, "[P3] Usage: %s <msgid>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    msgid = atoi(argv[1]);
    
    /* Zablokuj sygnały zewnętrzne - P3 nie powinien na nie reagować */
    block_external_signals();
    
    /* Skonfiguruj handler dla powiadomień FIFO */
    setup_fifo_handler();
    
    /* Wyślij sygnał gotowości do procesu głównego PRZED otwarciem innych FIFO */
    ready_fifo = open(FIFO_READY, O_WRONLY);
    if (ready_fifo >= 0) {
        ready_msg.pid = getpid();
        strcpy(ready_msg.process_name, "P3");
        ssize_t n = write(ready_fifo, &ready_msg, sizeof(ready_msg));
        if (n != sizeof(ready_msg)) {
            perror("[P3] write FIFO_READY");
        } else {
            fprintf(stderr, "[P3] Wysłano potwierdzenie gotowości\n");
        }
        /* Pozostaw FIFO otwarte - zostanie zamknięte automatycznie po zakończeniu procesu */
    } else {
        perror("[P3] open FIFO_READY");
        fprintf(stderr, "[P3] Nie udało się wysłać potwierdzenia gotowości\n");
    }
    
    /* Otwórz FIFO do odczytu powiadomień o sygnałach */
    fifo_fd = open(FIFO_P3, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("[P3] open FIFO_P3");
        exit(EXIT_FAILURE);
    }
    
    /* Otwórz FIFO komend do odbierania poleceń */
    cmd_fifo_fd = open(FIFO_CMD, O_RDONLY);
    if (cmd_fifo_fd < 0) {
        perror("[P3] open FIFO_CMD");
        close(fifo_fd);
        exit(EXIT_FAILURE);
    }
    
    fprintf(stderr, "[P3] Czekam na komendy...\n");
    
    /* Główna pętla: czekaj na komendy i przetwarzaj je */
    while (1) {
        /* Sprawdź żądanie zakończenia */
        if (should_terminate) {
            fprintf(stderr, "[P3] Zażądano zakończenia, kończę\n");
            break;
        }
        
        /* Użyj select aby czekać na komendę z timeoutem do okresowego sprawdzania sygnałów */
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(cmd_fifo_fd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000; /* 500ms */
        
        int select_result = select(cmd_fifo_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) {
                /* Sprawdź czy otrzymano sygnał */
                if (signal_received) {
                    signal_received = 0;
                    ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
                    if (sig_n > 0) {
                        fprintf(stderr, "[P3] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                                sig_info.signal_num, sig_info.sender_pid);
                        
                        if (sig_info.signal_num == SIGTERM) {
                            fprintf(stderr, "[P3] Otrzymano SIGTERM - zakończenie\n");
                            
                            /* Powiadom P2 aby odczytał FIFO */
                            int fifo_p2 = open(FIFO_P2, O_WRONLY | O_NONBLOCK);
                            if (fifo_p2 >= 0) {
                                write(fifo_p2, &sig_info, sizeof(sig_info));
                                close(fifo_p2);
                                fprintf(stderr, "[P3] Powiadomiono P2 o sygnale przez FIFO\n");
                            } else {
                                fprintf(stderr, "[P3] Nie można otworzyć FIFO_P2 dla powiadomienia P2\n");
                            }
                            
                            should_terminate = 1;
                            break;
                        }
                    }
                }
                continue;
            }
            perror("[P3] select");
            break;
        } else if (select_result == 0) {
            /* Timeout - sprawdź sygnały */
            if (signal_received) {
                signal_received = 0;
                ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
                if (sig_n > 0) {
                    fprintf(stderr, "[P3] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                            sig_info.signal_num, sig_info.sender_pid);
                    
                    if (sig_info.signal_num == SIGTERM) {
                        fprintf(stderr, "[P3] Otrzymano SIGTERM - zakończenie\n");
                        
                        /* Powiadom P2 aby odczytał FIFO */
                        int fifo_p2 = open(FIFO_P2, O_WRONLY | O_NONBLOCK);
                        if (fifo_p2 >= 0) {
                            write(fifo_p2, &sig_info, sizeof(sig_info));
                            close(fifo_p2);
                            fprintf(stderr, "[P3] Powiadomiono P2 o sygnale przez FIFO\n");
                        } else {
                            fprintf(stderr, "[P3] Nie można otworzyć FIFO_P2 dla powiadomienia P2\n");
                        }
                        
                        should_terminate = 1;
                        break;
                    }
                }
            }
            continue; /* Timeout, kontynuuj czekanie */
        }
        
        /* Dane dostępne w cmd_fifo */
        ssize_t n = read(cmd_fifo_fd, &cmd, sizeof(cmd));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[P3] read FIFO_CMD");
            break;
        } else if (n == 0) {
            /* FIFO komend zamknięte - proces główny zakończony */
            fprintf(stderr, "[P3] Komenda FIFO zamknięta, zakończenie\n");
            break;
        }
        
        /* Przetwórz komendę */
        if (cmd.cmd_type == CMD_TYPE_STDIN) {
            input_file = stdin;
            fprintf(stderr, "[P3] Czytanie ze stdin\n");
        } else if (cmd.cmd_type == CMD_TYPE_FILE) {
            input_file = fopen(cmd.filename, "r");
            if (!input_file) {
                perror("[P3] fopen");
                fprintf(stderr, "[P3] Nie można otworzyć pliku: %s\n", cmd.filename);
                /* Wyślij koniec danych aby wskazać błąd/zakończenie */
                msg.mtype = MSG_TYPE;
                msg.end_flag = 1;
                strcpy(msg.mtext, "");
                msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0);
                
                /* Wyślij powiadomienie o zakończeniu do procesu głównego */
                int fifo_main = open(FIFO_MAIN, O_WRONLY | O_NONBLOCK);
                if (fifo_main >= 0) {
                    comp_msg.msg_type = MSG_TYPE_COMPLETION;
                    comp_msg.pid = getpid();
                    comp_msg.cmd_type = cmd.cmd_type;
                    ssize_t written = write(fifo_main, &comp_msg, sizeof(comp_msg));
                    if (written < 0) {
                        perror("[P3] write completion to FIFO_MAIN (error case)");
                    }
                    close(fifo_main);
                }
                continue;
            }
            fprintf(stderr, "[P3] Czytanie z pliku: %s\n", cmd.filename);
        }
        
        /* Czytaj i wysyłaj wszystkie linie */
        msg.mtype = MSG_TYPE;
        msg.end_flag = 0;
        
        while (1) {
            /* Sprawdź czy jesteśmy zawieszeni - czekaj aż otrzymamy SIGCONT */
            while (is_suspended) {
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 100000; /* 100ms */
                select(0, NULL, NULL, NULL, &tv);
                
                /* Sprawdź sygnały FIFO podczas zawieszenia */
                if (signal_received) {
                    signal_received = 0;
                    ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
                    if (sig_n > 0) {
                        if (sig_info.signal_num == SIGCONT) {
                            fprintf(stderr, "[P3] Wznawianie ze stanu zawieszenia (przez FIFO)\n");
                            is_suspended = 0;
                        } else if (sig_info.signal_num == SIGTERM) {
                            fprintf(stderr, "[P3] SIGTERM podczas zawieszenia - zakończę po ukończeniu pliku\n");
                            should_terminate = 1;
                        }
                        
                        /* Przekaż do P2 */
                        int fifo_p2 = open(FIFO_P2, O_WRONLY);
                        if (fifo_p2 >= 0) {
                            write(fifo_p2, &sig_info, sizeof(sig_info));
                            close(fifo_p2);
                            fprintf(stderr, "[P3] Przekazano sygnał %d do P2 podczas zawieszenia\n", sig_info.signal_num);
                        }
                    }
                }
            }
            
            /* Dla stdin, użyj select() aby umożliwić sprawdzanie sygnałów przed blokowaniem na fgets() */
            if (input_file == stdin) {
                fd_set read_fds;
                struct timeval timeout;
                
                while (1) {
                    /* Sprawdź zawieszenie ponownie na wypadek gdyby zostało ustawione po zewnętrznym sprawdzeniu */
                    if (is_suspended) {
                        break; /* Wróć do pętli obsługi zawieszenia */
                    }
                    
                    FD_ZERO(&read_fds);
                    FD_SET(STDIN_FILENO, &read_fds);
                    FD_SET(fifo_fd, &read_fds);
                    
                    int max_fd = (STDIN_FILENO > fifo_fd) ? STDIN_FILENO : fifo_fd;
                    
                    timeout.tv_sec = 0;
                    timeout.tv_usec = 100000; /* 100ms */
                    
                    int select_result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
                    
                    if (select_result < 0) {
                        if (errno == EINTR) continue;
                        perror("[P3] select");
                        break;
                    }
                    
                    /* Sprawdź powiadomienie o sygnale przez FIFO najpierw (wyższy priorytet) */
                    if (signal_received) {
                        signal_received = 0;
                        ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
                        if (sig_n > 0) {
                            fprintf(stderr, "[P3] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                                    sig_info.signal_num, sig_info.sender_pid);
                            
                            /* Obsłuż zawieszenie/wznowienie */
                            if (sig_info.signal_num == SIGTSTP) {
                                fprintf(stderr, "[P3] Przechodzę w stan zawieszenia podczas odczytu stdin\n");
                                is_suspended = 1;
                                
                                /* Przekaż do P2 */
                                int fifo_p2 = open(FIFO_P2, O_WRONLY);
                                if (fifo_p2 >= 0) {
                                    write(fifo_p2, &sig_info, sizeof(sig_info));
                                    close(fifo_p2);
                                    fprintf(stderr, "[P3] Przekazano SIGTSTP do P2\n");
                                }
                                break; /* Wyjdź z pętli select aby obsłużyć zawieszenie */
                            } else if (sig_info.signal_num == SIGCONT) {
                                fprintf(stderr, "[P3] Otrzymano SIGCONT podczas odczytu stdin\n");
                                is_suspended = 0;
                                
                                /* Przekaż do P2 */
                                int fifo_p2 = open(FIFO_P2, O_WRONLY);
                                if (fifo_p2 >= 0) {
                                    write(fifo_p2, &sig_info, sizeof(sig_info));
                                    close(fifo_p2);
                                    fprintf(stderr, "[P3] Przekazano SIGCONT do P2\n");
                                }
                                /* Kontynuuj czytanie */
                            } else if (sig_info.signal_num == SIGTERM) {
                                fprintf(stderr, "[P3] Otrzymano powiadomienie SIGTERM podczas stdin - zakończę po EOF\n");
                                should_terminate = 1;
                                
                                /* Przekaż do P2 */
                                int fifo_p2 = open(FIFO_P2, O_WRONLY);
                                if (fifo_p2 >= 0) {
                                    write(fifo_p2, &sig_info, sizeof(sig_info));
                                    close(fifo_p2);
                                    fprintf(stderr, "[P3] Przekazano SIGTERM do P2\n");
                                }
                            }
                        }
                    }
                    
                    /* Sprawdź czy stdin ma gotowe dane */
                    if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                        /* Dane dostępne w stdin, odczytaj je */
                        break;
                    }
                    
                    /* Timeout - sprawdź sygnały i kontynuuj */
                }
                
                /* Jeśli zawieszony, wróć na górę głównej pętli */
                if (is_suspended) {
                    continue;
                }
            }
            
            /* Teraz odczytaj linię (dla plików, dzieje się to natychmiast; dla stdin, po potwierdzeniu select że dane są gotowe) */
            if (fgets(msg.mtext, MAX_LINE_LEN, input_file) == NULL) {
                /* Koniec pliku lub błąd */
                break;
            }
            
            /* Usuń końcowy znak nowej linii jeśli jest obecny */
            size_t len = strlen(msg.mtext);
            if (len > 0 && msg.mtext[len - 1] == '\n') {
                msg.mtext[len - 1] = '\0';
            }
            
            fprintf(stderr, "[P3] Wysyłanie linii: %s\n", msg.mtext);
            
            if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) < 0) {
                if (errno == EINTR) {
                    fprintf(stderr, "[P3] msgsnd przerwane przez sygnał - ponawiam\n");
                    /* Ponawiaj msgsnd - sygnał został dostarczony ale musimy wysłać tę wiadomość */
                    while (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) < 0) {
                        if (errno != EINTR) {
                            perror("[P3] msgsnd retry failed");
                            break;
                        }
                    }
                } else {
                    perror("[P3] msgsnd");
                    break;
                }
            }
            
            /* Sprawdź powiadomienia o sygnałach przez FIFO */
            if (signal_received) {
                signal_received = 0;
                ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
                if (sig_n > 0) {
                    fprintf(stderr, "[P3] Otrzymano powiadomienie o sygnale przez FIFO: %d od PID %d\n",
                            sig_info.signal_num, sig_info.sender_pid);
                    
                    /* Obsłuż zawieszenie/wznowienie */
                    if (sig_info.signal_num == SIGTSTP) {
                        fprintf(stderr, "[P3] Przechodzę w stan zawieszenia\n");
                        is_suspended = 1;
                    } else if (sig_info.signal_num == SIGCONT) {
                        fprintf(stderr, "[P3] Wznawianie ze stanu zawieszenia\n");
                        is_suspended = 0;
                    } else if (sig_info.signal_num == SIGTERM) {
                        fprintf(stderr, "[P3] Otrzymano powiadomienie SIGTERM - zakończę po ukończeniu bieżącego pliku\n");
                        should_terminate = 1;
                        /* NIE przerywaj tutaj - pozwól ukończyć czytanie pliku */
                    }
                    
                    /* Powiadom P2 aby odczytał FIFO */
                    int fifo_p2 = open(FIFO_P2, O_WRONLY);
                    if (fifo_p2 >= 0) {
                        ssize_t written = write(fifo_p2, &sig_info, sizeof(sig_info));
                        if (written < 0) {
                            perror("[P3] write to FIFO_P2");
                        }
                        close(fifo_p2);
                        fprintf(stderr, "[P3] Powiadomiono P2 o sygnale przez FIFO\n");
                    }
                }
            }
        }
        
        fprintf(stderr, "[P3] Zakończono czytanie wejścia\n");
        
        /* Zamknij plik jeśli był otwarty, lub wyczyść EOF na stdin */
        if (input_file && input_file != stdin) {
            fclose(input_file);
            input_file = NULL;
        } else if (input_file == stdin) {
            /* Wyczyść flagę EOF na stdin żeby można było go ponownie odczytać */
            clearerr(stdin);
            fprintf(stderr, "[P3] Wyczyszczono EOF na stdin do ponownego użycia\n");
        }
        
        /* Wyślij wiadomość o końcu danych */
        msg.end_flag = 1;
        strcpy(msg.mtext, "");
        fprintf(stderr, "[P3] Wysyłanie wiadomości o końcu danych\n");
        
        if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) < 0) {
            if (errno == EINTR) {
                fprintf(stderr, "[P3] msgsnd (koniec-danych) przerwane - ponawiam\n");
                /* Ponawiaj aż do sukcesu lub prawdziwego błędu */
                while (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) < 0) {
                    if (errno != EINTR) {
                        perror("[P3] msgsnd (end-of-data) retry failed");
                        break;
                    }
                }
            } else {
                perror("[P3] msgsnd (end-of-data)");
                break;
            }
        }
        
        /* Wyślij powiadomienie o zakończeniu do procesu głównego */
        int fifo_main = open(FIFO_MAIN, O_WRONLY | O_NONBLOCK);
        if (fifo_main >= 0) {
            comp_msg.msg_type = MSG_TYPE_COMPLETION;
            comp_msg.pid = getpid();
            comp_msg.cmd_type = cmd.cmd_type;
            ssize_t written = write(fifo_main, &comp_msg, sizeof(comp_msg));
            if (written < 0 && errno != EAGAIN) {
                perror("[P3] write completion to FIFO_MAIN");
            } else if (written == sizeof(comp_msg)) {
                fprintf(stderr, "[P3] Wysłano powiadomienie o zakończeniu do procesu głównego\n");
            }
            close(fifo_main);
        }
        
        /* Sprawdź oczekujące sygnały jeszcze raz przed czekaniem na następną komendę */
        if (signal_received) {
            signal_received = 0;
            ssize_t sig_n = read(fifo_fd, &sig_info, sizeof(sig_info));
            if (sig_n > 0) {
                fprintf(stderr, "[P3] Otrzymano oczekujący sygnał po zakończeniu pliku: %d od PID %d\n",
                        sig_info.signal_num, sig_info.sender_pid);
                
                if (sig_info.signal_num == SIGTERM) {
                    fprintf(stderr, "[P3] Otrzymano SIGTERM - zakończenie\n");
                    
                    /* Powiadom P2 */
                    int fifo_p2 = open(FIFO_P2, O_WRONLY | O_NONBLOCK);
                    if (fifo_p2 >= 0) {
                        write(fifo_p2, &sig_info, sizeof(sig_info));
                        close(fifo_p2);
                        fprintf(stderr, "[P3] Powiadomiono P2 o SIGTERM\n");
                    }
                    
                    should_terminate = 1;
                }
            }
        }
        
        /* Jeśli zażądano zakończenia, wyjdź po ukończeniu bieżącej operacji */
        if (should_terminate) {
            fprintf(stderr, "[P3] Zakończenie po ukończeniu bieżącej operacji\n");
            break;
        }
        
        fprintf(stderr, "[P3] Czekam na następną komendę...\n");
    }
    
    /* Sprzątanie */
    close(fifo_fd);
    close(cmd_fifo_fd);
    
    fprintf(stderr, "[P3] Proces P3 kończy działanie\n");
    return 0;
}
