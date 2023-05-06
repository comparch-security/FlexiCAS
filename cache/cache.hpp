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
#include "cache/index.hpp"
#include "cache/replace.hpp"

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

  virtual ~CMMetadataBase() {}
};

class CMDataBase
{
public:
  virtual void reset() = 0; // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const = 0; // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) = 0; // write a 64b data with wmask
  virtual void write(uint64_t *wdata) = 0; // write the whole cache block

  virtual ~CMDataBase() {}
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
  virtual ~MetadataMSI() {}

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
  virtual ~Data64B() {}

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
  CMMetadataBase *meta;                 // meta array
  CMDataBase *data;                     // data array, could be null

public:
  CacheArrayBase(std::string name = "") : id(CacheID::new_id()), name(name), meta(nullptr), data(nullptr) {}

  virtual ~CacheArrayBase() {
    delete [] meta;
    if(data != nullptr) delete [] data;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;

  virtual const CMMetadataBase * get_meta(uint32_t s, uint32_t w) const = 0;
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) = 0;

  virtual const CMDataBase * get_data(uint32_t s, uint32_t w) const = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;
};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT>
class CacheArrayNorm : public CacheArrayBase
{
public:
  const uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(std::string name = "") : CacheArrayBase(name) {
    size_t num = nset * NW;
    meta = new MT[num];
    if(!std::is_void<DT>::value) data = new DT[num];
  }

  virtual ~CacheArrayNorm() {
    free(meta);
    if(!std::is_void<DT>::value) free(data);
  }

  // @jinchi ToDo: implement these functions
  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(int i=0; i<NW; i++)
      if(meta[s*NW + i].match(addr)) {
        *w = i;
        return true;
      }

    return false;
  }

  virtual const CMMetadataBase * get_meta(uint32_t s, uint32_t w) const { return &(meta[s*NW + w]); }
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) { return &(meta[s*NW + w]); }

  virtual const CMDataBase * get_data(uint32_t s, uint32_t w) const {
    return std::is_void<DT>::value ? nullptr : &(data[s*NW + w]);
  }

  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    return std::is_void<DT>::value ? nullptr : &(data[s*NW + w]);
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
  // MIRAGE: parition number of CacheArrayNorm (meta only) with one separate CacheArrayNorm for storing data (in derived class)
  std::vector<CacheArrayBase *> arrays;

  IndexFuncBase *indexer; // index resolver
  ReplaceFuncBase *replacer; // replacer

public:
  CacheBase(IndexFuncBase *indexer,
            std::string name)
    : name(name),
      indexer(indexer)
  {}
  virtual ~CacheBase() {
    for(auto a: arrays) delete a;
    delete indexer;
    delete replacer;
  }

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w
                   ) const = 0;

  virtual const CMMetadataBase *read(uint32_t ai, uint32_t s, uint32_t w) = 0;  // obtain the cache block for read
  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w) = 0;  // obtain the cache block for modification
  virtual CMMetadataBase *write(uint32_t ai, uint32_t s, uint32_t w) = 0; // obtain the cache block for write
  virtual void invalidate(uint32_t ai, uint32_t s, uint32_t w) = 0; // invalidate a cache block
  virtual const CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) const = 0;
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) = 0;

};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC>
class CacheSkewed : public CacheBase
{
public:
  CacheSkewed(std::string name = "")
    : CacheBase(new IDX(), new RPC(), name)
  {
    arrays.resize(P);
    for(auto a:arrays) a = new CacheArrayNorm<IW,NW,MT,DT>();
  }

  // @jinchi ToDo: implement these functions
  virtual const CMMetadataBase *read(uint32_t ai, uint32_t s, uint32_t w) = 0;  // obtain the cache block for read
  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w) = 0;  // obtain the cache block for modification
  virtual CMMetadataBase *write(uint32_t ai, uint32_t s, uint32_t w) = 0; // obtain the cache block for write
  virtual void invalidate(uint32_t ai, uint32_t s, uint32_t w) = 0; // invalidate a cache block
  virtual const CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) const = 0;
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) = 0;
};

// Normal set-associative cache
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC>;

/* Example: a 128-set 8-way set-associative cache using
     a 48-bit address system,
     64B cache block,
     normal index,
     LRU replacement policy

  CacheNorm<7, 8, MetadataMSI<48, 7+6>, Data64B, IndexNorm<7, 6>, ReplaceLRU<7, 8> > cache;
*/



#endif
