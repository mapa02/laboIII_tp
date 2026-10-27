#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
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
 * 200 milisegundos entre mediciones.
 */
#define INTERVALO_NANOSEGUNDOS 200000000L


volatile sig_atomic_t detener = 0;
volatile sig_atomic_t reporte_manual = 0;
volatile sig_atomic_t reporte_periodico = 0;


void manejar_senal(int senal)
{
    if (senal == SIGINT)
    {
        detener = 1;
    }
    else if (senal == SIGUSR1)
    {
        reporte_manual = 1;
    }
    else if (senal == SIGALRM)
    {
        reporte_periodico = 1;
    }
}


int instalar_senales(void)
{
    struct sigaction accion;

    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = manejar_senal;
    sigemptyset(&accion.sa_mask);

    if (sigaction(SIGINT, &accion, NULL) == -1 ||
        sigaction(SIGUSR1, &accion, NULL) == -1 ||
        sigaction(SIGALRM, &accion, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    return 0;
}


struct DatosWorker
{
    int id;
    mqd_t cola;
    mqd_t cola_alertas;
    double umbral;
    struct MemoriaCompartida *memoria;
    sem_t *sem_estadisticas;
};


void limpiar_recursos_anteriores(void)
{
    mq_unlink(COLA_NOMBRE);
    shm_unlink(SHM_NOMBRE);
    sem_unlink(SEM_ESTADISTICAS_NOMBRE);
}


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



mqd_t crear_cola_alertas(void)
{
    struct mq_attr attr;
    mqd_t cola;

    attr.mq_flags = 0;
    attr.mq_maxmsg = COLA_MAX_MENSAJES;
    attr.mq_msgsize = sizeof(struct Alerta);
    attr.mq_curmsgs = 0;

    cola = mq_open(COLA_ALERTAS_NOMBRE,
                   O_CREAT | O_RDWR, 0600, &attr);

    if (cola == (mqd_t)-1)
    {
        perror("mq_open alertas");
    }

    return cola;
}


void *funcion_logger(void *arg)
{
    mqd_t cola = *(mqd_t *)arg;
    struct Alerta alerta;
    ssize_t bytes;
    FILE *archivo;

    archivo = fopen(ARCHIVO_LOG, "w");

    if (archivo == NULL)
    {
        perror("fopen monitoreo.log");
        return NULL;
    }

    while (1)
    {
        bytes = mq_receive(cola, (char *)&alerta,
                           sizeof(alerta), NULL);

        if (bytes == -1)
        {
            if (errno == EINTR)
                continue;

            perror("mq_receive alertas");
            break;
        }

        if (alerta.sensor_id == ALERTA_FIN)
            break;

        fprintf(archivo,
                "Sensor %d | Valor: %.2f | Umbral: %.2f | Timestamp: %ld\n",
                alerta.sensor_id,
                alerta.valor,
                alerta.umbral,
                (long)alerta.timestamp);

        fflush(archivo);
    }

    fclose(archivo);
    return NULL;
}


struct MemoriaCompartida *crear_memoria_compartida(
    int cantidad_sensores)
{
    int fd;
    struct MemoriaCompartida *memoria;

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

    if (ftruncate(
            fd,
            sizeof(struct MemoriaCompartida)) == -1)
    {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NOMBRE);
        return NULL;
    }

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

    memset(
        memoria,
        0,
        sizeof(struct MemoriaCompartida)
    );

    memoria->cantidad_sensores = cantidad_sensores;

    return memoria;
}


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
 * Simula el intervalo de tiempo entre
 * una medicion del sensor y la siguiente.
 */
void esperar_entre_mediciones(void)
{
    struct timespec espera;

    espera.tv_sec = 0;
    espera.tv_nsec = INTERVALO_NANOSEGUNDOS;

    nanosleep(&espera, NULL);
}


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

        /*
         * No esperamos despues de la ultima
         * medicion porque el sensor ya termino.
         */
        if (i + 1 < cantidad)
        {
            esperar_entre_mediciones();
        }
    }

    mq_close(cola_sensor);

    return 0;
}


void actualizar_estadistica(
    struct Estadistica *estadistica,
    double valor)
{
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

    if (sem_post(datos->sem_estadisticas) == -1)
    {
        perror("sem_post");
        return -1;
    }

    return 0;
}


void *funcion_worker(void *arg)
{
    struct DatosWorker *datos;
    struct Medicion medicion;
    struct Alerta alerta;
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

        if (medicion.valor > datos->umbral)
        {
            alerta.timestamp = medicion.timestamp;
            alerta.sensor_id = medicion.sensor_id;
            alerta.valor = medicion.valor;
            alerta.umbral = datos->umbral;

            if (mq_send(
                    datos->cola_alertas,
                    (char *)&alerta,
                    sizeof(struct Alerta),
                    0) == -1)
            {
                perror("mq_send alerta");
                return NULL;
            }
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




void atender_reportes(
    struct MemoriaCompartida *memoria,
    sem_t *semaforo,
    int intervalo)
{
    struct MemoriaCompartida copia;

    if (!reporte_manual && !reporte_periodico)
        return;

    if (reporte_periodico)
    {
        reporte_periodico = 0;
        alarm((unsigned int)intervalo);
    }

    reporte_manual = 0;

    if (sem_wait(semaforo) == -1)
    {
        perror("sem_wait reporte");
        return;
    }

    copia = *memoria;

    if (sem_post(semaforo) == -1)
    {
        perror("sem_post reporte");
        return;
    }

    printf("\\n[REPORTE DE ESTADISTICAS]\\n");
    mostrar_resumen(&copia);
    fflush(stdout);
}


/*
 * Convierte un argumento a entero.
 * Devuelve 0 si es valido y -1 si es invalido.
 */
int convertir_entero(const char *texto, int *resultado)
{
    char *fin;
    long numero;

    errno = 0;
    numero = strtol(texto, &fin, 10);

    if (texto == fin || *fin != '\0')
    {
        return -1;
    }

    if (errno == ERANGE ||
        numero < INT_MIN ||
        numero > INT_MAX)
    {
        return -1;
    }

    *resultado = (int)numero;
    return 0;
}

int main(int argc, char *argv[])
{
    int cantidad_sensores;
    int mediciones_por_sensor;
    int umbral_alertas;
    int intervalo_reportes;
    int i;
    int error;

    pid_t pid;
    pid_t pids[MAX_SENSORES];
    int sensores_creados = 0;
    int estado;
    int sensores_con_error = 0;

    mqd_t cola;
    mqd_t cola_alertas;

    struct MemoriaCompartida *memoria;
    sem_t *sem_estadisticas;

    pthread_t logger;
    pthread_t workers[CANT_WORKERS];
    struct DatosWorker datos_workers[CANT_WORKERS];

    struct Medicion fin;
    struct Alerta fin_alerta;

    if (argc != 5)
    {
        fprintf(
            stderr,
            "Uso: %s <sensores> "
            "<mediciones_por_sensor> <umbral>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    if (convertir_entero(argv[1], &cantidad_sensores) == -1)
    {
        fprintf(stderr, "Error: cantidad de sensores invalida.\\n");
        return EXIT_FAILURE;
    }

    if (convertir_entero(argv[2], &mediciones_por_sensor) == -1)
    {
        fprintf(stderr, "Error: cantidad de mediciones invalida.\\n");
        return EXIT_FAILURE;
    }

    if (convertir_entero(argv[3], &umbral_alertas) == -1 ||
        umbral_alertas < 0 || umbral_alertas > 100)
    {
        fprintf(stderr, "Umbral invalido (0 a 100).\n");
        return EXIT_FAILURE;
    }

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

    if (convertir_entero(argv[4], &intervalo_reportes) == -1 ||
        intervalo_reportes < 1)
    {
        fprintf(stderr, "Intervalo de reportes invalido.\\n");
        return EXIT_FAILURE;
    }

    if (instalar_senales() == -1)
    {
        return EXIT_FAILURE;
    }

    limpiar_recursos_anteriores();

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
     * Creamos todos los procesos sensores
     * antes de crear los threads workers.
     */
    for (i = 0;
         i < cantidad_sensores;
         i++)
    {
        pid = fork();

        if (pid == -1)
        {
            int j;

            perror("fork");

            /* Detener los hijos que ya fueron creados. */
            for (j = 0; j < sensores_creados; j++)
            {
                kill(pids[j], SIGTERM);
            }

            /* Esperar a todos para evitar zombies. */
            for (j = 0; j < sensores_creados; j++)
            {
                while (waitpid(pids[j], NULL, 0) == -1 &&
                       errno == EINTR)
                {
                    /* Reintentar si una senal interrumpe waitpid. */
                }
            }

            sem_close(sem_estadisticas);
            sem_unlink(SEM_ESTADISTICAS_NOMBRE);

            munmap(memoria, sizeof(struct MemoriaCompartida));
            shm_unlink(SHM_NOMBRE);

            mq_close(cola);
            mq_unlink(COLA_NOMBRE);

            return EXIT_FAILURE;
        }

        if (pid == 0)
        {
            int resultado;

            mq_close(cola);

            resultado = ejecutar_sensor(
                i,
                mediciones_por_sensor
            );

            if (resultado == -1)
            {
                exit(EXIT_FAILURE);
            }

            exit(EXIT_SUCCESS);
        }

        /* Guardar el PID para poder esperar a este hijo. */
        pids[sensores_creados] = pid;
        sensores_creados++;
    }

    /* Crear la segunda cola e iniciar el logger. */
    cola_alertas = crear_cola_alertas();

    if (cola_alertas == (mqd_t)-1)
    {
        fprintf(stderr, "No se pudo crear la cola de alertas.\n");
        return EXIT_FAILURE;
    }

    error = pthread_create(
        &logger, NULL, funcion_logger, &cola_alertas
    );

    if (error != 0)
    {
        fprintf(stderr, "Error creando logger: %s\n",
                strerror(error));
        mq_close(cola_alertas);
        mq_unlink(COLA_ALERTAS_NOMBRE);
        return EXIT_FAILURE;
    }

    /*
     * Creamos el pool fijo de workers.
     */
    for (i = 0;
         i < CANT_WORKERS;
         i++)
    {
        datos_workers[i].id = i;
        datos_workers[i].cola = cola;
        datos_workers[i].cola_alertas = cola_alertas;
        datos_workers[i].umbral = (double)umbral_alertas;
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
            int j;
            int resultado_wait;
            pid_t terminado;

            fprintf(stderr,
                    "Error creando worker %d: %s\n",
                    i, strerror(error));

            /*
             * Detener los sensores que siguen ejecutandose.
             */
            for (j = 0; j < sensores_creados; j++)
            {
                if (kill(pids[j], SIGTERM) == -1 &&
                    errno != ESRCH)
                {
                    perror("kill");
                }
            }

            /*
             * Esperar a los hijos para evitar zombies.
             */
            for (j = 0; j < sensores_creados; j++)
            {
                do
                {
                    terminado = waitpid(pids[j], NULL, 0);
                }
                while (terminado == -1 && errno == EINTR);

                if (terminado == -1 && errno != ECHILD)
                {
                    perror("waitpid");
                }
            }

            /*
             * Ya no quedan productores.
             * Enviar FIN a los workers que si se crearon.
             */
            fin.timestamp = time(NULL);
            fin.valor = 0.0;
            fin.sensor_id = SENSOR_FIN;

            for (j = 0; j < i; j++)
            {
                if (mq_send(cola, (char *)&fin,
                            sizeof(fin), 0) == -1)
                {
                    perror("mq_send fin");
                }
            }

            /*
             * Esperar a los workers existentes.
             */
            for (j = 0; j < i; j++)
            {
                resultado_wait = pthread_join(
                    workers[j], NULL);

                if (resultado_wait != 0)
                {
                    fprintf(stderr,
                            "Error en pthread_join: %s\n",
                            strerror(resultado_wait));
                }
            }

            /*
             * Liberar recursos IPC.
             */
            sem_close(sem_estadisticas);
            sem_unlink(SEM_ESTADISTICAS_NOMBRE);

            munmap(memoria,
                   sizeof(struct MemoriaCompartida));
            shm_unlink(SHM_NOMBRE);

            mq_close(cola);
            mq_unlink(COLA_NOMBRE);

            return EXIT_FAILURE;
        }
    }

    alarm((unsigned int)intervalo_reportes);

    /*
     * Esperamos a que todos los sensores
     * terminen de producir mediciones.
     */
    for (i = 0;
         i < sensores_creados;
         i++)
    {
        pid_t terminado;

        do
        {
            terminado = waitpid(pids[i], &estado, 0);

            if (terminado == -1 && errno == EINTR)
            {
                atender_reportes(
                    memoria, sem_estadisticas, intervalo_reportes
                );
            }
        }
        while (terminado == -1 && errno == EINTR);

        atender_reportes(
            memoria, sem_estadisticas, intervalo_reportes
        );

        if (terminado == -1)
        {
            perror("waitpid");
            sensores_con_error = 1;
        }
        else if (!WIFEXITED(estado) ||
                 WEXITSTATUS(estado) != EXIT_SUCCESS)
        {
            fprintf(stderr,
                    "Sensor con PID %ld termino con error.\n",
                    (long)pids[i]);
            sensores_con_error = 1;
        }
    }

    alarm(0);

    /*
     * Ya no pueden aparecer mediciones nuevas.
     * Enviamos un mensaje de fin por worker.
     */
    fin.timestamp = time(NULL);
    fin.valor = 0.0;
    fin.sensor_id = SENSOR_FIN;

    for (i = 0;
         i < CANT_WORKERS;
         i++)
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
     * Esperamos la terminacion de todos
     * los workers.
     */
    for (i = 0;
         i < CANT_WORKERS;
         i++)
    {
        pthread_join(
            workers[i],
            NULL
        );
    }

    /* Los workers ya no generan nuevas alertas. */
    fin_alerta.timestamp = time(NULL);
    fin_alerta.sensor_id = ALERTA_FIN;
    fin_alerta.valor = 0.0;
    fin_alerta.umbral = 0.0;

    if (mq_send(cola_alertas, (char *)&fin_alerta,
                sizeof(fin_alerta), 0) == -1)
    {
        perror("mq_send fin alerta");
        return EXIT_FAILURE;
    }

    pthread_join(logger, NULL);

    mq_close(cola_alertas);
    mq_unlink(COLA_ALERTAS_NOMBRE);

    mostrar_resumen(memoria);

    /*
     * Liberacion ordenada de recursos IPC.
     */
    sem_close(sem_estadisticas);
    sem_unlink(
        SEM_ESTADISTICAS_NOMBRE
    );

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

    if (sensores_con_error)
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}