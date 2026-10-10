#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <mqueue.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#include "scada.h"

#define VALOR_MIN 0.0
#define VALOR_MAX 100.0


/*
 * Informacion que recibe cada worker.
 */
struct DatosWorker
{
    int id;
    mqd_t cola;
    struct MemoriaCompartida *memoria;
    sem_t *sem_estadisticas;
};


/*
 * Elimina recursos que hayan quedado
 * de una ejecucion anterior.
 */
void limpiar_recursos_anteriores(void)
{
    mq_unlink(COLA_NOMBRE);
    shm_unlink(SHM_NOMBRE);
    sem_unlink(SEM_ESTADISTICAS_NOMBRE);
}


/*
 * Crea la cola POSIX utilizada para transportar
 * mediciones desde los sensores hacia los workers.
 */
mqd_t crear_cola(void)
{
    struct mq_attr attr;
    mqd_t cola;

    attr.mq_flags = 0;
    attr.mq_maxmsg = COLA_MAX_MENSAJES;
    attr.mq_msgsize = sizeof(struct Medicion);
    attr.mq_curmsgs = 0;

    cola = mq_open(
        COLA_NOMBRE,
        O_CREAT | O_RDWR,
        0600,
        &attr
    );

    if (cola == (mqd_t)-1)
    {
        perror("mq_open");
    }

    return cola;
}


/*
 * Crea la memoria compartida donde se guardan
 * las estadisticas del sistema.
 */
struct MemoriaCompartida *crear_memoria_compartida(
    int cantidad_sensores)
{
    int fd;
    struct MemoriaCompartida *memoria;

    /*
     * 1. Crear/abrir memoria compartida.
     */
    fd = shm_open(
        SHM_NOMBRE,
        O_CREAT | O_RDWR,
        0600
    );

    if (fd == -1)
    {
        perror("shm_open");
        return NULL;
    }

    /*
     * 2. Darle tamaño.
     */
    if (ftruncate(
            fd,
            sizeof(struct MemoriaCompartida)) == -1)
    {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NOMBRE);

        return NULL;
    }

    /*
     * 3. Mapearla al espacio del proceso.
     */
    memoria = mmap(
        NULL,
        sizeof(struct MemoriaCompartida),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0
    );

    close(fd);

    if (memoria == MAP_FAILED)
    {
        perror("mmap");
        shm_unlink(SHM_NOMBRE);

        return NULL;
    }

    /*
     * Inicializar todo en cero.
     */
    memset(
        memoria,
        0,
        sizeof(struct MemoriaCompartida)
    );

    memoria->cantidad_sensores = cantidad_sensores;

    return memoria;
}


/*
 * Crea el semaforo que protege
 * las estadisticas compartidas.
 *
 * Valor inicial 1 = exclusion mutua.
 */
sem_t *crear_semaforo_estadisticas(void)
{
    sem_t *sem;

    sem = sem_open(
        SEM_ESTADISTICAS_NOMBRE,
        O_CREAT,
        0600,
        1
    );

    if (sem == SEM_FAILED)
    {
        perror("sem_open");
    }

    return sem;
}


/*
 * Genera un valor aleatorio entre
 * VALOR_MIN y VALOR_MAX.
 */
double generar_valor(void)
{
    double proporcion;

    proporcion =
        (double)rand() /
        (double)RAND_MAX;

    return VALOR_MIN +
           proporcion *
           (VALOR_MAX - VALOR_MIN);
}


/*
 * Codigo ejecutado por cada proceso sensor.
 */
int ejecutar_sensor(int sensor_id, int cantidad)
{
    mqd_t cola_sensor;
    struct Medicion medicion;
    int i;

    srand(
        (unsigned int)time(NULL) ^
        (unsigned int)getpid()
    );

    cola_sensor = mq_open(
        COLA_NOMBRE,
        O_WRONLY
    );

    if (cola_sensor == (mqd_t)-1)
    {
        perror("mq_open sensor");
        return -1;
    }

    for (i = 0; i < cantidad; i++)
    {
        medicion.timestamp = time(NULL);
        medicion.sensor_id = sensor_id;
        medicion.valor = generar_valor();

        if (mq_send(
                cola_sensor,
                (char *)&medicion,
                sizeof(struct Medicion),
                0) == -1)
        {
            perror("mq_send");
            mq_close(cola_sensor);

            return -1;
        }

        printf(
            "[Sensor %d] Medicion %d: %.2f\n",
            sensor_id,
            i + 1,
            medicion.valor
        );
    }

    mq_close(cola_sensor);

    return 0;
}


/*
 * Actualiza las estadisticas con un nuevo valor.
 *
 * Esta funcion supone que quien la llama
 * ya tomo el semaforo.
 */
void actualizar_estadistica(
    struct Estadistica *estadistica,
    double valor)
{
    /*
     * La primera medicion inicializa
     * minimo y maximo.
     */
    if (estadistica->cantidad == 0)
    {
        estadistica->minimo = valor;
        estadistica->maximo = valor;
    }
    else
    {
        if (valor < estadistica->minimo)
        {
            estadistica->minimo = valor;
        }

        if (valor > estadistica->maximo)
        {
            estadistica->maximo = valor;
        }
    }

    estadistica->cantidad++;
    estadistica->suma += valor;
    estadistica->ultimo = valor;
}


/*
 * Procesa una medicion.
 *
 * Actualiza tanto las estadisticas
 * del sensor como las globales.
 */
int procesar_medicion(
    struct DatosWorker *datos,
    struct Medicion *medicion)
{
    int sensor_id;

    sensor_id = medicion->sensor_id;

    if (sensor_id < 0 ||
        sensor_id >= datos->memoria->cantidad_sensores)
    {
        fprintf(
            stderr,
            "Sensor invalido: %d\n",
            sensor_id
        );

        return -1;
    }

    /*
     * Entrar a la seccion critica.
     */
    if (sem_wait(datos->sem_estadisticas) == -1)
    {
        perror("sem_wait");
        return -1;
    }

    actualizar_estadistica(
        &datos->memoria->sensores[sensor_id],
        medicion->valor
    );

    actualizar_estadistica(
        &datos->memoria->global,
        medicion->valor
    );

    /*
     * Salir de la seccion critica.
     */
    if (sem_post(datos->sem_estadisticas) == -1)
    {
        perror("sem_post");
        return -1;
    }

    return 0;
}


/*
 * Funcion ejecutada por cada worker thread.
 */
void *funcion_worker(void *arg)
{
    struct DatosWorker *datos;
    struct Medicion medicion;
    ssize_t bytes;

    datos = (struct DatosWorker *)arg;

    printf(
        "[Worker %d] Iniciado\n",
        datos->id
    );

    while (1)
    {
        bytes = mq_receive(
            datos->cola,
            (char *)&medicion,
            sizeof(struct Medicion),
            NULL
        );

        if (bytes == -1)
        {
            perror("mq_receive");
            return NULL;
        }

        /*
         * Mensaje especial de terminacion.
         */
        if (medicion.sensor_id == SENSOR_FIN)
        {
            break;
        }

        if (procesar_medicion(
                datos,
                &medicion) == -1)
        {
            return NULL;
        }

        printf(
            "[Worker %d] Sensor %d -> %.2f\n",
            datos->id,
            medicion.sensor_id,
            medicion.valor
        );
    }

    printf(
        "[Worker %d] Terminado\n",
        datos->id
    );

    return NULL;
}


/*
 * Muestra las estadisticas finales.
 */
void mostrar_resumen(
    struct MemoriaCompartida *memoria)
{
    int i;
    double promedio;

    printf("\n");
    printf("=================================\n");
    printf("ESTADISTICAS FINALES\n");
    printf("=================================\n");

    for (i = 0;
         i < memoria->cantidad_sensores;
         i++)
    {
        if (memoria->sensores[i].cantidad > 0)
        {
            promedio =
                memoria->sensores[i].suma /
                memoria->sensores[i].cantidad;

            printf(
                "Sensor %d: cant=%d "
                "prom=%.2f min=%.2f "
                "max=%.2f ultimo=%.2f\n",
                i,
                memoria->sensores[i].cantidad,
                promedio,
                memoria->sensores[i].minimo,
                memoria->sensores[i].maximo,
                memoria->sensores[i].ultimo
            );
        }
    }

    if (memoria->global.cantidad > 0)
    {
        promedio =
            memoria->global.suma /
            memoria->global.cantidad;

        printf(
            "\nGLOBAL: cant=%d "
            "prom=%.2f min=%.2f "
            "max=%.2f ultimo=%.2f\n",
            memoria->global.cantidad,
            promedio,
            memoria->global.minimo,
            memoria->global.maximo,
            memoria->global.ultimo
        );
    }
}


int main(int argc, char *argv[])
{
    int cantidad_sensores;
    int mediciones_por_sensor;
    int i;
    int error;

    pid_t pid;
    mqd_t cola;

    struct MemoriaCompartida *memoria;
    sem_t *sem_estadisticas;

    pthread_t workers[CANT_WORKERS];
    struct DatosWorker datos_workers[CANT_WORKERS];

    struct Medicion fin;

    /*
     * argv[1] = cantidad de sensores
     * argv[2] = mediciones por sensor
     */
    if (argc != 3)
    {
        fprintf(
            stderr,
            "Uso: %s <sensores> <mediciones_por_sensor>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    cantidad_sensores = atoi(argv[1]);
    mediciones_por_sensor = atoi(argv[2]);

    if (cantidad_sensores < 1 ||
        cantidad_sensores > MAX_SENSORES)
    {
        fprintf(
            stderr,
            "Cantidad de sensores invalida. "
            "Maximo: %d\n",
            MAX_SENSORES
        );

        return EXIT_FAILURE;
    }

    if (mediciones_por_sensor < 1)
    {
        fprintf(
            stderr,
            "La cantidad de mediciones "
            "debe ser mayor a 0\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * Limpiar recursos viejos antes
     * de comenzar una nueva ejecucion.
     */
    limpiar_recursos_anteriores();

    /*
     * Crear recursos IPC.
     */
    cola = crear_cola();

    if (cola == (mqd_t)-1)
    {
        return EXIT_FAILURE;
    }

    memoria =
        crear_memoria_compartida(
            cantidad_sensores
        );

    if (memoria == NULL)
    {
        mq_close(cola);
        mq_unlink(COLA_NOMBRE);

        return EXIT_FAILURE;
    }

    sem_estadisticas =
        crear_semaforo_estadisticas();

    if (sem_estadisticas == SEM_FAILED)
    {
        munmap(
            memoria,
            sizeof(struct MemoriaCompartida)
        );

        shm_unlink(SHM_NOMBRE);

        mq_close(cola);
        mq_unlink(COLA_NOMBRE);

        return EXIT_FAILURE;
    }

    printf("SCADA iniciado\n");
    printf(
        "Sensores: %d\n",
        cantidad_sensores
    );
    printf(
        "Mediciones por sensor: %d\n\n",
        mediciones_por_sensor
    );

    /*
     * Crear un proceso hijo por sensor.
     */
    for (i = 0; i < cantidad_sensores; i++)
    {
        pid = fork();

        if (pid == -1)
        {
            perror("fork");
            return EXIT_FAILURE;
        }

        if (pid == 0)
        {
            int resultado;

            /*
             * El hijo no usa directamente
             * el descriptor heredado.
             */
            mq_close(cola);

            resultado =
                ejecutar_sensor(
                    i,
                    mediciones_por_sensor
                );

            if (resultado == -1)
            {
                exit(EXIT_FAILURE);
            }

            exit(EXIT_SUCCESS);
        }
    }

    /*
     * Crear el pool de worker threads.
     */
    for (i = 0; i < CANT_WORKERS; i++)
    {
        datos_workers[i].id = i;
        datos_workers[i].cola = cola;
        datos_workers[i].memoria = memoria;
        datos_workers[i].sem_estadisticas =
            sem_estadisticas;

        error = pthread_create(
            &workers[i],
            NULL,
            funcion_worker,
            &datos_workers[i]
        );

        if (error != 0)
        {
            fprintf(
                stderr,
                "Error creando worker %d\n",
                i
            );

            return EXIT_FAILURE;
        }
    }

    /*
     * Esperar a que terminen
     * todos los sensores.
     */
    for (i = 0; i < cantidad_sensores; i++)
    {
        wait(NULL);
    }

    /*
     * Una vez terminados los sensores,
     * avisar a los workers que no habra
     * nuevas mediciones.
     */
    fin.timestamp = time(NULL);
    fin.valor = 0.0;
    fin.sensor_id = SENSOR_FIN;

    for (i = 0; i < CANT_WORKERS; i++)
    {
        if (mq_send(
                cola,
                (char *)&fin,
                sizeof(struct Medicion),
                0) == -1)
        {
            perror("mq_send fin");
            return EXIT_FAILURE;
        }
    }

    /*
     * Esperar a los worker threads.
     */
    for (i = 0; i < CANT_WORKERS; i++)
    {
        pthread_join(
            workers[i],
            NULL
        );
    }

    /*
     * Mostrar resultados finales.
     */
    mostrar_resumen(memoria);

    /*
     * Liberar recursos IPC.
     */
    sem_close(sem_estadisticas);
    sem_unlink(SEM_ESTADISTICAS_NOMBRE);

    munmap(
        memoria,
        sizeof(struct MemoriaCompartida)
    );

    shm_unlink(SHM_NOMBRE);

    mq_close(cola);
    mq_unlink(COLA_NOMBRE);

    printf(
        "\nProcesamiento terminado\n"
    );

    return EXIT_SUCCESS;
}