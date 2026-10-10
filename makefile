CC = gcc
CFLAGS = -std=c89 -D_XOPEN_SOURCE=700 -pedantic -Wall -Werror
LIBS = -lrt -lpthread

all: colector consulta

colector: colector.c scada.h
	$(CC) $(CFLAGS) colector.c -o colector $(LIBS)

consulta: consulta.c scada.h
	$(CC) $(CFLAGS) consulta.c -o consulta $(LIBS)

clean:
	rm -f colector consulta *.o

.PHONY: all clean