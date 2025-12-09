/**
 * @file mesafs-list.c
 * @brief Lista archivos en MesaFS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE             512
#define MESAFS_MAGIC            0x4D455341
#define MESAFS_BLOCK_SIZE       4096
#define MESAFS_DIRECT_BLOCKS    10

#define MESAFS_INODE_TABLE_START    2
#define MESAFS_DATA_START           10

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint32_t first_data_block;
    uint8_t  reserved[476];
} __attribute__((packed)) mesafs_superblock_t;

typedef struct {
    uint32_t inode_num;
    uint8_t  type;
    uint8_t  flags;
    uint16_t links;
    uint32_t size;
    uint32_t blocks_used;
    uint32_t direct_blocks[MESAFS_DIRECT_BLOCKS];
    uint32_t indirect_block;
    uint64_t created;
    uint64_t modified;
    uint8_t  reserved[36];
} __attribute__((packed)) mesafs_inode_t;

typedef struct {
    uint32_t inode;
    uint8_t  type;
    uint8_t  name_len;
    char     name[58];
} __attribute__((packed)) mesafs_dirent_t;

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <disk.img>\n", argv[0]);
        return 1;
    }
    
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror("Cannot open disk");
        return 1;
    }
    
    uint8_t mbr[512];
    fread(mbr, 1, 512, fp);
    
    uint32_t part_lba = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = &mbr[446 + i * 16];
        if (entry[4] == 0x77) {
            part_lba = entry[8] | (entry[9] << 8) | (entry[10] << 16) | (entry[11] << 24);
            break;
        }
    }
    
    if (part_lba == 0) {
        printf("No MesaFS partition found\n");
        fclose(fp);
        return 1;
    }
    
    uint32_t part_offset = part_lba * SECTOR_SIZE;
    printf("Partition at LBA %u (offset %u)\n", part_lba, part_offset);
    
    /* Leer bloque 0 (contiene superblock) */
    uint8_t block[MESAFS_BLOCK_SIZE];
    fseek(fp, part_offset, SEEK_SET);
    fread(block, 1, MESAFS_BLOCK_SIZE, fp);
    
    mesafs_superblock_t *sb = (mesafs_superblock_t *)block;
    
    printf("\n=== Superblock ===\n");
    printf("Magic: 0x%08X %s\n", sb->magic, sb->magic == MESAFS_MAGIC ? "(OK)" : "(INVALID!)");
    printf("Version: %u\n", sb->version);
    printf("Block size: %u\n", sb->block_size);
    printf("Total blocks: %u\n", sb->total_blocks);
    printf("Free blocks: %u\n", sb->free_blocks);
    printf("Total inodes: %u\n", sb->total_inodes);
    printf("Free inodes: %u\n", sb->free_inodes);
    printf("Root inode: %u\n", sb->root_inode);
    printf("First data block: %u\n", sb->first_data_block);
    
    if (sb->magic != MESAFS_MAGIC) {
        fclose(fp);
        return 1;
    }
    
    /* Leer root inode */
    fseek(fp, part_offset + MESAFS_INODE_TABLE_START * MESAFS_BLOCK_SIZE, SEEK_SET);
    fread(block, 1, MESAFS_BLOCK_SIZE, fp);
    
    mesafs_inode_t *inodes = (mesafs_inode_t *)block;
    mesafs_inode_t *root = &inodes[sb->root_inode];
    
    printf("\n=== Root Inode (%u) ===\n", sb->root_inode);
    printf("Type: %u (2=DIR)\n", root->type);
    printf("Size: %u\n", root->size);
    printf("Blocks used: %u\n", root->blocks_used);
    printf("First block: %u\n", root->direct_blocks[0]);
    
    /* Leer directorio raíz */
    printf("\n=== Root Directory ===\n");
    
    fseek(fp, part_offset + root->direct_blocks[0] * MESAFS_BLOCK_SIZE, SEEK_SET);
    fread(block, 1, MESAFS_BLOCK_SIZE, fp);
    
    mesafs_dirent_t *entries = (mesafs_dirent_t *)block;
    int max_entries = MESAFS_BLOCK_SIZE / sizeof(mesafs_dirent_t);
    int count = 0;
    
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode != 0) {
            printf("  [%d] inode=%u type=%u name='%s'\n",
                   i, entries[i].inode, entries[i].type, entries[i].name);
            count++;
        }
    }
    
    if (count == 0) {
        printf("  (empty)\n");
    }
    
    printf("\nTotal: %d entries\n", count);
    
    fclose(fp);
    return 0;
}