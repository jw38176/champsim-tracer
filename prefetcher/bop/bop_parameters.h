# ifndef _BOP_PARAMETERS_H_
# define _BOP_PARAMETERS_H_

namespace bop_space
{ 
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // BOP
  # define SCORE_MAX            (31)  // Max. score to update the best offset
  # define ROUND_MAX            (100) // Max. round to update the best offset
  # define BAD_SCORE            (10)  // Score at which the HWP is disabled
  # define RR_SIZE              (64)  // Number of entries in RR Table
  # define TAG_BITS             (12)  // Bits used to store the tag
  # define OFFSET_LIST_SIZE     (46)  // Number of entries in the offsets list

  # define NEGATIVE_OFFSETS_ENABLE    (false) // Initialize the offsets list also with negative values \
                                              (i.e. the table will have half of the entries with positive \
                                              offsets and the other half with negative ones)
};
# endif
