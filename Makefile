
MODE ?=
NCORE ?= `nproc`

MAKE = make
CXX = g++

# c++17 + concept by default, c++20 would work as well
CXXSTD = --std=c++17 -fconcepts
#CXXSTD = --std=c++20

ifeq ($(MODE), release)
    CXXFLAGS = $(CXXSTD) -O2 -DNDEBUG -I. -fPIC
else ifeq ($(MODE), debug)
    CXXFLAGS = $(CXXSTD) -O0 -g -I. -fPIC
else
    CXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
endif

ifeq ($(MODE), debug)
    DSLCXXFLAGS = $(CXXSTD) -O1 -g -I. -fPIC
else
    DSLCXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
endif

UTIL_HEADERS  = $(wildcard util/*.hpp)
CACHE_HEADERS = $(wildcard cache/*.hpp)
DSL_HEADERS   = $(wildcard dsl/*.hpp)

CRYPTO_LIB    = cryptopp/libcryptopp.a
CACHE_OBJS    = cache/metadata.o
UTIL_OBJS     = util/random.o util/query.o util/monitor.o

all: libflexicas.a

.PONY: all

$(CRYPTO_LIB):
	$(MAKE) -C cryptopp -j$(NCORE)

$(CACHE_OBJS) : %o:%cpp $(CACHE_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(UTIL_OBJS) : %o:%cpp $(CACHE_HEADERS) $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@


REGRESSION_TESTS = \
	c1-l1 \
	c2-l2 c2-l2-mesi c2-l2-exc c2-l2-exc-mi c2-l2-exc-mesi \
	c4-l3 c4-l3-exc c4-l3-exc-mesi c4-l3-intel \
	c2-l2-mirage

REGRESSION_TESTS_EXE = $(patsubst %, regression/%, $(REGRESSION_TESTS))
REGRESSION_TESTS_LOG = $(patsubst %, regression/%.log, $(REGRESSION_TESTS))
REGRESSION_TESTS_RST = $(patsubst %, regression/%.out, $(REGRESSION_TESTS))

$(REGRESSION_TESTS_EXE): %:%.cpp $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS)
	$(CXX) $(CXXFLAGS) $< $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB) -o $@

$(REGRESSION_TESTS_LOG): %.log:%
	$< > $@

$(REGRESSION_TESTS_RST): %.out: %.log %.expect
	diff $^ 2>$@

regression: $(REGRESSION_TESTS_RST)

clean-regression:
	-rm $(REGRESSION_TESTS_LOG) $(REGRESSION_TESTS_EXE) $(REGRESSION_TESTS_RST)

libflexicas.a: $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS)
	ar rvs $@ $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB)

.PHONY: regression

clean:
	$(MAKE) clean-regression
	-rm $(UTIL_OBJS) $(CACHE_OBJS)

.PHONY: clean
