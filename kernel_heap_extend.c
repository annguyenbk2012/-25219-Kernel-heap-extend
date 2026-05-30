#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#define PAGE_SIZE 4096
#define ALIGN8(x) (((x) + 7) & ~7)
#define NUM_BINS 3
#define MIN_PAYLOAD 8
#define MIN_REGION_SIZE (4 * PAGE_SIZE)
#define EMPTY_REGION_CACHE_LIMIT 2
#define LARGE_ALLOCATION_THRESHOLD (8 * PAGE_SIZE)
#define MAX_REGIONS 128
#define STRESS_STEPS 2000  // Số lượng thao tác test
#define MAX_PTRS 200       // Số lượng con trỏ quản lý tối đa cùng lúc
#define MAX_POINTERS 50

const size_t bin_limits[NUM_BINS] = {64, 512, 40960};

typedef struct block {
    size_t size;
    struct block *next;
    struct block *prev;
    union {
        int free;
        uint64_t force_align;
    } info;
} block_t;

typedef struct {
    size_t size;
    int free;
} footer_t;

typedef struct heap_region {
    void *start;
    size_t size;
    int in_use;
    int dedicated;
} heap_region_t;

typedef enum {
    REGION_KEPT_IN_CACHE,
    REGION_RELEASED_TO_OS
} region_action_t;

#define METADATA_SIZE sizeof(block_t)
#define FOOTER_SIZE sizeof(footer_t)

static block_t *free_lists[NUM_BINS] = {NULL, NULL, NULL};
static heap_region_t heap_regions[MAX_REGIONS];

static size_t round_up_to_page(size_t size) {
    return ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
}

static int get_bin_index(size_t size) {
    for (int i = 0; i < NUM_BINS - 1; i++) {
        if (size <= bin_limits[i]) {
            return i;
        }
    }
    return NUM_BINS - 1;
}

static void set_footer(block_t *block) {
    footer_t *footer = (footer_t *)((char *)block + METADATA_SIZE + block->size);
    footer->size = block->size;
    footer->free = block->info.free;
}

static heap_region_t *allocate_region_slot(void) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!heap_regions[i].in_use) {
            heap_regions[i].in_use = 1;
            heap_regions[i].start = NULL;
            heap_regions[i].size = 0;
            heap_regions[i].dedicated = 0;
            return &heap_regions[i];
        }
    }

    return NULL;
}

static heap_region_t *find_region_by_address(void *ptr) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        char *start;
        char *end;

        if (!heap_regions[i].in_use) {
            continue;
        }

        start = (char *)heap_regions[i].start;
        end = start + heap_regions[i].size;
        if ((char *)ptr >= start && (char *)ptr < end) {
            return &heap_regions[i];
        }
    }

    return NULL;
}

static block_t *region_first_block(heap_region_t *region) {
    return (block_t *)region->start;
}

static int region_is_whole_free_block(heap_region_t *region, block_t *block) {
    size_t full_payload_size = region->size - METADATA_SIZE - FOOTER_SIZE;

    return (void *)block == region->start &&
           block->info.free &&
           block->size == full_payload_size;
}

static int count_cached_empty_regions(void) {
    int count = 0;

    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!heap_regions[i].in_use || heap_regions[i].dedicated) {
            continue;
        }

        block_t *block = region_first_block(&heap_regions[i]);
        if (region_is_whole_free_block(&heap_regions[i], block)) {
            count++;
        }
    }

    return count;
}

static void add_to_list(block_t *block) {
    int bin = get_bin_index(block->size);

    block->next = free_lists[bin];
    block->prev = NULL;
    if (free_lists[bin]) {
        free_lists[bin]->prev = block;
    }
    free_lists[bin] = block;
}

static void remove_from_list(block_t *block) {
    int bin = get_bin_index(block->size);

    if (block->prev) {
        block->prev->next = block->next;
    } else if (free_lists[bin] == block) {
        free_lists[bin] = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
}

static void split_block(block_t *slot, size_t size) {
    if (slot->size < size + METADATA_SIZE + FOOTER_SIZE + MIN_PAYLOAD) {
        return;
    }

    size_t total_old_size = slot->size;
    block_t *new_block = (block_t *)((char *)slot + METADATA_SIZE + size + FOOTER_SIZE);

    slot->size = size;
    set_footer(slot);

    new_block->size = total_old_size - size - METADATA_SIZE - FOOTER_SIZE;
    new_block->info.free = 1;
    new_block->next = NULL;
    new_block->prev = NULL;
    set_footer(new_block);
    add_to_list(new_block);

    printf("[DEBUG] Splitting: remainder block %p (size: %zu) added to bin %d\n",
           (void *)new_block, new_block->size, get_bin_index(new_block->size));
}

static block_t *grow_heap(size_t size) {
    size_t total_needed = ALIGN8(size + METADATA_SIZE + FOOTER_SIZE);
    size_t alloc_size = round_up_to_page(total_needed);
    int dedicated = 0;
    heap_region_t *region;
    block_t *new_block;

    if (alloc_size < MIN_REGION_SIZE) {
        alloc_size = MIN_REGION_SIZE;
    }

    if (alloc_size >= LARGE_ALLOCATION_THRESHOLD) {
        dedicated = 1;
    }

    region = allocate_region_slot();
    if (!region) {
        printf("[ERROR] No free region metadata slots left.\n");
        return NULL;
    }

    printf("[SYSTEM] grow_heap: request %zu bytes (%zu pages) via mmap%s...\n",
           alloc_size,
           alloc_size / PAGE_SIZE,
           dedicated ? " [dedicated]" : "");

    new_block = (block_t *)mmap(
        NULL,
        alloc_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (new_block == MAP_FAILED) {
        region->in_use = 0;
        return NULL;
    }

    region->start = new_block;
    region->size = alloc_size;
    region->dedicated = dedicated;

    new_block->size = alloc_size - METADATA_SIZE - FOOTER_SIZE;
    new_block->info.free = 1;
    new_block->next = NULL;
    new_block->prev = NULL;
    set_footer(new_block);
    add_to_list(new_block);
    return new_block;
}

static block_t *coalesce(block_t *block) {
    heap_region_t *region = find_region_by_address(block);
    char *region_start;
    char *region_end;

    if (!region) {
        return block;
    }

    region_start = (char *)region->start;
    region_end = region_start + region->size;

    for (;;) {
        int merged = 0;
        char *block_start = (char *)block;
        char *block_end = block_start + METADATA_SIZE + block->size + FOOTER_SIZE;

        if (block_end + (ptrdiff_t)METADATA_SIZE + (ptrdiff_t)FOOTER_SIZE <= region_end) {
            block_t *next = (block_t *)block_end;
            if (next->info.free) {
                remove_from_list(next);
                block->size += METADATA_SIZE + FOOTER_SIZE + next->size;
                set_footer(block);
                merged = 1;
            }
        }

        if (block_start > region_start + (ptrdiff_t)FOOTER_SIZE) {
            footer_t *prev_footer = (footer_t *)(block_start - FOOTER_SIZE);
            char *prev_start = block_start - FOOTER_SIZE - prev_footer->size - METADATA_SIZE;

            if (prev_start >= region_start) {
                block_t *prev = (block_t *)prev_start;
                if (prev_footer->free && prev->info.free) {
                    remove_from_list(prev);
                    prev->size += METADATA_SIZE + FOOTER_SIZE + block->size;
                    set_footer(prev);
                    block = prev;
                    merged = 1;
                }
            }
        }

        if (!merged) {
            return block;
        }
    }
}

static region_action_t release_or_cache_region(block_t *block) {
    heap_region_t *region = find_region_by_address(block);
    int cached_empty_regions;

    if (!region || !region_is_whole_free_block(region, block)) {
        add_to_list(block);
        return REGION_KEPT_IN_CACHE;
    }

    if (region->dedicated) {
        printf("[SYSTEM] Releasing dedicated empty region %p (%zu bytes) back to OS.\n",
               region->start, region->size);
        munmap(region->start, region->size);
        region->in_use = 0;
        return REGION_RELEASED_TO_OS;
    }

    cached_empty_regions = count_cached_empty_regions();
    if (cached_empty_regions > EMPTY_REGION_CACHE_LIMIT) {
        printf("[SYSTEM] Releasing extra empty region %p (%zu bytes) back to OS.\n",
               region->start, region->size);
        munmap(region->start, region->size);
        region->in_use = 0;
        return REGION_RELEASED_TO_OS;
    }

    printf("[SYSTEM] Keeping empty region %p (%zu bytes) cached.\n",
           region->start, region->size);
    add_to_list(block);
    return REGION_KEPT_IN_CACHE;
}

void *kalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    size = ALIGN8(size);
    int start_bin = get_bin_index(size);

    for (int i = start_bin; i < NUM_BINS; i++) {
        block_t *curr = free_lists[i];
        while (curr) {
            if (curr->info.free && curr->size >= size) {
                remove_from_list(curr);
                split_block(curr, size);
                curr->info.free = 0;
                set_footer(curr);
                return (void *)(curr + 1);
            }
            curr = curr->next;
        }
    }

    block_t *new_block = grow_heap(size);
    if (!new_block) {
        return NULL;
    }

    remove_from_list(new_block);
    split_block(new_block, size);
    new_block->info.free = 0;
    set_footer(new_block);
    return (void *)(new_block + 1);
}

void kfree(void *ptr) {
    block_t *curr;
    int final_bin;
    region_action_t action;

    if (!ptr) {
        return;
    }

    curr = (block_t *)ptr - 1;
    if (!find_region_by_address(curr)) {
        printf("[WARN] Ignored invalid pointer %p\n", ptr);
        return;
    }

    if (curr->info.free) {
        printf("[WARN] Ignored double free for %p\n", ptr);
        return;
    }

    curr->info.free = 1;
    set_footer(curr);
    curr = coalesce(curr);

    final_bin = get_bin_index(curr->size);
    action = release_or_cache_region(curr);

    if (action == REGION_RELEASED_TO_OS) {
        printf("[INFO] Freed block %p; its region was returned to the OS.\n", ptr);
    } else {
        printf("[INFO] Freed block %p to bin %d.\n", ptr, final_bin);
    }
}

void print_bins_status(void) {
    printf("\n--- BIN STATUS (SEGREGATED LISTS) ---\n");
    for (int i = 0; i < NUM_BINS; i++) {
        block_t *curr = free_lists[i];
        printf("BIN [%d] (limit: %zu bytes):\n", i, bin_limits[i]);
        if (!curr) {
            printf("  (empty)\n");
        }
        while (curr) {
            footer_t *f = (footer_t *)((char *)curr + METADATA_SIZE + curr->size);
            printf("  [Block %p] Size: %zu | Free: %d | Footer Size: %zu\n",
                   (void *)curr, curr->size, curr->info.free, f->size);
            curr = curr->next;
        }
    }
    printf("------------------------------------\n");
}

void print_region_status(void) {
    printf("\n--- REGION STATUS ---\n");
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!heap_regions[i].in_use) {
            continue;
        }

        block_t *block = region_first_block(&heap_regions[i]);
        printf("Region %p | Size: %zu | Dedicated: %s | Whole free: %s\n",
               heap_regions[i].start,
               heap_regions[i].size,
               heap_regions[i].dedicated ? "yes" : "no",
               region_is_whole_free_block(&heap_regions[i], block) ? "yes" : "no");
    }
    printf("---------------------\n");
}

int main(void) {
printf("=== KHỞI ĐỘNG BÀI STRESS TEST TỰ ĐỘNG ===\n");
    
    // Khởi tạo số ngẫu nhiên ngẫu nhiên theo thời gian
    srand(time(NULL)); 

    void *ptrs[MAX_PTRS] = {NULL};
    size_t sizes[MAX_PTRS] = {0};
    
    int success_allocs = 0;
    int success_frees = 0;

    for (int i = 0; i < STRESS_STEPS; i++) {
        // Chọn ngẫu nhiên một vị trí (slot) trong mảng con trỏ
        int slot = rand() % MAX_PTRS;

        if (ptrs[slot] == NULL) {
            // Nếu slot trống -> Tiến hành KALLOC
            // Kích thước ngẫu nhiên từ 8 đến 5000 bytes
            size_t size = (rand() % 5000) + 8; 
            
            ptrs[slot] = kalloc(size);
            sizes[slot] = size;

            if (ptrs[slot] != NULL) {
                success_allocs++;
                // GHI THỬ DỮ LIỆU: Điền một ký tự đặc trưng vào vùng nhớ vừa cấp phát
                // để lát nữa kiểm tra xem có bị thằng khác ghi đè lên không
                unsigned char *buf = (unsigned char *)ptrs[slot];
                for (size_t j = 0; j < size; j++) {
                    buf[j] = (unsigned char)(slot & 0xFF);
                }
            }
        } else {
            // Nếu slot đang có dữ liệu -> KIỂM TRA DỮ LIỆU TRƯỚC KHI KFREE
            unsigned char *buf = (unsigned char *)ptrs[slot];
            int is_corrupted = 0;
            
            for (size_t j = 0; j < sizes[slot]; j++) {
                if (buf[j] != (unsigned char)(slot & 0xFF)) {
                    is_corrupted = 1;
                    break;
                }
            }

            if (is_corrupted) {
                printf("[LỖI NGHIÊM TRỌNG] Bộ nhớ tại slot %d (ptr: %p) đã bị dính vùng nhớ khác đè lên!\n", slot, ptrs[slot]);
                print_bins_status();
                return 1; // Dừng chương trình vì bị lỗi quản lý con trỏ
            }

            // Nếu dữ liệu toàn vẹn -> Tiến hành KFREE
            kfree(ptrs[slot]);
            ptrs[slot] = NULL;
            sizes[slot] = 0;
            success_frees++;
        }

        // Cứ sau 500 bước thì in tiến độ ra màn hình
        if (i % 500 == 0) {
            printf("[TIẾN ĐỘ] Đã chạy %d bước... (Cấp phát thành công: %d, Giải phóng: %d)\n", 
                   i, success_allocs, success_frees);
        }
    }

    // Giải phóng toàn bộ những con trỏ còn sót lại cuối ngày
    for (int slot = 0; slot < MAX_PTRS; slot++) {
        if (ptrs[slot] != NULL) {
            kfree(ptrs[slot]);
            success_frees++;
        }
    }

    printf("\n=== KẾT QUẢ BÀI STRESS TEST: ===\n");
    printf("Tổng số lần cấp phát thành công: %d\n", success_allocs);
    printf("Tổng số lần giải phóng thành công: %d\n", success_frees);
    
    // In trạng thái cuối cùng để kiểm tra xem Free List có bị rò rỉ dữ liệu không
    print_bins_status();
    print_region_status();
    // Mảng để lưu trữ tạm thời các con trỏ được cấp phát, giúp bạn dễ quản lý khi tương tác
    void *allocated_pointers[MAX_POINTERS];
    size_t pointer_sizes[MAX_POINTERS];
    int total_ptrs = 0;

    // Khởi tạo mảng quản lý con trỏ ban đầu đều rỗng
    for (int i = 0; i < MAX_POINTERS; i++) {
        allocated_pointers[i] = NULL;
        pointer_sizes[i] = 0;
    }

    printf("=== HEAP INTERACTIVE CLI MANAGEMENT ===\n");
    printf("Code mo phong Kernel Heap Allocator bang mmap()\n");

    while (1) {
        printf("\n==================================================\n");
        printf("DANH SACH CON TRO DANG QUAN LY (Tong so: %d):\n", total_ptrs);
        
        int has_ptr = 0;
        for (int i = 0; i < MAX_POINTERS; i++) {
            if (allocated_pointers[i] != NULL) {
                printf("  [%d] Dia chi: %p | Kich thuoc da xin: %zu bytes\n", 
                       i, allocated_pointers[i], pointer_sizes[i]);
                has_ptr = 1;
            }
        }
        if (!has_ptr) {
            printf("  (Hien tai chua co con tro nao duoc cap phat)\n");
        }

        printf("\nCHON HANH DONG:\n");
        printf("1. Kalloc (Cap phat bo nho moi)\n");
        printf("2. Kfree (Giai phong con tro dang co)\n");
        printf("3. Print Status (Xem chi tiet phan manh Heap/Bins/Regions)\n");
        printf("4. Exit (Thoat chuong trinh)\n");
        printf("Nhap lua chon cua ban (1-4): ");

        int choice;
        if (scanf("%d", &choice) != 1) {
            // Xóa bộ đệm bàn phím nếu nhập sai định dạng ký tự
            while (getchar() != '\n');
            printf("[LOI] Vui long chi nhap so tu 1 den 4!\n");
            continue;
        }

        if (choice == 1) {
            // TÌM SLOT TRỐNG TRONG MẢNG ĐỂ LƯU CON TRỎ MỚI
            int slot = -1;
            for (int i = 0; i < MAX_POINTERS; i++) {
                if (allocated_pointers[i] == NULL) {
                    slot = i;
                    break;
                }
            }

            if (slot == -1) {
                printf("[LOI] Mang quan ly con tro da day (%d slots). Hay kfree bot trước!\n", MAX_POINTERS);
                continue;
            }

            size_t req_size;
            printf("Nhap so bytes muon cap phat (size_t): ");
            if (scanf("%zu", &req_size) != 1 || req_size <= 0) {
                while (getchar() != '\n');
                printf("[LOI] Kich thuoc khong hop le!\n");
                continue;
            }

            printf("\n--- Tien hanh goi kalloc(%zu) ---\n", req_size);
            void *ptr = kalloc(req_size);

            if (ptr != NULL) {
                allocated_pointers[slot] = ptr;
                pointer_sizes[slot] = req_size;
                total_ptrs++;
                printf("[THANH CONG] kalloc thanh cong! Luu tai ID [%d], Dia chi: %p\n", slot, ptr);
            } else {
                printf("[THAT BAI] kalloc khong the cap phat bo nho (He thong het tai nguyen)!\n");
            }

        } else if (choice == 2) {
            int id_to_free;
            printf("Nhap ID cua con tro muon giai phong (xem so o dau dong [x]): ");
            if (scanf("%d", &id_to_free) != 1 || id_to_free < 0 || id_to_free >= MAX_POINTERS) {
                while (getchar() != '\n');
                printf("[LOI] ID nhap vao khong hop le!\n");
                continue;
            }

            if (allocated_pointers[id_to_free] == NULL) {
                printf("[LOI] Slot [%d] dang trong, khong co con tro de giai phong!\n", id_to_free);
                continue;
            }

            printf("\n--- Tien hanh goi kfree cho ID [%d] (Dia chi: %p) ---\n", id_to_free, allocated_pointers[id_to_free]);
            kfree(allocated_pointers[id_to_free]);

            // Xóa thông tin con trỏ khỏi mảng quản lý sau khi đã giải phóng thành công
            allocated_pointers[id_to_free] = NULL;
            pointer_sizes[id_to_free] = 0;
            total_ptrs--;

        } else if (choice == 3) {
            // Gọi lại 2 hàm in debug có sẵn của bạn để xem trạng thái phân mảnh
            print_bins_status();
            print_region_status();

        } else if (choice == 4) {
            printf("Dang thoat chuong trinh... Giai phong toan bo truoc khi thoat.\n");
            for (int i = 0; i < MAX_POINTERS; i++) {
                if (allocated_pointers[i] != NULL) {
                    kfree(allocated_pointers[i]);
                }
            }
            printf("Tam biet!\n");
            break;

        } else {
            printf("[LOI] Lua chon khong hop le! Vui long nhap tu 1 den 4.\n");
        }
    }

    return 0;
}
