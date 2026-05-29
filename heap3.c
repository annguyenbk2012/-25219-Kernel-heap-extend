#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_SIZE 1024
// Macro căn chỉnh 8-byte
#define ALIGN8(x) (((x) + 7) & ~7)

// Tối ưu hóa block_t: Sử dụng union và đảm bảo alignment
typedef struct block {
    size_t size;            // 8 bytes
    struct block* next;     // 8 bytes
    union {
        int free;           // 4 bytes
        uint64_t force_align; // Ép toàn bộ union/struct này căn theo 8 bytes
    } info;
} block_t; 

// Kích thước metadata sau khi đã chuẩn hóa
#define METADATA_SIZE sizeof(block_t)

static char heap[HEAP_SIZE] __attribute__((aligned(8))); // Heap cũng phải bắt đầu ở địa chỉ chia hết cho 8
block_t* head = NULL;

void init_heap() {
    head = (block_t*)heap;
    // Đảm bảo size của user data cũng được ALIGN8
    head->size = ALIGN8(HEAP_SIZE - METADATA_SIZE);
    head->info.free = 1;
    head->next = NULL;
}

void split_block(block_t* fitting_slot, size_t size) {
    // Tính toán vị trí mới: phải đảm bảo block mới cũng bắt đầu tại địa chỉ ALIGN8
    block_t* new_block = (block_t*)((char*)fitting_slot + METADATA_SIZE + size);
    
    new_block->size = fitting_slot->size - size - METADATA_SIZE;
    new_block->info.free = 1;
    new_block->next = fitting_slot->next;

    fitting_slot->size = size;
    fitting_slot->next = new_block;
}

void* kalloc(size_t size) {
    size = ALIGN8(size); // Chuẩn hóa yêu cầu người dùng
    block_t* curr = head;

    while (curr != NULL) {
        if (curr->info.free && curr->size >= size) {
            // Tối thiểu còn dư METADATA_SIZE + 8 byte dữ liệu mới thực hiện split
            if (curr->size >= size + METADATA_SIZE + 8) {
                split_block(curr, size);
            }
            curr->info.free = 0;
            return (void*)(curr + 1); // Trả về vùng nhớ ngay sau metadata
        }
        curr = curr->next;
    }
    return NULL;
}

void merge_blocks() {
    block_t* curr = head;
    while (curr && curr->next) {
        if (curr->info.free && curr->next->info.free) {
            curr->size += curr->next->size + METADATA_SIZE;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_t* block = (block_t*)ptr - 1;
    block->info.free = 1;
    merge_blocks();
}

void print_heap() {
    block_t* curr = head;
    printf("\n--- Heap Status (Metadata Size: %zu) ---\n", METADATA_SIZE);
    while (curr) {
        printf("Block [%p] | UserData: [%p] | size: %zu | free: %d\n", 
               (void*)curr, (void*)(curr + 1), curr->size, curr->info.free);
        curr = curr->next;
    }
}

int main() {
    init_heap();
    void* p1 = kalloc(100); // Sẽ được làm tròn thành 104
    void* p2 = kalloc(200); // Sẽ được làm tròn thành 200
    
    print_heap();
    kfree(p1);
    printf("\nSau khi free p1:");
    print_heap();
    
    return 0;
}