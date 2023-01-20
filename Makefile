MKFILE  = Makefile
CFLAGS = -pthread -Wall -Werror -Wextra -pedantic 
CC = clang
COMPILEC = ${CC} ${CFLAGS}

MODULES = httpserver methods queue
CSOURCE = ${MODULES:=.c}
EXECBIN = httpserver
OBJECTS = ${CSOURCE:.c=.o}

all: ${EXECBIN}

${EXECBIN}: ${OBJECTS} ${MKFILE}
	${COMPILEC} -o $@ ${OBJECTS}

%.o: %.c
	${COMPILEC} -c $<

debug: ${CSOURCE}
	${CC} ${CFLAGS} ${CSOURCE} -g

clean:
	rm ${OBJECTS} ${EXECBIN}
