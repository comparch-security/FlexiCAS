#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <random>
#include <memory>
#include <set>
#include <map>

//////////////// define meta block type ////////////////////

struct type_meta_48b_64B_MSI_dirty {
  uint64_t     tag   : 48-6;
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty
  void init() {
    tag = 0;
    state = 0;
    dirty = 0;
  }
};

// default meta block type
typedef type_meta_48b_64B_MSI_dirty cm_meta_t;

//////////////// define cache block type ////////////////////

struct type_data_64B {
  uint64_t data[8];
};

// default data block type
typedef type_data_64B cm_data_t;

//////////////// define cache array ////////////////////

// record and generate the unique IDs for caches
// We will need to use these ids in the reporter for fast locating a cache
class CacheID {
  static std::set<uint32_t> ids;
  static std::default_random_engine gen;
  // use a unique pointer here to handle static member initialization and auto destroy
  static std::unique_ptr<std::uniform_int_distribution<uint32_t> > dist;
public:
  // reset the seed for the uniform random generator
  void seed(uint32_t s) { gen.seed(s); }

  // generate a new unique id
  uint32_t static new_id() {
    if(!dist) dist = std::unique_ptr<std::uniform_int_distribution<uint32_t> >(new std::uniform_int_distribution<uint32_t>(1ul, 1ul<<30));
    uint32_t id = (*dist)(gen);
    while(ids.count(id)) id = (*dist)(gen);
    ids.insert(id);
    return id;
  }
};

// the interface base class for a cache array:
//   metadata and data array
template<typename MT, typename DT>
class CacheArrayBase
{
public:
  const uint32_t id;                    // a unique id to identify this cache
  const std::string name;               // an optional name to describe this cache

  CacheArrayBase() // anonymous cache
    : id(CacheID::new_id()), name("") {}
  CacheArrayBase(std::string name) // named cache
    : id(CacheID::new_id()), name(name) {}

  virtual MT read_meta(uint32_t s, uint32_t w) = 0;
  virtual DT read_data(uint32_t s, uint32_t w) = 0;
  virtual void write_meta(uint32_t s, uint32_t w, MT) = 0;
  virtual void write_data(uint32_t s, uint32_t w, DT) = 0;
  virtual void evict_meta(uint32_t s, uint32_t w, MT) = 0;
  virtual void evict_data(uint32_t s, uint32_t w, DT) = 0;
};

// set associative cache array
template<typename MT, typename DT>
class CacheArrayNorm : public CacheArrayBase<MT,DT>
{
public:
  uint32_t nset, nway;  // number of sets and ways

  CacheArrayNorm(uint32_t nset, uint32_t nway)
    : CacheArrayBase(), nset(nset), nway(nway)
  {
    init();
  }

  CacheArrayNorm(uint32_t nset, uint32_t nway, std::string name)
    : CacheArrayBase(name), nset(nset), nway(nway)
  {
    init();
  }

  // toDo: implement these methods
  virtual MT read_meta(uint32_t s, uint32_t w);
  virtual DT read_data(uint32_t s, uint32_t w);
  virtual void write_meta(uint32_t s, uint32_t w, MT);
  virtual void write_data(uint32_t s, uint32_t w, DT);
  virtual void evict_meta(uint32_t s, uint32_t w, MT);
  virtual void evict_data(uint32_t s, uint32_t w, DT);

private:
  MT *meta; // meta array
  DT *data; // data array

  void init() {
    size_t num = nset * nway;

    size_t meta_size = num * sizeof(MT);
    meta = (MT *)malloc(meta_size);
    for(size_t i=0; i<num; i++) meta[i].init();

    if(!std::is_void<DT>::value) {
      size_t data_size = num * sizeof(DT);
      data = (DT *)malloc(data_size);
    }
  }
};




#endif
