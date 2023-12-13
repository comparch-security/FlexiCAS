#ifndef CM_UTIL_CACHE_TYPE_HPP
#define CM_UTIL_CACHE_TYPE_HPP

// a header for ease the construction of cache types

#include "cache/exclusive.hpp"
#include "cache/mirage.hpp"
#include "cache/mesi.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"

#include <vector>

template<typename CT, typename CPT>
inline std::vector<CoherentCacheBase *> cache_generator(int size, const std::string& name_prefix) {
  auto policy = new CPT();
  auto array = std::vector<CoherentCacheBase *>(size);
  for(int i=0; i<size; i++) array[i] = new CT(policy, name_prefix + (size > 1 ? "-"+std::to_string(i) : ""));
  return array;
}

inline auto get_l1_core_interface(std::vector<CoherentCacheBase *>& array) {
  auto core = std::vector<CoreInterface *>(array.size());
  for(unsigned int i=0; i<array.size(); i++)
    core[i] = static_cast<CoreInterface *>(array[i]->inner);
  return core;
}

template<int IW, int WN, int DW, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <int, int, bool> class DRPT,
         template <typename, bool, bool> class CPT,
         bool isL1, bool isLLC, bool uncache, bool isExc, typename DLY, bool EnMon>
inline auto cache_type_compile(int size, const std::string& name_prefix) {
  typedef IndexNorm<IW,6> index_type;
  typedef RPT<IW,WN,true> replace_type;
  typedef DRPT<IW,DW,true> ext_replace_type;

  constexpr bool isDir  = std::is_same_v<MBT, MetadataDirectoryBase>;
  constexpr bool isMESI = std::is_same_v<CPT<MetadataDirectoryBase, false, true>, MESIPolicy<MetadataDirectoryBase, false, true> >;
  constexpr bool isMSI  = std::is_same_v<CPT<MetadataDirectoryBase, false, true>, MSIPolicy<MetadataDirectoryBase, false, true> >;
  constexpr bool isMI   = std::is_same_v<CPT<MetadataDirectoryBase, false, true>, MIPolicy<MetadataDirectoryBase, false, true> >;

  // ports
  typedef typename std::conditional<isL1, CoreInterface,
                                    typename std::conditional<isExc,
                                      typename std::conditional<isDir, ExclusiveInnerCohPortDirectory, ExclusiveInnerCohPortBroadcast>::type,
                                      InnerCohPort>::type >::type input_type;
  typedef typename std::conditional<isLLC || uncache, OuterCohPortUncached,
                                    typename std::conditional<isExc,
                                      typename std::conditional<isDir, ExclusiveOuterCohPortDirectory, ExclusiveOuterCohPortBroadcast>::type,
                                      OuterCohPort>::type >::type output_type;

  // MESI
  typedef MetadataMESIDirectory<48, IW, IW+6> mesi_metadata_type;
  typedef typename std::conditional<isExc,
                                    ExclusiveMESIPolicy<mesi_metadata_type, true, isLLC>,
                                    MESIPolicy<mesi_metadata_type, false, isLLC>
                                    >::type mesi_policy_type;
  if constexpr (isMESI && isExc) assert(isDir);

  // MSI
  typedef typename std::conditional<isDir, MetadataMSIDirectory<48, IW, IW+6>, MetadataMSIBroadcast<48, IW, IW+6> >::type msi_metadata_type;
  typedef typename std::conditional<isExc,
                                    ExclusiveMSIPolicy<msi_metadata_type, isDir, isLLC>,
                                    MSIPolicy<msi_metadata_type, false, isLLC>
                                    >::type msi_policy_type;

  // MI
  typedef MetadataMIBroadcast<48, IW, IW+6> mi_metadata_type;
  typedef MIPolicy<mi_metadata_type, true, isLLC> mi_policy_type;
  if constexpr (isMI) assert(!isDir && !isExc);

  typedef typename std::conditional<isMESI, mesi_metadata_type,
          typename std::conditional<isMSI,  msi_metadata_type,
                                            mi_metadata_type>::type >::type metadata_type;

  typedef typename std::conditional<isMESI, mesi_policy_type,
          typename std::conditional<isMSI,  msi_policy_type,
                                            mi_policy_type>::type >::type policy_type;

  typedef typename std::conditional<
    isExc, typename std::conditional<
             isDir, CacheNormExclusiveDirectory<IW, WN, DW, metadata_type, DT, index_type, replace_type, ext_replace_type, DLY, EnMon>,
                    CacheNormExclusiveBroadcast<IW, WN,     metadata_type, DT, index_type, replace_type,                   DLY, EnMon> >::type,
           CacheNorm<IW, WN, metadata_type, DT, index_type, replace_type, DLY, EnMon> >::type cache_base_type;

  typedef CoherentCacheNorm<cache_base_type, output_type, input_type> cache_type;
  return cache_generator<cache_type, policy_type>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <typename, bool, bool> class CPT,
         bool isLLC, bool uncache, typename DLY, bool EnMon>
inline auto cache_gen_l1(int size, const std::string& name_prefix) {
  return cache_type_compile<IW, WN, 1, DT, MBT, RPT, ReplaceLRU, CPT, true, isLLC, uncache, false, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <typename, bool, bool> class CPT,
         bool isLLC, typename DLY, bool EnMon>
inline auto cache_gen_l2_inc(int size, const std::string& name_prefix) {
  return cache_type_compile<IW, WN, 1, DT, MBT, RPT, ReplaceLRU, CPT, false, isLLC, false, false, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <typename, bool, bool> class CPT,
         bool isLLC, typename DLY, bool EnMon>
inline auto cache_gen_l2_exc(int size, const std::string& name_prefix) {
  return cache_type_compile<IW, WN, 1,  DT, MBT, RPT, ReplaceLRU, CPT, false, isLLC, false, true, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, int DW, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <int, int, bool> class DRPT,
         template <typename, bool, bool> class CPT,
         bool isLLC, typename DLY, bool EnMon>
inline auto cache_gen_l2_exc(int size, const std::string& name_prefix) {
  constexpr bool isDir  = std::is_same_v<MBT, MetadataDirectoryBase>;
  assert(isDir);
  return cache_type_compile<IW, WN, DW, DT, MBT, RPT, DRPT,       CPT, false, isLLC, false, true, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <typename, bool, bool> class CPT,
         typename DLY, bool EnMon>
inline auto cache_gen_llc_inc(int size, const std::string& name_prefix) {
  return cache_type_compile<IW, WN, 1, DT, MBT, RPT, ReplaceLRU, CPT, false, true, false, false, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <typename, bool, bool> class CPT,
         typename DLY, bool EnMon>
inline auto cache_gen_llc_exc(int size, const std::string& name_prefix) {
  return cache_type_compile<IW, WN, 1,  DT, MBT, RPT, ReplaceLRU, CPT, false, true, false, true, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, int DW, typename DT, typename MBT,
         template <int, int, bool> class RPT,
         template <int, int, bool> class DRPT,
         template <typename, bool, bool> class CPT,
         typename DLY, bool EnMon>
inline auto cache_gen_llc_exc(int size, const std::string& name_prefix) {
  constexpr bool isDir  = std::is_same_v<MBT, MetadataDirectoryBase>;
  assert(isDir);
  return cache_type_compile<IW, WN, DW, DT, MBT, RPT, DRPT,       CPT, false, true, false, true, DLY, EnMon>(size, name_prefix);
}

template<int IW, int WN, int EW, int P, int MaxRelocN, typename DT,
         template <int, int, bool> class MRPT,
         template <int, int, bool> class DRPT,
         typename DLY, bool EnMon, bool EnableRelocation>
inline auto cache_gen_llc_mirage(int size, const std::string& name_prefix) {
  typedef MirageMetadataMSIBroadcast<48,0,6> meta_metadata_type;
  typedef MirageDataMeta data_metadata_type;
  typedef IndexSkewed<IW, 6, P> meta_index_type;
  typedef IndexRandom<IW, 6> data_index_type;
  typedef MRPT<IW, WN, true> meta_replace_type;
  typedef DRPT<IW, WN*P, true> data_replace_type;

  typedef MirageCache<IW, WN, EW, P, MaxRelocN,
                      meta_metadata_type, DT, data_metadata_type,
                      meta_index_type, data_index_type,
                      meta_replace_type, data_replace_type,
                      DLY, EnMon, EnableRelocation> cache_base_type;
  typedef MSIPolicy<meta_metadata_type, false, true> policy_type;
  typedef CoherentCacheNorm<cache_base_type, OuterCohPortUncached, MirageInnerCohPort<meta_metadata_type, cache_base_type> > cache_type;
  return cache_generator<cache_type, policy_type>(size, name_prefix);
}

#endif
