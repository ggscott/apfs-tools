#include <stdio.h>
#include <sys/errno.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "apfs/io.h"
#include "apfs/func/boolean.h"
#include "apfs/func/cksum.h"

#include "apfs/struct/object.h"
#include "apfs/struct/nx.h"
#include "apfs/struct/omap.h"

#include "apfs/string/object.h"
#include "apfs/string/nx.h"
#include "apfs/string/omap.h"
#include "apfs/string/btree.h"

/**
 * Print usage info for this program.
 */
void print_usage(char* program_name) {
    printf("Usage:   %s <container>\nExample: %s /dev/disk0s2\n\n", program_name, program_name);
}

int main(int argc, char** argv) {
    printf("\n");

    // Extrapolate CLI arguments, exit if invalid
    if (argc != 2) {
        printf("Incorrect number of arguments.\n");
        print_usage(argv[0]);
        return 1;
    }
    nx_path = argv[1];
    
    // Open (device special) file corresponding to an APFS container, read-only
    printf("Opening file at `%s` in read-only mode ... ", nx_path);
    nx = fopen(nx_path, "rb");
    if (!nx) {
        fprintf(stderr, "\nABORT: ");
        report_fopen_error();
        printf("\n");
        return -errno;
    }
    printf("OK.\nSimulating a mount of the APFS container.\n");
    
    // Using `nx_superblock_t*`, but allocating a whole block of memory.
    // This way, we can read the entire block and validate its checksum,
    // but still have direct access to the fields in `nx_superblock_t`
    // without needing to epxlicitly cast to that datatype.
    nx_superblock_t* nxsb = malloc(nx_block_size);
    if (!nxsb) {
        fprintf(stderr, "ABORT: Could not allocate sufficient memory to create `block_buf`.\n");
        return -1;
    }

    if (read_blocks(nxsb, 0x0, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to successfully read block 0x0.\n");
        return -1;
    }

    printf("Validating checksum of block 0x0 ... ");
    if (is_cksum_valid(nxsb)) {
        printf("OK.\n");
    } else {
        printf("FAILED.\n!! APFS ERROR !! Checksum of block 0x0 should validate, but it doesn't. Proceeding as if it does.\n");
    }

    printf("\nDetails of block 0x0:\n");
    printf("--------------------------------------------------------------------------------\n");
    print_nx_superblock_info(nxsb);
    printf("--------------------------------------------------------------------------------\n");
    printf("\n");

    if (!is_nx_superblock(nxsb)) {
        printf("!! APFS ERROR !! Block 0x0 should be a container superblock, but it isn't. Proceeding as if it is.\n\n");
    }
    if (nxsb->nx_magic != NX_MAGIC) {
        printf("!! APFS ERROR !! Container superblock at 0x0 doesn't have the correct magic number. Proceeding as if it does.\n");
    }

    printf("Locating the checkpoint descriptor area:\n");
    
    uint32_t xp_desc_blocks = nxsb->nx_xp_desc_blocks & ~(1 << 31);
    printf("- Its length is %u blocks.\n", xp_desc_blocks);

    char (*xp_desc)[nx_block_size] = malloc(xp_desc_blocks * nx_block_size);
    if (!xp_desc) {
        fprintf(stderr, "ABORT: Could not allocate sufficient memory for %u blocks.\n", xp_desc_blocks);
        return -1;
    }

    if (nxsb->nx_xp_desc_blocks >> 31) {
        printf("- It is not contiguous.\n");
        printf("- The Physical OID of the B-tree representing it is 0x%llx.\n", nxsb->nx_xp_desc_base);
        printf("END: The ability to handle this case has not yet been implemented.\n\n");   // TODO: implement case when xp_desc area is not contiguous
        return 0;
    } else {
        printf("- It is contiguous.\n");
        printf("- The address of its first block is 0x%llx.\n", nxsb->nx_xp_desc_base);

        printf("Loading the checkpoint descriptor area into memory ... ");
        if (read_blocks(xp_desc, nxsb->nx_xp_desc_base, xp_desc_blocks) != xp_desc_blocks) {
            fprintf(stderr, "\nABORT: Failed to read all blocks in the checkpoint descriptor area.\n");
            return -1;
        }
        printf("OK.\n");
    }

    printf("Locating the most recent well-formed container superblock in the checkpoint descriptor area:\n");
    uint32_t i_latest_nx = 0;

    for (uint32_t i = 0; i < xp_desc_blocks; i++) {
        if (!is_cksum_valid(xp_desc[i])) {
            printf("- !! APFS WARNING !! Block at index %u within this area failed checksum validation. Skipping it.\n", i);
            continue;
        }
        
        if (is_nx_superblock(xp_desc[i])) {
            if ( ((nx_superblock_t*)xp_desc[i])->nx_magic  !=  NX_MAGIC ) {
                printf("- !! APFS WARNING !! Container superblock at index %u within this area is malformed; incorrect magic number. Skipping it.\n", i);
                continue;
            }

            if ( ((nx_superblock_t*)xp_desc[i])->nx_o.o_xid  >  ((nx_superblock_t*)xp_desc[i_latest_nx])->nx_o.o_xid ) {
                i_latest_nx = i;
            }
        } else if (!is_checkpoint_map_phys(xp_desc[i])) {
            printf("- !! APFS ERROR !! Block at index %u within this area is not a container superblock or checkpoint map. Skipping it.\n", i);
            continue;
        }
    }

    // Don't need a copy of the block 0x0 NXSB which is stored in `nxsb`
    // anymore; replace that data with the latest NXSB.
    // This also lets us avoid repeatedly casting to `nx_superblock_t*`.
    memcpy(nxsb, xp_desc[i_latest_nx], sizeof(nx_superblock_t));

    printf("- It lies at index %u within the checkpoint descriptor area.\n", i_latest_nx);

    printf("\nDetails of this container superblock:\n");
    printf("--------------------------------------------------------------------------------\n");
    print_nx_superblock_info(nxsb);
    printf("--------------------------------------------------------------------------------\n");
    printf("- The corresponding checkpoint starts at index %u within the checkpoint descriptor area, and spans %u blocks.\n\n", nxsb->nx_xp_desc_index, nxsb->nx_xp_desc_len);

    // Copy the contents of the checkpoint we are currently considering to its
    // own array for easy access. The checkpoint descriptor area is a ring
    // buffer stored as an array, so doing this also allows us to handle the
    // case where the checkpoint we're considering wraps around the ring buffer.
    printf("Loading the corresponding checkpoint ... ");
    
    // The array `xp` will comprise the blocks in the checkpoint, in order.
    char (*xp)[nx_block_size] = malloc(nxsb->nx_xp_desc_len * nx_block_size);
    if (!xp) {
        fprintf(stderr, "\nABORT: Couldn't allocate sufficient memory.\n");
        return -1;
    }

    if (nxsb->nx_xp_desc_index + nxsb->nx_xp_desc_len <= xp_desc_blocks) {
        // The simple case: the checkpoint is already contiguous in `xp_desc`.
        memcpy(xp, xp_desc[nxsb->nx_xp_desc_index], nxsb->nx_xp_desc_len * nx_block_size);
    } else {
        // The case where the checkpoint wraps around from the end of the
        // checkpoint descriptor area to the start.
        uint32_t segment_1_len = xp_desc_blocks - nxsb->nx_xp_desc_index;
        uint32_t segment_2_len = nxsb->nx_xp_desc_len - segment_1_len;
        memcpy(xp,                 xp_desc + nxsb->nx_xp_desc_index, segment_1_len * nx_block_size);
        memcpy(xp + segment_1_len, xp_desc,                          segment_2_len * nx_block_size);
    }
    printf("OK.\n");
    
    // We could `free(xp_desc)` at this point, but instead, we retain our copy
    // of the checkpoint descriptor area in case any of the Ephemeral objects
    // referenced by the current checkpoint are malformed; then, we can
    // retrieve an older checkpoint without having to read the checkpoint
    // descriptor area again.

    printf("\nDetails of each block in this checkpoint:\n");
    printf("--------------------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < nxsb->nx_xp_desc_len; i++) {
        if (is_nx_superblock(xp[i])) {
            print_nx_superblock_info(xp[i]);
        } else {
            assert(is_checkpoint_map_phys(xp[i]));
            print_checkpoint_map_phys_info(xp[i]);
        }
        printf("--------------------------------------------------------------------------------\n");
    }

    uint32_t xp_obj_len = 0;    // This variable will equal the number of
    // checkpoint-mappings = no. of Ephemeral objects used by this checkpoint.
    printf("\nDetails of each checkpoint-mapping in this checkpoint:\n");
    printf("--------------------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < nxsb->nx_xp_desc_len; i++) {
        if (is_checkpoint_map_phys(xp[i])) {
            print_checkpoint_map_phys_mappings(xp[i]);
            xp_obj_len += ((checkpoint_map_phys_t*)xp[i])->cpm_count;
        }
    }
    printf("- There are %u checkpoint-mappings in this checkpoint.\n\n", xp_obj_len);

    printf("Reading the Ephemeral objects used by this checkpoint ... ");
    char (*xp_obj)[nx_block_size] = malloc(xp_obj_len * nx_block_size);
    if (!xp_obj) {
        fprintf(stderr, "\nABORT: Could not allocate sufficient memory for `xp_obj`.\n");
        return -1;
    }
    uint32_t num_read = 0;
    for (uint32_t i = 0; i < nxsb->nx_xp_desc_len; i++) {
        if (is_checkpoint_map_phys(xp[i])) {
            checkpoint_map_phys_t* xp_map = xp[i];  // Avoid lots of casting
            for (uint32_t j = 0; j < xp_map->cpm_count; j++) {
                if (read_blocks(xp_obj[num_read], xp_map->cpm_map[j].cpm_paddr, 1) != 1) {
                    fprintf(stderr, "\nABORT: Failed to read block 0x%llx.\n", xp_map->cpm_map[j].cpm_paddr);
                    return -1;
                }
                num_read++;
            }
        }
    }
    printf("OK.\n");
    assert(num_read = xp_obj_len);

    printf("Validating the Ephemeral objects ... ");
    for (uint32_t i = 0; i < xp_obj_len; i++) {
        if (!is_cksum_valid(xp_obj[i])) {
            printf("FAILED.\n");
            printf("An Ephemeral object used by this checkpoint is malformed. Going back to look at the previous checkpoint instead.\n");
            
            // TODO: Handle case where data for a given checkpoint is malformed
            printf("END: Handling of this case has not yet been implemented.\n");
            return 0;
        }
    }
    printf("OK.\n");

    free(xp);
    free(xp_desc);

    printf("\nDetails of the Ephemeral objects:\n");
    printf("--------------------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < xp_obj_len; i++) {
        print_obj_hdr_info(xp_obj[i]);
        printf("--------------------------------------------------------------------------------\n");
    }
    printf("\n");

    printf("The container superblock states that the container object map has Physical OID 0x%llx.\n", nxsb->nx_omap_oid);

    printf("Loading the container object map ... ");
    omap_phys_t* nx_omap = malloc(nx_block_size);
    if (read_blocks(nx_omap, nxsb->nx_omap_oid, 1) != 1) {
        printf("\nABORT: Could not allocate sufficient memory for `nx_omap`.\n");
        return -1;
    }
    printf("OK.\n");
    
    printf("Validating the container object map ... ");
    if (!is_cksum_valid(nx_omap)) {
        printf("FAILED.\n");
        printf("This container object map is malformed. Going back to look at the previous checkpoint instead.\n");
        
        // TODO: Handle case where a given container object map is malformed
        printf("END: Handling of this case has not yet been implemented.\n");
        return 0;
    }
    printf("OK.\n");

    printf("\nDetails of the container object map:\n");
    printf("--------------------------------------------------------------------------------\n");
    print_omap_phys_info(nx_omap);
    printf("--------------------------------------------------------------------------------\n");
    printf("\n");

    if ((nx_omap->om_tree_type & OBJ_STORAGETYPE_MASK) != OBJ_PHYSICAL) {
        printf("END: The container object map B-tree is not of the Physical storage type, and therefore it cannot be located.\n");
        return 0;
    }

    printf("Reading the root node of the container object map B-tree ... ");
    char* nx_omap_btree = malloc(nx_block_size);
    if (!nx_omap_btree) {
        fprintf(stderr, "\nABORT: Could not allocate sufficient memory for `nx_omap_btree`.\n");
        return -1;
    }
    if (read_blocks(nx_omap_btree, nx_omap->om_tree_oid, 1) != 1) {
        fprintf(stderr, "\nABORT: Failed to read block 0x%llx.\n", nx_omap->om_tree_oid);
        return -1;
    }
    printf("OK.\n");

    printf("Validating the root node of the container object map B-tree ... ");
    if (!is_cksum_valid(nx_omap_btree)) {
        printf("FAILED.\n");
    } else {
        printf("OK.\n");
    }

    printf("\nDetails of the container object map B-tree:\n");
    printf("--------------------------------------------------------------------------------\n");
    print_btree_node_phys(nx_omap_btree);
    printf("--------------------------------------------------------------------------------\n");
    printf("\n");

    uint32_t num_file_systems = 0;
    for (uint32_t i = 0; i < NX_MAX_FILE_SYSTEMS; i++) {
        if (nxsb->nx_fs_oid[i] == 0) {
            break;
        }
        num_file_systems++;
    }
    printf("The container superblock lists %u APFS volumes, with the following Virtual OIDs:\n", num_file_systems);
    for (uint32_t i = 0; i < num_file_systems; i++) {
        printf("- 0x%llx\n", nxsb->nx_fs_oid[i]);
    }
    printf("\n");

    // Closing statements; de-allocate all memory, close all file descriptors.
    free(nx_omap_btree);
    free(nx_omap);
    free(xp_obj);
    free(nxsb);
    fclose(nx);
    printf("END: All done.\n");
    return 0;
}
