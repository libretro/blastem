
dis : dis.o 68kinst.o
	$(CC) -o dis dis.o 68kinst.o
	
test_x86 : test_x86.o gen_x86.o
	$(CC) -o test_x86 test_x86.o gen_x86.o

gen_fib : gen_fib.o gen_x86.o mem.o
	$(CC) -o gen_fib gen_fib.o gen_x86.o mem.o
	
%.o : %.S
	$(CC) -c -o $@ $<

%.o : %.c
	$(CC) -ggdb -c -o $@ $<

clean :
	rm -rf dis test_x86 gen_fib *.o
