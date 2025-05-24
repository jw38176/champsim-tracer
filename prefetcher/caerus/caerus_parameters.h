# ifndef _CAERUS_PARAMETERS_H_
# define _CAERUS_PARAMETERS_H_

namespace caerus_space
{ 
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // CAERUS
  # define SCORE_MAX            (31)  // Max. score to update the best offset
  # define ROUND_MAX            (100) // Max. round to update the best offset
  # define RR_SIZE              (256) // Number of entries in RR Table
  # define TAG_BITS             (12)  // Bits used to store the tag
  # define OFFSET_LIST_SIZE     (26)  // Number of entries in the offsets list
  # define NUM_OFFSETS          (4)   // The number of offsets that are learnt
  # define ACCURACY_THRESHOLD   (0)   // Saturing counter value for pc/offset to be accurate

  constexpr std::size_t HOLDING_TABLE_SIZE = 128;  // Size of the holding table
  constexpr std::size_t ACCURACY_TABLE_SIZE = 128; // Size of the pc accuracy table
  constexpr std::size_t EVICTION_TABLE_SIZE = 128; // Size of the RR victim table 
};
# endif