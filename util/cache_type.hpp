#ifndef CM_UTIL_CACHE_TYPE_HPP
#define CM_UTIL_CACHE_TYPE_HPP

// a header for ease the construction of cache types

#include "cache/exclusive.hpp"
#include "cache/mirage.hpp"
#include "cache/mesi.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/memory.hpp"

namespace ct {
  template<typename MT>
  constexpr bool is_dir() { return std::is_same_v<MT, MetadataDirectoryBase>; }

  template<template <bool, bool, typename> class CPT>
  constexpr bool is_inc_mi() {
    return std::is_same_v<CPT<false, true, CohPolicyBase>, MIPolicy<false, true, CohPolicyBase> >;
  }

  template<template <bool, bool, typename> class CPT>
  constexpr bool is_inc_msi() {
    return std::is_same_v<CPT<false, true, CohPolicyBase>, MSIPolicy<false, true, CohPolicyBase> >;
  }

  template<template <bool, bool, typename> class CPT>
  constexpr bool is_inc_mesi() {
    return std::is_same_v<CPT<false, true, CohPolicyBase>, MESIPolicy<false, true, CohPolicyBase> >;
  }

  template<template <bool, bool, typename> class CPT>
  constexpr bool is_exc_msi() {
    return std::is_same_v<CPT<false, true, CohPolicyBase>, ExclusiveMSIPolicy<false, true, CohPolicyBase> >;
  }

  template<template <bool, bool, typename> class CPT>
  constexpr bool is_exc_mesi() {
    return std::is_same_v<CPT<false, true, CohPolicyBase>, ExclusiveMESIPolicy<false, true, CohPolicyBase> >;
  }

  template<bool isDir, typename MTDir, typename MTBCast>
  using metadata_sel_dir = std::conditional_t<isDir, MTDir, MTBCast>;

  template<template <bool, bool, typename> class CPT, typename MT, int IW>
  using metadata_type =
    std::conditional_t<is_inc_mi<CPT>(), MetadataMIBroadcast<48, IW, IW+6>,
    std::conditional_t<is_inc_msi<CPT>() || is_exc_msi<CPT>(), metadata_sel_dir<is_dir<MT>(), MetadataMSIDirectory<48, IW, IW+6>, MetadataMSIBroadcast<48, IW, IW+6> >,
    std::conditional_t<is_inc_mesi<CPT>() || is_exc_mesi<CPT>(), MetadataMESIDirectory<48, IW, IW+6>,
                       void> > >;

  template<typename Policy, bool isDir, bool EnMT>
  using input_port_exc = std::conditional_t<isDir, ExclusiveInnerCohPortDirectory<Policy, EnMT>,
                                            ExclusiveInnerCohPortBroadcast<Policy, EnMT> >;

  template<typename Policy, bool isL1, bool isDir, bool isExc, bool EnMT>
  using input_port_type =
    std::conditional_t<isL1, CoreInterface<Policy, EnMT>,
    std::conditional_t<isExc, input_port_exc<Policy, isDir, EnMT>,
                       InnerCohPort<Policy, EnMT> > >;

  template<typename Policy, bool isDir, bool EnMT>
  using output_port_exc = std::conditional_t<isDir, ExclusiveOuterCohPortDirectory<Policy, EnMT>,
                                             ExclusiveOuterCohPortBroadcast<Policy, EnMT> >;

  template<typename Policy, bool uncached, bool isDir, bool isExc, bool EnMT>
  using output_port_type =
    std::conditional_t<uncached, OuterCohPortUncached<Policy, EnMT>,
    std::conditional_t<isExc, output_port_exc<Policy, isDir, EnMT>,
                       OuterCohPort<Policy, EnMT> > >;
}

template<typename CT>
inline std::vector<CoherentCacheBase *> cache_generator(int size, const std::string& name_prefix) {
  auto array = std::vector<CoherentCacheBase *>(size);
  for(int i=0; i<size; i++) array[i] = new CT(name_prefix + (size > 1 ? "-"+std::to_string(i) : ""));
  return array;
}

inline auto get_l1_core_interface(std::vector<CoherentCacheBase *>& array) {
  auto core = std::vector<CoreInterfaceBase *>(array.size());
  for(unsigned int i=0; i<array.size(); i++)
    core[i] = dynamic_cast<CoreInterfaceBase *>(array[i]->inner);
  return core;
}

template<int IW, int WN, int DW, typename DT, typename MT,
         template <int, int, bool> class RPT,
         template <int, int, bool> class DRPT,
         template <bool, bool, typename> class CPT, typename Policy,
         bool isL1, bool uncached, typename DLY, bool EnMon, bool EnMT>
inline auto cache_gen(int size, const std::string& name_prefix) {
  using index_type = IndexNorm<IW,6>;
  using replace_type = RPT<IW,WN,true>;
  using ext_replace_type = DRPT<IW,DW,true>;
  constexpr bool isDir = ct::is_dir<MT>();
  constexpr bool isExc = ct::is_exc_msi<CPT>() || ct::is_exc_mesi<CPT>();
  static_assert(!(isExc && EnMT), "multithread support ia not available for exclusive caches!");
  using metadata_type = ct::metadata_type<CPT, MT, IW>;
  using input_type = ct::input_port_type<Policy, isL1, isDir, isExc, EnMT>;
  using output_type = ct::output_port_type<Policy, uncached, isDir, isExc, EnMT>;
  using cache_base_type =
    std::conditional_t<isExc,
    std::conditional_t<isDir,
      CacheNormExclusiveDirectory<IW, WN, DW, metadata_type, DT, index_type, replace_type, ext_replace_type, DLY, EnMon>,
      CacheNormExclusiveBroadcast<IW, WN,     metadata_type, DT, index_type, replace_type,                   DLY, EnMon> >,
                        CacheNorm<IW, WN,     metadata_type, DT, index_type, replace_type,                   DLY, EnMon, EnMT> >;

  using cache_type = CoherentCacheNorm<cache_base_type, output_type, input_type>;
  return cache_generator<cache_type>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MT,
         template <int, int, bool> class RPT,
         template <bool, bool, typename> class CPT, typename Policy,
         bool uncached, typename DLY, bool EnMon, bool EnMT = false>
inline auto cache_gen_l1(int size, const std::string& name_prefix) {
  return cache_gen<IW, WN, 1, DT, MT, RPT, ReplaceLRU, CPT, Policy, true, uncached, DLY, EnMon, EnMT>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MT,
         template <int, int, bool> class RPT,
         template <bool, bool, typename> class CPT, typename Policy,
         bool uncached, typename DLY, bool EnMon, bool EnMT = false>
inline auto cache_gen_inc(int size, const std::string& name_prefix) {
  return cache_gen<IW, WN, 1, DT, MT, RPT, ReplaceLRU, CPT, Policy, false, uncached, DLY, EnMon, EnMT>(size, name_prefix);
}

template<int IW, int WN, typename DT, typename MT,
         template <int, int, bool> class RPT,
         template <bool, bool, typename> typename CPT, typename Policy,
         bool uncached, typename DLY, bool EnMon>
inline auto cache_gen_exc(int size, const std::string& name_prefix) {
  static_assert(ct::is_exc_msi<CPT>());
  return cache_gen<IW, WN, 1, DT, MT, RPT, ReplaceLRU, CPT, Policy, false, uncached, DLY, EnMon, false>(size, name_prefix);
}

template<int IW, int WN, int DW, typename DT, typename MT,
         template <int, int, bool> class RPT,
         template <int, int, bool> class DRPT,
         template <bool, bool, typename> class CPT, typename Policy,
         bool uncached, typename DLY, bool EnMon>
inline auto cache_gen_exc(int size, const std::string& name_prefix) {
  static_assert(ct::is_exc_mesi<CPT>() && ct::is_dir<MT>());
  return cache_gen<IW, WN, DW, DT, MT, RPT, DRPT, CPT, Policy, false, uncached, DLY, EnMon, false>(size, name_prefix);
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
  typedef CoherentCacheNorm<cache_base_type, OuterCohPortUncached<false>, MirageInnerCohPort<meta_metadata_type, cache_base_type, false> > cache_type;
  return cache_generator<cache_type, policy_type>(size, name_prefix);
}

#endif
