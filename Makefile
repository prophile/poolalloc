pool.o: pool.c pool.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f pool.o
