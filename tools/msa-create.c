/**
 * @file msa-create.c
 * @brief Herramienta para crear paquetes .msa para MesaOS
 * 
 * Compilar: gcc -o msa-create msa-create.c
 * Uso: ./msa-create <nombre> <version> <directorio> <salida.msa>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ==================== Constantes ==================== */

#define MSA_MAGIC           0x4153454D  /* "MESA" */
#define MSA_VERSION         1
#define MSA_NAME_MAX        64
#define MSA_PATH_MAX        256
#define MSA_DESC_MAX        256
#define MSA_MAX_FILES       256
#define MSA_MAX_DEPS        16

/* ==================== Estructuras (deben coincidir con MesaOS) ==================== */

typedef struct {
    uint32_t magic;                         /* MSA_MAGIC */
    uint32_t version;                       /* Versión del formato */
    char     name[MSA_NAME_MAX];            /* Nombre del paquete */
    char     pkg_version[16];               /* Versión del paquete */
    char     author[MSA_NAME_MAX];          /* Autor */
    char     description[MSA_DESC_MAX];     /* Descripción */
    uint32_t num_files;                     /* Cantidad de archivos */
    uint32_t total_size;                    /* Tamaño total descomprimido */
    uint32_t header_size;                   /* Tamaño del header + file table */
    uint16_t num_deps;                      /* Número de dependencias */
    char     deps[MSA_MAX_DEPS][MSA_NAME_MAX]; /* Dependencias */
    uint32_t checksum;                      /* CRC32 simple */
    uint8_t  reserved[128];                 /* Reservado */
} __attribute__((packed)) msa_header_t;

typedef struct {
    char     path[MSA_PATH_MAX];            /* Ruta de instalación */
    uint32_t size;                          /* Tamaño del archivo */
    uint32_t offset;                        /* Offset en el archivo .msa */
    uint32_t mode;                          /* Permisos (estilo UNIX) */
    uint8_t  type;                          /* 0=archivo, 1=directorio, 2=symlink */
    uint8_t  executable;                    /* 1 si es ejecutable */
    uint8_t  reserved[54];                  /* Padding a 320 bytes */
} __attribute__((packed)) msa_file_entry_t;

/* ==================== Variables Globales ==================== */

static msa_file_entry_t files[MSA_MAX_FILES];
static int file_count = 0;
static char *file_data[MSA_MAX_FILES];
static uint32_t total_data_size = 0;
static char base_dir[1024];

/* ==================== Funciones ==================== */

static uint32_t calculate_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static int scan_directory(const char *dir_path, const char *install_prefix) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror("opendir");
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        char full_path[1024];
        char install_path[MSA_PATH_MAX];
        
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        snprintf(install_path, sizeof(install_path), "%s/%s", install_prefix, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) != 0) {
            perror("stat");
            continue;
        }
        
        if (file_count >= MSA_MAX_FILES) {
            fprintf(stderr, "Error: Too many files (max %d)\n", MSA_MAX_FILES);
            closedir(dir);
            return -1;
        }
        
        if (S_ISDIR(st.st_mode)) {
            /* Directorio */
            msa_file_entry_t *f = &files[file_count];
            memset(f, 0, sizeof(*f));
            strncpy(f->path, install_path, MSA_PATH_MAX - 1);
            f->type = 1;  /* Directorio */
            f->mode = st.st_mode & 0777;
            f->size = 0;
            f->offset = 0;
            file_data[file_count] = NULL;
            file_count++;
            
            printf("  [DIR]  %s\n", install_path);
            
            /* Recursivo */
            if (scan_directory(full_path, install_path) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Archivo regular */
            FILE *fp = fopen(full_path, "rb");
            if (!fp) {
                perror("fopen");
                continue;
            }
            
            char *data = malloc(st.st_size);
            if (!data) {
                perror("malloc");
                fclose(fp);
                continue;
            }
            
            if (fread(data, 1, st.st_size, fp) != (size_t)st.st_size) {
                perror("fread");
                free(data);
                fclose(fp);
                continue;
            }
            fclose(fp);
            
            msa_file_entry_t *f = &files[file_count];
            memset(f, 0, sizeof(*f));
            strncpy(f->path, install_path, MSA_PATH_MAX - 1);
            f->type = 0;  /* Archivo */
            f->mode = st.st_mode & 0777;
            f->size = st.st_size;
            f->executable = (st.st_mode & S_IXUSR) ? 1 : 0;
            
            file_data[file_count] = data;
            total_data_size += st.st_size;
            
            printf("  [FILE] %s (%u bytes)%s\n", install_path, 
                   (unsigned)st.st_size, f->executable ? " [exec]" : "");
            
            file_count++;
        }
    }
    
    closedir(dir);
    return 0;
}

static void print_usage(const char *prog) {
    printf("MesaOS Package Creator v1.0\n\n");
    printf("Usage: %s [options] <source-dir> <output.msa>\n\n", prog);
    printf("Options:\n");
    printf("  -n <name>        Package name (required)\n");
    printf("  -v <version>     Package version (default: 1.0.0)\n");
    printf("  -a <author>      Author name\n");
    printf("  -d <description> Package description\n");
    printf("  -D <dep>         Add dependency (can repeat)\n");
    printf("  -p <prefix>      Install prefix (default: /)\n");
    printf("  -h               Show this help\n");
    printf("\nExample:\n");
    printf("  %s -n hello -v 1.0.0 -a \"John\" -d \"Hello World\" ./pkg-root hello.msa\n", prog);
}

int main(int argc, char **argv) {
    char *name = NULL;
    char *version = "1.0.0";
    char *author = "Unknown";
    char *description = "";
    char *prefix = "";
    char *deps[MSA_MAX_DEPS];
    int num_deps = 0;
    
    int opt;
    while ((opt = getopt(argc, argv, "n:v:a:d:D:p:h")) != -1) {
        switch (opt) {
            case 'n': name = optarg; break;
            case 'v': version = optarg; break;
            case 'a': author = optarg; break;
            case 'd': description = optarg; break;
            case 'D':
                if (num_deps < MSA_MAX_DEPS) {
                    deps[num_deps++] = optarg;
                }
                break;
            case 'p': prefix = optarg; break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (optind + 2 > argc || !name) {
        print_usage(argv[0]);
        return 1;
    }
    
    char *source_dir = argv[optind];
    char *output_file = argv[optind + 1];
    
    printf("Creating package: %s v%s\n", name, version);
    printf("Source: %s\n", source_dir);
    printf("Output: %s\n", output_file);
    printf("\nScanning files...\n");
    
    /* Escanear directorio */
    if (scan_directory(source_dir, prefix) != 0) {
        fprintf(stderr, "Error scanning directory\n");
        return 1;
    }
    
    printf("\nFound %d files/directories\n", file_count);
    
    /* Calcular offsets */
    uint32_t header_size = sizeof(msa_header_t) + (file_count * sizeof(msa_file_entry_t));
    uint32_t current_offset = header_size;
    
    for (int i = 0; i < file_count; i++) {
        if (files[i].type == 0) {  /* Solo archivos */
            files[i].offset = current_offset;
            current_offset += files[i].size;
        }
    }
    
    /* Crear header */
    msa_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = MSA_MAGIC;
    header.version = MSA_VERSION;
    strncpy(header.name, name, MSA_NAME_MAX - 1);
    strncpy(header.pkg_version, version, 15);
    strncpy(header.author, author, MSA_NAME_MAX - 1);
    strncpy(header.description, description, MSA_DESC_MAX - 1);
    header.num_files = file_count;
    header.total_size = total_data_size;
    header.header_size = header_size;
    header.num_deps = num_deps;
    
    for (int i = 0; i < num_deps; i++) {
        strncpy(header.deps[i], deps[i], MSA_NAME_MAX - 1);
    }
    
    /* Escribir archivo */
    FILE *out = fopen(output_file, "wb");
    if (!out) {
        perror("fopen output");
        return 1;
    }
    
    /* Escribir header */
    fwrite(&header, sizeof(header), 1, out);
    
    /* Escribir file table */
    for (int i = 0; i < file_count; i++) {
        fwrite(&files[i], sizeof(msa_file_entry_t), 1, out);
    }
    
    /* Escribir datos */
    for (int i = 0; i < file_count; i++) {
        if (files[i].type == 0 && file_data[i]) {
            fwrite(file_data[i], 1, files[i].size, out);
        }
    }
    
    /* Calcular y escribir checksum */
    fseek(out, 0, SEEK_END);
    long total_size = ftell(out);
    fseek(out, 0, SEEK_SET);
    
    uint8_t *all_data = malloc(total_size);
    fread(all_data, 1, total_size, out);
    header.checksum = calculate_crc32(all_data, total_size);
    free(all_data);
    
    /* Reescribir header con checksum */
    fseek(out, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, out);
    
    fclose(out);
    
    printf("\nPackage created successfully!\n");
    printf("  Total size: %ld bytes\n", total_size);
    printf("  Files: %d\n", file_count);
    printf("  Data size: %u bytes\n", total_data_size);
    
    /* Limpiar */
    for (int i = 0; i < file_count; i++) {
        if (file_data[i]) free(file_data[i]);
    }
    
    return 0;
}