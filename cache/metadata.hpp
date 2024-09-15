#ifndef CM_CACHE_METADATA_HPP
#define CM_CACHE_METADATA_HPP

#include <string>
#include <boost/format.hpp>
#include "util/concept_macro.hpp"
#include "util/multithread.hpp"

class CMDataBase
{
public:
  virtual ~CMDataBase() = default;
  // implement a totally useless base class
  virtual void reset() {} // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const { return 0; } // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) {} // write a 64b data with wmask
  virtual void write(uint64_t *wdata) {} // write the whole cache block
  virtual void copy(const CMDataBase *block) = 0; // copy the content of block
  virtual std::string to_string() const = 0;
};

// typical 64B data block
class Data64B : public CMDataBase
{
protected:
  uint64_t data[8] = {0};

public:
  virtual void reset() override { for(auto &d:data) d = 0; }
  virtual uint64_t read(unsigned int index) const override { return data[index]; }
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) override { data[index] = (data[index] & (~wmask)) | (wdata & wmask); }
  virtual void write(uint64_t *wdata) override { for(int i=0; i<8; i++) data[i] = wdata[i]; }

  virtual void copy(const CMDataBase *m_block) override {
    auto block = static_cast<const Data64B *>(m_block);
    for(int i=0; i<8; i++) data[i] = block->data[i];
  }

  virtual std::string to_string() const override {
    return (boost::format("%016x %016x %016x %016x %016x %016x %016x %016x")
            % data[0] % data[1] % data[2] % data[3] % data[4] % data[5] % data[6] % data[7]).str();
  }
};

// a common base between data metadata and normal coherence metadata
class CMMetadataCommon
{
public:
  virtual ~CMMetadataCommon() = default;
  virtual void to_invalid() = 0;      // change state to invalid
  virtual bool is_valid() const = 0;
  virtual bool match(uint64_t addr) const = 0;
  virtual void to_extend() = 0;

  // support multithread (should not be here but MIRAGE need a data metadata which does not derive from CMMetadataBase)
  virtual void lock() {}
  virtual void unlock() {}
};

// base class for all metadata supporting coherence
// assuming the minimal coherence protocol is MI
class CMMetadataBase : public CMMetadataCommon
{
protected:
  unsigned int state     : 3;
  unsigned int dirty     : 1; // 0: clean, 1: dirty
  unsigned int extend    : 1; // 0: cache meta, 1: extend directory meta
public:
  static const unsigned int state_invalid   = 0; // 000 invalid
  static const unsigned int state_shared    = 1; // 001 clean, shared
  static const unsigned int state_modified  = 6; // 100 may dirty, exclusive
  static const unsigned int state_exclusive = 4; // 110 clean, exclusive
  static const unsigned int state_owned     = 2; // 010 may dirty, shared

  CMMetadataBase() : state(0), dirty(0), extend(0) {}

  // implement a totally useless base class
  virtual bool match(uint64_t addr) const override { return false; } // wether an address match with this block
  virtual void init(uint64_t addr) {}                                // initialize the meta for addr
  virtual uint64_t addr(uint32_t s) const { return 0; }              // assemble the block address from the metadata

  virtual void to_invalid() override { state = state_invalid; dirty = 0; }
  virtual void to_shared(int32_t coh_id) { state = state_shared; }
  virtual void to_modified(int32_t coh_id) { state = state_modified; }
  virtual void to_exclusive(int32_t coh_id) { state = state_exclusive; }
  virtual void to_owned(int32_t coh_id) { state = state_owned; }
  virtual void to_dirty() { dirty = 1; }
  virtual void to_clean() { dirty = 0; }
  virtual void to_extend() override { extend = 1; }
  virtual bool is_valid() const override { return state; }
  __always_inline bool is_shared() const { return state == state_shared; }
  __always_inline bool is_modified() const {return state == state_modified; }
  __always_inline bool is_exclusive() const { return state == state_exclusive; }
  __always_inline bool is_owned() const { return state == state_owned; }
  virtual bool is_dirty() const { return dirty; }
  __always_inline bool is_extend() const { return extend; }

  virtual bool allow_write() const { return 0 != (state & 0x4); }

  virtual void sync(int32_t coh_id) {}     // sync after probe
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }

  virtual CMMetadataBase * get_outer_meta() { return nullptr; } // return the outer metadata if supported
  virtual const CMMetadataBase * get_outer_meta() const { return nullptr; }

  virtual std::string to_string() const {
    std::string str_state; str_state.reserve(16);
    switch(state) {
    case state_invalid:   str_state.append("I"); break;
    case state_shared:    str_state.append("S"); break;
    case state_modified:  str_state.append("M"); break;
    case state_exclusive: str_state.append("E"); break;
    case state_owned:     str_state.append("O"); break;
    default:              str_state.append("X");
    }

    return str_state.append(is_dirty() ? "d" : "c").append(allow_write() ? "W" : "R");
  }

  virtual void copy(const CMMetadataBase *meta) {
    state  = meta->state;
    dirty  = meta->dirty;
  }
};

typedef CMMetadataBase MetadataBroadcastBase;

class MetadataDirectoryBase : public MetadataBroadcastBase
{
  __always_inline void add_sharer_help(int32_t coh_id) { if(coh_id != -1) add_sharer(coh_id); }

protected:
  uint64_t sharer = 0;
  __always_inline void add_sharer(int32_t coh_id) { sharer |= (1ull << coh_id); }
  __always_inline void clean_sharer(){ sharer = 0; }
  __always_inline void delete_sharer(int32_t coh_id){ sharer &= ~(1ull << coh_id); }
  __always_inline bool is_sharer(int32_t coh_id) const { return ((1ull << coh_id) & (sharer))!= 0; }

public:
  virtual void to_invalid()                 override { MetadataBroadcastBase::to_invalid();         clean_sharer();          }
  virtual void to_shared(int32_t coh_id)    override { MetadataBroadcastBase::to_shared(coh_id);    add_sharer_help(coh_id); }
  virtual void to_modified(int32_t coh_id)  override { MetadataBroadcastBase::to_modified(coh_id);  add_sharer_help(coh_id); }
  virtual void to_exclusive(int32_t coh_id) override { MetadataBroadcastBase::to_exclusive(coh_id); add_sharer_help(coh_id); }
  virtual void to_owned(int32_t coh_id)     override { MetadataBroadcastBase::to_owned(coh_id);     add_sharer_help(coh_id); }
  __always_inline bool is_exclusive_sharer(int32_t coh_id) const {return (1ull << coh_id) == sharer; }

  virtual void copy(const CMMetadataBase *m_meta) override {
    MetadataBroadcastBase::copy(m_meta);
    auto meta = static_cast<const MetadataDirectoryBase *>(m_meta);
    sharer = meta->get_sharer();
  }

  virtual void sync(int32_t coh_id) override { if(coh_id != -1) { delete_sharer(coh_id); } }

  __always_inline uint64_t get_sharer() const { return sharer; }
  __always_inline void set_sharer(uint64_t c_sharer) { sharer = c_sharer; }

  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const override {
    return is_sharer(target_id) && (target_id != request_id);
  }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const override {
    return is_sharer(target_id) && (target_id != request_id);
  }
};

// Metadata Mixer
// AW    : address width
// IW    : index width
// TOfst : tag offset
// MT    : metadata type
// OutMT : the metadata type to store outer cache state
template <int AW, int IW, int TOfst, typename MT> requires C_DERIVE<MT, CMMetadataBase>
class MetadataMixer : public MT
{
protected:
  uint64_t     tag = 0;
  constexpr static uint64_t mask = (1ull << (AW-TOfst)) - 1;
  CMMetadataBase outer_meta; // maintain a copy of metadata for hierarchical coherence support
                             // this outer metadata is responsible only to record the S/M/E/O state seen by the outer
                             // whether the block is dirty, shared by inner caches, and directory, etc. are hold by the metadata

public:
  virtual CMMetadataBase * get_outer_meta() override { return &outer_meta; }
  virtual const CMMetadataBase * get_outer_meta() const override { return &outer_meta; }

  virtual bool match(uint64_t addr) const override { return MT::is_valid() && ((addr >> TOfst) & mask) == tag; }
  virtual void init(uint64_t addr) override {
    tag = (addr >> TOfst) & mask;
    CMMetadataBase::state = 0;
  }
  virtual uint64_t addr(uint32_t s) const {
    uint64_t addr = tag << TOfst;
    if constexpr (IW > 0) {
      constexpr uint64_t index_mask = (1ull << IW) - 1;
      addr |= (s & index_mask) << (TOfst - IW);
    }
    return addr;
  }
  virtual void sync(int32_t coh_id) override {}

  virtual void to_invalid() override { MT::to_invalid(); outer_meta.to_invalid(); }
  virtual void to_dirty() override { outer_meta.to_dirty(); } // directly use the outer meta so the dirty state is release to outer when evicted
  virtual void to_clean() override { outer_meta.to_clean(); }
  virtual bool is_dirty() const override { return outer_meta.is_dirty(); }
  virtual bool allow_write() const override {return outer_meta.allow_write(); }

  virtual void copy(const CMMetadataBase *m_meta) override {
    // ATTN! tag is not coped.
    MT::copy(m_meta);
    outer_meta.copy(m_meta->get_outer_meta());
  }
};

template <int AW, int IW, int TOfst, typename MT> requires C_DERIVE<MT, MetadataBroadcastBase> && (!C_DERIVE<MT, MetadataDirectoryBase>)
using MetadataBroadcast = MetadataMixer<AW, IW, TOfst, MT>;

template <int AW, int IW, int TOfst, typename MT> requires C_DERIVE<MT, MetadataDirectoryBase>
using MetadataDirectory = MetadataMixer<AW, IW, TOfst, MT>;

// support a relocated bit in the metadata for dynamic remap randomized caches
template<typename MT> requires C_DERIVE<MT, CMMetadataBase>
class MetadataWithRelocate : public MT
{
protected:
  bool relocated = false;
public:
  __always_inline void to_relocated()   { relocated = true;  }
  __always_inline void to_unrelocated() { relocated = false; }
  __always_inline bool is_relocated()   { return relocated;  }
};

// A wrapper for implementing the multithread required cache line lock utility
template <typename MT> requires C_DERIVE<MT, CMMetadataCommon>
class MetaLock : public MT {
  std::mutex mtx;

#ifdef CHECK_MULTI
  // verify no double lock or unlock
  std::atomic<uint64_t> locked;
#endif

public:
  virtual void lock() override {
#ifdef CHECK_MULTI
    uint64_t thread_id = global_lock_checker->thread_id();
    assert(locked.load() != thread_id || 0 ==
            "This cache line has already be locked by this thread and should not be locked by this thread again!");
#endif
    mtx.lock();
#ifdef CHECK_MULTI
    global_lock_checker->push(this);
    locked = thread_id;
#endif
  }

  virtual void unlock() override {
#ifdef CHECK_MULTI
    //uint64_t thread_id = global_lock_checker->thread_id();
    assert(locked.load() != 0 || 0 ==
           "This cache line has already be unlocked and should not be unlocked again!");
    locked = 0;
    global_lock_checker->pop(this);
#endif
    mtx.unlock();
  }
};

#endif
