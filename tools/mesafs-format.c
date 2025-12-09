/**
 * @file mesafs-format.c
 * @brief Formatea una partición como MesaFS (compatible con MesaOS)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE             512
#define MESAFS_MAGIC            0x4D455341  /* "MESA" - igual que MesaOS */
#define MESAFS_VERSION          1
#define MESAFS_BLOCK_SIZE       4096
#define MESAFS_TYPE_DIR         2
#define MESAFS_FLAG_USED        0x01

/* Layout igual que mesafs.h */
#define MESAFS_BLOCK_BITMAP_BLOCK   0
#define MESAFS_INODE_BITMAP_BLOCK   1
#define MESAFS_INODE_TABLE_START    2
#define MESAFS_INODE_TABLE_BLOCKS   8
#define MESAFS_DATA_START           10
#define MESAFS_DIRECT_BLOCKS        10

/* Superbloque (512 bytes, igual que MesaOS) */
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

/* Inodo (128 bytes, igual que MesaOS) */
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

static void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <disk.img>\n", argv[0]);
        return 1;
    }
    
    FILE *fp = fopen(argv[1], "r+b");
    if (!fp) {
        perror("Cannot open disk");
        return 1;
    }
    
    /* Leer MBR */
    uint8_t mbr[512];
    fread(mbr, 1, 512, fp);
    
    uint32_t part_lba = 0;
    uint32_t part_sectors = 0;
    
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = &mbr[446 + i * 16];
        uint8_t type = entry[4];
        if (type == 0x77) {
            part_lba = entry[8] | (entry[9] << 8) | (entry[10] << 16) | (entry[11] << 24);
            part_sectors = entry[12] | (entry[13] << 8) | (entry[14] << 16) | (entry[15] << 24);
            printf("Found MesaFS partition: LBA %u, %u sectors\n", part_lba, part_sectors);
            break;
        }
    }
    
    if (part_lba == 0) {
        printf("No MesaFS partition found (type 0x77)\n");
        fclose(fp);
        return 1;
    }
    
    uint32_t part_offset = part_lba * SECTOR_SIZE;
    uint32_t total_blocks = part_sectors / 8;  /* 8 sectores = 1 bloque */
    uint32_t total_inodes = 256;
    
    printf("Formatting MesaFS...\n");
    printf("  Partition offset: %u bytes (LBA %u)\n", part_offset, part_lba);
    printf("  Total blocks: %u\n", total_blocks);
    printf("  Block size: %d\n", MESAFS_BLOCK_SIZE);
    printf("  Data starts at block: %d\n", MESAFS_DATA_START);
    
    /* === Crear Superbloque === */
    mesafs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = MESAFS_MAGIC;
    sb.version = MESAFS_VERSION;
    sb.block_size = MESAFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.free_blocks = total_blocks - MESAFS_DATA_START - 1;  /* -1 para root dir */
    sb.total_inodes = total_inodes;
    sb.free_inodes = total_inodes - 2;  /* 0 reservado, 1 es root */
    sb.root_inode = 1;
    sb.first_data_block = MESAFS_DATA_START;
    
    /* Escribir superbloque en el primer sector de la partición */
    uint8_t sector[SECTOR_SIZE];
    memset(sector, 0, SECTOR_SIZE);
    memcpy(sector, &sb, sizeof(sb));
    fseek(fp, part_offset, SEEK_SET);
    fwrite(sector, 1, SECTOR_SIZE, fp);
    printf("  Superblock written at offset %u\n", part_offset);
    
    /* === Crear Block Bitmap (bloque 0, pero después del superblock sector) === */
    /* En MesaOS, read_block(0) lee desde partition_lba + 0*8 sectores */
    /* Pero el superblock está en partition_lba sector 0 */
    /* Esto significa que bloque 0 y superblock comparten espacio! */
    /* El superblock ocupa los primeros 512 bytes del bloque 0 */
    
    uint8_t block[MESAFS_BLOCK_SIZE];
    memset(block, 0, MESAFS_BLOCK_SIZE);
    
    /* El superblock ya está en los primeros 512 bytes */
    /* Ahora escribimos el bitmap de bloques en el resto del bloque 0? */
    /* NO - según el layout, bitmap está en bloque 0 completo */
    /* Pero superblock se lee con disk_read_sector(partition_lba) */
    /* Y bitmaps con read_block que hace partition_lba + block*8 */
    
    /* Vamos a escribir los bloques correctamente */
    /* Bloque 0 = bitmap de bloques (empieza en partition_lba) */
    memset(block, 0, MESAFS_BLOCK_SIZE);
    
    /* Marcar bloques 0-9 como usados (metadatos) */
    for (int i = 0; i < MESAFS_DATA_START; i++) {
        bitmap_set(block, i);
    }
    /* Marcar bloque 10 (primer bloque de datos) para root dir */
    bitmap_set(block, MESAFS_DATA_START);
    
    /* Pero espera - el superblock también está aquí! */
    /* Copiamos el superblock al inicio del bloque */
    memcpy(block, &sb, sizeof(sb));
    
    fseek(fp, part_offset + MESAFS_BLOCK_BITMAP_BLOCK * MESAFS_BLOCK_SIZE, SEEK_SET);
    fwrite(block, 1, MESAFS_BLOCK_SIZE, fp);
    printf("  Block bitmap written (block 0)\n");
    
    /* === Crear Inode Bitmap (bloque 1) === */
    memset(block, 0, MESAFS_BLOCK_SIZE);
    bitmap_set(block, 0);  /* Inodo 0 reservado */
    bitmap_set(block, 1);  /* Inodo 1 = root */
    
    fseek(fp, part_offset + MESAFS_INODE_BITMAP_BLOCK * MESAFS_BLOCK_SIZE, SEEK_SET);
    fwrite(block, 1, MESAFS_BLOCK_SIZE, fp);
    printf("  Inode bitmap written (block 1)\n");
    
    /* === Crear Inode Table (bloques 2-9) === */
    memset(block, 0, MESAFS_BLOCK_SIZE);
    
    /* Root inode está en el inodo 1 */
    /* Cada bloque tiene 32 inodos (4096/128) */
    /* Inodo 1 está en bloque 2, índice 1 */
    mesafs_inode_t *inodes = (mesafs_inode_t *)block;
    
    /* Inodo 0 - reservado (vacío) */
    
    /* Inodo 1 - root directory */
    inodes[1].inode_num = 1;
    inodes[1].type = MESAFS_TYPE_DIR;
    inodes[1].flags = MESAFS_FLAG_USED;
    inodes[1].links = 1;
    inodes[1].size = 0;
    inodes[1].blocks_used = 1;
    inodes[1].direct_blocks[0] = MESAFS_DATA_START;  /* Bloque 10 */
    
    fseek(fp, part_offset + MESAFS_INODE_TABLE_START * MESAFS_BLOCK_SIZE, SEEK_SET);
    fwrite(block, 1, MESAFS_BLOCK_SIZE, fp);
    printf("  Inode table written (block 2), root inode at index 1\n");
    
    /* Limpiar resto de bloques de inodos */
    memset(block, 0, MESAFS_BLOCK_SIZE);
    for (int b = 1; b < MESAFS_INODE_TABLE_BLOCKS; b++) {
        fseek(fp, part_offset + (MESAFS_INODE_TABLE_START + b) * MESAFS_BLOCK_SIZE, SEEK_SET);
        fwrite(block, 1, MESAFS_BLOCK_SIZE, fp);
    }
    
    /* === Crear Root Directory (bloque 10) === */
    memset(block, 0, MESAFS_BLOCK_SIZE);
    fseek(fp, part_offset + MESAFS_DATA_START * MESAFS_BLOCK_SIZE, SEEK_SET);
    fwrite(block, 1, MESAFS_BLOCK_SIZE, fp);
    printf("  Root directory written (block %d)\n", MESAFS_DATA_START);
    
    fclose(fp);
    
    printf("\nMesaFS formatted successfully!\n");
    printf("  Magic: 0x%08X\n", sb.magic);
    printf("  Total blocks: %u\n", sb.total_blocks);
    printf("  Free blocks: %u\n", sb.free_blocks);
    printf("  Root inode: %u\n", sb.root_inode);
    
    return 0;
}