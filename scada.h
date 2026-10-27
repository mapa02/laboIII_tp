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
 * Mensaje especial para indicar
 * que un worker debe terminar.
 */
#define SENSOR_FIN -1

/*
 * Nombres de recursos IPC POSIX.
 */
#define COLA_NOMBRE "/scada_mediciones"
#define COLA_ALERTAS_NOMBRE "/scada_alertas"
#define ARCHIVO_LOG "monitoreo.log"
#define ALERTA_FIN -1
#define SHM_NOMBRE "/scada_estadisticas"
#define SEM_ESTADISTICAS_NOMBRE "/scada_sem_estadisticas"

/*
 * Mensaje que genera un sensor
 * y viaja por la cola.
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
 * No guardamos promedio porque se calcula
 * como suma / cantidad.
 */
/*
 * Mensaje enviado por un worker al logger
 * cuando una medicion supera el umbral.
 */
struct Alerta
{
    time_t timestamp;
    int sensor_id;
    double valor;
    double umbral;
};

/*
 * Estadisticas acumuladas.
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
 * Estructura que vive en memoria compartida.
 *
 * Contiene las estadisticas de cada sensor
 * y las estadisticas globales.
 */
struct MemoriaCompartida
{
    int cantidad_sensores;

    struct Estadistica sensores[MAX_SENSORES];
    struct Estadistica global;
};

#endif