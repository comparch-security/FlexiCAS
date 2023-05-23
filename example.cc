// This is an example cache system definition file that can be automatically compiled into a cache implementation

const AddrWidth = 48;    // 48b addr
const BlockOffset = 6;   // 64B cache block
const L1IW = 6;          // L1 64 sets
const L1WN = 8;          // L1 8 ways
const L1TagOffset = 12;
const LLCIW = 10;        // LLC 1024 sets
const LLCWN = 16;        // LLC 16 ways
const LLCTagOffset = 6;  // random index
const LLCPartitionN = 2; // LLC skew partitions

// optimal options
const EnableMonitor = 0; // disable

// initiate the L1 cache
type data_type        = Data64B();
type l1_metadata_type = MetadataMSI(AddrWidth, L1IW, L1TagOffset);
type l1_indexer_type  = IndexNorm(L1IW, BlockOffset);
type l1_replacer_type = ReplaceLRU(L1IW, L1WN);
type l1_type          = CacheNorm(L1IW, L1WN, l1_metadata_type, data_type, l1_indexer_type, l1_replacer_type, EnableMonitor);
type l1_inner_type    = CoreInterfaceMSI(l1_metadata_type, data_type, false);
type l1_outer_type    = OuterPortMSI(l1_metadata_type, data_type);     // support reverse probe
type l1_cache_type    = CoherentL1CacheNorm(l1_type, l1_outer_type, l1_inner_type);

create l1 = l1_cache_type(2); // 2 L1 caches


// initiate the llc
type llc_metadata_type = MetadataMSI(AddrWidth, 0, LLCTagOffset);
type llc_indexer_type  = IndexSkewed(LLCIW, BlockOffset, LLCPartitionN);
type llc_replacer_type = ReplaceLRU(LLCIW, LLCWN);
type llc_type          = CacheSkewed(LLCIW, LLCWN, LLCPartitionN, llc_metadata_type, data_type, llc_indexer_type, llc_replacer_type, EnableMonitor);
type llc_inner_type    = InnerPortMSIBroadcast(llc_metadata_type, data_type, true);
type llc_outer_type    = OuterPortMSIUncached(llc_metadata_type, data_type);
type llc_cache_type    = CoherentCacheNorm(llc_type, llc_outer_type, llc_inner_type);

create llc = llc_cache_type(1); // shared llc

//static SimpleMemoryModel<data_type> *mem = new SimpleMemoryModel<data_type>();

// connect the two levels
//l1->outer->connect(llc->inner, llc->inner->connect(l1->outer));
//llc->outer->connect(mem, mem->connect(llc->outer));

// attach PFC
//auto pfc = new PFCMonitor();
//llc->attach_monitor(pfc);
