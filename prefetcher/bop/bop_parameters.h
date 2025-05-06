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
  # define BAD_SCORE            (1)   // Score at which the HWP is disabled
  # define RR_SIZE              (256) // Number of entries in RR Table
  # define TAG_BITS             (12)  // Bits used to store the tag
  # define OFFSET_LIST_SIZE     (26)  // Number of entries in the offsets list

  // Toggle to include negative offsets in the offset list
  // If enabled, half of the offset list will contain negative offsets.     
  #define NEGATIVE_OFFSETS_ENABLE (false) 
};

# endif // _BOP_PARAMETERS_H_
