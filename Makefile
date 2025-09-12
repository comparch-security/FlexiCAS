
MODE ?=
NCORE ?= `nproc`

MAKE = make
CXX = g++

# c++17 + concept by default, c++20 would work as well, spike enforce 20
CXXSTD = --std=c++2a

ifeq ($(MODE), release)
	CXXFLAGS = $(CXXSTD) -O3 -DNDEBUG -I. -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS)
	REGRESS_LD_FLAGS =
else ifeq ($(MODE), debug)
	CXXFLAGS = $(CXXSTD) -O0 -g -I. -Wall -Werror -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS) -DCHECK_MULTI
	REGRESS_LD_FLAGS =
else ifeq ($(MODE), debug-multi)
	CXXFLAGS = $(CXXSTD) -O0 -g -I. -Wall -Werror -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS) -DCHECK_MULTI -DBOOST_STACKTRACE_LINK -DBOOST_STACKTRACE_USE_BACKTRACE
	REGRESS_LD_FLAGS = -lboost_stacktrace_backtrace -ldl -lbacktrace
else
	CXXFLAGS = $(CXXSTD) -O2 -I. -fPIC
	CXXFLAGS_MULTI = $(CXXFLAGS)
	REGRESS_LD_FLAGS =
endif

UTIL_HEADERS  = $(wildcard util/*.hpp)
CACHE_HEADERS = $(wildcard cache/*.hpp)
RSA_HEADERS   = $(wildcard rsa/*.hpp)

CRYPTO_LIB    = cryptopp/libcryptopp.a
UTIL_OBJS     = util/random.o util/query.o util/statistics.o
RSA_OBJS      = rsa/monitor.o

all: libflexicas.a

.PONY: all

$(CRYPTO_LIB):
	$(MAKE) -C cryptopp -j$(NCORE)

$(UTIL_OBJS) : %o:%cpp $(CACHE_HEADERS) $(UTIL_HEADERS)
	$(CXX) $(CXXFLAGS_MULTI) -c $< -o $@

$(RSA_OBJS) : %o:%cpp $(CACHE_HEADERS) $(UTIL_HEADERS) $(RSA_HEADERS)
	$(CXX) $(CXXFLAGS_MULTI) -c $< -o $@

REGRESSION_TESTS = \
	c1-l1 \
	c2-l2 c2-l2-mesi c2-l2-exc c2-l2-exc-mi c2-l2-exc-mesi \
	c4-l3 c4-l3-exc c4-l3-exc-mesi c4-l3-intel \
	c2-l2-mirage c2-l2-remap

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
	$(CXX) $(CXXFLAGS_MULTI) $< $(UTIL_OBJS) $(CRYPTO_LIB) $(REGRESS_LD_FLAGS) -o $@

$(PARALLEL_REGRESSION_TESTS_RST): %.out: %
	timeout 2m $< 2>$@

clean-regression:
	-rm $(REGRESSION_TESTS_LOG) $(REGRESSION_TESTS_EXE) $(REGRESSION_TESTS_RST)
	-rm $(PARALLEL_REGRESSION_TESTS_EXE) $(PARALLEL_REGRESSION_TESTS_RST)

ALG_TESTS = \
	ppp ct ctpp effective-cloc-acc empty-refill prime-probe prime-probe-prefetch

ALG_TESTS_EXE = $(patsubst %, alg/%, $(ALG_TESTS))

$(ALG_TESTS_EXE): %:%.cpp $(UTIL_OBJS) $(RSA_OBJS) $(CRYPTO_LIB) $(CACHE_HEADERS) $(RSA_HEADERS) alg/common.hpp
	$(CXX) $(CXXFLAGS) $< $(UTIL_OBJS) $(RSA_OBJS) $(CRYPTO_LIB) -lboost_program_options -o $@

alg-tests: $(ALG_TESTS_EXE)

libflexicas.a: $(UTIL_OBJS) $(RSA_OBJS) $(CRYPTO_LIB)
	ar rvs $@ $(UTIL_OBJS) $(RSA_OBJS) $(CRYPTO_LIB)

.PHONY: regression

clean:
	-rm $(UTIL_OBJS) $(RSA_OBJS)
	-rm $(ALG_TESTS_EXE)

.PHONY: clean
