#include "config.hpp"
#include "cache/memory.hpp"
#include "cache/msi.hpp"

typedef Data64B data_type;
typedef MetadataMSI<WIDTH,L1_INDEX,L1_TAG_OFFSET,MetadataMSISupport> l1_metadata_type;
typedef IndexNorm<L1_INDEX,OFFSET> l1_indexer_type;
typedef ReplaceLRU<L1_INDEX,L1_WAY> l1_replacer_type;
typedef void l1_delay_type;
typedef CacheNorm<L1_INDEX,L1_WAY,l1_metadata_type,data_type,l1_indexer_type,l1_replacer_type,l1_delay_type,false> l1_type;
typedef MSIPolicy<l1_metadata_type, true, false> l1_policy;
typedef CoreInterface l1_inner_type;
typedef OuterCohPort l1_outer_type;
typedef CoherentL1CacheNorm<l1_type, l1_outer_type, l1_inner_type> l1_cache_type;


typedef ReplaceLRU<L2_INDEX,L2_WAY> l2_replacer_type;
typedef void l2_delay_type;
#ifdef LEVEL_TWO_INCLUSIVE
  typedef OuterCohPort l2_outer_type;
  typedef InnerCohPort l2_inner_type;
  #ifdef LEVEL_TWO_SKEWED
    #ifdef LEVEL_TWO_ENABLEDIRECTORY
      typedef MetadataMSIDirectory<WIDTH,0,OFFSET,MetadataMSIDirectorySupport> l2_metadata_type;
    #else
      typedef MetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> l2_metadata_type;
    #endif
    typedef IndexSkewed<L2_INDEX,OFFSET,L2_SKEW> l2_indexer_type;
    typedef CacheSkewed<L2_INDEX,L2_WAY,L2_SKEW,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_delay_type,false> l2_type;
  #else
    #ifdef LEVEL_TWO_ENABLEDIRECTORY
      typedef MetadataMSIDirectory<WIDTH,L2_INDEX,L2_TAG_OFFSET,MetadataMSIDirectorySupport> l2_metadata_type;
    #else
      typedef MetadataMSI<WIDTH,L2_INDEX,L2_TAG_OFFSET,MetadataMSISupport> l2_metadata_type;
    #endif
    typedef IndexNorm<L2_INDEX,OFFSET> l2_indexer_type;
    typedef CacheNorm<L2_INDEX,L2_WAY,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_delay_type,false> l2_type;
  #endif
  #ifdef TWO_LEVEL_CACHE
    typedef MSIPolicy<l2_metadata_type, false, true> l2_policy;
  #else
    typedef MSIPolicy<l2_metadata_type, false, false> l2_policy;
  #endif
#elif defined(LEVEL_TWO_EXCLUSIVE)
  typedef ExclusiveOuterPortMSI l2_outer_type;
  typedef ExclusiveInnerPortMSI l2_inner_type;
  typedef ReplaceLRU<L2_INDEX,L2_EXTEND_WAY> l2_d_replacer_type;
  #ifdef LEVEL_TWO_SKEWED
    typedef IndexSkewed<L2_INDEX,OFFSET,L2_SKEW> l2_indexer_type;
    #ifdef LEVEL_TWO_ENABLEDIRECTORY
      typedef MetadataMSIDirectory<WIDTH,0,OFFSET,MetadataMSIDirectorySupport> l2_metadata_type;
      typedef CacheSkewedExclusive<L2_INDEX,L2_WAY,L2_EXTEND_WAY,L2_SKEW,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_d_replacer_type,l2_delay_type,false,true> l2_type;
    #else
      typedef MetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> l2_metadata_type;
      typedef CacheSkewedExclusive<L2_INDEX,L2_WAY,0,L2_SKEW,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_d_replacer_type,l2_delay_type,false,false> l2_type;
    #endif
  #ifdef TWO_LEVEL_CACHE
    typedef ExclusiveMSIPolicy<l2_metadata_type, true> l2_policy;
  #else
    typedef ExclusiveMSIPolicy<l2_metadata_type, false> l2_policy;
  #endif
  #else
    typedef IndexNorm<L2_INDEX,OFFSET> l2_indexer_type;
    #ifdef LEVEL_TWO_ENABLEDIRECTORY
      typedef MetadataMSIDirectory<WIDTH,L2_INDEX,L2_TAG_OFFSET,MetadataMSIDirectorySupport> l2_metadata_type;
      typedef CacheNormExclusive<L2_INDEX,L2_WAY,L2_EXTEND_WAY,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_d_replacer_type,l2_delay_type,false,true> l2_type;
    #else
      typedef MetadataMSI<WIDTH,L2_INDEX,L2_TAG_OFFSET,MetadataMSISupport> l2_metadata_type;
      typedef CacheNormExclusive<L2_INDEX,L2_WAY,0,l2_metadata_type,data_type,l2_indexer_type,l2_replacer_type,l2_d_replacer_type,l2_delay_type,false,false> l2_type;
    #endif
    #ifdef TWO_LEVEL_CACHE
      typedef ExclusiveMSIPolicy<l2_metadata_type, true> l2_policy;
    #else
      typedef ExclusiveMSIPolicy<l2_metadata_type, false> l2_policy;
    #endif
  #endif
#else
  typedef MirageDataMeta l2_datameta_type;
  typedef MirageMetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> l2_metadata_type;
  typedef IndexSkewed<L2_INDEX,OFFSET,L2_SKEW> l2_m_indexer_type;
  typedef IndexRandom<L2_INDEX,OFFSET> l2_d_indexer_type;
  typedef ReplaceLRU<L2_INDEX,L2_WAY+L2_M_EXTEND_WAY> l2_m_replacer_type;
  typedef ReplaceCompleteRandom<L2_INDEX,L2_SKEW*L2_WAY> l2_d_replacer_type;
  typedef MirageCache<L2_INDEX,L2_WAY,L2_M_EXTEND_WAY,L2_SKEW,L2_MAX_RElocN,l2_metadata_type,data_type,l2_datameta_type,l2_m_indexer_type,l2_d_indexer_type,l2_m_replacer_type,l2_d_replacer_type,l2_delay_type,false,L2_EN_RelocN> l2_type;
  typedef MirageMSIPolicy<l2_metadata_type, l2_type> l2_policy;
  typedef MirageInnerPort<l2_metadata_type, l2_type> l2_inner_type;
  typedef MirageOuterPort l2_outer_type;
#endif
typedef CoherentCacheNorm<l2_type, l2_outer_type, l2_inner_type> l2_cache_type;

#ifdef THREE_LEVEL_CACHE
  typedef ReplaceLRU<LLC_INDEX,LLC_WAY> llc_replacer_type;
  typedef DelayCoherentCache<8,20,40> llc_delay_type;
  typedef SliceHashIntelCAS<2> llc_hash_func;
  typedef SliceDispatcher<llc_hash_func> llc_dispatcher_type;
  #ifdef LEVEL_THREE_INCLUSIVE
    typedef OuterCohPort llc_outer_type;
    typedef InnerCohPort llc_inner_type;
    #ifdef LEVEL_THREE_SKEWED
      #ifdef LEVEL_THREE_ENABLEDIRECTORY
        typedef MetadataMSIDirectory<WIDTH,0,OFFSET,MetadataMSIDirectorySupport> llc_metadata_type;
      #else
        typedef MetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> llc_metadata_type;
      #endif
      typedef IndexSkewed<LLC_INDEX,OFFSET,LLC_SKEW> llc_indexer_type;
      typedef CacheSkewed<LLC_INDEX,LLC_WAY,LLC_SKEW,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_delay_type,true> llc_type;
    #else
      typedef MetadataMSI<WIDTH,LLC_INDEX,OFFSET,MetadataMSISupport> llc_metadata_type;
      typedef IndexNorm<LLC_INDEX,OFFSET> llc_indexer_type;
      typedef CacheNorm<LLC_INDEX,LLC_WAY,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_delay_type,true> llc_type;
    #endif
    typedef MSIPolicy<llc_metadata_type, false, true> llc_policy;
  #elif defined(LEVEL_THREE_EXCLUSIVE)
    typedef ExclusiveOuterPortMSI llc_outer_type;
    typedef ExclusiveInnerPortMSI llc_inner_type;
    typedef ReplaceRandom<LLC_INDEX,LLC_EXTEND_WAY> llc_d_replacer_type;
    #ifdef LEVEL_THREE_SKEWED
      typedef IndexSkewed<LLC_INDEX,OFFSET,LLC_SKEW> llc_indexer_type;
      #ifdef LEVEL_THREE_ENABLEDIRECTORY
        typedef MetadataMSIDirectory<WIDTH,0,OFFSET,MetadataMSIDirectorySupport> llc_metadata_type;
        typedef CacheSkewedExclusive<LLC_INDEX,LLC_WAY,LLC_EXTEND_WAY,LLC_SKEW,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_d_replacer_type,llc_delay_type,true,true> llc_type;
      #else
        typedef MetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> llc_metadata_type;
        typedef CacheSkewedExclusive<LLC_INDEX,LLC_WAY,0,LLC_SKEW,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_d_replacer_type,llc_delay_type,true,false> llc_type;
      #endif
    #else
      typedef IndexNorm<LLC_INDEX,OFFSET> llc_indexer_type;
      #ifdef LEVEL_THREE_ENABLEDIRECTORY
        typedef MetadataMSIDirectory<WIDTH,LLC_INDEX,LLC_TAG_OFFSET,MetadataMSIDirectorySupport> llc_metadata_type;
        typedef CacheNormExclusive<LLC_INDEX,LLC_WAY,LLC_EXTEND_WAY,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_d_replacer_type,llc_delay_type,true,true> llc_type;
      #else
        typedef MetadataMSI<WIDTH,LLC_INDEX,LLC_TAG_OFFSET,MetadataMSISupport> llc_metadata_type;
        typedef CacheNormExclusive<LLC_INDEX,LLC_WAY,0,llc_metadata_type,data_type,llc_indexer_type,llc_replacer_type,llc_d_replacer_type,llc_delay_type,true,false> llc_type;
      #endif
    #endif
    typedef ExclusiveMSIPolicy<llc_metadata_type, true> llc_policy; 
  #else
    typedef MirageDataMeta llc_datameta_type;
    typedef MirageMetadataMSI<WIDTH,0,OFFSET,MetadataMSISupport> llc_metadata_type;
    typedef IndexSkewed<LLC_INDEX,OFFSET,LLC_SKEW> llc_m_indexer_type;
    typedef IndexRandom<LLC_INDEX,OFFSET> llc_d_indexer_type;
    typedef ReplaceLRU<LLC_INDEX,LLC_WAY+LLC_M_EXTEND_WAY> llc_m_replacer_type;
    typedef ReplaceCompleteRandom<LLC_INDEX,LLC_SKEW*LLC_WAY> llc_d_replacer_type;
    typedef MirageCache<LLC_INDEX,LLC_WAY,LLC_M_EXTEND_WAY,LLC_SKEW,LLC_MAX_RElocN,llc_metadata_type,data_type,llc_datameta_type,llc_m_indexer_type,llc_d_indexer_type,llc_m_replacer_type,llc_d_replacer_type,llc_delay_type,true,true> llc_type;
    typedef MirageMSIPolicy<llc_metadata_type, llc_type> llc_policy;
    typedef MirageInnerPort<llc_metadata_type, llc_type> llc_inner_type;
    typedef MirageOuterPort llc_outer_type;
  #endif
  typedef CoherentCacheNorm<llc_type, llc_outer_type, llc_inner_type> llc_cache_type;
#endif

typedef DelayMemory<100> memory_delay_type;
typedef SimpleMemoryModel<data_type,memory_delay_type> memory_type;

