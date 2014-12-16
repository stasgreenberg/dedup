#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>
#include <crypto/hash.h>
#include <linux/scatterlist.h>
#include <linux/bootmem.h>
#include <linux/dedup.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mmzone.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/time.h>

static struct kobject *stats_kobj;
static int stats;
static int collect_stats;

static unsigned long start_block = 0; // From which block to start dedup (testing)
static long blocks_count; // How much blocks compared
static struct dedup_blk_info blocksArray;// = NULL; // Holds all data about blocks
static sector_t duplicatedBlocks; // Number of duplicated blocks

static int need_to_init = 2;
static char* bdev_name = NULL;
static struct block_device *dedup_bdev = NULL;

// max blocks to allocate, the maximus kmalloc can afford is 128k.
// This will be allocated by alloc_bootmem int start_kernel() init/main.c
static const long BLOCKS_MAX_COUNT = (DEDUP_ALLOC_BOOTMEM_BSIZE / sizeof(u8*));

static long equal_read_count = 0, total_read_count = 0;
// ------------------------ for tests ------------------------------
void print_dedup_data_structure(void);
// -----------------------------------------------------------------

int calc_hash(char* data, size_t size, u8* hash_out);

void dedup_add_total_read(void) { ++total_read_count; }
void dedup_add_equal_read(void) { ++equal_read_count; }

/*
 * Checks if the block is inside our dedup range
 * return 1 if the block is in range
 */
int dedup_is_in_range(sector_t block)
{
	int res = 0;
	if ((block >= start_block)  && (block < (start_block + blocks_count)))
		res = 1;

	return res;
}

/*
 * Will return a pointer to the block device, used for the dedup access.
 * The device is found by its name, configured in bdev_name.
 * If there is no such device, NULL will be returned.
 */
struct block_device* get_our_bdev(void)
{
	struct block_device *bdev =
			lookup_bdev((bdev_name) ? bdev_name : DEDUP_BDEV_NAME);

	return ((bdev == NULL) ?NULL : blkdev_get_by_dev(bdev->bd_dev, FMODE_READ|FMODE_WRITE, NULL));
}

/**
 * get the page associated with the block inside our dedup structure
 */
struct page* dedup_get_block_page(sector_t block)
{
	struct page *res = NULL;

	// Check if the block is inside our range
	if (dedup_is_in_range(block)) {
		// Get the page pointer stored inside the dedup structure
		res = blocksArray.pages[block - start_block];
		if (res != NULL) {
			// If its not NULL, check if the page is up to date and used
			if (PageLRU(res) && PageUptodate(res)) {
				page_cache_get(res);
			}
			else {
				// Cannot use page, need to read from bdev
				blocksArray.pages[block - start_block] = NULL;
				res = NULL;
			}
		}
	}
	else
		printk("get_block_page: block not in range.\n");

	return res;
}

/*
 * Uses kernel's function to read sector's data to read the requested block
 */
void read_block(char *dest, size_t size, sector_t block)
{
	// Sector size is 512, so we calculate the block's sector index
	sector_t sector = block * (dedup_get_block_size() / 512);
	Sector sect;
	// Read data
	void *tmp = read_dev_sector(dedup_bdev, sector, &sect);

	if (!tmp) {
		printk(KERN_ERR "failed to read sector.\n");
		return;
	}

	// Copy and release sector
	memcpy(dest, (char *)tmp, size);
	put_dev_sector(sect);
}

/*
 * The "stats" file where a statistics is read from.
 */
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
						  char *buf)
{
	printk(KERN_ERR "**************************** STATS *****************************\n");
	printk(KERN_ERR "total duplicated blocks = %ld\n", duplicatedBlocks);
	printk(KERN_ERR "equal read = %ld\n", equal_read_count);
	printk(KERN_ERR "total read = %ld\n", total_read_count);
	printk(KERN_ERR "**************************** STATS *****************************\n");

	return sprintf(buf, "%d\n", stats);
}

/*
 * Get block device's logical block size
 */
size_t dedup_get_block_size(void)
{
	static size_t block_size = 4096;

	if (dedup_bdev == NULL) {
		// Get block device
		dedup_bdev = get_our_bdev();
		// Get block size
		block_size = dedup_bdev->bd_block_size;//bdev_logical_block_size(dedup_bdev);
		// Release block device
		blkdev_put(dedup_bdev, FMODE_READ|FMODE_WRITE);
		dedup_bdev = NULL;
	}
	else
		block_size = dedup_bdev->bd_block_size;//bdev_logical_block_size(dedup_bdev);

	return block_size;
}

/*
 * Used for debug, to see if we read the right block
 */
void print_block(int block_num)
{
	// PRINT
	char *curr_data;
	size_t block_size;

	// Get BDEV
	dedup_bdev = get_our_bdev();
	if(!dedup_bdev) {
		printk("get bdev failed.\n");
		return;
	}

	// Read block
	block_size = dedup_get_block_size();
	curr_data = (char *)kmalloc(block_size, GFP_KERNEL);

	if (!curr_data)	{
		printk("Failed to kmalloc buf to store block data.\n");
		return;
	}

	read_block(curr_data, block_size, block_num);

	// Print block
	printk("block no.%d: \"%s\"\n", block_num, curr_data);

	// Release
	kfree(curr_data);
	blkdev_put(dedup_bdev, FMODE_READ|FMODE_WRITE);
	dedup_bdev = NULL;
}
/*
* Input help function, used to handle several commands:
* 'block 12345' sets start block to be 12345.
* 'block comp' sets comp_on to be 1 thus performs blocks compare.
* 'block hash' sets comp_on to be 0.
* 'dedup 12' performs blocks read and compare on 12 blocks starting from start_block.
*/
long check_input(const char *buffer)
{
	char dedup[] = "dedup";
  	char op[] = "2147483647";			/* max int */
	char end;
	int name_len;
	long n = -2;
	int params = sscanf (buffer,"%5s %10s %c", dedup, op, &end);

	if (params == 2) {
		if (strncmp ("dedup", dedup, 5) == 0) {
			if (strncmp ("off", op, 3) == 0) {
				/* stop command */
				n = 0;
			}
			else if (sscanf (op, "%ld", &n) == 1) {
				/* set block count */
				if (n == 0)
					n = -2;
			}
			else
				/* invalid input */
				n = -2;
		}
		else if (strncmp ("block", dedup, 5) == 0) {
			if (sscanf (op, "%ld", &n) == 1) {
				start_block = n;
				printk(KERN_ERR "start_block = %lu.\n", start_block);
				n = -1;
			}
			else
				/* invalid input */
				n = -2;
		}
		else if (strncmp ("setbd", dedup, 5) == 0) {
			kfree(bdev_name);

			name_len = strlen(op);
			bdev_name = kmalloc(name_len, GFP_KERNEL);
			if (!bdev_name) {
				printk("bdev_name allocation failed.\n");
				return -2;
			}

			memcpy(bdev_name, op, name_len);
			printk("bdev_name = %s, len = %d.\n", bdev_name, name_len);
			n = -1;
		}
		else if (strncmp ("print", dedup, 5) == 0) {
			// print block!!!!!!! - DEBUG
			if (sscanf (op, "%ld", &n) == 1) {
				printk(KERN_ERR "printing block %ld.\n", n);
				print_block(n);
				n = -1;
			}
			else if (strncmp ("tree", op, 4) == 0) {
				/* print tree command */
				print_dedup_data_structure();
				n = -1;
			}
			else
				/* invalid input */
				n = -2;
		}
	}

	return n;
}
 
/*
 * Handle sysfs input to control dedup actions
 */
static ssize_t stats_store(struct kobject *kobj, struct kobj_attribute *attr,
						   const char *buf, size_t count)
{
	long result;
	result = check_input(buf);

	if (result > 0) {
		// Turn dedup ON
		collect_stats = DEDUP_ON;
		blocks_count = (result > BLOCKS_MAX_COUNT) ? BLOCKS_MAX_COUNT : result;
		printk(KERN_ERR "\n---------------\n-     On     -\n- blocks_count = %lu -\n---------------\n", blocks_count);
		duplicatedBlocks = 0;
		if (dedup_calc()) {
			printk(KERN_ERR "calc dedup failed...\n");
		}
	}
	else if (result == 0) {
		// Turn dedup OFF
		printk(KERN_ERR "\n-------\n- Off -\n-------\n");
		collect_stats = DEDUP_ON;
	}
	else if (result == -1) {
		// Some parameter was changed
		printk(KERN_ERR "parameter saved.\n");
	}
	else {
		printk(KERN_ERR "invalid input :(\n");
	}

    return count;
}
 
static struct kobj_attribute stats_attribute =
    __ATTR(stats, 0666, stats_show, stats_store);


static struct attribute *attrs[] = {
    &stats_attribute.attr,
    NULL,
};
 

static struct attribute_group attr_group = {
    .attrs = attrs,
};

/*
* called from init/main.c to perform alloc_bootmem on startup.
* performs all allocations needed to manage dedup structure.
*/
int __init dedup_init(void)
{
	long blk_info_alloc_size;
	blocks_count = BLOCKS_MAX_COUNT;

	printk("********************* Dedup Init ******************************\n");

	if (!blocksArray.hashes && !blocksArray.pages && !blocksArray.equal_blocks)
	{
		blk_info_alloc_size = DEDUP_ALLOC_BOOTMEM_BSIZE;
		printk("allocating %lu bytes in bootmem.\n", blk_info_alloc_size);
		blocksArray.hashes = (u8**)alloc_bootmem(blk_info_alloc_size);
		blocksArray.pages = (struct page **)alloc_bootmem(blk_info_alloc_size);
		blocksArray.equal_blocks = (sector_t *)alloc_bootmem(blk_info_alloc_size);
		blocksArray.hash_crc = (u32 *)alloc_bootmem(blk_info_alloc_size);

		if (!blocksArray.hashes || !blocksArray.pages || !blocksArray.equal_blocks) {
			printk(KERN_ERR "dedup_sysfs.c: failed to allocate blocks array.\n");
			return -1;
		}
	}
	else
		printk("blocksArray already allocated!!!\n");

	printk("dedup_sysfs initialized successfully!\n");
	printk("***************************************************************\n");

	return 0;
}

/*
* Create a simple kobject with the name of "dedup",
* located under /sys/kernel/
*/
static int __init stats_init(void)
{
	int retval;


	stats_kobj = kobject_create_and_add("dedup", kernel_kobj);
	if (!stats_kobj)
		return -ENOMEM;

	/* Create the files associated with this kobject */
	retval = sysfs_create_group(stats_kobj, &attr_group);
	if (retval)
	   kobject_put(stats_kobj);

	printk(".....:::::::: module loaded :) :::::::::.....\n");

	return retval;
}

/*
 * Opens block device and performs read and compare operations
 */
int dedup_calc(void)
{
	// Check if we did not init already
	if (need_to_init)
	{
		// Get bdev
		dedup_bdev = get_our_bdev();
		if(!dedup_bdev) {
			printk(KERN_ERR "get bdev failed.\n");
			return -1;
		}

		if (blocks_count > BLOCKS_MAX_COUNT)
			blocks_count = BLOCKS_MAX_COUNT;

		printk(KERN_ERR "blocks count = %ld (max = %ld)\n", blocks_count, BLOCKS_MAX_COUNT);
		printk(KERN_ERR "each block logical size is (%ld)\n", dedup_get_block_size());

		// Initialize block structure
		if (dedup_init_blocks()) {
			blkdev_put(dedup_bdev, FMODE_READ|FMODE_WRITE);
			dedup_bdev = NULL;
			return -1;
		}

		// Release bdev
		blkdev_put(dedup_bdev, FMODE_READ|FMODE_WRITE);
		dedup_bdev = NULL;
		need_to_init = 0;
		printk(KERN_ERR "blocks init done!\n");
	}

	return 0;
}

/*
 * When page is being changed by the kernel, we must update the dedup structure:
 * 		1. unlink changed block from equal blocks
 * 		2. calculate new hash and crc
 * 		3. link to new equal blocks (if exists)
 */
int dedup_update_page_changed(sector_t block, char* block_data)
{
	size_t block_size = dedup_get_block_size();
	sector_t currblock, equal_block;

	// Todo: add support if there is more than 1 block in page - check them all
	if (!dedup_is_in_range(block)) {
		trace_printk("block not in range %ld", block);
		return 0;
	}

	block = block - start_block;
	equal_block = block;

	trace_printk("page is being updated : block = %ld\n", block);

	// Remove from dedup structure
	dedup_remove_block_duplication(block);
	// Calc hash
	calc_hash(block_data, block_size, blocksArray.hashes[block]);
	// Calc crc32
	blocksArray.hash_crc[block] = crc32_le(0, blocksArray.hashes[block], SHA256_DIGEST_SIZE);

	// Go over other blocks
	for (currblock = 0; currblock < blocks_count; ++currblock) {
		// If blocks equal, update dedup structure
		if (currblock != block) {
			// first, compare crc - should be faster
			if (blocksArray.hash_crc[currblock] == blocksArray.hash_crc[block]){
				// If hash array is NULL then there is a block at lower index
				// that is equal to this block and it was already compared to.
				if (blocksArray.hashes[currblock] && blocksArray.hashes[block] &&
					memcmp(blocksArray.hashes[currblock], blocksArray.hashes[block], SHA256_DIGEST_SIZE) == 0)
				{
					equal_block = currblock;
					break;
				}
			}
		}
	}

	if (block != equal_block) {
		trace_printk("found new duplicated block ! %ld = %ld\n", block + start_block, equal_block + start_block);
		dedup_set_block_duplication(equal_block, block);
	}

	return 0;
}

/*
 * Calculates block's hash value, to avoid all block compare.
 * hash_out must be allocated outside.
 */
int calc_hash(char* data, size_t size, u8* hash_out)
{
	struct hash_desc sha256_desc;
	struct scatterlist sg;

	sha256_desc.tfm = crypto_alloc_hash("sha256", 0, CRYPTO_ALG_ASYNC);
	sg_init_one(&sg, data, size);

	crypto_hash_init(&sha256_desc);
	crypto_hash_update(&sha256_desc, &sg, size);
	crypto_hash_final(&sha256_desc, hash_out);

	crypto_free_hash(sha256_desc.tfm);

	return 0;
}

/*
 * After we read and calculate all data, this function builds the final structure.
 * It will compare all crc and hashes (when needed) and link equal blocks.
 */
void test_final_hash_compare(void)
{
	sector_t i, j, equal_block;

	// Go over all blocks
	for (i = 0; i < blocks_count; ++i) {
		equal_block = i;
		// Loop until current block
		for (j = 0; j < i; ++j) {
			// first, compare crc - should be faster
			if (blocksArray.hash_crc[i] == blocksArray.hash_crc[j]){
				// If hash array is NULL then there is a block at lower index
				// that is equal to this block and it was already compared to.
				if (blocksArray.hashes[j] && blocksArray.hashes[i] &&
					memcmp(blocksArray.hashes[i], blocksArray.hashes[j], SHA256_DIGEST_SIZE) == 0)
				{
					equal_block = j;
					break;
				}
			}
		}

		// Check if equal block was found
		if (equal_block != i) {
			dedup_set_block_duplication(equal_block, i);
		}
	}
}

/*
 * Go over all blocks, read, hash, compare.
 */
int dedup_init_blocks(void)
{
	const int status_update_step = blocks_count / 10;
	sector_t block_idx;
	sector_t next_status_block = status_update_step;

	printk(KERN_ERR "Initializing blocks array. blocks_count = %lu.\n", blocks_count);
	// Go over all block and initialize blocks array
	for (block_idx = 0; block_idx < blocks_count; ++block_idx) {
		// Init blocks info
		blocksArray.equal_blocks[block_idx] = block_idx;
		blocksArray.pages[block_idx] = NULL;
		blocksArray.hashes[block_idx] = NULL;

		// READ AND HASH BLOCKS BEFORE COMPARATION
		blocksArray.hashes[block_idx] = (u8*)kmalloc(SHA256_DIGEST_SIZE, GFP_KERNEL);
		blocksArray.hash_crc[block_idx] = 0;

		if (blocksArray.hashes[block_idx] == NULL) {
			printk(KERN_ERR "failed to alloc hash buffer.\n");
			return -1;
		}
	}

	printk(KERN_ERR "Looking for equal blocks.\n");
	// Go over all block set equal
	for (block_idx = 0; block_idx < blocks_count; ++block_idx) {
		// Find equal block
		dedup_calc_block_hash_crc(block_idx);

		if (block_idx == next_status_block) {
			next_status_block += status_update_step;
			if (next_status_block > blocks_count)
				next_status_block = blocks_count;
			printk(KERN_ERR "%lu out of %lu blocks compared.\n",
					block_idx, blocks_count);
		}
	}

	trace_printk("before hash compare loop\n");
	test_final_hash_compare();
	trace_printk("after hash compare loop\n");

	printk(KERN_ERR "//---------------- Dedup Report ---------------//\n");
	printk(KERN_ERR "%lu duplicated blocks were found.\n", duplicatedBlocks);
	printk(KERN_ERR "//---------------------------------------------//");

	return 0;
}

/*
 * Used in mpage_readpages() fs/mpage.c
 */
int dedup_wait_for_init(void) { return need_to_init; }

/*
 * Used for debug.
 * Prints the entire dedup structure to show which of the blocks are linked to each other.
 */
void print_dedup_data_structure(void)
{
	int i, j;
	// Init array used to indicate if we already printed this link
	char* tmp_buf = (char *)kmalloc(blocks_count, GFP_KERNEL);
	if (!tmp_buf) {
		printk("failed to alloc tmp_buf\n");
		return;
	}
	// All set to 1. 1 means we need to print.
	memset(tmp_buf, 1, blocks_count);

	// Go over all blocks
	for (i = 0; i < blocks_count; ++i) {
		// Check if we need to print this link
		if (tmp_buf[i]) {
			// Make sure it will not be printed next time
			tmp_buf[i] = 0;
			j = blocksArray.equal_blocks[i];
			if (i == j)
				// Ignore blocks that do not have equal blocks
				continue;

			printk("%d", i);

			// Loop all equal blocks
			while (j != i && tmp_buf[j]){
				// Make sure it will not be printed next time
				tmp_buf[j] = 0;
				printk("->%d", j);
				j = blocksArray.equal_blocks[j];
			}

			printk("\n");
		}
	}

	kfree(tmp_buf);
}

/*
 * Returns the next equal block inside the dedup structure
 */
sector_t dedup_get_next_equal_block(sector_t block)
{
	sector_t next_equal = block;

	// Check if in dedup range
	if (dedup_is_in_range(block)) {
		next_equal = blocksArray.equal_blocks[block - start_block];
		next_equal += start_block;
	}

	return next_equal;
}

/*
 * Update dedup structure with block's hash and crc
 */
void dedup_calc_block_hash_crc(sector_t block)
{
	size_t block_size = dedup_get_block_size();
	char *block_data;

	if (block >= blocks_count)
		// outside dedup range
		return;

	block_data = (char*)kmalloc(block_size, GFP_KERNEL);
	if (block_data == NULL) {
		printk(KERN_ERR "failed allocating block data buffer.\n");
		return;
	}

	// Read block
	read_block(block_data, block_size, start_block + block);
	// Calc hash
	calc_hash(block_data, block_size, blocksArray.hashes[block]);
	// Calc crc32
	blocksArray.hash_crc[block] = crc32_le(0, blocksArray.hashes[block], SHA256_DIGEST_SIZE);

	kfree(block_data);
}

/*
 * gets 2 duplicated blocks and updates the list circulation
 */
void dedup_set_block_duplication(sector_t old_block, sector_t new_block)
{
	sector_t tmp_next = blocksArray.equal_blocks[old_block];
	blocksArray.equal_blocks[old_block] = new_block;
	blocksArray.equal_blocks[new_block] = tmp_next;
	++duplicatedBlocks;
}

/*
 * removes duplicated block from the list
 */
void dedup_remove_block_duplication(sector_t block)
{
	long next = block;

	// if the block has no other equal blocks, ignore
	if (blocksArray.equal_blocks[next] == block)
		return;

	while(blocksArray.equal_blocks[next] != block) {
		next = blocksArray.equal_blocks[next];
	}

	blocksArray.equal_blocks[next] = blocksArray.equal_blocks[block];
	blocksArray.equal_blocks[block] = block;

	--duplicatedBlocks;
}

/*
* Used in mpage_readpages() fs/mpage.c
* Keeps a connection between block and its page
*/
void dedup_update_block_page(struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = (mapping) ? page->mapping->host : NULL;

	if (inode != NULL) {
		// get the block number
		sector_t page_block = bmap(inode, page->index);

		// Check if the block is inside dedup range
		if (dedup_is_in_range(page_block)) {
			// Update block's page reference
			blocksArray.pages[page_block - start_block] = page;
		}
	}
	else
		printk("inode is NULL :(\n");
}

/*
* One page may contain several blocks
* This function will go over page's block and return their indexes inside bdev
*/
sector_t *dedup_get_page_physical_blocks(struct page *page, int *nr_blocks)
{
	struct inode *inode = page->mapping->host;
	sector_t block;
	int curr_block = 0;
	unsigned long index = page->index;
	int blocksize = dedup_get_block_size();
	sector_t *blocks;

	// Calc blocks count
	*nr_blocks = ((PAGE_SIZE + blocksize - 1) / blocksize);
	// Allocate buffer to store all blocks' indexes
	blocks = (sector_t*)kmalloc(*nr_blocks * sizeof(sector_t), GFP_KERNEL);

	if (!blocks) {
		printk("failed to kmalloc page's blocks array\n");
		return NULL;
	}

	// Get all blocks
	block = index << (PAGE_SHIFT - inode->i_blkbits);
	while (curr_block < *nr_blocks) {
		blocks[curr_block++] = bmap(inode, block);
		block++;
	}

	return blocks;
}
 
static void __exit stats_exit(void)
{
	kobject_put(stats_kobj);
}
 
module_init(stats_init);
module_exit(stats_exit);
MODULE_LICENSE("GPL");
