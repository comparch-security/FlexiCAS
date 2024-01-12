////////////////////////////////////////////////////////////////////////////////
// CONFIG TEST DIFFICULTY
// #define LEVEL_1
// #define LEVEL_2
// #define LEVEL_3
#define LEVEL_4
// #define LEVEL_5



////////////////////////////////////////////////////////////////////////////////
// CONFIG CACHE LEVEL
#if defined(LEVEL_1) || defined(LEVEL_2)
  #define TWO_LEVEL_CACHE
#else
  #define THREE_LEVEL_CACHE
#endif

////////////////////////////////////////////////////////////////////////////////
// CONFIG EVERY LEVEL CACHE
#define OFFSET 6
#define WIDTH 48

// FIRST LEVEL
#define L1_INDEX 2
#define L1_WAY   2
#define L1_TAG_OFFSET L1_INDEX + OFFSET

// SECOND LEVEL
#define L2_INDEX 3
#define L2_WAY   4

#define LEVEL_TWO_INCLUSIVE
// #define LEVEL_TWO_EXCLUSIVE
// #define LEVEL_TWO_MIRAGE
#if defined(LEVEL_TWO_EXCLUSIVE) || defined(LEVEL_TWO_INCLUSIVE)
    // #define LEVEL_TWO_ENABLEDIRECTORY
  #if defined(LEVEL_TWO_EXCLUSIVE) 
    #define L2_EXTEND_WAY 4
  #endif
#endif

#ifndef LEVEL_TWO_MIRAGE
  // #define LEVEL_TWO_SKEWED
  #define LEVEL_TWO_NORM
#endif

#if defined(LEVEL_TWO_MIRAGE) || defined(LEVEL_TWO_SKEWED)
  #define L2_SKEW 2
  #if defined(LEVEL_TWO_MIRAGE)
    #define L2_M_EXTEND_WAY 6
    #define L2_EN_RelocN 1
    #define L2_MAX_RElocN 1
  #endif
#elif defined(LEVEL_TWO_NORM)
  #define L2_TAG_OFFSET L2_INDEX + OFFSET
#endif



// THIRD LEVEL (IF CONFIG THREE_LEVEL_CACHE)
#define LLC_INDEX 4
#define LLC_WAY   4

#ifdef THREE_LEVEL_CACHE
  #define LEVEL_THREE_INCLUSIVE
  // #define LEVEL_THREE_EXCLUSIVE 
  // #define LEVEL_THREE_MIRAGE

  #if defined(LEVEL_THREE_EXCLUSIVE) || defined(LEVEL_THREE_INCLUSIVE)
      // #define LEVEL_THREE_ENABLEDIRECTORY
    #if defined(LEVEL_THREE_EXCLUSIVE)
      #define LLC_EXTEND_WAY 1
    #endif
  #endif

  #ifndef LEVEL_THREE_MIRAGE
    // #define LEVEL_THREE_SKEWED
    #define LEVEL_THREE_NORM
  #endif

  #if defined(LEVEL_THREE_MIRAGE) || defined(LEVEL_THREE_SKEWED)
    #define LLC_SKEW 2
    #if defined(LEVEL_THREE_MIRAGE)
      #define LLC_M_EXTEND_WAY 1
      #define LLC_EN_RelocN 0
      #define LLC_MAX_RElocN 1
    #endif
  #elif defined(LEVEL_THREE_NORM)
    #define LLC_TAG_OFFSET LLC_INDEX + OFFSET
  #endif

#endif

////////////////////////////////////////////////////////////////////////////////
// CONFIG RANDOM SEED
#define RANDOM_SEED

