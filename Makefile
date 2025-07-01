SERVER=server
CLIENT=client
TESTER=as2_testbench
CFLAGS=-O2 -g -Wall -pedantic
LDFLAGS=-lpthread

all: ${SERVER} ${CLIENT} ${TESTER}

${SERVER}: server.c
	gcc ${CFLAGS} -o ${SERVER} server.c ${LDFLAGS}

${CLIENT}: client.c
	gcc ${CFLAGS} -o ${CLIENT} client.c

${TESTER}: as2_testbench.c linebuffer.c linebuffer.h
	gcc ${CFLAGS} -o ${TESTER} as2_testbench.c linebuffer.c

.PHONY: test
test: all
	./test.sh

.PHONY: clean
clean:
	rm -rf *.o *~ ${SERVER} ${CLIENT} ${TESTER} accounts.dat bank.log /tmp/bank_socket