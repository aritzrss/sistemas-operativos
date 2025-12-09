#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdbool.h>
#include <mqueue.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "hospital.h"

// --- CONFIGURACIÓN ---
#define NOMBRE_COLA "/cola_hospital"
#define MAX_MSG_SIZE 256

// --- VARIABLES GLOBALES ---
int pacientes_dados_de_alta = 0;
pid_t pid_hospital, pid_recepcion;
mqd_t mq_recepcion; // Descriptor de la cola de mensajes

// Semáforos para coordinar los hilos dentro del Hospital
sem_t sem_diag_listo; // Indica que Exploración terminó y Diagnóstico puede empezar
sem_t sem_farm_listo; // Indica que Diagnóstico terminó y Farmacia puede empezar
// Semáforos para proteger los buffers compartidos (mutex)
sem_t mutex_buffer1;
sem_t mutex_buffer2;

// Buffers para pasar el nombre del paciente entre hilos
char buffer_expl_a_diag[128];
char buffer_diag_a_farm[128];

// Flag para terminar bucles limpiamente
volatile sig_atomic_t fin_simulacion = 0;

int tiempo_aleatorio(int min, int max) {
    return rand() % (max - min + 1) + min;
}

// --- MANEJADORES DE SEÑALES ---

// Manejador para contar pacientes dados de alta (Para el proceso Recepción)
void manejador_alta(int sig) {
    pacientes_dados_de_alta++;
    printf("[Recepción] AVISO: Paciente dado de alta. Total: %d\n", pacientes_dados_de_alta);
}

// Manejador para CTRL+C (Limpieza de recursos)
void manejador_fin(int sig) {
    fin_simulacion = 1;
}

// --- HILOS DEL HOSPITAL ---

void* exploracion(void* args) {
    printf("[Exploración] Comienzo mi ejecución...\n");
    char paciente_actual[128];
    struct mq_attr attr;
    
    // Abrir la cola en modo lectura
    mqd_t mq = mq_open(NOMBRE_COLA, O_RDONLY);
    if (mq == (mqd_t)-1) {
        perror("[Exploración] Error abriendo cola");
        pthread_exit(NULL);
    }

    while (!fin_simulacion) {
        printf("[Exploración] Esperando a un paciente...\n");
        
        // Recibir mensaje de la cola (bloqueante)
        ssize_t bytes_read = mq_receive(mq, paciente_actual, 128, NULL);
        if (bytes_read < 0) {
            if (errno == EINTR) continue; // Si fue interrumpido por señal, reintentar
            perror("[Exploración] Error recibiendo mensaje");
            break; 
        }

        printf("[Exploración] Recibido paciente: %s. Realizando exploración...\n", paciente_actual);
        sleep(tiempo_aleatorio(1, 3));
        
        // Pasar paciente a Diagnóstico de forma segura
        sem_wait(&mutex_buffer1); // Bloqueo acceso al buffer
        strcpy(buffer_expl_a_diag, paciente_actual);
        sem_post(&mutex_buffer1); // Libero acceso
        
        printf("[Exploración] Exploración completa. Enviando a diagnóstico...\n");
        sem_post(&sem_diag_listo); // Aviso al hilo Diagnóstico
    }
    mq_close(mq);
    return NULL;
}

void* diagnostico(void* args) {
    printf("[Diagnóstico] Comienzo mi ejecución...\n");
    char paciente_actual[128];

    while (!fin_simulacion) {
        // Esperar a que Exploración avise
        sem_wait(&sem_diag_listo);
        if (fin_simulacion) break;

        // Leer del buffer compartido 1
        sem_wait(&mutex_buffer1);
        strcpy(paciente_actual, buffer_expl_a_diag);
        sem_post(&mutex_buffer1);

        printf("[Diagnóstico] Atendiendo a %s. Realizando pruebas...\n", paciente_actual);
        sleep(tiempo_aleatorio(2, 4)); // Tiempos ajustados para fluidez

        // Pasar paciente a Farmacia
        sem_wait(&mutex_buffer2);
        strcpy(buffer_diag_a_farm, paciente_actual);
        sem_post(&mutex_buffer2);

        printf("[Diagnóstico] Diagnóstico completado para %s. Notificando farmacia...\n", paciente_actual);
        sem_post(&sem_farm_listo); // Aviso al hilo Farmacia
    }
    return NULL;
}

void* farmacia(void* args) {
    printf("[Farmacia] Comienzo mi ejecución...\n");
    char paciente_actual[128];

    while (!fin_simulacion) {
        // Esperar a que Diagnóstico avise
        sem_wait(&sem_farm_listo);
        if (fin_simulacion) break;

        // Leer del buffer compartido 2
        sem_wait(&mutex_buffer2);
        strcpy(paciente_actual, buffer_diag_a_farm);
        sem_post(&mutex_buffer2);

        printf("[Farmacia] Preparando medicación para %s...\n", paciente_actual);
        sleep(tiempo_aleatorio(1, 3));
        
        printf("[Farmacia] Medicación lista para %s. Enviando señal de alta...\n", paciente_actual);
        
        // Enviar señal SIGUSR1 al proceso Recepción
        kill(pid_recepcion, SIGUSR1);
    }
    return NULL;
}

// --- MAIN ---

void main(int argc, char* argv[]) {
    srand(time(NULL));

    // Configuración de la Cola de Mensajes
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 128;
    attr.mq_curmsgs = 0;

    // Crear la cola (el padre o recepción la crea, pero debe existir antes de usarla)
    mq_recepcion = mq_open(NOMBRE_COLA, O_CREAT | O_RDWR, 0644, &attr);
    if (mq_recepcion == (mqd_t)-1) {
        perror("Error creando cola de mensajes");
        exit(1);
    }

    // Preparar señal SIGINT (CTRL+C) para limpieza
    signal(SIGINT, manejador_fin);

    pid_recepcion = fork();

    if (pid_recepcion < 0) {
        perror("Error en fork recepción");
        exit(1);
    }

    if (pid_recepcion != 0) {
        // --- PADRE (Raíz) ---
        pid_hospital = fork();
        
        if (pid_hospital < 0) {
            perror("Error en fork hospital");
            kill(pid_recepcion, SIGKILL); // Matar al otro hijo si este falla
            exit(1);
        }

        if (pid_hospital != 0) {
            // PROCESO PADRE: Espera a que terminen los hijos (o CTRL+C)
            printf("[Main] Sistema iniciado. Presione CTRL+C para terminar.\n");
            
            // Esperar a los hijos. 
            // Si wait devuelve error (porque pulsaste CTRL+C), salimos del wait.
            wait(NULL);
            wait(NULL);

            // --- CORRECCIÓN PARA EVITAR MENSAJES HUÉRFANOS ---
            printf("\n[Main] Deteniendo procesos hijos...\n");
            kill(pid_hospital, SIGKILL);  // Asegura que Hospital muera YA
            kill(pid_recepcion, SIGKILL); // Asegura que Recepción muera YA
            
            // Esperamos un momento para que el sistema operativo limpie sus salidas
            wait(NULL); // Recoger restos del primer kill
            wait(NULL); // Recoger restos del segundo kill
            // --------------------------------------------------

            printf("[Main] Limpiando recursos...\n");
            mq_close(mq_recepcion);
            mq_unlink(NOMBRE_COLA);
            
            printf("[Main] Fin del programa.\n");
            
        } else {
            // --- PROCESO HOSPITAL ---
            // Ignorar SIGUSR1 (solo le interesa a recepción) y manejar SIGINT
            signal(SIGUSR1, SIG_IGN);
            signal(SIGINT, manejador_fin); // Para salir del pause/wait

            printf("[Hospital] Comienzo mi ejecución...\n");

            // Inicializar semáforos (0 = compartido entre hilos del mismo proceso)
            sem_init(&sem_diag_listo, 0, 0);
            sem_init(&sem_farm_listo, 0, 0);
            sem_init(&mutex_buffer1, 0, 1); // Mutex iniciados a 1
            sem_init(&mutex_buffer2, 0, 1);

            pthread_t t_expl, t_diag, t_farm;

            // Crear hilos
            pthread_create(&t_expl, NULL, exploracion, NULL);
            pthread_create(&t_diag, NULL, diagnostico, NULL);
            pthread_create(&t_farm, NULL, farmacia, NULL);

            // Esperar hilos (Join)
            pthread_join(t_expl, NULL);
            pthread_join(t_diag, NULL);
            pthread_join(t_farm, NULL);

            // Destruir semáforos
            sem_destroy(&sem_diag_listo);
            sem_destroy(&sem_farm_listo);
            sem_destroy(&mutex_buffer1);
            sem_destroy(&mutex_buffer2);
            
            printf("[Hospital] Proceso finalizado.\n");
            exit(0);
        }
    } else {
        // --- PROCESO RECEPCIÓN ---
        printf("[Recepción] Comienzo mi ejecución...\n");

        // Registrar manejador para señal de alta (SIGUSR1)
        struct sigaction sa;
        sa.sa_handler = manejador_alta;
        sa.sa_flags = SA_RESTART; // Importante para que mq_send no falle por interrupción
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        
        // Manejador para salir
        signal(SIGINT, manejador_fin);

        int contador_pacientes = 1;

        while (!fin_simulacion) {
            char paciente[128];
            sprintf(paciente, "Paciente_%d", contador_pacientes++);

            sleep(tiempo_aleatorio(2, 5)); // Simula llegada de pacientes
            if (fin_simulacion) break;

            printf("[Recepción] Registrando nuevo paciente: %s...\n", paciente);

            // Enviar a la cola
            if (mq_send(mq_recepcion, paciente, strlen(paciente) + 1, 0) == -1) {
                if (errno != EINTR) perror("[Recepción] Error enviando mensaje");
            } else {
                printf("[Recepción] Paciente %s enviado a exploración.\n", paciente);
            }
        }
        
        mq_close(mq_recepcion);
        printf("[Recepción] Proceso finalizado.\n");
        exit(0);
    }
}

// para ejecutar: gcc hospital.c -o hospital -lpthread -lrt 