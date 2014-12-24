// dedup.h

#ifndef __DEDUP_H
#define __DEDUP_H

#include <crypto/sha.h>

// Definitions
#define DEDUP_ON 	1
#define DEDUP_OFF 	0

#define DEDUP_ALLOC_BOOTMEM_BSIZE (64000000)/*try this: 67108864*/

#define DEDUP_BDEV_NAME "/dev/sda1"

// Variables

struct dedup_blk_info{
	u8 **hashes;				// sha256 of block data
	struct page **pages;		// reference to block's page
	u32 *hash_crc;				// crc value of block sha256
	sector_t *equal_blocks;		// circular vector of equal blocks
};

// Functions
// Init
int dedup_calc(void);
int dedup_init_blocks(void);
// Dedup
void dedup_set_block_duplication(sector_t block1, sector_t block2);
void dedup_remove_block_duplication(sector_t block);
void dedup_calc_block_hash_crc(sector_t block);
sector_t dedup_get_next_equal_block(sector_t block);
int dedup_update_page_changed(sector_t block, char* block_data);
sector_t *dedup_get_page_physical_blocks(struct page *page, int *nr_blocks);
// Help
int dedup_wait_for_init(void);
size_t dedup_get_block_size(void);
struct page* dedup_get_block_page(sector_t nBlock);
int dedup_is_in_range(sector_t block);
int dedup_is_our_bdev(struct block_device *bdev);
void dedup_update_block_page(struct page *page);

// Count statistics
void dedup_add_total_read(void);
void dedup_add_equal_read(void);

#endif
