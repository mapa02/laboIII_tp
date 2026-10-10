#ifndef SCADA_H
#define SCADA_H

#include <time.h>

/*
 * Configuracion general.
 */
#define MAX_SENSORES 32
#define CANT_WORKERS 4
#define COLA_MAX_MENSAJES 10

/*
 * Valor reservado para indicar a un worker
 * que no quedan mas mediciones.
 */
#define SENSOR_FIN -1

/*
 * Nombres de recursos IPC POSIX.
 */
#define COLA_NOMBRE "/scada_mediciones"
#define SHM_NOMBRE "/scada_estadisticas"
#define SEM_ESTADISTICAS_NOMBRE "/scada_sem_estadisticas"

/*
 * Mensaje enviado por los procesos sensores.
 */
struct Medicion
{
    time_t timestamp;
    double valor;
    int sensor_id;
};

/*
 * Estadisticas acumuladas.
 *
 * El promedio no se almacena porque puede calcularse
 * como suma / cantidad.
 */
struct Estadistica
{
    int cantidad;
    double suma;
    double minimo;
    double maximo;
    double ultimo;
};

/*
 * Estado que vive en memoria compartida.
 *
 * Los workers lo modifican y el programa consulta
 * podra leerlo desde otro proceso.
 */
struct MemoriaCompartida
{
    int cantidad_sensores;

    struct Estadistica sensores[MAX_SENSORES];
    struct Estadistica global;
};

#endif