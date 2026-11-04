// memory_manager.c
//Bit & Byte
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Configuración del Sistema
#define PAGE_SIZE 256          // Tamaño de página y marco (bytes)
#define NUM_PAGES 256         // Número de páginas virtuales
#define TLB_SIZE 16           // Entradas en la TLB

// Cambiar PHYSICAL_FRAMES a 128 para la Fase 2 (Reemplazo de páginas)
#define PHYSICAL_FRAMES 256   // 256 para Fase 1 | 128 para Fase 2

#define MASK_PAGE 0xFF00
#define SHIFT_PAGE 8
#define MASK_OFFSET 0x00FF

// Estructura para entrada de la TLB
typedef struct {
    int page_number;
    int frame_number;
    unsigned long last_used; // Para reemplazo LRU en TLB
} TLBEntry;

// Estructura para entrada de la Tabla de Páginas
typedef struct {
    int frame_number;
    bool valid;
    unsigned long last_used; // Para reemplazo LRU en memoria física (Fase 2)
} PageTableEntry;

// Componentes del Administrador de Memoria
TLBEntry tlb[TLB_SIZE];
PageTableEntry page_table[NUM_PAGES];
int8_t physical_memory[PHYSICAL_FRAMES][PAGE_SIZE]; // signed char explícito

// Estado de marcos de memoria física
bool frame_allocated[PHYSICAL_FRAMES];
int page_in_frame[PHYSICAL_FRAMES]; // Mapeo inverso: marco -> página (útil para invalidar en reemplazo)

// Variables Globales de Control y Estadísticas
int tlb_entries_count = 0;
int next_tlb_fifo_idx = 0;
int next_free_frame = 0;

unsigned long access_counter = 0; // Reloj lógico para LRU
int total_addresses = 0;
int tlb_hits = 0;
int page_faults = 0;

// Inicialización de estructuras
void init_memory_manager() {
    for (int i = 0; i < TLB_SIZE; i++) {
        tlb[i].page_number = -1;
        tlb[i].frame_number = -1;
        tlb[i].last_used = 0;
    }
    for (int i = 0; i < NUM_PAGES; i++) {
        page_table[i].frame_number = -1;
        page_table[i].valid = false;
        page_table[i].last_used = 0;
    }
    for (int i = 0; i < PHYSICAL_FRAMES; i++) {
        frame_allocated[i] = false;
        page_in_frame[i] = -1;
    }
}

// Búsqueda en la TLB
int search_tlb(int page_number) {
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].page_number == page_number) {
            tlb[i].last_used = access_counter; // Actualizar LRU
            return tlb[i].frame_number;
        }
    }
    return -1; // TLB Miss
}

// Inserción / Reemplazo en la TLB (LRU)
void update_tlb(int page_number, int frame_number) {
    // Si ya existe en la TLB, solo actualizamos
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].page_number == page_number) {
            tlb[i].frame_number = frame_number;
            tlb[i].last_used = access_counter;
            return;
        }
    }

    // Buscar celda libre o víctima (LRU)
    int target_idx = -1;
    if (tlb_entries_count < TLB_SIZE) {
        target_idx = tlb_entries_count++;
    } else {
        // Encontrar la entrada menos recientemente usada (LRU)
        unsigned long min_used = tlb[0].last_used;
        target_idx = 0;
        for (int i = 1; i < TLB_SIZE; i++) {
            if (tlb[i].last_used < min_used) {
                min_used = tlb[i].last_used;
                target_idx = i;
            }
        }
    }

    tlb[target_idx].page_number = page_number;
    tlb[target_idx].frame_number = frame_number;
    tlb[target_idx].last_used = access_counter;
}

// Invalida una página en la TLB si fue victimizada
void invalidate_tlb_entry(int page_number) {
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].page_number == page_number) {
            tlb[i].page_number = -1;
            tlb[i].frame_number = -1;
            tlb[i].last_used = 0;
            break;
        }
    }
}

// Selección de Marco Libre o Reemplazo de Página (Fase 2 LRU)
int allocate_frame(int page_number, FILE *backing_store) {
    int target_frame = -1;

    // 1. Buscar si hay marco libre disponible
    if (next_free_frame < PHYSICAL_FRAMES) {
        target_frame = next_free_frame++;
    } else {
        // 2. Memoria física llena: Algoritmo de Reemplazo LRU
        unsigned long min_used = access_counter;
        int victim_page = -1;

        for (int p = 0; p < NUM_PAGES; p++) {
            if (page_table[p].valid && page_table[p].last_used < min_used) {
                min_used = page_table[p].last_used;
                victim_page = p;
            }
        }

        target_frame = page_table[victim_page].frame_number;

        // Invalidar la página víctima en Tabla de Páginas y TLB
        page_table[victim_page].valid = false;
        page_table[victim_page].frame_number = -1;
        invalidate_tlb_entry(victim_page);
    }

    // Leer los 256 bytes de la página solicitada desde BACKING_STORE
    if (fseek(backing_store, page_number * PAGE_SIZE, SEEK_SET) != 0) {
        perror("Error al posicionar cursor en BACKING_STORE");
        exit(EXIT_FAILURE);
    }

    if (fread(physical_memory[target_frame], sizeof(int8_t), PAGE_SIZE, backing_store) != PAGE_SIZE) {
        perror("Error al leer desde BACKING_STORE");
        exit(EXIT_FAILURE);
    }

    // Actualizar mapeos del nuevo marco
    frame_allocated[target_frame] = true;
    page_in_frame[target_frame] = page_number;

    return target_frame;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Uso: %s addresses.txt BACKING_STORE\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *address_file = fopen(argv[1], "r");
    if (!address_file) {
        perror("Error al abrir archivo de direcciones");
        return EXIT_FAILURE;
    }

    FILE *backing_store = fopen(argv[2], "rb");
    if (!backing_store) {
        perror("Error al abrir archivo BACKING_STORE");
        fclose(address_file);
        return EXIT_FAILURE;
    }

    init_memory_manager();

    int logical_address;
    while (fscanf(address_file, "%d", &logical_address) == 1) {
        access_counter++;
        total_addresses++;

        // Extraer número de página y desplazamiento de 8 bits
        int page_number = (logical_address & MASK_PAGE) >> SHIFT_PAGE;
        int offset = logical_address & MASK_OFFSET;
        int frame_number = -1;

        // Step 1: Consultar TLB
        frame_number = search_tlb(page_number);

        if (frame_number != -1) {
            // TLB Hit
            tlb_hits++;
        } else {
            // TLB Miss: Consultar Tabla de Páginas
            if (page_table[page_number].valid) {
                frame_number = page_table[page_number].frame_number;
            } else {
                // Page Fault: Cargar desde BACKING_STORE
                page_faults++;
                frame_number = allocate_frame(page_number, backing_store);

                page_table[page_number].frame_number = frame_number;
                page_table[page_number].valid = true;
            }

            // Actualizar TLB tras un Miss
            update_tlb(page_number, frame_number);
        }

        // Actualizar LRU timestamp en la tabla de páginas
        page_table[page_number].last_used = access_counter;

        // Calcular dirección física y recuperar byte almacenado
        int physical_address = (frame_number << SHIFT_PAGE) | offset;
        int8_t value = physical_memory[frame_number][offset];

        // Formato de salida requerido por la práctica
        printf("Virtual address: %d Physical address: %d Value: %d\n",
               logical_address, physical_address, value);
    }

    // Impresión de Estadísticas Finales
    printf("\n================ RESUMEN DE ESTADÍSTICAS ================\n");
    printf("Total de Direcciones Procesadas: %d\n", total_addresses);
    printf("Fallos de Página (Page Faults) : %d (%.2f%%)\n",
           page_faults, (page_faults / (double)total_addresses) * 100.0);
    printf("Aciertos en TLB (TLB Hits)     : %d (%.2f%%)\n",
           tlb_hits, (tlb_hits / (double)total_addresses) * 100.0);
    printf("=========================================================\n");

    fclose(address_file);
    fclose(backing_store);
    return EXIT_SUCCESS;
}

