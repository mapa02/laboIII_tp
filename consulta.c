#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>

#include "scada.h"


/*
 * Muestra las estadisticas de una copia local.
 *
 * La copia permite liberar rapidamente el semaforo
 * y no mantener bloqueados a los workers mientras
 * imprimimos por pantalla.
 */
void mostrar_estadisticas(
    struct MemoriaCompartida *datos)
{
    int i;
    double promedio;

    printf("\n");
    printf("=================================\n");
    printf("ESTADISTICAS ACTUALES\n");
    printf("=================================\n");

    for (i = 0;
         i < datos->cantidad_sensores;
         i++)
    {
        if (datos->sensores[i].cantidad > 0)
        {
            promedio =
                datos->sensores[i].suma /
                datos->sensores[i].cantidad;

            printf(
                "Sensor %d: cant=%d "
                "prom=%.2f min=%.2f "
                "max=%.2f ultimo=%.2f\n",
                i,
                datos->sensores[i].cantidad,
                promedio,
                datos->sensores[i].minimo,
                datos->sensores[i].maximo,
                datos->sensores[i].ultimo
            );
        }
        else
        {
            printf(
                "Sensor %d: sin mediciones\n",
                i
            );
        }
    }

    if (datos->global.cantidad > 0)
    {
        promedio =
            datos->global.suma /
            datos->global.cantidad;

        printf(
            "\nGLOBAL: cant=%d "
            "prom=%.2f min=%.2f "
            "max=%.2f ultimo=%.2f\n",
            datos->global.cantidad,
            promedio,
            datos->global.minimo,
            datos->global.maximo,
            datos->global.ultimo
        );
    }
    else
    {
        printf(
            "\nGLOBAL: sin mediciones\n"
        );
    }
}


int main(void)
{
    int fd;

    struct MemoriaCompartida *memoria;
    struct MemoriaCompartida copia;

    sem_t *sem_estadisticas;

    /*
     * La memoria debe haber sido creada previamente
     * por el colector.
     */
    fd = shm_open(
        SHM_NOMBRE,
        O_RDONLY,
        0
    );

    if (fd == -1)
    {
        perror("shm_open");
        fprintf(
            stderr,
            "El colector debe estar ejecutandose.\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * Consulta solo necesita leer las estadisticas.
     */
    memoria = mmap(
        NULL,
        sizeof(struct MemoriaCompartida),
        PROT_READ,
        MAP_SHARED,
        fd,
        0
    );

    close(fd);

    if (memoria == MAP_FAILED)
    {
        perror("mmap");
        return EXIT_FAILURE;
    }

    /*
     * Abrimos el mismo semaforo creado por
     * el colector. No usamos O_CREAT porque
     * consulta no es el creador del recurso.
     */
    sem_estadisticas = sem_open(
        SEM_ESTADISTICAS_NOMBRE,
        0
    );

    if (sem_estadisticas == SEM_FAILED)
    {
        perror("sem_open");

        munmap(
            memoria,
            sizeof(struct MemoriaCompartida)
        );

        return EXIT_FAILURE;
    }

    /*
     * Tomamos el semaforo solamente el tiempo
     * necesario para obtener una fotografia
     * consistente de las estadisticas.
     */
    if (sem_wait(sem_estadisticas) == -1)
    {
        perror("sem_wait");

        sem_close(sem_estadisticas);

        munmap(
            memoria,
            sizeof(struct MemoriaCompartida)
        );

        return EXIT_FAILURE;
    }

    copia = *memoria;

    if (sem_post(sem_estadisticas) == -1)
    {
        perror("sem_post");

        sem_close(sem_estadisticas);

        munmap(
            memoria,
            sizeof(struct MemoriaCompartida)
        );

        return EXIT_FAILURE;
    }

    /*
     * A partir de aca imprimimos la copia local,
     * sin mantener bloqueado el recurso compartido.
     */
    mostrar_estadisticas(&copia);

    sem_close(sem_estadisticas);

    munmap(
        memoria,
        sizeof(struct MemoriaCompartida)
    );

    return EXIT_SUCCESS;
}
