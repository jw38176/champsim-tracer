# ifndef _KAIROS_PARAMETERS_H_
# define _KAIROS_PARAMETERS_H_

namespace kairos_space
{ 
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // KAIROS
  # define SCORE_MAX            (31)  // Max. score to update the best offset
  # define ROUND_MAX            (100) // Max. round to update the best offset
  # define BAD_SCORE            (10)  // Score at which the HWP is disabled
  # define RR_SIZE              (64)  // Number of entries in RR Table
  # define TAG_BITS             (12)  // Bits used to store the tag
  # define OFFSET_LIST_SIZE     (26)  // Number of entries in the offsets list
  # define NUM_OFFSETS          (1)   // The number of offsets that are learnt

  constexpr std::size_t PREFETCH_TABLE_SIZE = 128; // Size of the recent prefetches table
};
# endif