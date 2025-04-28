# ifndef _BOP_PARAMETERS_H_
# define _BOP_PARAMETERS_H_

namespace bop_space
{ 

//     struct BOPPrefetcherParams
//     : public QueuedPrefetcherParams
// {
//     unsigned bad_score;
//     int degree;
//     Cycles delay_queue_cycles;
//     bool delay_queue_enable;
//     unsigned delay_queue_size = 55;
//     bool negative_offsets_enable;
//     unsigned offset_list_size;
//     unsigned round_max;
//     unsigned rr_size;
//     unsigned score_max;
//     unsigned tag_bits;
// };
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // BOP
  # define SCORE_MAX            (31) // Max. score to update the best offset
  # define ROUND_MAX            (100) // Max. round to update the best offset
  # define BAD_SCORE            (10) // Score at which the HWP is disabled
  # define RR_SIZE              (64) // Number of entries of each RR bank
  # define TAG_BITS             (12) // Bits used to store the tag
  # define OFFSET_LIST_SIZE     (46) // Number of entries in the offsets list

  # define NEGATIVE_OFFSETS_ENABLE    (false) // Initialize the offsets list also with negative values \
                                               (i.e. the table will have half of the entries with positive \
                                               offsets and the other half with negative ones)

  # define DELAY_QUEUE_ENABLE   (false) // Enable the delay queue 
  # define DELAY_QUEUE_SIZE     (15) // Number of entries in the delay queue
  # define DELAY_QUEUE_CYCLES   (60) // Cycles to delay a write in the left RR table from the delay \
                                        queue

    // # BOP is a degree one prefetcher
    // degree = Param.Int(1, "Number of prefetches to generate")

    // queue_squash = True
    // queue_filter = True
    // cache_snoop = True
    // prefetch_on_pf_hit = True
    // on_miss = True
    // on_inst = False
};
# endif
