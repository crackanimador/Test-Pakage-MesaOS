/**
 * @file inject-file.c
 * @brief Inyecta un archivo en MesaFS (compatible con MesaOS)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE             512
#define MESAFS_MAGIC            0x4D455341  /* "MESA" */
#define MESAFS_BLOCK_SIZE       4096
#define MESAFS_TYPE_FILE        1
#define MESAFS_TYPE_DIR         2
#define MESAFS_FLAG_USED        0x01
#define MESAFS_MAX_FILENAME     56
#define MESAFS_DIRECT_BLOCKS    10

#define MESAFS_BLOCK_BITMAP_BLOCK   0
#define MESAFS_INODE_BITMAP_BLOCK   1
#define MESAFS_INODE_TABLE_START    2
#define MESAFS_DATA_START           10

/* Superbloque */
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

/* Inodo (128 bytes) */
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

/* Entrada de directorio (64 bytes) */
typedef struct {
    uint32_t inode;
    uint8_t  type;
    uint8_t  name_len;
    char     name[58];
} __attribute__((packed)) mesafs_dirent_t;

static FILE *disk_fp = NULL;
static uint32_t part_offset = 0;

static void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static int bitmap_test(uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

static int read_block(uint32_t block_num, void *buf) {
    fseek(disk_fp, part_offset + block_num * MESAFS_BLOCK_SIZE, SEEK_SET);
    return fread(buf, 1, MESAFS_BLOCK_SIZE, disk_fp) == MESAFS_BLOCK_SIZE ? 0 : -1;
}

static int write_block(uint32_t block_num, const void *buf) {
    fseek(disk_fp, part_offset + block_num * MESAFS_BLOCK_SIZE, SEEK_SET);
    return fwrite(buf, 1, MESAFS_BLOCK_SIZE, disk_fp) == MESAFS_BLOCK_SIZE ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        printf("Usage: %s <disk.img> <source-file> <dest-path>\n", argv[0]);
        printf("Example: %s disk.img hello.msa /hello.msa\n", argv[0]);
        return 1;
    }
    
    const char *disk_path = argv[1];
    const char *source_file = argv[2];
    const char *dest_path = argv[3];
    
    /* Abrir disco */
    disk_fp = fopen(disk_path, "r+b");
    if (!disk_fp) {
        perror("Cannot open disk");
        return 1;
    }
    
    /* Buscar partición MesaFS */
    uint8_t mbr[512];
    fread(mbr, 1, 512, disk_fp);
    
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
        fclose(disk_fp);
        return 1;
    }
    
    part_offset = part_lba * SECTOR_SIZE;
    printf("Found MesaFS partition at LBA %u (offset %u)\n", part_lba, part_offset);
    
    /* Leer superblock (primeros 512 bytes del bloque 0) */
    uint8_t block[MESAFS_BLOCK_SIZE];
    if (read_block(0, block) != 0) {
        printf("Failed to read superblock\n");
        fclose(disk_fp);
        return 1;
    }
    
    mesafs_superblock_t *sb = (mesafs_superblock_t *)block;
    
    if (sb->magic != MESAFS_MAGIC) {
        printf("Invalid MesaFS magic: 0x%08X (expected 0x%08X)\n", sb->magic, MESAFS_MAGIC);
        fclose(disk_fp);
        return 1;
    }
    
    printf("MesaFS: %u blocks, %u free, %u inodes, %u free\n",
           sb->total_blocks, sb->free_blocks, sb->total_inodes, sb->free_inodes);
    
    /* Guardar copia del superblock para actualizar después */
    mesafs_superblock_t sb_copy = *sb;
    
    /* Leer bitmaps */
    /* Block bitmap está en bloque 0, pero superblock usa primeros 512 bytes */
    /* Los bits de bitmap empiezan después del superblock en el mismo bloque */
    uint8_t *block_bitmap = block;  /* Reutilizamos el bloque 0 */
    
    uint8_t inode_bitmap[MESAFS_BLOCK_SIZE];
    if (read_block(MESAFS_INODE_BITMAP_BLOCK, inode_bitmap) != 0) {
        printf("Failed to read inode bitmap\n");
        fclose(disk_fp);
        return 1;
    }
    
    /* Leer archivo fuente */
    FILE *src = fopen(source_file, "rb");
    if (!src) {
        perror("Cannot open source file");
        fclose(disk_fp);
        return 1;
    }
    
    fseek(src, 0, SEEK_END);
    long file_size = ftell(src);
    fseek(src, 0, SEEK_SET);
    
    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        perror("malloc");
        fclose(src);
        fclose(disk_fp);
        return 1;
    }
    fread(file_data, 1, file_size, src);
    fclose(src);
    
    printf("Source file: %s (%ld bytes)\n", source_file, file_size);
    
    /* Extraer nombre del archivo */
    const char *filename = dest_path;
    if (dest_path[0] == '/') filename++;
    
    /* Calcular bloques necesarios */
    uint32_t blocks_needed = (file_size + MESAFS_BLOCK_SIZE - 1) / MESAFS_BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1;
    
    if (blocks_needed > MESAFS_DIRECT_BLOCKS) {
        printf("File too large (max %d blocks = %d bytes)\n", 
               MESAFS_DIRECT_BLOCKS, MESAFS_DIRECT_BLOCKS * MESAFS_BLOCK_SIZE);
        free(file_data);
        fclose(disk_fp);
        return 1;
    }
    
    /* Asignar inodo */
    uint32_t new_inode = 0;
    for (uint32_t i = 2; i < sb_copy.total_inodes; i++) {
        if (!bitmap_test(inode_bitmap, i)) {
            new_inode = i;
            bitmap_set(inode_bitmap, i);
            sb_copy.free_inodes--;
            break;
        }
    }
    
    if (new_inode == 0) {
        printf("No free inodes\n");
        free(file_data);
        fclose(disk_fp);
        return 1;
    }
    
    printf("Allocated inode: %u\n", new_inode);
    
    /* Asignar bloques de datos */
    uint32_t data_blocks[MESAFS_DIRECT_BLOCKS] = {0};
    uint32_t blocks_allocated = 0;
    
    for (uint32_t i = MESAFS_DATA_START + 1; i < sb_copy.total_blocks && blocks_allocated < blocks_needed; i++) {
        if (!bitmap_test(block_bitmap, i)) {
            data_blocks[blocks_allocated] = i;
            bitmap_set(block_bitmap, i);
            sb_copy.free_blocks--;
            blocks_allocated++;
        }
    }
    
    if (blocks_allocated < blocks_needed) {
        printf("Not enough free blocks (need %u, got %u)\n", blocks_needed, blocks_allocated);
        free(file_data);
        fclose(disk_fp);
        return 1;
    }
    
    printf("Allocated %u data blocks\n", blocks_allocated);
    
    /* Escribir datos del archivo */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < blocks_allocated; i++) {
        uint8_t data_block[MESAFS_BLOCK_SIZE];
        memset(data_block, 0, MESAFS_BLOCK_SIZE);
        
        uint32_t chunk = (file_size - offset > MESAFS_BLOCK_SIZE) ? MESAFS_BLOCK_SIZE : (file_size - offset);
        memcpy(data_block, file_data + offset, chunk);
        
        write_block(data_blocks[i], data_block);
        offset += chunk;
    }
    
    /* Crear inodo del archivo */
    uint32_t inode_block_num = MESAFS_INODE_TABLE_START + (new_inode / 32);
    uint32_t inode_index = new_inode % 32;
    
    uint8_t inode_block[MESAFS_BLOCK_SIZE];
    read_block(inode_block_num, inode_block);
    
    mesafs_inode_t *inodes = (mesafs_inode_t *)inode_block;
    memset(&inodes[inode_index], 0, sizeof(mesafs_inode_t));
    
    inodes[inode_index].inode_num = new_inode;
    inodes[inode_index].type = MESAFS_TYPE_FILE;
    inodes[inode_index].flags = MESAFS_FLAG_USED;
    inodes[inode_index].links = 1;
    inodes[inode_index].size = file_size;
    inodes[inode_index].blocks_used = blocks_allocated;
    
    for (uint32_t i = 0; i < blocks_allocated; i++) {
        inodes[inode_index].direct_blocks[i] = data_blocks[i];
    }
    
    write_block(inode_block_num, inode_block);
    
    /* Leer directorio raíz */
    /* Root inode es 1, su bloque de datos está en direct_blocks[0] */
    uint8_t root_inode_block[MESAFS_BLOCK_SIZE];
    read_block(MESAFS_INODE_TABLE_START, root_inode_block);
    mesafs_inode_t *root_inodes = (mesafs_inode_t *)root_inode_block;
    uint32_t root_dir_block = root_inodes[1].direct_blocks[0];
    
    printf("Root directory at block %u\n", root_dir_block);
    
    uint8_t dir_block[MESAFS_BLOCK_SIZE];
    read_block(root_dir_block, dir_block);
    
    /* Buscar slot libre */
    mesafs_dirent_t *entries = (mesafs_dirent_t *)dir_block;
    int max_entries = MESAFS_BLOCK_SIZE / sizeof(mesafs_dirent_t);
    int free_slot = -1;
    
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode == 0) {
            free_slot = i;
            break;
        }
    }
    
    if (free_slot < 0) {
        printf("Root directory full\n");
        free(file_data);
        fclose(disk_fp);
        return 1;
    }
    
    /* Agregar entrada */
    entries[free_slot].inode = new_inode;
    entries[free_slot].type = MESAFS_TYPE_FILE;
    entries[free_slot].name_len = strlen(filename);
    strncpy(entries[free_slot].name, filename, MESAFS_MAX_FILENAME);
    
    write_block(root_dir_block, dir_block);
    
    /* Actualizar superblock y bitmaps */
    memcpy(block, &sb_copy, sizeof(sb_copy));
    write_block(0, block);
    write_block(MESAFS_INODE_BITMAP_BLOCK, inode_bitmap);
    
    free(file_data);
    fclose(disk_fp);
    
    printf("\nFile injected successfully!\n");
    printf("  Inode: %u\n", new_inode);
    printf("  Blocks: %u\n", blocks_allocated);
    printf("  Size: %ld bytes\n", file_size);
    
    return 0;
}