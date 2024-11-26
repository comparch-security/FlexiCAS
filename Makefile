
MODE ?= release
NCORE ?= `nproc`

MAKE = make
CXX = g++

# c++17 + concept by default, c++20 would work as well
CXXSTD = --std=c++17 -fconcepts
#CXXSTD = --std=c++20

ifeq ($(MODE), release)
	CXXFLAGS = $(CXXSTD) -O3 -DNDEBUG -I. -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS)
	REGRESS_LD_FLAGS =
else ifeq ($(MODE), debug)
	CXXFLAGS = $(CXXSTD) -O0 -DTRY_LOCK -g -I. -Wall  -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS) 
	REGRESS_LD_FLAGS =
else ifeq ($(MODE), debug-multi)
	CXXFLAGS = $(CXXSTD) -O0 -g -I. -Wall -Werror -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS) -DCHECK_MULTI -DBOOST_STACKTRACE_LINK -DBOOST_STACKTRACE_USE_BACKTRACE
	REGRESS_LD_FLAGS = -lboost_stacktrace_backtrace -ldl -lbacktrace
else
	CXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS) -DTRY_LOCK -DNDEBUG
	REGRESS_LD_FLAGS =
endif

UTIL_HEADERS  = $(wildcard util/*.hpp)
CACHE_HEADERS = $(wildcard cache/*.hpp)
REPLAYER_HEADERS = $(wildcard replayer/*.hpp)

CRYPTO_LIB    = cryptopp/libcryptopp.a
UTIL_OBJS     = util/random.o util/query.o util/statistics.o util/zfstream.o

all: libflexicas.a

.PONY: all

$(CRYPTO_LIB):
	$(MAKE) -C cryptopp -j$(NCORE)

$(UTIL_OBJS) : %o:%cpp $(CACHE_HEADERS) $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS_MULTI) -c $< -o $@


REGRESSION_TESTS = \
	c1-l1 \
	c2-l2 c2-l2-mesi c2-l2-exc c2-l2-exc-mi c2-l2-exc-mesi \
	c4-l3 c4-l3-exc c4-l3-exc-mesi c4-l3-intel \
	

REGRESSION_TESTS_EXE = $(patsubst %, regression/%, $(REGRESSION_TESTS))
REGRESSION_TESTS_LOG = $(patsubst %, regression/%.log, $(REGRESSION_TESTS))
REGRESSION_TESTS_RST = $(patsubst %, regression/%.out, $(REGRESSION_TESTS))

$(REGRESSION_TESTS_EXE): %:%.cpp $(UTIL_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS)
	$(CXX) $(CXXFLAGS) $< $(UTIL_OBJS) $(CRYPTO_LIB) -o $@

$(REGRESSION_TESTS_LOG): %.log:%
	$< > $@

$(REGRESSION_TESTS_RST): %.out: %.log %.expect
	diff $^ 2>$@

PARALLEL_REGRESSION_TESTS = multi-l2-msi multi-l3-msi

PARALLEL_REGRESSION_TESTS_EXE = $(patsubst %, regression/%, $(PARALLEL_REGRESSION_TESTS))
PARALLEL_REGRESSION_TESTS_RST = $(patsubst %, regression/%.out, $(PARALLEL_REGRESSION_TESTS))

$(PARALLEL_REGRESSION_TESTS_EXE): %:%.cpp $(UTIL_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS)
	$(CXX) $(CXXFLAGS_MULTI) $< $(UTIL_OBJS) $(CRYPTO_LIB) $(REGRESS_LD_FLAGS) -o $@ -lz

$(PARALLEL_REGRESSION_TESTS_RST): %.out: %
	timeout 2m $< 2>$@

REPLAYER_REGRESSION_TESTS = replayer

REPLAYER_REGRESSION_TESTS_EXE = $(patsubst %, regression/%, $(REPLAYER_REGRESSION_TESTS))
REPLAYER_REGRESSION_TESTS_RST = $(patsubst %, regression/%.out, $(REPLAYER_REGRESSION_TESTS))

$(REPLAYER_REGRESSION_TESTS_EXE): %:%.cpp $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB) $(REPLAYER_HEADERS)
	$(CXX) $(CXXFLAGS) $< $(CACHE_OBJS) $(UTIL_OBJS) $(CRYPTO_LIB) -o $@ -lz

$(REPLAYER_REGRESSION_TESTS_RST): %.out: %
	$< 1>$@

replayer-regression: $(REPLAYER_REGRESSION_TESTS_RST)

regression: $(REGRESSION_TESTS_RST) $(PARALLEL_REGRESSION_TESTS_RST) $(REPLAYER_REGRESSION_TESTS_RST)

clean-regression:
	-rm $(REGRESSION_TESTS_LOG) $(REGRESSION_TESTS_EXE) $(REGRESSION_TESTS_RST)
	-rm $(PARALLEL_REGRESSION_TESTS_EXE) $(PARALLEL_REGRESSION_TESTS_RST)
	-rm $(REPLAYER_REGRESSION_TESTS_EXE) $(REPLAYER_REGRESSION_TESTS_RST)

PERFORMANCE_TESTS = multi-l2-msi multi-l3-msi replay
 
PERFORMANCE_TESTS_EXE = $(patsubst %, performance/%, $(PERFORMANCE_TESTS))
PERFORMANCE_TESTS_RST = $(patsubst %, performance/%.out, $(PERFORMANCE_TESTS))

$(PERFORMANCE_TESTS_EXE): %:%.cpp $(UTIL_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS) $(REPLAYER_HEADERS) $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS_MULTI) $< $(UTIL_OBJS) $(CRYPTO_LIB) $(REGRESS_LD_FLAGS) -o $@ -lz

$(PERFORMANCE_TESTS_RST): %.out: %

performance: $(PERFORMANCE_TESTS_EXE)

clean-performance:
	-rm $(PERFORMANCE_TESTS_EXE) $(PERFORMANCE_TESTS_RST)

libflexicas.a: $(UTIL_OBJS) $(CRYPTO_LIB)
	ar rvs $@ $(UTIL_OBJS) $(CRYPTO_LIB)

.PHONY: regression

clean:
	-$(MAKE) clean-regression
	-$(MAKE) clean-parallel-regression temp.log
	-$(MAKE) clean-performance
	-rm $(UTIL_OBJS)

.PHONY: clean
