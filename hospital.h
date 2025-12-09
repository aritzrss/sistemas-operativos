#ifndef HOSPITAL_H
#define HOSPITAL_H

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
#include <time.h>

// --- CONSTANTES DE CONFIGURACIÓN ---
#define NOMBRE_COLA "/cola_hospital"
#define MAX_MSG_SIZE 256
#define LONGITUD_PACIENTE 128

// --- ESTRUCTURAS DE DATOS (Si fueran necesarias) ---
// En este caso usamos strings simples, pero si quisieras pasar 
// más datos (id, gravedad), definirías aquí un struct:
/*
typedef struct {
    int id;
    char nombre[LONGITUD_PACIENTE];
} Paciente;
*/

// --- PROTOTIPOS DE FUNCIONES ---
int tiempo_aleatorio(int min, int max);
void manejador_alta(int sig);
void manejador_fin(int sig);

void* exploracion(void* args);
void* diagnostico(void* args);
void* farmacia(void* args);

#endif // HOSPITAL_H