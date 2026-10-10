# TP Integrador - Laboratorio III

## Sistema SCADA concurrente

Trabajo práctico integrador correspondiente a Laboratorio III.

El programa simula un sistema SCADA en el cual distintos procesos
sensores generan mediciones, que son enviadas al proceso colector
mediante una cola de mensajes POSIX.

El colector procesa las mediciones concurrentemente utilizando un
pool de threads workers y mantiene estadísticas en memoria compartida.

Un segundo programa, `consulta`, permite visualizar las estadísticas
mientras el colector continúa ejecutándose.

---

## Arquitectura

El flujo principal del sistema es:

Sensor (proceso)
        |
        | mq_send()
        v
Cola de mensajes POSIX
        |
        | mq_receive()
        v
Pool de workers (threads)
        |
        | actualización protegida por semáforo
        v
Memoria compartida
        |
        v
Programa consulta

---

## Componentes

### colector

Es el programa principal.

Se encarga de:

- crear la cola de mensajes;
- crear la memoria compartida;
- crear el semáforo de protección;
- crear los procesos sensores mediante `fork()`;
- crear el pool de threads workers;
- esperar la finalización de los sensores;
- indicar la terminación de los workers;
- mostrar las estadísticas finales;
- liberar los recursos IPC.

### Sensores

Cada sensor es un proceso hijo creado mediante `fork()`.

Cada sensor:

1. genera valores aleatorios;
2. construye una estructura `Medicion`;
3. envía la medición a la cola POSIX;
4. espera un pequeño intervalo antes de generar la siguiente medición.

### Workers

El colector crea un pool fijo de workers.

Los workers consumen concurrentemente las mediciones utilizando
`mq_receive()`.

Cada medición es procesada por un único worker.

Los workers actualizan:

- cantidad de mediciones;
- suma;
- promedio;
- mínimo;
- máximo;
- último valor;

tanto por sensor como a nivel global.

### Memoria compartida

Las estadísticas se almacenan en un objeto de memoria compartida POSIX.

Esto permite que un proceso independiente pueda consultar las
estadísticas mientras el colector sigue ejecutándose.

### Sincronización

El acceso a las estadísticas está protegido mediante un semáforo
POSIX con nombre.

Los workers toman el semáforo antes de modificar las estadísticas.

El programa `consulta` también utiliza el mismo semáforo para obtener
una copia consistente de los datos.

### consulta

`consulta` es un segundo ejecutable independiente.

Se conecta a la memoria compartida creada por el colector, obtiene una
copia de las estadísticas protegida por el semáforo y luego las muestra
por pantalla.

La copia permite liberar rápidamente el semáforo y evitar mantener
bloqueados a los workers durante los `printf()`.

---

## Compilación

El proyecto debe compilarse en un entorno Linux con soporte para
IPC POSIX.

Desde el directorio del proyecto:

```bash
make clean
make