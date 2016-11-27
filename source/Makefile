CFLAGS = -g -Wall

ALL:	dos_ls dos_cp
.PHONY: ALL clean

dos_ls:	dos_ls.o dos.o
	$(CC) $(CFLAGS) -o dos_ls dos_ls.o dos.o

dos_cp:	dos_cp.o dos.o
	$(CC) $(CFLAGS) -o dos_cp dos_cp.o dos.o

clean:
	-rm -f dos.o dos_ls dos_ls.o dos_cp dos_cp.o
