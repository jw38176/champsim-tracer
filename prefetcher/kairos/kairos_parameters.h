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
  # define OFFSET_LIST_SIZE     (46)  // Number of entries in the offsets list

  # define PREFETCH_TABLE_SETS  (512) // Number of sets in the recent prefetches table
  # define PREFETCH_TABLE_WAYS  (4)   // Number of ways in the recent prefetches table

  # define NEGATIVE_OFFSETS_ENABLE    (false) // Initialize the offsets list also with negative values \
                                              (i.e. the table will have half of the entries with positive \
                                              offsets and the other half with negative ones)
};
# endif