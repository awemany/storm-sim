# (C)opyright 2019 awemany - see file COPYING for details
#CXXFLAGS= -g -Wall -fprofile-arcs -ftest-coverage 
#LDFLAGS = -g -fprofile-arcs -ftest-coverage
CXXFLAGS= -g -Wall -O3
LDFLAGS = -g -Wall -O3
OBJECTS = main.o scheduler.o deltablocks.o

HEADERS=scheduler.h deltablocks.h tinyformat.h

SOURCES=$(OBJECTS:.o=.cpp)

%.o:	%.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

all:	simdeltablocks



simdeltablocks: $(OBJECTS)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o simdeltablocks

clean:
	rm -f $(OBJECTS) simdeltablocks *.gcno *.gcda
