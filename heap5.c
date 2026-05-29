#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h> // Thư viện cho mmap và munmap (Nâng cấp Tuần 5)
#include <unistd.h>

// Định nghĩa kích thước Heap là 4KB (tương đương 1 Page trong Virtual Memory)
#define HEAP_SIZE 4096 
// Macro căn chỉnh 8-byte (Từ Tuần 3) [2]
#define ALIGN8(x) (((x) + 7) & ~7)

// Cấu trúc Metadata tối ưu hóa (Từ Tuần 3) [2]
typedef struct block {
    size_t size;            
    struct block* next;     
    union {
        int free;           
        uint64_t force_align; 
    } info;
} block_t;

#define METADATA_SIZE sizeof(block_t)

// Thay thế static char heap bằng con trỏ vùng nhớ ảo [1]
block_t* head = NULL;

// --- KHỞI TẠO HEAP VỚI VIRTUAL MEMORY (Tuần 5) ---
void init_heap() {
    // Sử dụng mmap để xin 1 Page từ hệ điều hành thay vì dùng mảng tĩnh [1]
    head = (block_t*)mmap(NULL, HEAP_SIZE, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (head == MAP_FAILED) {
        perror("mmap failed");
        return;
    }

    head->size = ALIGN8(HEAP_SIZE - METADATA_SIZE);
    head->info.free = 1;
    head->next = NULL;
    
    printf("[SYSTEM] Heap initialized using Virtual Memory at address: %p\n", (void*)head);
}

// --- GIẢI PHÓNG TOÀN BỘ VÙNG NHỚ ẢO (Nâng cấp thêm cho Tuần 5) ---
void destroy_heap() {
    if (head != NULL) {
        if (munmap(head, HEAP_SIZE) == 0) {
            printf("[SYSTEM] Virtual Memory released via munmap successfully.\n");
            head = NULL;
        }
    }
}

// --- CƠ CHẾ GỘP BLOCK (Tuần 4) [3] ---
void merge_blocks() {
    block_t* curr = head;
    while (curr && curr->next) {
        if (curr->info.free && curr->next->info.free) {
            curr->size += curr->next->size + METADATA_SIZE;
            curr->next = curr->next->next;
            printf("[DEBUG] Merged two adjacent free blocks.\n");
        } else {
            curr = curr->next;
        }
    }
}

// --- CHIA BLOCK (Để tối ưu sử dụng bộ nhớ) ---
void split_block(block_t* slot, size_t size) {
    if (slot->size >= size + METADATA_SIZE + 8) { // Chỉ chia nếu đủ chỗ cho metadata mới + ít nhất 8 byte dữ liệu
        block_t* new_block = (block_t*)((char*)slot + METADATA_SIZE + size);
        new_block->size = slot->size - size - METADATA_SIZE;
        new_block->info.free = 1;
        new_block->next = slot->next;
        
        slot->size = size;
        slot->info.free = 0;
        slot->next = new_block;
    }
}

// --- CẤP PHÁT BỘ NHỚ (Kế thừa ALIGN8 Tuần 3) [3] ---
void* kalloc(size_t size) {
    size = ALIGN8(size); 
    block_t* curr = head;
    while (curr) {
        if (curr->info.free && curr->size >= size) {
            split_block(curr, size);
            curr->info.free = 0;
            return (void*)(curr + 1);
        }
        curr = curr->next;
    }
    return NULL;
}

// --- KIỂM TRA ĐỊA CHỈ HỢP LỆ (Tuần 4) [3] ---
int is_valid_ptr(void* ptr) {
    if (!head || !ptr) return 0;
    // Kiểm tra xem ptr có nằm trong phạm vi vùng nhớ ảo đã cấp phát không
    return (ptr > (void*)head && ptr < (void*)((char*)head + HEAP_SIZE));
}

// --- GIẢI PHÓNG BỘ NHỚ (Tuần 4) [3] ---
void kfree(void* ptr) {
    if (!is_valid_ptr(ptr)) {
        printf("[ERROR] Invalid pointer free attempt!\n");
        return;
    }
    block_t* curr = (block_t*)ptr - 1;
    curr->info.free = 1;
    printf("[INFO] Block at %p freed.\n", ptr);
    merge_blocks(); // Tự động gộp sau khi free
}

void print_heap() {
    block_t* curr = head;
    printf("\n--- Current Heap Status ---\n");
    while (curr) {
        printf("Block [%p] | Size: %zu | Free: %d\n", (void*)curr, curr->size, curr->info.free);
        curr = curr->next;
    }
    printf("---------------------------\n");
}

// --- HÀM MAIN THỂ HIỆN CÁC NÂNG CẤP ---
int main() {
    printf("=== OS PROJECT: WEEK 5 UPGRADE (VIRTUAL MEMORY) ===\n");

    // 1. Thể hiện nâng cấp Tuần 5: Khởi tạo từ bộ nhớ ảo
    init_heap();
    print_heap();

    // 2. Thể hiện nâng cấp Tuần 3: Căn chỉnh 8-byte
    printf("\n[TEST] Allocating 100 bytes (should align to 104)... \n");
    void* p1 = kalloc(100);
    
    printf("[TEST] Allocating 200 bytes... \n");
    void* p2 = kalloc(200);
    print_heap();

    // 3. Thể hiện nâng cấp Tuần 4: Giải phóng và Gộp block tự động
    printf("\n[TEST] Freeing first block to test coalescing... \n");
    kfree(p1);
    
    printf("[TEST] Freeing second block to see automatic merge... \n");
    kfree(p2);
    print_heap();

    // 4. Thể hiện nâng cấp Tuần 5: Giải phóng toàn bộ trang nhớ ảo
    destroy_heap();

    return 0;
}