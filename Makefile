
MAKE = make
CXX = g++
CXXFLAGS = --std=c++17 -O2 -I. -fPIC

CONFIG ?= example

ifneq ("$(wildcard $(CONFIG).def)","")
    CONFIG_FILE = $(CONFIG).def
else ifneq ("$(wildcard $(CONFIG))","")
    CONFIG_FILE = $(CONFIG)
else ifneq ("$(wildcard config/$(CONFIG).def)","")
    CONFIG_FILE = config/$(CONFIG).def
else ifneq ("$(wildcard config/$(CONFIG))","")
    CONFIG_FILE = config/$(CONFIG)
endif

UTIL_HEADERS  = $(wildcard util/*.hpp)
CACHE_HEADERS = $(wildcard cache/*.hpp)
DSL_HEADERS   = $(wildcard dsl/*.hpp)

UTIL_OBJS     = util/random.o
DSL_OBJS      = dsl/dsl.o dsl/entity.o dsl/statement.o dsl/type_description.o

all: lib$(CONFIG).a

lib$(CONFIG).a : $(CONFIG).cpp $(UTIL_OBJS) $(CACHE_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $(CONFIG).o
	ar rvs $@ $(CONFIG).o $(UTIL_OBJS)

$(CONFIG).cpp : $(CONFIG_FILE) dsl-decoder
	./dsl-decoder $(CONFIG_FILE) $(CONFIG)

dsl-decoder : $(DSL_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(DSL_OBJS) : %o:%cpp $(DSL_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(UTIL_OBJS) : %o:%cpp $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-rm $(UTIL_OBJS)
	-rm $(DSL_OBJS)
	-rm dsl-decoder
	-rm $(CONFIG).cpp $(CONFIG).hpp
	-rm lib$(CONFIG).a

