# ifndef _CAERUS_PARAMETERS_H_
# define _CAERUS_PARAMETERS_H_

namespace caerus_space
{ 
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // CAERUS

  # define TAG_BITS                 (12)  // Bits used to store the tag

  // OFFSET TRAINING 
  # define SCORE_MAX                (31)  // Max. score to update the best offset
  # define ROUND_MAX                (100) // Max. round to update the best offset
  # define BAD_SCORE                (1)   // Score at which the offset is disabled
  # define OFFSET_LIST_SIZE         (52)  // Number of entries in the offsets list
  # define NUM_OFFSETS              (8)   // The number of offsets that are learnt
  # define TRAIN_SPEED              (1) // The number of offset candidates considered on each training instance 
  # define NEGATIVE_OFFSETS_ENABLE  (true) // If enabled, half of the offset list will contain negative offsets

  // RECENT PREFETCHES
  # define RECENT_PREFETCHES_SIZE   (128)  // Size of the recent prefetches table

  // ACCURACY TRACKING
  # define ACCURACY_THRESHOLD       (8)   // Saturing counter value for pc/offset to be accurate
  # define ACCURACY_INCREMENT       (1)   // Increment value for accuracy table
  # define ACCURACY_DECREMENT       (1)   // Decrement value for accuracy table

  // HARDWARE STRUCTURES
  # define RR_SIZE                  (256) // Number of entries in RR Table
  # define HOLDING_TABLE_SIZE       (128) // Size of the holding table
  # define ACCURACY_TABLE_SIZE      (128) // Size of the pc accuracy table
  # define EVICTION_TABLE_SIZE      (128) // Size of the RR victim table 

  // CROSS PAGE
  # define ALLOW_CROSS_PAGE         (true) // Determines if prefetches are allowed to cross page boundaries

  // EXPERIMENTAL FEATURES
  # define OVERLAP_LEAKAGE          (false) // Determines if overlap leakage is enabled
  # define LEAKAGE_PERIOD           (50)   // One out of X overlapping prefetches will be trained 



};
# endif