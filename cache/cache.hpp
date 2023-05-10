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
  // implement a totally useless base class
  virtual bool match(uint64_t addr) const { return false; } // wether an address match with this block
  virtual void reset() {}            // reset the metadata
  virtual void to_invalid() {}       // change state to invalid
  virtual void to_shared() {}        // change to shared
  virtual void to_modified() {}      // change to modified
  virtual void to_owned() {}         // change to owned
  virtual void to_exclusive() {}     // change to exclusive
  virtual void to_dirty() {}         // change to dirty
  virtual void to_clean() {}         // change to dirty
  virtual bool is_valid() const { return false; }
  virtual bool is_shared() const { return false; }
  virtual bool is_modified() const { return false; }
  virtual bool is_owned() const { return false; }
  virtual bool is_exclusive() const { return false; }
  virtual bool is_dirty() const { return false; }

  virtual ~CMMetadataBase() {}
};

class CMDataBase
{
public:
  // implement a totally useless base class
  virtual void reset() {} // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const { return 0; } // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) {} // write a 64b data with wmask
  virtual void write(uint64_t *wdata) {} // write the whole cache block
  virtual void copy(const CMDataBase *block) {} // copy the content of block

  virtual ~CMDataBase() {}
};

// typical 64B data block
class Data64B : public CMDataBase
{
protected:
  uint64_t data[8];

public:
  Data64B() : data{0} {}
  virtual ~Data64B() {}

  virtual void reset() { for(auto d:data) d = 0; }
  virtual uint64_t read(unsigned int index) const { return data[index]; }
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) { data[index] = (data[index] & (~wmask)) | (wdata & wmask); }
  virtual void write(uint64_t *wdata) { for(int i=0; i<8; i++) data[i] = wdata[i]; }
  virtual void copy(const CMDataBase *m_block) {
    auto block = static_cast<const Data64B *>(m_block);
    for(int i=0; i<8; i++) data[i] = block->data[i];
  }
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
template<int IW, int NW, typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type, // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
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

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w);

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
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, IDX>::value>::type>  // IDX <- IndexFuncBase
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
  virtual const CMMetadataBase *read(uint32_t ai, uint32_t s, uint32_t w);  // obtain the cache block for read
  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w);  // obtain the cache block for modification
  virtual CMMetadataBase *write(uint32_t ai, uint32_t s, uint32_t w); // obtain the cache block for write
  virtual void invalidate(uint32_t ai, uint32_t s, uint32_t w); // invalidate a cache block
  virtual const CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) const;
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w);
};

// Normal set-associative cache
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC>;

/* Example: a 128-set 8-way set-associative cache using
     a 48-bit address system,
     no cache block,
     normal index,
     LRU replacement policy,
     MSI coherence protocol

  CacheNorm<7, 8, MetadataMSI<48, 7+6>, void, IndexNorm<7, 6>, ReplaceLRU<7, 8> > cache;
*/



#endif
