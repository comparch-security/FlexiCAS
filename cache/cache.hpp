#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <set>
#include <map>
#include <vector>

#include "util/random.hpp"

class CMMetadataBase
{
public:
  virtual bool match(uint64_t addr) const = 0;  // wether an address match with this block
  virtual void reset() = 0;                     // reset the metadata
  virtual void to_invalid() = 0;                // change state to invalid
  virtual void to_shared() = 0;                 // change to shared
  virtual void to_modified() = 0;               // change to modified
  virtual void to_owned() = 0;                  // change to owned
  virtual void to_exclusive() = 0;              // change to exclusive
  virtual void to_dirty() = 0;                  // change to dirty
  virtual bool is_valid() const = 0;
  virtual bool is_shared() const = 0;
  virtual bool is_modified() const = 0;
  virtual bool is_owned() const = 0;
  virtual bool is_exclusive() const = 0;
  virtual bool is_dirty() const = 0;
};

class CMDataBase
{
public:
  virtual void reset() = 0; // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const = 0; // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) = 0; // write a 64b data with wmask
  virtual void write(uint64_t *wdata) = 0; // write the whole cache block
};

// Note: may be we should move this into another header

// metadata supporting MSI coherency
// AW    : address width
// TOfst : tag offset
template <int AW, int TOfst>
class MetadataMSI : public CMMetadataBase
{
protected:
  uint64_t     tag   : AW-TOfst;
  unsigned int state : 2; // 0: invalid, 1: shared, 2:modify
  unsigned int dirty : 1; // 0: clean, 1: dirty

  static const uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  MetadataMSI() : tag(0), state(0), dirty(0) {}

  virtual bool match(uint64_t addr) { return ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; state = 0; dirty = 0; }
  virtual void to_invalid() { state = 0; }
  virtual void to_shared() { state = 1; }
  virtual void to_modified() { state = 2; }
  virtual void to_owned() { state = 2; }     // not supported, equal to modified
  virtual void to_exclusive() { state = 2; } // not supported, equal to modified
  virtual void to_dirty() { dirty = 1; }
  virtual bool is_valid() const { return state; }
  virtual bool is_shared() const { return state == 1; }
  virtual bool is_modified() const {return state == 2; }
  virtual bool is_owned() const { return state == 2; } // not supported, equal to modified
  virtual bool is_exclusive()  { return state == 2; }  // not supported, equal to modified
  virtual bool is_dirty() const { return dirty; }
  
};

// typical 64B data block
class Data64B : public CMDataBase
{
protected:
  uint64_t data[8];

public:
  Data64B() : data{0} {}
  virtual uint64_t read(unsigned int index) { return data[index]; }
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) { data[index] = (data[index] & (~wmask)) | (wdata & wmask); }
  virtual void write(uint64_t *wdata) { for(int i=0; i<8; i++) data[i] = wdata[i]; }
};

//////////////// define cache array ////////////////////

// record and generate the unique IDs for caches
// We will need to use these ids in the reporter for fast locating a cache
class CacheID
{
protected:
  static std::set<uint32_t> ids;
public:
  // generate a new unique id
  uint32_t static new_id() {
    uint32_t id = cm_get_random_uint32();
    while(ids.count(id)) id = cm_get_random_uint32();
    ids.insert(id);
    return id;
  }
};

// base class for a cache array:
class CacheArrayBase
{
protected:
  const uint32_t id;                    // a unique id to identify this cache
  const std::string name;               // an optional name to describe this cache

public:
  CacheArrayBase(std::string name = "") : id(CacheID::new_id()), name(name) {}

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;

  // locate a data block in a meta and data separate cache, such as MIRAGE
  virtual bool locate_data(uint32_t ms, uint32_t mw, uint32_t *ds, uint32_t *dw) const {
    *ds = ms; *dw = mw;
    return true;
  }

  // locate a data block in a meta and data separate cache, such as MIRAGE
  virtual bool locate_meta(uint32_t ds, uint32_t dw, uint32_t *ms, uint32_t *mw) const {
    *ms = ds; *mw = dw;
    return true;
  }

  virtual const CMMetadataBase * get_meta(uint32_t s, uint32_t w) const = 0;
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) = 0;

  virtual const CMDataBase * get_data(uint32_t ds, uint32_t dw) const = 0;
  virtual CMDataBase * get_data(uint32_t ds, uint32_t dw) = 0;
};

// normal set associative cache array
template<typename MT, typename DT>
class CacheArrayNorm : public CacheArrayBase
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

  virtual ~CacheArrayNorm() {
    free(meta);
    if(!std::is_void<DT>::value) free(data);
  }

  // @jinchi ToDo: implement these functions
  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(int i=0; i<nway; i++)
      if(meta[s*nway + i].match(addr)) {
        *w = i;
        return true;
      }

    return false;
  }

  virtual const CMMetadataBase * get_meta(uint32_t s, uint32_t w) const;
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w);

  // @jinchi remember to check whether DT is void
  virtual const CMDataBase * get_data(uint32_t ds, uint32_t dw) const;
  virtual CMDataBase * get_data(uint32_t ds, uint32_t dw);

protected:
  MT *meta; // meta array
  DT *data; // data array

  void init() {
    size_t num = nset * nway;

    size_t meta_size = num * sizeof(MT);
    meta = (MT *)malloc(meta_size);
    for(size_t i=0; i<num; i++) meta[i].reset();

    if(!std::is_void<DT>::value) {
      size_t data_size = num * sizeof(DT);
      data = (DT *)malloc(data_size);
      // for(size_t i=0; i<num; i++) data[i].reset();
    }
  }
};

//////////////// define cache ////////////////////

// base class for a cache
class CacheBase
{
protected:
  const std::string name;               // an optional name to describe this cache

  // a vector of cache arrays
  // set-associative: one CacheArrayNorm objects
  // with VC: two CacheArrayNorm objects (one fully associative)
  // skewed: partition number of CacheArrayNorm objects (each as a single cache array)
  // MIRAGE: parition number of CacheArrayNorm (configured with separate meta and data array)
  std::vector<CacheArrayBase *> arrays;

public:
  CacheBase(std::string name = "") : name(name) {}

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w,
                   uint32_t *ds, uint32_t *dw // data set and way index when data array is separate as in MIRAGE
                   ) const = 0;

  virtual const CMMetadataBase *read(uint64_t addr) = 0;  // obtain the cache block for read
  virtual CMMetadataBase *access(uint64_t addr) = 0;  // obtain the cache block for modification (other than simple write) ? necessary ?
  virtual CMMetadataBase *write(uint64_t addr) = 0; // obtain the cache block for write
};

// normal set associate cache
class CacheNorm : public CacheBase
{
};


#endif
