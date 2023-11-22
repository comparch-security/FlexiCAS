#ifndef CM_CACHE_METADATA_HPP
#define CM_CACHE_METADATA_HPP

#include <cstdint>
#include "util/concept_macro.hpp"

class CMMetadataBase
{
public:
  // implement a totally useless base class
  virtual bool match(uint64_t addr) const { return false; } // wether an address match with this block
  virtual void reset() {}                                   // reset the metadata
  virtual void init(uint64_t addr) = 0;                     // initialize the meta for addr
  virtual uint64_t addr(uint32_t s) const = 0;              // assemble the block address from the metadata
  virtual void sync(int32_t coh_id) = 0;                    // sync after probe

  virtual void to_invalid() = 0;                            // change state to invalid
  virtual void to_shared(int32_t coh_id) = 0;               // change to shared
  virtual void to_modified(int32_t coh_id) = 0;             // change to modified
  virtual void to_owned(int32_t coh_id) = 0;                // change to owned
  virtual void to_exclusive(int32_t coh_id) = 0;            // change to exclusive
  virtual void to_dirty() = 0;                              // change to dirty
  virtual void to_clean() = 0;                              // change to clean
  virtual void to_extend() = 0;                             // change to extend directory meta

  virtual bool is_valid() const { return false; }
  virtual bool is_shared() const { return false; }
  virtual bool is_modified() const { return false; }
  virtual bool is_owned() const { return false; }
  virtual bool is_exclusive() const { return false; }
  virtual bool is_dirty() const { return false; }
  virtual bool is_extend() const { return false; }

  virtual bool allow_write() const = 0;

  virtual CMMetadataBase * get_outer_meta() { return nullptr; } // return the outer metadata if supported
  virtual const CMMetadataBase * get_outer_meta() const { return nullptr; }

  virtual void copy(const CMMetadataBase *meta) = 0;        // copy the content of meta

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
  virtual void copy(const CMDataBase *block) = 0; // copy the content of block

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

// base class for all metadata supporting coherence
// assuming the minimal coherence protocol is MI
class MetadataBroadcastBase : public CMMetadataBase
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

  MetadataBroadcastBase() : state(0), dirty(0), extend(0) {}
  virtual ~MetadataBroadcastBase() {}

  virtual void to_invalid() { state = state_invalid; dirty = 0; }
  virtual void to_shared(int32_t coh_id) { state = state_shared; }
  virtual void to_modified(int32_t coh_id) { state = state_modified; }
  virtual void to_exclusive(int32_t coh_id) { state = state_exclusive; }
  virtual void to_owned(int32_t coh_id) { state = state_owned; }
  virtual void to_dirty() { dirty = 1; }
  virtual void to_clean() { dirty = 0; }
  virtual void to_extend() { extend = 1; }
  virtual bool is_valid() const { return state; }
  virtual bool is_shared() const { return state == state_shared; }
  virtual bool is_modified() const {return state == state_modified; }
  virtual bool is_exclusive() const { return state == state_exclusive; }
  virtual bool is_owned() const { return state == state_owned; }
  virtual bool is_dirty() const { return dirty; }
  virtual bool is_extend() const { return extend; }

  virtual bool allow_write() const {return 0 != (state & 0x4); }

  virtual void copy(const CMMetadataBase *m_meta) {
    auto meta = static_cast<const MetadataBroadcastBase *>(m_meta);
    state  = meta->state;
    dirty  = meta->dirty;
  }

  // coherence related methods
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; } 
  virtual void init(uint64_t addr) {}
  virtual uint64_t addr(uint32_t s) const { return 0; }
  virtual void sync(int32_t coh_id) {}
};

class MetadataDirectoryBase : public MetadataBroadcastBase
{
  void add_sharer_help(int32_t coh_id) {
    if(coh_id != -1) add_sharer(coh_id);
  }

protected:
  uint64_t sharer = 0;
  virtual void add_sharer(int32_t coh_id) { sharer |= (1ull << coh_id); }
  virtual void clean_sharer(){ sharer = 0; }
  virtual void delete_sharer(int32_t coh_id){ sharer &= ~(1ull << coh_id); }
  virtual bool is_sharer(int32_t coh_id) const { return ((1ull << coh_id) & (sharer))!= 0; }
  virtual bool is_exclusive_sharer(int32_t coh_id) const {return (1ull << coh_id) == sharer; }

public:
  virtual void to_invalid()                 { MetadataBroadcastBase::to_invalid();         clean_sharer();          }
  virtual void to_shared(int32_t coh_id)    { MetadataBroadcastBase::to_shared(coh_id);    add_sharer_help(coh_id); }
  virtual void to_modified(int32_t coh_id)  { MetadataBroadcastBase::to_modified(coh_id);  add_sharer_help(coh_id); }
  virtual void to_exclusive(int32_t coh_id) { MetadataBroadcastBase::to_exclusive(coh_id); add_sharer_help(coh_id); }
  virtual void to_owned(int32_t coh_id)     { MetadataBroadcastBase::to_owned(coh_id);     add_sharer_help(coh_id); }

  virtual void copy(const CMMetadataBase *m_meta) {
    MetadataBroadcastBase::copy(m_meta);
    auto meta = static_cast<const MetadataDirectoryBase *>(m_meta);
    sharer = meta->get_sharer();
  }

  virtual void sync(int32_t coh_id){ if(coh_id != -1) { delete_sharer(coh_id); } }

  virtual uint64_t get_sharer() const { return sharer; }
  virtual void set_sharer(uint64_t c_sharer){ sharer = c_sharer; }

  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const {
    return is_sharer(target_id) && (target_id != request_id);
  }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const {
    return is_sharer(target_id) && (target_id != request_id);
  }

  MetadataDirectoryBase() : MetadataBroadcastBase(), sharer(0) {}
  virtual ~MetadataDirectoryBase() {}
};

// Metadata Mixer
// AW    : address width
// IW    : index width
// TOfst : tag offset
// MT    : metadata type
// OutMT : the metadata type to store outer cache state
template <int AW, int IW, int TOfst, typename MT, typename OutMT>
  requires C_DERIVE(MT, MetadataBroadcastBase) && C_DERIVE_OR_VOID(OutMT, MetadataBroadcastBase) && !C_DERIVE(OutMT, MetadataDirectoryBase)
class MetadataMixer : public MT
{
protected:
  uint64_t     tag   : AW-TOfst;
  constexpr static uint64_t mask = (1ull << (AW-TOfst)) - 1;
  OutMT outer_meta; // maintain a copy of metadata for hierarchical coherence support
                    // this outer metadata is responsible only to record the S/M/E/O state seen by the outer
                    // whether the block is dirty, shared by inner caches, and directory, etc. are hold by the metadata

public:
  MetadataMixer() : tag(0) {}
  virtual ~MetadataMixer() {}

  virtual CMMetadataBase * get_outer_meta() { return &outer_meta; }
  virtual const CMMetadataBase * get_outer_meta() const { return &outer_meta; }

  virtual bool match(uint64_t addr) const { return MT::is_valid() && ((addr >> TOfst) & mask) == tag; }
  virtual void reset() { tag = 0; MT::state = 0; MT::dirty = 0; }
  virtual void init(uint64_t addr) { tag = (addr >> TOfst) & mask; MT::state = 0; MT::dirty = 0; }
  virtual uint64_t addr(uint32_t s) const {
    uint64_t addr = tag << TOfst;
    if constexpr (IW > 0) {
      constexpr uint32_t index_mask = (1 << IW) - 1;
      addr |= (s & index_mask) << (TOfst - IW);
    }
    return addr;
  }
  virtual void sync(int32_t coh_id) {}

  virtual void to_invalid() { MT::to_invalid(); outer_meta.to_invalid(); }
  virtual void to_dirty() { outer_meta.to_dirty(); } // directly use the outer meta so the dirty state is release to outer when evicted
  virtual void to_clean() { outer_meta.to_clean(); }
  virtual bool is_dirty() const { return outer_meta.is_dirty(); }
  virtual bool allow_write() const {return outer_meta.allow_write(); }

  virtual void copy(const CMMetadataBase *m_meta) {
    // ATTN! tag is not coped.
    MT::copy(m_meta);
    outer_meta.copy(m_meta->get_outer_meta());
  }
};

template <int AW, int IW, int TOfst, typename MT, typename OutMT = MetadataBroadcastBase>
  requires !C_DERIVE(MT, MetadataDirectoryBase)
using MetadataBroadcast = MetadataMixer<AW, IW, TOfst, MT, OutMT>;

template <int AW, int IW, int TOfst, typename MT, typename OutMT = MetadataBroadcastBase>
  requires C_DERIVE(MT, MetadataDirectoryBase)
using MetadataDirectory = MetadataMixer<AW, IW, TOfst, MT, OutMT>;

#endif
