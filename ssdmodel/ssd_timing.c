// DiskSim SSD support
// �2008 Microsoft Corporation. All Rights Reserved

#include "ssd.h"
#include "ssd_timing.h"
#include "ssd_clean.h"
#include "ssd_gang.h"
#include "ssd_utils.h"
#include "modules/ssdmodel_ssd_param.h"

#ifdef PN_SSD
void rosa_table_clean(ssd_t *s, ssd_element_metadata *metadata) {
    int i;
    static int logical_clean = 0;

    for(i = 0; i < metadata->rosa_table_size; i++) 
        metadata->rosa_table[i] = -1;
    if (logical_clean++ > s->params.logical_clean_interval) {
        for(i = 0; i < s->params.blocks_per_element; i++)
            metadata->block_usage[i].log_read_count = 0;
        logical_clean = 0;
    }
}

void rosa_table_add(ssd_t *s, int read_block, ssd_element_metadata *metadata) {
    int i;

    for(i = 0; i < metadata->rosa_table_size; i++) {
        int blk_index = metadata->rosa_table[i];
        long long int blk_ref;
        if(blk_index != -1)
            blk_ref = metadata->block_usage[blk_index].log_read_count / metadata->block_usage[blk_index].num_valid;
        long int read_ref = metadata->block_usage[read_block].log_read_count / metadata->block_usage[read_block].num_valid;
        
        if (metadata->rosa_table[i] != -1) {
            if(blk_index == read_block)
                return;

            if (blk_ref < read_ref) {
                metadata->rosa_table[i] = read_block;
                return;
            }
        } else if(read_ref > metadata->pcm_avg_read_count) { 
            if (metadata->block_usage[read_block].num_valid <= (s->params.pages_per_block / s->params.ria_gc_trigger)) {
                metadata->rosa_table[i] = read_block;
                return;
            }
        }
    }
}

int rosa_table_full(ssd_element_metadata *metadata) {
    int i;

    for(i = 0; i < metadata->rosa_table_size; i++) {
        if(metadata->rosa_table[i] == -1)
            return 0;
    }
    return 1;
}

void pcm_victim_select(ssd_t *s, ssd_element_metadata *metadata) {
    int i;
    unsigned int read_count = 0;
    unsigned int min_read_count = 0xffffffff;
    int interval_pcm   = metadata->pcm_interval;
    int pcm_blk  = -1;

    // Calculate PRAM average read count
    // Search PRAM block which has the lowest read count
    for(i = 0; i < metadata->pcm_usable_blocks; i++) {
        read_count += metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;

        if((metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block) <= min_read_count) {
            min_read_count = metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;
            pcm_blk = i * interval_pcm;
        }
    }
    metadata->pcm_victim_block    = pcm_blk;
    metadata->pcm_victim_page_pos = 0;
    metadata->pcm_avg_read_count  = read_count / metadata->pcm_usable_blocks;
}

double rosa_mig(ssd_t *s, ssd_element_metadata *metadata, int elem_num, int plane_num, int blk, int MOVE) {
    int i;
    double cost = 0;
    long long int read_count = 0;
    long long int max_read_count;
    long long int min_read_count = 0xffffffff;
    int page_max_read_count = 0;
    int interval_pcm   = metadata->pcm_interval;
    int index_blk;
    int nand_blk = -1;
    int pcm_blk  = metadata->pcm_victim_block;
    int nand_lpn = -1;
    int pcm_lpn = -1;
    int nand_page = -1, pcm_page = -1;
    int nand_pagepos= -1, pcm_pagepos = metadata->pcm_victim_page_pos;

    // Calculate PRAM average read count
    // Search PRAM block which has the lowest read count
#ifdef RIA2
    if(metadata->pcm_victim_block == -1) {
        pcm_victim_select(s, metadata);
        pcm_blk = metadata->pcm_victim_block;
        pcm_pagepos = metadata->pcm_victim_page_pos;
/*        for(i = 0; i < metadata->pcm_usable_blocks; i++) {
            temp = read_count;
            read_count += metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;

            if((metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block) <= min_read_count) {
                min_read_count = metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;
                pcm_blk = i * interval_pcm;
            }
        }
        metadata->pcm_victim_block = pcm_blk;
        metadata->pcm_avg_read_count = read_count / metadata->pcm_usable_blocks;*/
    } else {
        for(i = 0; i < metadata->pcm_usable_blocks; i++) {
            read_count += metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;
        }
        //metadata->pcm_victim_block = pcm_blk;
        metadata->pcm_avg_read_count = read_count / metadata->pcm_usable_blocks;
        pcm_blk = metadata->pcm_victim_block;
        pcm_pagepos = metadata->pcm_victim_page_pos;
    }
#else
    for(i = 0; i < metadata->pcm_usable_blocks; i++) {
        read_count += metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;

        if((metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block) <= min_read_count) {
            min_read_count = metadata->block_usage[i * interval_pcm].log_read_count / s->params.pages_per_block;
            pcm_blk = i * interval_pcm;
        }
    }
    //metadata->pcm_victim_block = pcm_blk;
    metadata->pcm_avg_read_count = read_count / metadata->pcm_usable_blocks;
#endif

    max_read_count = metadata->pcm_avg_read_count;

    // There are no NAND & PCM victim block on hot table
    if(pcm_blk == -1) 
        return cost;
    
    if (MOVE == RIA_MIG) {
        // Search NAND block which has max read count
        nand_blk = blk;
#ifdef PAGE_MIG
        for(i = 0; i < s->params.pages_per_block - 1; i++) {
            if (metadata->block_usage[nand_blk].page_read_count[i] >= page_max_read_count && metadata->block_usage[nand_blk].page[i] != -1) {
                page_max_read_count = metadata->block_usage[nand_blk].page_read_count[i];
                nand_lpn = metadata->block_usage[nand_blk].page[i];
                nand_page = metadata->lba_table[nand_lpn];
                nand_pagepos = nand_page % s->params.pages_per_block;
            }

            min_read_count = metadata->block_usage[pcm_blk].log_read_count;
            if (metadata->block_usage[pcm_blk].page_read_count[i] <= min_read_count) {
                min_read_count = metadata->block_usage[pcm_blk].page_read_count[i];
                pcm_lpn = metadata->block_usage[pcm_blk].page[i];
                pcm_page = metadata->lba_table[pcm_lpn];
                pcm_pagepos = pcm_page % s->params.pages_per_block;
            }
        }
#endif
        s->stat.tot_ria_mig ++; 
    } else {
        if (blk != -1)
            nand_blk = blk;
        else
            return cost;
        s->stat.tot_ria_gc ++;
    }

    // Calculate read count    
    //metadata->block_usage[pcm_blk].log_read_count = metadata->block_usage[nand_blk].log_read_count;
    metadata->block_usage[nand_blk].num_read_count = 0;

    unsigned int active_page;
    unsigned int active_block;
    unsigned int pagepos_in_block;
    unsigned int active_plane;
        
    unsigned int nand_plane;

    i = 0;
    do {
#ifdef PAGE_MIG
        if (MOVE != RIA_MIG) {
#endif

#ifdef RIA2
            if (pcm_pagepos > s->params.pages_per_block - 1)
            {
                pcm_victim_select(s, metadata);
                pcm_blk     = metadata->pcm_victim_block;
                pcm_pagepos = metadata->pcm_victim_page_pos;
            } 
            pcm_lpn = metadata->block_usage[pcm_blk].page[pcm_pagepos];
#else
            pcm_lpn = metadata->block_usage[pcm_blk].page[i];
#endif
            pcm_page = metadata->lba_table[pcm_lpn];
            nand_lpn = metadata->block_usage[nand_blk].page[i];
#ifdef PAGE_MIG
        }
#endif
        if(nand_lpn != -1) {
            // if this is the last page on the block, allocate a new block
            if (ssd_last_page_in_block(metadata->plane_meta[plane_num].active_page, s)) {
                _ssd_alloc_active_block(plane_num, elem_num, s);
            }

            // issue the write to the current active page.
            // we need to transfer the data across the serial pins for write.
            metadata->active_page = metadata->plane_meta[plane_num].active_page;

            active_page = metadata->active_page;
            active_block = SSD_PAGE_TO_BLOCK(active_page, s);
            pagepos_in_block = active_page % s->params.pages_per_block;
            active_plane = metadata->block_usage[active_block].plane_num;
            plane_num = active_plane;

            nand_plane = metadata->block_usage[nand_blk].plane_num;

            // NAND page migration to PCM block
            metadata->lba_table[nand_lpn] = pcm_page;

            // Previous NAND block invalidate
#ifdef PAGE_MIG
            if (MOVE == RIA_MIG)
                metadata->block_usage[nand_blk].page[nand_pagepos] = -1;
            else
                metadata->block_usage[nand_blk].page[i] = -1;
#else
            metadata->block_usage[nand_blk].page[i] = -1;
#endif
            metadata->block_usage[active_block].log_read_count += (metadata->block_usage[pcm_blk].log_read_count / s->params.pages_per_block);
            metadata->block_usage[pcm_blk].log_read_count      -= (metadata->block_usage[pcm_blk].log_read_count / s->params.pages_per_block);
            metadata->block_usage[pcm_blk].log_read_count      += (metadata->block_usage[nand_blk].log_read_count / metadata->block_usage[nand_blk].num_valid);
            metadata->block_usage[nand_blk].log_read_count     -= (metadata->block_usage[nand_blk].log_read_count / metadata->block_usage[nand_blk].num_valid);

            metadata->block_usage[nand_blk].num_valid --;
            metadata->plane_meta[nand_plane].valid_pages --;

            // PCM page migration to NAND block
            metadata->lba_table[pcm_lpn] = active_page;

            metadata->block_usage[active_block].page[pagepos_in_block] = pcm_lpn;
            metadata->block_usage[active_block].num_valid ++;
            metadata->plane_meta[active_plane].valid_pages ++;

            cost += s->params.pcm_read_latency;
            if (pagepos_in_block % 3 == 0)
                cost += s->params.lsb_write_latency;
            else if (pagepos_in_block % 3 == 1)
                cost += s->params.csb_write_latency;
            else
                cost += s->params.msb_write_latency;
            //s->stat.tot_pcm_read_count ++;
            //s->stat.tot_nand_write_count ++;

            if (MOVE == RIA_GC) {
                s->stat.tot_ria_gc_pcm_read_count ++;
                s->stat.tot_ria_gc_nand_write_count ++;
            } else {
                s->stat.tot_ria_mig_pcm_read_count ++;
                s->stat.tot_ria_mig_nand_write_count ++;
            }

#ifdef PAGE_MIG
            if (MOVE == RIA_MIG)
                metadata->block_usage[pcm_blk].page[pcm_pagepos] = nand_lpn;
            else
                metadata->block_usage[pcm_blk].page[i] = nand_lpn;
#else
#ifdef RIA2
            metadata->block_usage[pcm_blk].page[pcm_pagepos] = nand_lpn;
            pcm_pagepos++;
#else
            metadata->block_usage[pcm_blk].page[i] = nand_lpn;
#endif

#endif
            if (MOVE != RIA_GC) {
                if (i % 3 == 0) 
                    cost += s->params.lsb_read_latency; 
                else if (i % 3 == 1)
                    cost += s->params.csb_read_latency; 
                else
                    cost += s->params.msb_read_latency; 

                s->stat.tot_ria_mig_nand_read_count ++;
            } else {
                s->stat.tot_ria_gc_nand_read_count ++;
            }

            cost += s->params.pcm_write_latency; 
            //s->stat.tot_pcm_write_count ++;

            if (MOVE == RIA_GC) {
                s->stat.tot_ria_gc_pcm_write_count ++;
            } else {
                s->stat.tot_ria_mig_pcm_write_count ++;
            }

            ssd_assert_valid_pages(active_plane, metadata, s);
            
            // some sanity checking
            if (metadata->block_usage[active_block].num_valid >= s->params.pages_per_block) {
                fprintf(stderr, "Error: len %d of block %d is greater than or equal to pages per block %d\n",
                        metadata->block_usage[active_block].num_valid, active_block, s->params.pages_per_block);
                exit(1);
            }

            // go to the next free page
            metadata->active_page = active_page + 1;
            metadata->plane_meta[active_plane].active_page = metadata->active_page;

            // if this is the last data page on the block, let us write the 
            // summary page also
            if (ssd_last_page_in_block(metadata->active_page, s)) {
                // cost of transferring the summary page data
                cost += ssd_data_transfer_cost(s, SSD_SECTORS_PER_SUMMARY_PAGE);

                //cost of writeing the summary page data
                cost += s->params.lsb_write_latency;

                // seal the last summary page. since we use the summary page
                // as a metadata, we don't count it as a valid data page.
                metadata->block_usage[active_block].page[s->params.pages_per_block - 1] = -1;
                metadata->block_usage[active_block].state = SSD_BLOCK_SEALED;
            }

            s->elements[elem_num].stat.pages_moved ++;
#ifdef PAGE_MIG
            if (MOVE == RIA_MIG)
                return cost;
#endif 
        }
    } while(++i < s->params.pages_per_block - 1);

    if (metadata->block_usage[nand_blk].num_valid == 0) {
        cost += s->params.block_erase_latency;
        ssd_update_free_block_status(nand_blk, nand_plane, metadata, s);
        ssd_update_block_lifetime(simtime+cost, nand_blk, metadata);
    }
    return cost;
}

double read_reclaim(ssd_t *s, ssd_element_metadata *metadata, int elem_num, int plane_num, int blk) {
    int i;
    double cost = 0;
    int nand_blk = blk;
    int nand_lpn = -1;
    
    // Initiate physical block read count    
    metadata->block_usage[nand_blk].num_read_count = 0;

    s->stat.tot_rd_mig++;

    i = 0;
    do {
        nand_lpn = metadata->block_usage[nand_blk].page[i];
        if(nand_lpn != -1) {
            // if this is the last page on the block, allocate a new block
            if (ssd_last_page_in_block(metadata->plane_meta[plane_num].active_page, s)) {
                _ssd_alloc_active_block(plane_num, elem_num, s);
            }

            // issue the write to the current active page.
            // we need to transfer the data across the serial pins for write.
            metadata->active_page = metadata->plane_meta[plane_num].active_page;
            
            s->stat.tot_rd_nand_read_count ++;
            s->stat.tot_rd_nand_write_count ++;
            s->stat.tot_nand_write_count --;

            if (i % 3 == 0) 
                cost += s->params.lsb_read_latency; 
            else if (i % 3 == 1)
                cost += s->params.csb_read_latency; 
            else
                cost += s->params.msb_read_latency; 
            
            cost += _ssd_write_page_osr(s, metadata, nand_lpn);
        }
    } while(++i < s->params.pages_per_block - 1);

    if (metadata->block_usage[nand_blk].num_valid == 0) {
        cost += s->params.block_erase_latency;
        ssd_update_free_block_status(nand_blk, plane_num, metadata, s);
        ssd_update_block_lifetime(simtime+cost, nand_blk, metadata);
    }
 
    return cost;
}
#endif

struct my_timing_t {
    ssd_timing_t          t;
    ssd_timing_params    *params;
    int next_write_page[SSD_MAX_ELEMENTS];
};

int ssd_choose_element(ssd_timing_t *t, int blkno)
{
    struct my_timing_t *tt = (struct my_timing_t *) t;
    return (blkno/(tt->params->element_stride_pages*tt->params->page_size)) % tt->params->nelements;
}

int ssd_choose_aligned_count(int page_size, int blkno, int count)
{
    int res = page_size - (blkno % page_size);
    if (res > count)
        res = count;
    return res;
}

//////////////////////////////////////////////////////////////////////////////
//                Simple algorithm for writing and cleaning flash
//////////////////////////////////////////////////////////////////////////////

/*
 * implements the simple write policy wrote by ted.
 */
static double ssd_write_policy_simple(int blkno, int count, int elem_num, ssd_t *s)
{
    double cost;
    int pages_moved;        // for statistics
    struct my_timing_t *tt = (struct my_timing_t *)(s->timing_t);

    // the page position on-chip is calculated as follows (N is the number of chips per SSD):
    //               [integer division is assumed]
    //    absolute page number (APN) = block number / page_size
    //    PN = ((APN - (APN % (Stride * N))) / N) + (APN % Stride)
    //
    int blockpos, lastpos;
    int ppb = tt->params->pages_per_block;
    int last_pn = tt->next_write_page[elem_num];
    int apn = blkno/tt->params->page_size;
    int pn = ((apn - (apn % (tt->params->element_stride_pages*tt->params->nelements)))/
                      tt->params->nelements) + (apn % tt->params->element_stride_pages);

    blockpos = pn % ppb;
    lastpos = last_pn % ppb;

    if (last_pn >= 0 && pn >= last_pn && (pn/ppb == last_pn/ppb)) {
        // the cost of one page write
#ifndef PN_SSD
        cost = tt->params->page_write_latency;
#endif

        // plus the cost of copying the intermediate pages
#ifndef PN_SSD
        cost += (pn-last_pn) * (tt->params->page_read_latency + tt->params->page_write_latency);
#endif
        // stat - pages moved other than the page we wrote
        pages_moved = pn - last_pn;
    } else {
        // the cost of one page write
#ifndef PN_SSD
        cost = tt->params->page_write_latency;
#endif
        // plus the cost of an erase
        cost += tt->params->block_erase_latency;

        // stat
        s->elements[elem_num].stat.num_clean ++;

        // plus the cost of copying the blocks prior to this pn
#ifndef PN_SSD
        cost += blockpos * (tt->params->page_read_latency + tt->params->page_write_latency);
#endif
        // stat
        pages_moved = blockpos;

        if (last_pn >= 0) {
            // plus the cost of copying remaining pages in last written block
#ifndef PN_SSD
            cost += (ppb-lastpos) *
                (tt->params->page_read_latency + tt->params->page_write_latency);
#endif
            // stat
            pages_moved += ppb - lastpos;
        }
    }

    pn = pn + 1;
    if ((blockpos) == 0)
        tt->next_write_page[elem_num] = -1;
    else
        tt->next_write_page[elem_num] = pn;

    // stat
    s->elements[elem_num].stat.pages_moved += pages_moved;

    // plus the cost of moving the required sectors on/off chip
    cost += ssd_data_transfer_cost(s, count);

    return cost;
}

double ssd_read_policy_simple(int count, ssd_t *s)
{
#ifndef PN_SSD
    double cost = s->params.page_read_latency;
    cost += ssd_data_transfer_cost(s, count);

    return cost;
#else
    return 0;
#endif
}

//////////////////////////////////////////////////////////////////////////////
//                OSR algorithm for writing and cleaning flash
//////////////////////////////////////////////////////////////////////////////

/*
 * returns a count of the number of free bits in a plane.
 */
int ssd_free_bits(int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int i;
    int freecount = 0;
    int start = plane_num * s->params.blocks_per_plane;
    for (i = start; i < start + (int)s->params.blocks_per_plane; i ++) {
        if (!ssd_bit_on(metadata->free_blocks, i)) {
            freecount ++;
        }
    }

    return freecount;
}

/*
 * makes sure that the number of free blocks is equal to the number
 * of free bits in the bitmap block.
 */
void ssd_assert_plane_freebits(int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int f1;

    f1 = ssd_free_bits(plane_num, elem_num, metadata, s);
    ASSERT(f1 == metadata->plane_meta[plane_num].free_blocks);
}

/*

 * just to make sure that the free blocks maintained across various
 * metadata structures are the same.
 */
void ssd_assert_free_blocks(ssd_t *s, ssd_element_metadata *metadata)
{
    int tmp = 0;
    int i;

    //assertion
    for (i = 0; i < s->params.planes_per_pkg; i ++) {
        tmp += metadata->plane_meta[i].free_blocks;
    }
    ASSERT(metadata->tot_free_blocks == tmp);
}

/*
 * make sure that the following invariant holds always
 */
void ssd_assert_page_version(int prev_page, int active_page, ssd_element_metadata *metadata, ssd_t *s)
{
    int prev_block = prev_page / s->params.pages_per_block;
    int active_block = active_page / s->params.pages_per_block;

    if (prev_block != active_block) {
        ASSERT(metadata->block_usage[prev_block].bsn < metadata->block_usage[active_block].bsn);
    } else {
        ASSERT(prev_page < active_page);
    }
}

void ssd_assert_valid_pages(int plane_num, ssd_element_metadata *metadata, ssd_t *s)
{
#if SSD_ASSERT_ALL
    int i;
    int block_valid_pages = 0;
    int start_bitpos = plane_num * s->params.blocks_per_plane;

    for (i = start_bitpos; i < (start_bitpos + (int)s->params.blocks_per_plane); i ++) {
        int block = ssd_bitpos_to_block(i, s);
        block_valid_pages += metadata->block_usage[block].num_valid;
    }

    ASSERT(block_valid_pages == metadata->plane_meta[plane_num].valid_pages);
#else
    return;
#endif
}

double ssd_data_transfer_cost(ssd_t *s, int sectors_count)
{
    return (sectors_count * (SSD_BYTES_PER_SECTOR) * s->params.chip_xfer_latency);
}

int ssd_last_page_in_block(int page_num, ssd_t *s)
{
    return (((page_num + 1) % s->params.pages_per_block) == 0);
}

int ssd_bitpos_to_block(int bitpos, ssd_t *s)
{
    int block = -1;

    switch(s->params.plane_block_mapping) {
        case PLANE_BLOCKS_CONCAT:
            block = bitpos;
            break;

        case PLANE_BLOCKS_PAIRWISE_STRIPE:
        {
            int plane_pair = bitpos / (s->params.blocks_per_plane*2);
            int stripe_no = (bitpos/2) % s->params.blocks_per_plane;
            int offset = bitpos % 2;

            block = plane_pair * (s->params.blocks_per_plane*2)  + \
                                                        (stripe_no * 2) + \
                                                        offset;
        }
        break;

        case PLANE_BLOCKS_FULL_STRIPE:
        {
            int stripe_no = bitpos % s->params.blocks_per_plane;
            int col_no = bitpos / s->params.blocks_per_plane;
            block = (stripe_no * s->params.planes_per_pkg) + col_no;
        }
        break;

        default:
            fprintf(stderr, "Error: unknown plane_block_mapping %d\n", s->params.plane_block_mapping);
            exit(1);
    }

    return block;
}

int ssd_block_to_bitpos(ssd_t *currdisk, int block)
{
    int bitpos = -1;

    // mark the bit position as allocated.
    // we need to first find the bit that corresponds to the block
    switch(currdisk->params.plane_block_mapping) {
        case PLANE_BLOCKS_CONCAT:
            bitpos = block;
        break;

        case PLANE_BLOCKS_PAIRWISE_STRIPE:
        {
            int block_index = block % (2*currdisk->params.blocks_per_plane);
            int pairs_to_skip = block / (2*currdisk->params.blocks_per_plane);
            int planes_to_skip = block % 2;
            int blocks_to_skip = block_index / 2;

            bitpos = pairs_to_skip * (2*currdisk->params.blocks_per_plane) + \
                        planes_to_skip * currdisk->params.blocks_per_plane + \
                        blocks_to_skip;
        }
        break;

        case PLANE_BLOCKS_FULL_STRIPE:
        {
            int planes_to_skip = block % currdisk->params.planes_per_pkg;
            int blocks_to_skip = block / currdisk->params.planes_per_pkg;
            bitpos = planes_to_skip * currdisk->params.blocks_per_plane + blocks_to_skip;
        }
        break;

        default:
            fprintf(stderr, "Error: unknown plane_block_mapping %d\n",
                currdisk->params.plane_block_mapping);
            exit(1);
    }

    ASSERT((bitpos >= 0) && (bitpos < currdisk->params.blocks_per_element));

    return bitpos;
}

int ssd_bitpos_to_plane(int bitpos, ssd_t *s)
{
    return (bitpos / s->params.blocks_per_plane);
}


/*
 * writes a single page into the active block of a ssd element.
 * this code assumes that there is a valid active page where the write can go
 * without invoking new cleaning.
 */
double _ssd_write_page_osr(ssd_t *s, ssd_element_metadata *metadata, int lpn)
{
    double cost;
    unsigned int active_page = metadata->active_page;
    unsigned int active_block = SSD_PAGE_TO_BLOCK(active_page, s);
    unsigned int pagepos_in_block = active_page % s->params.pages_per_block;
    unsigned int active_plane = metadata->block_usage[active_block].plane_num;

    // see if this logical page no is already mapped.
    if (metadata->lba_table[lpn] != -1) {

        // since the lpn is going to be written to a new location,
        // its previous copy is invalid now. therefore reduce the block
        // usage of the previous copy's block.

        unsigned int prev_page = metadata->lba_table[lpn];
        unsigned int prev_block = SSD_PAGE_TO_BLOCK(prev_page, s);
        unsigned int pagepos_in_prev_block = prev_page % s->params.pages_per_block;
        unsigned int prev_plane = metadata->block_usage[prev_block].plane_num;

        // make sure the version numbers are correct
        //ssd_assert_page_version(prev_page, active_page, metadata, s);

        if (metadata->block_usage[prev_block].page[pagepos_in_prev_block] != lpn) {
            fprintf(stderr, "Error: lpn %d not found in prev block %d pos %d\n",
                lpn, prev_block, pagepos_in_prev_block);
            ASSERT(0);
        } else {
#ifdef PN_SSD
            if (metadata->block_usage[prev_block].nBlocktype == NAND_TYPE) {
                metadata->block_usage[active_block].log_read_count += (metadata->block_usage[prev_block].log_read_count / metadata->block_usage[prev_block].num_valid);
                metadata->block_usage[prev_block].log_read_count   -= (metadata->block_usage[prev_block].log_read_count / metadata->block_usage[prev_block].num_valid);
#endif
                metadata->block_usage[prev_block].page[pagepos_in_prev_block] = -1;
                metadata->block_usage[prev_block].num_valid --;
                metadata->plane_meta[prev_plane].valid_pages --;
                ssd_assert_valid_pages(prev_plane, metadata, s);
#ifdef PAGE_MIG
                metadata->block_usage[prev_block].num_read_count -= metadata->block_usage[prev_block].page_read_count[pagepos_in_prev_block];
                metadata->block_usage[prev_block].page_read_count[pagepos_in_prev_block] = 0;
#endif
#ifdef PN_SSD
            } else {
                cost = s->params.pcm_write_latency;
                s->stat.tot_pcm_write_count ++;
                return cost;                
            }
#endif
        }
    } 

    // add the entry to the lba table
    metadata->lba_table[lpn] = active_page;

    // increment the usage count on the active block
    metadata->block_usage[active_block].page[pagepos_in_block] = lpn;
    metadata->block_usage[active_block].num_valid ++;
    metadata->plane_meta[active_plane].valid_pages ++;
    ssd_assert_valid_pages(active_plane, metadata, s);

    // some sanity checking
    if (metadata->block_usage[active_block].num_valid >= s->params.pages_per_block) {
        fprintf(stderr, "Error: len %d of block %d is greater than or equal to pages per block %d\n",
            metadata->block_usage[active_block].num_valid, active_block, s->params.pages_per_block);
        exit(1);
    }

    // add the cost of the write
#ifdef PN_SSD
    s->stat.tot_nand_write_count ++;
#endif

#ifdef PN_SSD
    if (pagepos_in_block % 3 == 0)
        cost = s->params.lsb_write_latency;
    else if (pagepos_in_block % 3 == 1)
        cost = s->params.csb_write_latency;
    else
        cost = s->params.msb_write_latency;
#else
    cost = s->params.page_write_latency;
#endif
    //printf("lpn %d active pg %d\n", lpn, active_page);

    // go to the next free page
    metadata->active_page = active_page + 1;
    metadata->plane_meta[active_plane].active_page = metadata->active_page;

    // if this is the last data page on the block, let us write the
    // summary page also
    if (ssd_last_page_in_block(metadata->active_page, s)) {
        // cost of transferring the summary page data
        cost += ssd_data_transfer_cost(s, SSD_SECTORS_PER_SUMMARY_PAGE);

        // cost of writing the summary page data
#ifdef PN_SSD
        cost += s->params.lsb_write_latency;
#else
        cost += s->params.page_write_latency;
#endif

        // seal the last summary page. since we use the summary page
        // as a metadata, we don't count it as a valid data page.
        metadata->block_usage[active_block].page[s->params.pages_per_block - 1] = -1;
        metadata->block_usage[active_block].state = SSD_BLOCK_SEALED;
        //printf("SUMMARY: lpn %d active pg %d\n", lpn, active_page);
    }
    return cost;
}

/*
 * description: this routine finds the next free block inside a ssd
 * element/plane and returns it for writing. this routine can be called
 * during cleaning and therefore, must be guaranteed to find a free block
 * without invoking any further cleaning (otherwise there will be a
 * circular dep). we can ensure this by invoking the cleaning algorithm
 * when the num of free blocks is high enough to help in cleaning.
 */
void _ssd_alloc_active_block(int plane_num, int elem_num, ssd_t *s)
{
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);
    unsigned char *free_blocks = metadata->free_blocks;
    int active_block = -1;
    int prev_pos;
    int bitpos;

    if (plane_num != -1) {
        prev_pos = metadata->plane_meta[plane_num].block_alloc_pos;
    } else {
        prev_pos = metadata->block_alloc_pos;
    }

    // find a free bit
    bitpos = ssd_find_zero_bit(free_blocks, s->params.blocks_per_element, prev_pos);
    ASSERT((bitpos >= 0) && (bitpos < s->params.blocks_per_element));

    // check if we found the free bit in the plane we wanted to
    if (plane_num != -1) {
        if (ssd_bitpos_to_plane(bitpos, s) != plane_num) {
            //printf("Error: We wanted a free block in plane %d but found another in %d\n",
            //  plane_num, ssd_bitpos_to_plane(bitpos, s));
            //printf("So, starting the search again for plane %d\n", plane_num);

            // start from the beginning
            metadata->plane_meta[plane_num].block_alloc_pos = plane_num * s->params.blocks_per_plane;
            prev_pos = metadata->plane_meta[plane_num].block_alloc_pos;
            bitpos = ssd_find_zero_bit(free_blocks, s->params.blocks_per_element, prev_pos);
            ASSERT((bitpos >= 0) && (bitpos < s->params.blocks_per_element));

            if (ssd_bitpos_to_plane(bitpos, s) != plane_num) {
                printf("Error: this plane %d is full\n", plane_num);
                printf("this case is not yet handled\n");
                exit(1);
            }
        }

        metadata->plane_meta[plane_num].block_alloc_pos = \
            (plane_num * s->params.blocks_per_plane) + ((bitpos+1) % s->params.blocks_per_plane);
    } else {
        metadata->block_alloc_pos = (bitpos+1) % s->params.blocks_per_element;
    }

    // find the block num
    active_block = ssd_bitpos_to_block(bitpos, s);

    if (active_block != -1) {
        plane_metadata *pm;

        // make sure we're doing the right thing
        ASSERT(metadata->block_usage[active_block].plane_num == ssd_bitpos_to_plane(bitpos, s));
        ASSERT(metadata->block_usage[active_block].bsn == 0);
        ASSERT(metadata->block_usage[active_block].num_valid == 0);
        ASSERT(metadata->block_usage[active_block].state == SSD_BLOCK_CLEAN);

        if (plane_num == -1) {
            plane_num = metadata->block_usage[active_block].plane_num;
        } else {
            ASSERT(plane_num == metadata->block_usage[active_block].plane_num);
        }

        pm = &metadata->plane_meta[plane_num];

        // reduce the total number of free blocks
        metadata->tot_free_blocks --;
        pm->free_blocks --;

        //assertion
        ssd_assert_free_blocks(s, metadata);

        // allocate the block
        ssd_set_bit(free_blocks, bitpos);
        metadata->block_usage[active_block].state = SSD_BLOCK_INUSE;
        metadata->block_usage[active_block].bsn = metadata->bsn ++;

        // start from the first page of the active block
        pm->active_page = active_block * s->params.pages_per_block;
        metadata->active_page = pm->active_page;

        //ssd_assert_plane_freebits(plane_num, elem_num, metadata, s);
    } else {
        fprintf(stderr, "Error: cannot find a free block in ssd element %d\n", elem_num);
        exit(-1);
    }
}

listnode **ssd_pick_parunits(ssd_req **reqs, int total, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int i;
    int lpn;
    int prev_page;
    int prev_block;
    int plane_num;
    int parunit_num;
    listnode **parunits;
    int filled = 0;

    // parunits is an array of linked list structures, where
    // each entry in the array corresponds to one parallel unit
    parunits = (listnode **)malloc(SSD_PARUNITS_PER_ELEM(s) * sizeof(listnode *));
    for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
        ll_create(&parunits[i]);
    }

    // first, fill in the reads
    for (i = 0; i < total; i ++) {
        if (reqs[i]->is_read) {
            // get the logical page number corresponding to this blkno
            lpn = ssd_logical_pageno(reqs[i]->blk, s);
            prev_page = metadata->lba_table[lpn];
            ASSERT(prev_page != -1);
            prev_block = SSD_PAGE_TO_BLOCK(prev_page, s);
            plane_num = metadata->block_usage[prev_block].plane_num;
            parunit_num = metadata->plane_meta[plane_num].parunit_num;
            reqs[i]->plane_num = plane_num;
            ll_insert_at_tail(parunits[parunit_num], (void*)reqs[i]);
            filled ++;
        }
    }

    // if all the reqs are reads, return
    if (filled == total) {
        return parunits;
    }

    for (i = 0; i < total; i ++) {

        // we need to find planes for the writes
        if (!reqs[i]->is_read) {
            int j;
            int prev_bsn = -1;
            int min_valid;

            plane_num = -1;
            lpn = ssd_logical_pageno(reqs[i]->blk, s);
            prev_page = metadata->lba_table[lpn];
            ASSERT(prev_page != -1);
            prev_block = SSD_PAGE_TO_BLOCK(prev_page, s);
            prev_bsn = metadata->block_usage[prev_block].bsn;

            if (s->params.alloc_pool_logic == SSD_ALLOC_POOL_PLANE) {
                plane_num = metadata->block_usage[prev_block].plane_num;
            } else {
                // find a plane with the max no of free blocks
                j = metadata->plane_to_write;
                do {
                    int active_block;
                    int active_bsn;
                    plane_metadata *pm = &metadata->plane_meta[j];

                    active_block = SSD_PAGE_TO_BLOCK(pm->active_page, s);
                    active_bsn = metadata->block_usage[active_block].bsn;

                    // see if we can write to this block
                    if ((active_bsn > prev_bsn) ||
                        ((active_bsn == prev_bsn) && (pm->active_page > (unsigned int)prev_page))) {
                        int free_pages_in_act_blk;
                        int k;
                        int p;
                        int tmp;
                        int size;
                        int additive = 0;

                        p = metadata->plane_meta[j].parunit_num;
                        size = ll_get_size(parunits[p]);

                        // check if this plane has been already selected for writing
                        // in this parallel unit
                        for (k = 0; k < size; k ++) {
                            ssd_req *r;
                            listnode *n = ll_get_nth_node(parunits[p], k);
                            ASSERT(n != NULL);

                            r = ((ssd_req *)n->data);

                            // is this plane has been already selected for writing?
                            if ((r->plane_num == j) &&
                                (!r->is_read)) {
                                additive ++;
                            }
                        }

                        // select a plane with the most no of free pages
                        free_pages_in_act_blk = s->params.pages_per_block - ((pm->active_page%s->params.pages_per_block) + additive);
                        tmp = pm->free_blocks * s->params.pages_per_block + free_pages_in_act_blk;

                        if (plane_num == -1) {
                            // this is the first plane that satisfies the above criterion
                            min_valid = tmp;
                            plane_num = j;
                        } else {
                            if (min_valid < tmp) {
                                min_valid = tmp;
                                plane_num = j;
                            }
                        }
                    }

                    // check out the next plane
                    j = (j+1) % s->params.planes_per_pkg;
                } while (j != metadata->plane_to_write);
            }

            if (plane_num != -1) {
                // start searching from the next plane
                metadata->plane_to_write = (plane_num+1) % s->params.planes_per_pkg;

                reqs[i]->plane_num = plane_num;
                parunit_num = metadata->plane_meta[plane_num].parunit_num;
                ll_insert_at_tail(parunits[parunit_num], (void *)reqs[i]);
                filled ++;
            } else {
                fprintf(stderr, "Error: cannot find a plane to write\n");
                exit(1);
            }
        }
    }

    ASSERT(filled == total);

    return parunits;
}

static double ssd_issue_overlapped_ios(ssd_req **reqs, int total, int elem_num, ssd_t *s)
{
    double max_cost = 0;
    double parunit_op_cost[SSD_MAX_PARUNITS_PER_ELEM];
    double parunit_tot_cost[SSD_MAX_PARUNITS_PER_ELEM];
    ssd_element_metadata *metadata;
    int lpn;
    int i;
    int read_cycle = 0;
    listnode **parunits;

#ifdef PN_SSD
    int ppage;
    int ppage_pos;
    int read_block;
#endif

    // all the requests must be of the same type
    for (i = 1; i < total; i ++) {
        ASSERT(reqs[i]->is_read == reqs[0]->is_read);
    }

    // is this a set of read requests?
    if (reqs[0]->is_read) {
        read_cycle = 1;
    }

    memset(parunit_tot_cost, 0, sizeof(double)*SSD_MAX_PARUNITS_PER_ELEM);

    // find the planes to which the reqs are to be issued
    metadata = &(s->elements[elem_num].metadata);
    parunits = ssd_pick_parunits(reqs, total, elem_num, metadata, s);

    // repeat until we've served all the requests
    while (1) {
        //double tot_xfer_cost = 0;
        double max_op_cost = 0;
        int active_parunits = 0;
        int op_count = 0;

        // do we still have any request to service?
        for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
            if (ll_get_size(parunits[i]) > 0) {
                active_parunits ++;
            }
        }

        // no more requests -- get out
        if (active_parunits == 0) {
            break;
        }

        // clear this arrays for storing costs
        memset(parunit_op_cost, 0, sizeof(double)*SSD_MAX_PARUNITS_PER_ELEM);

        // begin a round of serving. we serve one request per
        // parallel unit. if an unit has more than one request
        // in the list, they have to be serialized.
        max_cost = 0;
        for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
            int size;

            size = ll_get_size(parunits[i]);
            if (size > 0) {
                // this parallel unit has a request to serve
                ssd_req *r;
                listnode *n = ll_get_nth_node(parunits[i], 0);

                op_count ++;
                ASSERT(op_count <= active_parunits);

                // get the request
                r = (ssd_req *)n->data;
                lpn = ssd_logical_pageno(r->blk, s);
#ifdef PN_SSD
                ppage      = metadata->lba_table[lpn];
                read_block = SSD_PAGE_TO_BLOCK(ppage, s);
#ifdef PAGE_MIG
                ppage_pos  = ppage % s->params.pages_per_block;
#endif
#endif
                if (r->is_read) {
#ifdef PN_SSD
                    metadata->block_usage[read_block].num_read_count++;
                    metadata->block_usage[read_block].log_read_count++;
#ifdef PAGE_MIG
                    metadata->block_usage[read_block].page_read_count[ppage_pos]++;
#endif
                    if(metadata->block_usage[read_block].nBlocktype == NAND_TYPE) { 
                        if(metadata->block_usage[read_block].num_read_count >= s->params.read_threshold) {
#ifndef RIA
                            parunit_op_cost[i] += read_reclaim(s, metadata, elem_num, r->plane_num, read_block);
                            metadata->block_usage[read_block].erase_cnt++;
#else
                            double rosa_mig_cost = rosa_mig(s, metadata, elem_num, r->plane_num, read_block, RIA_MIG);
                            if(rosa_mig_cost != 0)
                                parunit_op_cost[i] += rosa_mig_cost;
                            else
                                parunit_op_cost[i] += read_reclaim(s, metadata, elem_num, r->plane_num, read_block);

                            metadata->block_usage[read_block].erase_cnt++;
#endif
                        } else {
                            rosa_table_add(s, read_block, metadata);
                        }
                    }

                    if(metadata->block_usage[read_block].nBlocktype == NAND_TYPE) {
                        if (ppage % 3 == 0)
                            parunit_op_cost[i] += s->params.lsb_read_latency;
                        else if (ppage % 3 == 1)
                            parunit_op_cost[i] += s->params.csb_read_latency;
                        else
                            parunit_op_cost[i] += s->params.msb_read_latency;

                        s->stat.tot_nand_read_count ++;
                    } else {
                        parunit_op_cost[i] += s->params.pcm_read_latency;
                        s->stat.tot_pcm_read_count ++;
                    }
#else
                    parunit_op_cost[i] = s->params.page_read_latency;
#endif
                } else {
                    int plane_num = r->plane_num;
                    // if this is the last page on the block, allocate a new block
                    if (ssd_last_page_in_block(metadata->plane_meta[plane_num].active_page, s)) {
                        _ssd_alloc_active_block(plane_num, elem_num, s);
                    }

                    // issue the write to the current active page.
                    // we need to transfer the data across the serial pins for write.
                    metadata->active_page = metadata->plane_meta[plane_num].active_page;
                    //printf("elem %d plane %d ", elem_num, plane_num);
                    parunit_op_cost[i] = _ssd_write_page_osr(s, metadata, lpn);
                }

                ASSERT(r->count <= s->params.page_size);

                // calc the cost: the access time should be something like this
                // for read
                if (read_cycle) {
                    if (SSD_PARUNITS_PER_ELEM(s) > 4) {
                        printf("modify acc time here ...\n");
                        ASSERT(0);
                    }
                    if (op_count == 1) {
                        r->acctime = parunit_op_cost[i] + ssd_data_transfer_cost(s,s->params.page_size);
                        r->schtime = parunit_tot_cost[i] + (op_count-1)*ssd_data_transfer_cost(s,s->params.page_size) + r->acctime;
                    } else {
                        r->acctime = ssd_data_transfer_cost(s,s->params.page_size);
                        r->schtime = parunit_tot_cost[i] + op_count*ssd_data_transfer_cost(s,s->params.page_size) + parunit_op_cost[i];
                    }
                } else {
                    // for write
                    r->acctime = parunit_op_cost[i] + ssd_data_transfer_cost(s,s->params.page_size);
                    r->schtime = parunit_tot_cost[i] + (op_count-1)*ssd_data_transfer_cost(s,s->params.page_size) + r->acctime;
                }


                // find the maximum cost for this round of operations
                if (max_cost < r->schtime) {
                    max_cost = r->schtime;
                }

                // release the node from the linked list
                ll_release_node(parunits[i], n);
            }
        }

        // we can start the next round of operations only after all
        // the operations in the first round are over because we're
        // limited by the one set of pins to all the parunits
        for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
            parunit_tot_cost[i] = max_cost;
        }
    }

    for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
        ll_release(parunits[i]);
    }
    free(parunits);

    return max_cost;
}

static double ssd_write_one_active_page(int blkno, int count, int elem_num, ssd_t *s)
{
    double cost = 0;
    int cleaning_invoked = 0;
    ssd_element_metadata *metadata;
    int lpn;

    metadata = &(s->elements[elem_num].metadata);

    // get the logical page number corresponding to this blkno
    lpn = ssd_logical_pageno(blkno, s);

    // see if there are any free pages left inside the active block.
    // as per the osr design, the last page is used as a summary page.
    // so if the active_page is already pointing to the summary page,
    // then we need to find another free block to act as active block.
    if (ssd_last_page_in_block(metadata->active_page, s)) {

        // do we need to create more free blocks for future writes?
        if (ssd_start_cleaning(-1, elem_num, s)) {

            printf ("We should not clean here ...\n");
            ASSERT(0);

            // if we're cleaning in the background, this should
            // not get executed
            if (s->params.cleaning_in_background) {
                exit(1);
            }

            cleaning_invoked = 1;
            cost += ssd_clean_element_no_copyback(elem_num, s);
        }

        // if we had invoked the cleaning, we must again check if we
        // need an active block before allocating one. this check is
        // needed because the above cleaning procedure might have
        // allocated new active blocks during the process of cleaning,
        // which might still have free pages for writing.
        if (!cleaning_invoked ||
            ssd_last_page_in_block(metadata->active_page, s)) {
            _ssd_alloc_active_block(-1, elem_num, s);
        }
    }


    // issue the write to the current active page
    cost += _ssd_write_page_osr(s, metadata, lpn);
    cost += ssd_data_transfer_cost(s, count);

    return cost;
}

/*
 * computes the time taken to read/write data in a flash element
 * using just one active page.
 */
static void ssd_compute_access_time_one_active_page
(ssd_req **reqs, int total, int elem_num, ssd_t *s)
{
    // we assume that requests have been broken down into page sized chunks
    double cost;
    int blkno;
    int count;
    int is_read;

    // we can serve only one request at a time
    ASSERT(total == 1);

    blkno = reqs[0]->blk;
    count = reqs[0]->count;
    is_read = reqs[0]->is_read;

    if (is_read) {
        // there are no separate read policies.
        switch(s->params.write_policy) {
            case DISKSIM_SSD_WRITE_POLICY_SIMPLE:
            case DISKSIM_SSD_WRITE_POLICY_OSR:
                // the cost of one page read
                cost = ssd_read_policy_simple(count, s);
                break;

            default:
                fprintf(stderr, "Error: unknown write policy %d in ssd_compute_access_time\n",
                    s->params.write_policy);
                exit(1);
        }
    } else {
        switch(s->params.write_policy) {
            case DISKSIM_SSD_WRITE_POLICY_SIMPLE:
                cost = ssd_write_policy_simple(blkno, count, elem_num, s);
                break;

            case DISKSIM_SSD_WRITE_POLICY_OSR:
                cost = ssd_write_one_active_page(blkno, count, elem_num, s);
                break;

            default:
                fprintf(stderr, "Error: unknown write policy %d in ssd_compute_access_time\n",
                    s->params.write_policy);
                exit(1);
        }
    }

    reqs[0]->acctime = cost;
    reqs[0]->schtime = cost;
}

/*
 * with multiple planes, we can support either single active page or multiple
 * active pages.
 *
 * one-active-page-per-element: we can just have one active page per
 * element and move this active page to different blocks whenever a block
 * gets full. this is the basic osr design.
 *
 * one-active-page-per-plane: since each ssd element consists of multiple
 * parallel planes, a more dynamic design would have one active page
 * per plane. but this slightly complicates the design due to page versions.
 *
 * page versions are identified using a block sequence number (bsn) and page
 * number. bsn is a monotonically increasing num stored in the first page of
 * each block. so, if a same logical page 'p' is stored on two different physical
 * pages P and P', we find the latest version by comparing <BSN(P), P>
 * and <BSN(P'), P'>. in the simple, one-active-page-per-element design
 * whenever a block gets full and the active page is moved to a different
 * block, the bsn is increased. this way, we can always ensure that the latest
 * copy of a page will have its <BSN, P'> higher than its previous versions
 * (because BSN is monotonically increasing and physical pages in a block are
 * written only in increasing order). however, with multiple active pages say,
 * P0 to Pi with their corresponding bsns BSN0 to BSNi, we have to make sure
 * that the invariant is still maintained.
 */
void ssd_compute_access_time(ssd_t *s, int elem_num, ssd_req **reqs, int total)
{
    if (total == 0) {
        return;
    }

    if (s->params.copy_back == SSD_COPY_BACK_DISABLE) {
        ssd_compute_access_time_one_active_page(reqs, total, elem_num, s);
    } else {
        ssd_issue_overlapped_ios(reqs, total, elem_num, s);
    }
}

static void ssd_free_timing_element(ssd_timing_t *t)
{
    free(t);
}

ssd_timing_t  *ssd_new_timing_t(ssd_timing_params *params)
{
    int i;
    struct my_timing_t *tt = malloc(sizeof(struct my_timing_t));
    tt->params = params;
    tt->t.choose_element = ssd_choose_element;
    //tt->t.choose_aligned_count = ssd_choose_aligned_count;
    tt->t.free = ssd_free_timing_element;
    for (i=0; i<SSD_MAX_ELEMENTS; i++)
        tt->next_write_page[i] = -1;
    return &tt->t;
}
