# System Komunikacji Międzyprocesowej (IPC)

Projekt wieloprocesowy w języku C prezentujący różne mechanizmy komunikacji międzyprocesowej (IPC) w systemach POSIX/Linux:
- **Kolejki komunikatów System V** (`msgget`, `msgsnd`, `msgrcv`)
- **Łącza nienazwane (Pipes)** (`pipe`, `read`, `write`)
- **Łącza nazwane (FIFOs)** (`mkfifo`, `open`, `read`, `write`)
- **Obsługa i propagacja sygnałów** (`sigaction`, `SIGTSTP`, `SIGCONT`, `SIGTERM`, `SIGUSR1`)

## Architektura procesów

1. **PM (Proces Główny - `main`)**:
   - Tworzy zasoby IPC (FIFOs, kolejkę komunikatów, pipe).
   - Uruchamia procesy potomne P1, P2, P3 i weryfikuje ich gotowość (`FIFO_READY`).
   - Udostępnia interaktywne menu lub przyjmuje parametry z linii poleceń.
   - Koordynuje przepływ komend do P3 (`FIFO_CMD`) oraz propagację sygnałów (`FIFO_MAIN` → `FIFO_P3`).
   - Dba o poprawne sprzątanie zasobów IPC po zakończeniu pracy.

2. **P3 (Proces Czytający - `p3`)**:
   - Czyta dane ze standardowego wejścia lub wskazanego pliku.
   - Wysyła wiersze do P2 za pośrednictwem kolejki komunikatów (`msgid`).
   - Po zakończeniu pliku/danych wysyła znacznik końca (`end_flag = 1`) oraz powiadamia PM o ukończeniu.

3. **P2 (Proces Liczący - `p2`)**:
   - Odbiera wiersze tekstu z kolejki komunikatów od P3.
   - Zlicza liczbę znaków w wierszu (`strlen`).
   - Przesyła obliczoną liczbę do P1 przez potok (pipe).
   - Jako jedyny proces przyjmuje bezpośrednie sygnały zewnętrzne (`SIGTSTP`, `SIGCONT`, `SIGTERM`), przekazuje powiadomienia do PM i koordynuje bezpieczne zakończenie po ukończeniu czytania.

4. **P1 (Proces Wyjścia - `p1`)**:
   - Odbiera liczby z potoku (od P2).
   - Wypisuje wyniki na standardowe wyjście (`stdout`).

## Kompilacja

```bash
make
```

Czyszczenie:
```bash
make clean
```

## Uruchomienie

### Tryb menu interaktywnego
```bash
./main
```

### Tryb wsadowy (z argumentami)
```bash
./main stdin
# lub
./main file <sciezka_do_pliku>
```

## Obsługa sygnałów

Sygnały należy wysyłać wyłącznie do procesu **P2**:
- `kill -SIGTSTP <PID_P2>` – wstrzymanie przetwarzania
- `kill -SIGCONT <PID_P2>` – wznowienie przetwarzania
- `kill -SIGTERM <PID_P2>` – bezpieczne zakończenie systemu po dokończeniu bieżącego przetwarzania
