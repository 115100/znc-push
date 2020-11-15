version = $(shell git describe --dirty || echo dev)

all: push.so

push.so: push.cpp
	CXXFLAGS="$(CXXFLAGS) -DPUSHVERSION=\"\\\"$(version)\\\"\" -lcurl" \
		 znc-buildmod push.cpp

install: push.so
	mkdir -p $(HOME)/.znc/modules/
	cp push.so $(HOME)/.znc/modules/push.so

clean:
	-rm -f push.so
