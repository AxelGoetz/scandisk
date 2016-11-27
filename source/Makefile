CFLAGS = -g -Wall

ALL:	dos_scandisk
.PHONY: ALL clean

dos_ls:	dos_ls.o dos.o
	$(CC) $(CFLAGS) -o dos_ls dos_ls.o dos.o

dos_cp:	dos_cp.o dos.o
	$(CC) $(CFLAGS) -o dos_cp dos_cp.o dos.o

dos_scandisk:	dos_scandisk.o dos.o
	$(CC) $(CFLAGS) -o dos_scandisk dos_scandisk.o dos.o

clean:
	-rm -f dos_scandisk.o dos_scandisk dos.o dos_ls dos_ls.o dos_cp dos_cp.o
