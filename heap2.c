#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_SIZE 1024
#define ALIGN8(x) (((x) + 7) & ~7)
#define MIN_SPLIT_SIZE 8

// Cấu trúc block
typedef struct block {
    size_t size;
    int free;
    struct block* next;
} block_t;

// Heap giả lập
static char heap[HEAP_SIZE];

// Head
block_t* head = NULL;

// ================= INIT =================
void init_heap() {
    head = (block_t*)heap;
    head->size = ALIGN8(HEAP_SIZE - sizeof(block_t));
    head->free = 1;
    head->next = NULL;
}

// ================= VALIDATION =================

// check ptr có nằm trong heap không
int is_valid_ptr(void* ptr) {
    if (ptr == NULL) return 0;

    if ((char*)ptr < heap || (char*)ptr >= heap + HEAP_SIZE)
        return 0;

    return 1;
}

// ================= SPLIT =================
int can_split(block_t* block, size_t size) {
    return block->size >= size + sizeof(block_t) + MIN_SPLIT_SIZE;
}

void split_block(block_t* block, size_t size) {
    size = ALIGN8(size);

    if (!can_split(block, size)) return;

    block_t* new_block = (block_t*)((char*)block + sizeof(block_t) + size);

    // kiểm tra tránh overflow heap
    if ((char*)new_block >= heap + HEAP_SIZE) {
        printf("ERROR: split overflow heap!\n");
        return;
    }

    new_block->size = block->size - size - sizeof(block_t);

    // kiểm tra block lỗi
    if (new_block->size <= 0) {
        printf("ERROR: split new error block!\n");
        return;
    }

    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

// ================= BEST-FIT =================
void* kalloc(size_t size) {
    size = ALIGN8(size);

    block_t *curr = head;
    block_t *best = NULL;

    // tìm best-fit 
    while (curr) {
        if (curr->free && curr->size >= size) {
            if (!best || curr->size < best->size) {
                best = curr;
            }
        }
        curr = curr->next;
    }

    if (!best) return NULL;

    if (can_split(best, size)) {
        split_block(best, size);
    }

    best->free = 0;
    return (void*)(best + 1);
}

// ================= MERGE =================
void merge_blocks() {
    block_t* curr = head;

    while (curr && curr->next) {
        if (curr->free && curr->next->free) {
            curr->size += curr->next->size + sizeof(block_t);
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

// ================= FREE =================
void kfree(void* ptr) {
    if (!ptr) {
        printf("WARNING: free(NULL)\n");
        return;
    }

    if (!is_valid_ptr(ptr)) {
        printf("ERROR: invalid pointer!\n");
        return;
    }

    block_t* block = (block_t*)ptr - 1;

    // check block nằm trong heap
    if ((char*)block < heap || (char*)block >= heap + HEAP_SIZE) {
        printf("ERROR: block out of heap!\n");
        return;
    }

    // check double free
    if (block->free) {
        printf("ERROR: double free detected!\n");
        return;
    }

    block->free = 1;

    merge_blocks();
}

// ================= DEBUG =================
void print_heap() {
    block_t* curr = head;

    printf("\nHeap status:\n");
    while (curr) {
        printf("Block | addr: %p | size: %zu | free: %d\n",
               (void*)curr, curr->size, curr->free);
        curr = curr->next;
    }
}

// ================= TEST =================
int main() {
    init_heap();

    void* p1 = kalloc(50);
    void* p2 = kalloc(150);
    void* p3 = kalloc(80);
    void* p4 = kalloc(100);
    void* p5 = kalloc(200);

    printf("\nStatus 1:");
    print_heap();

    kfree(p2);
    kfree(p4);
    void* p6= kalloc(60);

    printf("\nStatus 2:");
    printf("\ntest Best fit - free p2,p4 - cap p6\n");
    print_heap();

    // test double free
    printf("\nTest double free\n");
    kfree(p2);

    // test invalid free
    printf("\nTest invalid free\n");
    int x;
    kfree(&x);

    kfree(p1);
    kfree(p3);

    printf("\nStatus 3:");
    printf("\nFree p1,p3\n");
    print_heap();

    return 0;
}
