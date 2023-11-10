#ifndef CM_CACHE_METADATA_HPP
#define CM_CACHE_METADATA_HPP

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

// coherence related support classes

class MetadataCoherenceSupport // handle extra functions needed by coherence
{
public:
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; }
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return target_id != request_id; } 
};

typedef MetadataCoherenceSupport MetadataBroadcast;

// TODO : support owner
class  MetadataDirectorySupportBase
{
protected:
  uint64_t sharer = 0;
  virtual void add_sharer(int32_t coh_id) = 0;
  virtual void clean_sharer() = 0;
  virtual void delete_sharer(int32_t coh_id) = 0;
  virtual bool is_sharer(int32_t coh_id) const = 0;
public:
  virtual uint64_t get_sharer()= 0;
  virtual void set_sharer(uint64_t c_sharer) = 0;
  MetadataDirectorySupportBase() : sharer(0) {}
  virtual ~MetadataDirectorySupportBase() {}

};

class MetadataDirectorySupport : public MetadataCoherenceSupport, public MetadataDirectorySupportBase
{
protected:
  virtual void add_sharer(int32_t coh_id) {
    sharer |= (1ull << coh_id);
  }
  virtual void clean_sharer(){
    sharer = 0;
  }
  virtual void delete_sharer(int32_t coh_id){
    sharer &= ~(1ull << coh_id);
  }
  virtual bool is_sharer(int32_t coh_id) const {
    return ((1ull << coh_id) & (sharer))!= 0;
  }
public:
  virtual uint64_t get_sharer(){
    return sharer;
  }
  virtual void set_sharer(uint64_t c_sharer){
    sharer = c_sharer;
  }
  virtual bool evict_need_probe(int32_t target_id, int32_t request_id) const { return is_sharer(target_id) && (target_id != request_id);}
  virtual bool writeback_need_probe(int32_t target_id, int32_t request_id) const { return is_sharer(target_id) && (target_id != request_id);} 
};

// Metadata Mixer
// AW    : address width
// IW    : index width
// TOfst : tag offset
// MT    : metadata type
// ST    : coherence support
template <int AW, int IW, int TOfst, typename MT, typename ST, typename OutMT>
  requires C_DERIVE(MT, CMMetadataBase) && C_DERIVE(ST, MetadataCoherenceSupport) && C_DERIVE(OutMT, CMMetadataBase)
class MetadataMixer : public MT, public ST
{
protected:
  uint64_t     tag   : AW-TOfst;
  constexpr static uint64_t mask = (1ull << (AW-TOfst)) - 1;

public:
  OutMT meta_outer; // maintain a copy of metadata for hierarchical coherence support
  MetadataMixer() : tag(0) {}
  virtual ~MetadataMixer() {}

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

  virtual void copy(const CMMetadataBase *m_meta) {
    MT::copy(m_meta);
    auto meta = static_cast<const MetadataMixer *>(m_meta);
    tag = meta->tag;
  }
};


#endif
