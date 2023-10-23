
CONFIG ?= example
MODE ?=

MAKE = make
CXX = g++

# c++17 + concept by default, c++20 would work as well
CXXSTD = --std=c++17 -fconcepts
#CXXSTD = --std=c++20

ifeq ($(MODE), release)
    CXXFLAGS = $(CXXSTD) -O2 -DNDEBUG -I. -fPIC
else ifeq ($(MODE), debug)
    CXXFLAGS = $(CXXSTD) -O1 -g -I. -fPIC
else
    CXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
endif

ifeq ($(MODE), debug)
    DSLCXXFLAGS = $(CXXSTD) -O1 -g -I. -fPIC
else
    DSLCXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
endif

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
	$(CXX) $(DSLCXXFLAGS) $^ -o $@

$(DSL_OBJS) : %o:%cpp $(DSL_HEADERS)
	$(CXX) $(DSLCXXFLAGS) -c $< -o $@

$(UTIL_OBJS) : %o:%cpp $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-rm $(UTIL_OBJS)
	-rm $(DSL_OBJS)
	-rm dsl-decoder
	-rm $(CONFIG).cpp $(CONFIG).hpp
	-rm lib$(CONFIG).a

