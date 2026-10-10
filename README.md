# TP Integrador - Laboratorio III

## Sistema SCADA concurrente

Trabajo práctico integrador correspondiente a Laboratorio III.

El programa simula un sistema SCADA en el cual distintos procesos sensores generan mediciones que son enviadas al colector mediante una cola de mensajes POSIX.

El colector procesa las mediciones concurrentemente utilizando un pool de threads workers y mantiene estadísticas en memoria compartida.

Un segundo programa, `consulta`, permite visualizar las estadísticas mientras el colector continúa ejecutándose.

---

## Arquitectura

El flujo principal del sistema es:

    Sensores
    (procesos creados con fork)
             |
             | mq_send()
             v
    Cola de mensajes POSIX
             |
             | mq_receive()
             v
    Pool de workers
    (threads)
             |
             | actualización protegida
             | por semáforo
             v
    Memoria compartida
             |
             v
    Programa consulta

La cola se utiliza para transportar las mediciones generadas por los sensores.

La memoria compartida se utiliza para mantener el estado actual de las estadísticas y permitir que otro proceso pueda consultarlas.

---

## Archivos principales

### scada.h

Contiene las estructuras y constantes compartidas por los distintos programas.

Entre ellas:

- `struct Medicion`
- `struct Estadistica`
- `struct MemoriaCompartida`
- nombres de los recursos IPC
- cantidad máxima de sensores
- cantidad de workers

---

### colector.c

Es el programa principal del sistema.

Se encarga de:

- recibir la configuración por línea de comandos;
- crear la cola de mensajes;
- crear la memoria compartida;
- crear el semáforo;
- crear los procesos sensores mediante `fork()`;
- crear el pool de threads workers;
- esperar la finalización de los sensores;
- finalizar ordenadamente los workers;
- mostrar las estadísticas finales;
- liberar los recursos IPC.

---

## Sensores

Cada sensor es un proceso hijo creado mediante `fork()`.

Cada sensor:

1. genera un valor aleatorio;
2. construye una estructura `Medicion`;
3. identifica la medición mediante su `sensor_id`;
4. envía la medición a la cola POSIX mediante `mq_send()`;
5. espera un pequeño intervalo antes de generar la siguiente medición.

La espera entre mediciones se realiza con `nanosleep()` para simular el comportamiento temporal de un sensor real y permitir observar el sistema mientras se encuentra en ejecución.

La pausa no se utiliza como mecanismo de sincronización.

---

## Cola de mensajes

Las mediciones viajan desde los procesos sensores hasta el colector utilizando una cola de mensajes POSIX.

Los sensores actúan como productores.

Los workers actúan como consumidores.

Todos los workers consumen desde la misma cola utilizando `mq_receive()`.

Cada mensaje es retirado de la cola por un único worker.

---

## Workers

El colector crea un pool fijo de threads workers.

Los workers se crean mediante `pthread_create()`.

Cada worker:

1. espera una medición en la cola;
2. recibe la medición;
3. identifica a qué sensor pertenece;
4. actualiza las estadísticas correspondientes;
5. vuelve a esperar otra medición.

Los workers trabajan concurrentemente.

No es necesario que todos procesen exactamente la misma cantidad de mediciones. El reparto depende del orden de ejecución determinado por el sistema operativo.

Al finalizar, el colector espera a todos los workers utilizando `pthread_join()`.

---

## Estadísticas

Para cada sensor se almacenan:

- cantidad de mediciones;
- suma de valores;
- mínimo;
- máximo;
- último valor recibido.

También se mantienen las mismas estadísticas a nivel global.

El promedio no se almacena directamente.

Se calcula utilizando:

    promedio = suma / cantidad

De esta manera se evita mantener un dato redundante que debería actualizarse después de cada medición.

---

## Memoria compartida

Las estadísticas se almacenan en memoria compartida POSIX.

El colector crea el objeto mediante:

    shm_open()

Luego establece su tamaño mediante:

    ftruncate()

Y finalmente lo incorpora a su espacio de direcciones mediante:

    mmap()

La memoria compartida permite que distintos procesos accedan al mismo estado.

Esto permite que el programa `consulta` pueda observar las estadísticas generadas por el colector.

---

## Sincronización

Varios workers pueden intentar actualizar las estadísticas al mismo tiempo.

Además, el programa `consulta` puede intentar leerlas mientras están siendo modificadas.

Para evitar condiciones de carrera se utiliza un semáforo POSIX con nombre.

La actualización de las estadísticas se realiza dentro de una sección crítica:

    sem_wait()

    actualizar estadisticas

    sem_post()

El mismo semáforo también es utilizado por `consulta`.

---

## Programa consulta

`consulta.c` genera un segundo ejecutable independiente del colector.

Su función es adjuntarse a la memoria compartida existente y mostrar las estadísticas actuales.

`consulta` no crea la memoria compartida ni el semáforo.

Estos recursos deben haber sido creados previamente por el colector.

Para obtener una lectura consistente:

1. toma el semáforo;
2. copia la estructura de memoria compartida a una variable local;
3. libera inmediatamente el semáforo;
4. imprime la copia local.

Esto permite mantener la sección crítica lo más corta posible y evita bloquear innecesariamente a los workers mientras se imprime por pantalla.

---

## Terminación de los workers

Una vez que todos los sensores terminaron de producir mediciones, el colector debe finalizar los workers.

Para hacerlo utiliza un mensaje especial cuyo `sensor_id` es:

    SENSOR_FIN

El colector envía un mensaje de finalización por cada worker.

Cuando un worker recibe ese mensaje, sale de su ciclo y termina.

Se utiliza un mensaje por worker porque cada mensaje de la cola es consumido por un solo thread.

---

## Terminación normal

El colector espera primero la terminación de todos los procesos sensores mediante `wait()`.

Esto garantiza que ya no aparecerán nuevas mediciones.

Luego:

1. envía los mensajes `SENSOR_FIN`;
2. espera a los workers mediante `pthread_join()`;
3. muestra las estadísticas finales;
4. cierra los recursos IPC;
5. elimina los recursos IPC creados por el colector.

Se utilizan operaciones como:

    mq_close()
    mq_unlink()

    sem_close()
    sem_unlink()

    munmap()
    shm_unlink()

Cerrar un recurso significa que un proceso deja de utilizarlo.

Eliminarlo mediante `unlink` elimina su nombre del sistema cuando corresponde.

---

## Compilación

El proyecto fue desarrollado para ejecutarse en un entorno Linux con soporte para IPC POSIX.

Desde el directorio del proyecto:

    make clean
    make

Se generan dos ejecutables:

    colector
    consulta

---

## Ejecución

El colector recibe dos parámetros:

    ./colector <cantidad_sensores> <mediciones_por_sensor>

Por ejemplo:

    ./colector 3 50

Esto crea:

- 3 procesos sensores;
- 50 mediciones por sensor.

Por lo tanto, la cantidad total esperada es:

    3 x 50 = 150 mediciones

---

## Consultar estadísticas durante la ejecución

Mientras el colector se encuentra ejecutándose, desde otra terminal puede ejecutarse:

    ./consulta

Por ejemplo:

    ./colector 3 50

Y simultáneamente, desde otra terminal:

    ./consulta

`consulta` puede ejecutarse varias veces mientras el colector continúa procesando mediciones.

Cada ejecución muestra una fotografía del estado actual.

---

## Resultado esperado

Para una ejecución:

    ./colector 3 50

al finalizar debe cumplirse:

    Sensor 0: cant=50
    Sensor 1: cant=50
    Sensor 2: cant=50

    GLOBAL: cant=150

Los valores de promedio, mínimo, máximo y último valor dependen de las mediciones aleatorias generadas.

Una condición útil para verificar el correcto funcionamiento es:

    cantidad_global =
        cantidad_sensores * mediciones_por_sensor

Además:

    cantidad_global =
        suma de las cantidades de todos los sensores

---

## Mecanismos utilizados

Durante el desarrollo se utilizaron los siguientes conceptos de Laboratorio III:

- procesos;
- `fork()`;
- `wait()`;
- IPC;
- colas de mensajes POSIX;
- memoria compartida POSIX;
- semáforos POSIX;
- threads POSIX;
- `pthread_create()`;
- `pthread_join()`;
- concurrencia;
- sincronización;
- secciones críticas;
- configuración mediante `argc` y `argv`.

---

## Uso de Inteligencia Artificial

Durante el desarrollo del trabajo se utilizó ChatGPT como herramienta de apoyo.

La herramienta se utilizó para:

- analizar y dividir la consigna en etapas;
- repasar conceptos de procesos, threads e IPC;
- analizar alternativas de arquitectura;
- ayudar a detectar errores de compilación y ejecución;
- proponer implementaciones que luego fueron probadas;

Como información de contexto se utilizaron:

- consigna  del trabajo práctico;
- material blackboard de Laboratorio III;
- ejemplos proporcionados por la cátedra;
- resultados obtenidos durante las pruebas.

Las propuestas realizadas por la herramienta fueron revisadas y probadas durante el desarrollo.

Entre las decisiones tomadas durante la implementación se encuentran:

- utilizar una cola de mensajes para transportar las mediciones;
- utilizar memoria compartida para mantener las estadísticas;
- utilizar un semáforo para proteger el acceso concurrente a las estadísticas;
- utilizar un pool fijo de workers;
- calcular el promedio a partir de suma y cantidad en lugar de almacenarlo;
- copiar las estadísticas a memoria local en `consulta` para liberar rápidamente el semáforo;
- utilizar un mensaje especial por worker para realizar una terminación ordenada.

El código final fue compilado y probado en el entorno Linux utilizado para el trabajo práctico.