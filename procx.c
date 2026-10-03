/*
 * procx.c - İşletim Sistemleri Dönem Projesi
 * * Güncelleme: Adım 3 - CLI ve Thread Başlatma
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

// --- SABİTLER ---
#define SHM_KEY 1234
#define SEM_KEY 5678
#define MSG_KEY 9012
#define MAX_PROCESSES 20
#define CMD_LEN 64

// --- VERİ YAPILARI ---
typedef struct {
    pid_t pid;
    char name[CMD_LEN];
    time_t start_time;
    int is_running;
} ProcessInfo;

typedef struct {
    ProcessInfo procs[MAX_PROCESSES];
    int process_count;
} SharedData;

struct MessageBuffer {
    long mtype;
    char mtext[100];
};

// --- GLOBAL DEĞİŞKENLER ---
int shm_id, sem_id, msg_id;
SharedData *shared_mem;
int main_running = 1; // Ana döngü kontrolü

// --- FONKSİYON PROTOTİPLERİ ---
void init_system();
void clean_system();
void sem_lock();
void sem_unlock();
void *monitor_thread(void *arg);
void *ipc_listener_thread(void *arg);

// Komut Fonksiyonları
void cmd_start(char *args);
void cmd_list();
void cmd_kill(int pid);

// --- YARDIMCI FONKSİYONLAR ---
void sem_lock() {
    struct sembuf sb = {0, -1, 0};
    if (semop(sem_id, &sb, 1) == -1) { perror("sem_lock"); exit(1); }
}

void sem_unlock() {
    struct sembuf sb = {0, 1, 0};
    if (semop(sem_id, &sb, 1) == -1) { perror("sem_unlock"); exit(1); }
}

void init_system() {
    shm_id = shmget(SHM_KEY, sizeof(SharedData), 0666 | IPC_CREAT);
    if (shm_id < 0) { perror("shmget"); exit(1); }
    shared_mem = (SharedData *)shmat(shm_id, NULL, 0);

    msg_id = msgget(MSG_KEY, 0666 | IPC_CREAT);

    sem_id = semget(SEM_KEY, 1, 0666 | IPC_CREAT | IPC_EXCL);
    if (sem_id >= 0) {
        semctl(sem_id, 0, SETVAL, 1);
        shared_mem->process_count = 0;
        printf("[INFO] Sistem (SHM & Sem) OLUSTURULDU.\n");
    } else if (errno == EEXIST) {
        sem_id = semget(SEM_KEY, 1, 0666);
        printf("[INFO] Mevcut sisteme BAGLANILDI.\n");
    } else {
        perror("semget"); exit(1);
    }
    struct MessageBuffer msg;
    msg.mtype = 1; // Tip 1: Genel Duyuru
    snprintf(msg.mtext, 100, "Yeni bir terminal baglandi (PID: %d)", getpid());
    
    // Mesajı kuyruğa at (msgsnd)
    msgsnd(msg_id, &msg, sizeof(msg.mtext), 0);
}

void clean_system() {
    // Çıkış mesajı gönder
    struct MessageBuffer msg;
    msg.mtype = 1;
    snprintf(msg.mtext, 100, "Terminal ayrildi (PID: %d)", getpid());
    msgsnd(msg_id, &msg, sizeof(msg.mtext), 0);

    shmdt(shared_mem);
}

// --- THREAD FONKSİYONLARI ---
void *monitor_thread(void *arg) {
    int status;
    
    while(main_running) {
        // WNOHANG: Bekleme yapma, ölen varsa söyle, yoksa 0 dön.
        pid_t finished_pid = waitpid(-1, &status, WNOHANG);

        if (finished_pid > 0) {
            // Bir çocuk ölmüş! Listeden silmemiz lazım.
            sem_lock(); // Listeyi değiştirirken kilit şart!
            
            int found_index = -1;
            
            // Ölen PID'yi listede bul
            for (int i = 0; i < shared_mem->process_count; i++) {
                if (shared_mem->procs[i].pid == finished_pid) {
                    found_index = i;
                    break;
                }
            }

            if (found_index != -1) {
                printf("\n[Monitor] Process %d sonlandi (Status: %d). Listeden siliniyor...\nprocx> ", finished_pid, status);
                fflush(stdout); // Prompt'un bozulmasını engelle

                // Listeden silme (Kaydırma işlemi)
                // Silinecek elemanın yerine, son elemanı kopyalamak daha verimlidir ama
                // sıralamayı korumak için kaydırıyoruz:
                for (int j = found_index; j < shared_mem->process_count - 1; j++) {
                    shared_mem->procs[j] = shared_mem->procs[j+1];
                }
                
                shared_mem->process_count--; // Sayıyı azalt
            }
            
            sem_unlock(); // Kilidi aç
        }
        
        sleep(1); // İşlemciyi yormamak için 1 saniye bekle
    }
    return NULL;
}

void *ipc_listener_thread(void *arg) {
    struct MessageBuffer msg;
    
    while(main_running) {
        // msgrcv: Mesaj oku. 
        // 1: Sadece Tip 1 olan mesajları oku (Genel duyurular)
        // IPC_NOWAIT: Mesaj yoksa bekleme, devam et (Loop dönmeye devam etsin)
        if (msgrcv(msg_id, &msg, sizeof(msg.mtext), 1, IPC_NOWAIT) != -1) {
            // Mesaj geldiyse ekrana bas
            // \r ve fflush prompt'un bozulmasını engellemek içindir
            printf("\n[IPC] Mesaj: %s\nprocx> ", msg.mtext);
            fflush(stdout);
        }
        
        sleep(1); // İşlemciyi yormamak için bekle
    }
    return NULL;
}

// --- KOMUT İŞLEYİCİLER ---
void cmd_start(char *args) {
    if (args == NULL) {
        printf("Kullanim: start <program_adi> [argumanlar]\n");
        return;
    }

    // 1. Argümanları execvp için parçala (Örn: "sleep 10" -> ["sleep", "10", NULL])
    char *argv[16]; // Maksimum 16 argüman destekleyelim
    int i = 0;
    char *token = strtok(args, " "); // Boşluklara göre böl
    
    while (token != NULL && i < 15) {
        argv[i++] = token;
        token = strtok(NULL, " ");
    }
    argv[i] = NULL; // Liste sonu NULL olmalı

    if (i == 0) return; // Komut yoksa çık

    // 2. Maksimum process sınırı kontrolü (Kritik Bölge Başlangıcı)
    sem_lock();
    if (shared_mem->process_count >= MAX_PROCESSES) {
        printf("Hata: Maksimum process limitine (%d) ulasildi!\n", MAX_PROCESSES);
        sem_unlock();
        return;
    }
    
    // Yeri şimdiden ayırtıyoruz
    int slot = shared_mem->process_count;
    sem_unlock(); 
    // Kritik Bölge Bitişi (Fork öncesi kilidi bırakmalıyız ki child da takılmasın)

    // 3. FORK İşlemi
    pid_t pid = fork();

    if (pid < 0) {
        perror("Fork hatasi");
    } 
    else if (pid == 0) {
        // --- ÇOCUK PROSES (CHILD) ---
        // Burada execvp ile yeni programı çalıştırıyoruz.
        // Başarılı olursa bu satırdan sonrası asla çalışmaz, child hafızası değişir.
        execvp(argv[0], argv);
        
        // Buraya gelindiyse hata var demektir (örn: komut bulunamadı)
        perror("Exec hatasi");
        exit(1); 
    } 
    else {
        // --- EBEVEYN PROSES (PARENT) ---
        // Çocuğun bilgisini Paylaşılan Belleğe kaydediyoruz
        sem_lock();
        
        // Tekrar kontrol (Race condition ihtimaline karşı)
        slot = shared_mem->process_count; 
        
        shared_mem->procs[slot].pid = pid;
        strncpy(shared_mem->procs[slot].name, argv[0], CMD_LEN); // İsmi kopyala
        shared_mem->procs[slot].start_time = time(NULL);         // Şu anki zaman
        shared_mem->procs[slot].is_running = 1;
        
        shared_mem->process_count++; // Sayacı artır
        
        sem_unlock();
        
        printf("[INFO] Process baslatildi. PID: %d, Komut: %s\n", pid, argv[0]);
    }
}

void cmd_list() {
    sem_lock(); // Okurken verinin değişmesini engellemek için kilitliyoruz
    
    printf("\n%-10s %-20s %-20s %-10s\n", "PID", "ISIM", "BASLANGIC", "DURUM");
    printf("------------------------------------------------------------\n");
    
    if (shared_mem->process_count == 0) {
        printf("Listede hic process yok.\n");
    } else {
        for (int i = 0; i < shared_mem->process_count; i++) {
            ProcessInfo *p = &shared_mem->procs[i];
            
            // Zamanı okunabilir formata çevir
            char time_str[30];
            struct tm *tm_info = localtime(&p->start_time);
            strftime(time_str, 26, "%H:%M:%S", tm_info);
            
            printf("%-10d %-20s %-20s %-10s\n", 
                   p->pid, 
                   p->name, 
                   time_str, 
                   (p->is_running ? "Calisiyor" : "Bitti"));
        }
    }
    printf("------------------------------------------------------------\n");
    printf("Toplam Process: %d\n", shared_mem->process_count);
    
    sem_unlock(); // Kilidi aç
}

void cmd_kill(int pid) {
    if (pid <= 0) {
        printf("Gecersiz PID.\n");
        return;
    }

    // Processin bizim listemizde olup olmadığını kontrol etmek iyi olur
    // ama işletim sistemi zaten yetkimiz yoksa hata verir.
    
    printf(">> PID %d sonlandiriliyor...\n", pid);
    
    // SIGTERM (15): Kibarca kapat (Kaydetme şansı ver)
    // Eğer kapanmazsa SIGKILL (9) gerekebilir ama ödev için SIGTERM yeterli.
    if (kill(pid, SIGTERM) == 0) {
        printf("Sinyal gonderildi.\n");
        // Not: Listeden silme işini burada YAPMIYORUZ.
        // Process ölünce 'monitor_thread' onu yakalayıp silecek.
    } else {
        perror("Kill hatasi");
    }
}

// --- ANA FONKSİYON ---
int main(int argc, char *argv[]) {
    init_system();
    
    // Threadleri Başlat
    pthread_t t_monitor, t_listener;
    pthread_create(&t_monitor, NULL, monitor_thread, NULL);
    pthread_create(&t_listener, NULL, ipc_listener_thread, NULL);

    printf("ProcX Shell Aktif. Cikis icin 'exit' yazin.\n");

    char input[256];
    char *cmd, *args;

    while (main_running) {
        printf("procx> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;

        // Sondaki \n karakterini sil
        input[strcspn(input, "\n")] = 0;

        // Komutu parçala (Örn: "start sleep 10") -> cmd="start", args="sleep 10"
        cmd = strtok(input, " ");
        args = strtok(NULL, ""); // Geriye kalan her şeyi argüman al

        if (cmd == NULL) continue; // Boş enter'a basıldıysa geç

        if (strcmp(cmd, "exit") == 0) {
            main_running = 0; // Thread döngülerini bitir
            break;
        } 
        else if (strcmp(cmd, "start") == 0) {
            cmd_start(args);
        }
        else if (strcmp(cmd, "list") == 0) {
            cmd_list();
        }
        else if (strcmp(cmd, "kill") == 0) {
            if (args) cmd_kill(atoi(args));
            else printf("Hata: PID girilmedi.\n");
        }
        else {
            printf("Bilinmeyen komut: %s\n", cmd);
        }
    }

    // Çıkış işlemleri
    printf("Cikis yapiliyor...\n");
    // Threadlerin bitmesini bekle
    // pthread_join(t_monitor, NULL); 
    // pthread_join(t_listener, NULL);
    // Not: join yaparsak sleep bitene kadar bekler, o yüzden şimdilik hızlı çıkış için kapattım.
    
    clean_system();
    return 0;
}
