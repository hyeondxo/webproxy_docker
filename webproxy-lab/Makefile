CC ?= gcc
CFLAGS ?= -g -Wall -Wextra -O2 -I . -I tiny
LDFLAGS ?=

PROXY_BIN := proxy
TINY_BIN := tiny/tinyserver
TINYSRC := tiny

PORT ?= 8000
PROXY_PORT ?= 15213

.PHONY: all clean run run-tiny run-proxy

all: $(PROXY_BIN) $(TINY_BIN)

$(PROXY_BIN): proxy.o tiny/csapp.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TINY_BIN): $(TINYSRC)/tiny.o $(TINYSRC)/csapp.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

proxy.o: proxy.c tiny/csapp.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(TINYSRC)/tiny.o: $(TINYSRC)/tiny.c $(TINYSRC)/csapp.h
	$(CC) $(CFLAGS) -I $(TINYSRC) -c -o $@ $<

$(TINYSRC)/csapp.o: $(TINYSRC)/csapp.c $(TINYSRC)/csapp.h
	$(CC) $(CFLAGS) -I $(TINYSRC) -c -o $@ $<

run: run-tiny run-proxy

run-tiny: $(TINY_BIN)
	cd $(TINYSRC) && ./tinyserver $(PORT)

run-proxy: $(PROXY_BIN)
	./$(PROXY_BIN) $(PROXY_PORT)

clean:
	rm -f *~ *.o core *.tar *.zip *.gzip *.bzip *.gz
	rm -f $(PROXY_BIN) $(TINY_BIN)
	rm -f $(TINYSRC)/*.o
