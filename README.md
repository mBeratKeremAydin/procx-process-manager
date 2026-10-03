# PROCX: a multi-terminal process manager in C

An Operating Systems course project: an interactive shell (`procx>`) that starts and tracks child processes. Several terminals can run ProcX at the same time and see the same process table. It uses **System V shared memory** for the table, a **semaphore** as a mutex, a **message queue** for notices between terminals, and **POSIX threads** for background work.

> **Requires Linux** (System V IPC, `fork`).
> **Snapshot note:** the source header labels this version "Adım 3 - CLI ve Thread Başlatma" (step 3: CLI and thread start). Features beyond that step are not part of this snapshot (see [Known limitations](#known-limitations)).

## Commands

| Command | Behavior |
|---|---|
| `start <program> [args]` | `fork` + `execvp`; records PID, name and start time in the shared table (max 20 processes, up to 15 words including the program name) |
| `list` | prints PID, name, start time and status of every tracked process, from all terminals |
| `kill <pid>` | sends `SIGTERM` |
| `exit` | leaves the shell and announces it to the other terminals |

## How it works

- **Shared memory** (`SHM_KEY 1234`): a `SharedData` struct with up to 20 `ProcessInfo` entries and a counter.
- **Semaphore** (`SEM_KEY 5678`): one System V semaphore used as a mutex around every read and write of the table. The first terminal creates and initializes it (`IPC_CREAT | IPC_EXCL`); later terminals attach to the existing one.
- **Message queue** (`MSG_KEY 9012`): every terminal posts "terminal connected" and "terminal left" notices (type 1); each terminal's listener thread prints them.
- **Monitor thread**: polls `waitpid(-1, ..., WNOHANG)` once a second. When a child ends, it removes the entry from the shared table under the semaphore, so finished children are reaped (no zombies) and disappear from `list`.
- **IPC listener thread**: non-blocking `msgrcv` (`IPC_NOWAIT`), polled once a second.

## Build and run

```bash
make            # gcc -Wall -pthread -g -o procx procx.c
./procx
```

Try it with two terminals:

```text
# terminal 1
./procx
procx> start sleep 60
procx> list

# terminal 2
./procx         # prints "Mevcut sisteme BAGLANILDI." and `list` shows terminal 1's process
```

Program output is in Turkish (for example `Process baslatildi`). After all terminals have exited, remove the leftover IPC objects with `ipcs`, then `ipcrm -M 1234 -S 5678 -Q 9012`.

## Known limitations

Found while reviewing the source for this README. Nothing was changed, to keep the original submission intact.

- Step-3 snapshot (see the note at the top).
- Shared memory, semaphore and message queue are never removed (`IPC_RMID` is not called), so they persist after the last terminal exits.
- The `is_running` field is set once and never updated (finished processes are deleted from the table instead).
- `pthread_join` is commented out; the threads simply end with the process.
- Only the terminal that started a process reaps it. If that terminal exits first, the entry stays in the table.
- `kill` sends only `SIGTERM` and does not check that the PID is in the table.
- Fixed limits: 20 processes, 15 words per command.
- This snapshot was not recompiled when the repository was published (no Linux environment on the publishing machine); a Linux (ELF) binary from the original build existed.

## Türkçe özet

İşletim Sistemleri dönem projesi. ProcX, alt süreçleri başlatıp izleyen etkileşimli bir kabuktur. Birden çok terminal aynı süreç tablosunu **System V paylaşılan bellek** üzerinden görür, tabloya erişim **semaforla** korunur, terminaller arası bildirimler **mesaj kuyruğuyla** gider ve arka planda **iki pthread** çalışır (biri biten süreçleri `waitpid(WNOHANG)` ile toplar, diğeri duyuruları yazar). Komutlar: `start`, `list`, `kill`, `exit`. Linux gerekir. Kaynak dosya başlığı bu sürümü "Adım 3" olarak etiketler; bilinen sınırlar yukarıda listelenmiştir.

## License

MIT, see [LICENSE](LICENSE).
