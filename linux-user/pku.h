#include <sys/mman.h>
#include <sys/syscall.h>

static inline void
wrpkru(unsigned pkru)
{
    unsigned eax = pkru;
    unsigned ecx = 0;
    unsigned edx = 0;

    asm volatile(".byte 0x0f,0x01,0xef\n\t"
                 :
                 : "a"(eax), "c"(ecx), "d"(edx));
}

static inline unsigned pkey_set(unsigned pkey, unsigned long rights, unsigned long flags)
{
    unsigned pkru = (rights << (2 * pkey));
    wrpkru(pkru);
    return 0;
}

static inline unsigned pkey_mprotect(void *ptr, size_t size, unsigned long orig_prot, unsigned long pkey)
{
    return syscall(SYS_pkey_mprotect, ptr, size, orig_prot, pkey);
}

static inline unsigned pkey_alloc(void)
{
    return syscall(SYS_pkey_alloc, 0, 0);
}

static inline unsigned pkey_free(unsigned long pkey)
{
    return syscall(SYS_pkey_free, pkey);
}

#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE 0x2
#define PKEY_ACCESS_MASK (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)

//we need delete a address fast and add fast


typedef struct pku_global
{
    unsigned page_addr; //pku_global will store an unsignedeger
    unsigned tid;
    int pkey;// or pkey
    unsigned protected_counter;
    struct pku_global *right_child; // right child
    struct pku_global *left_child; // left child
}pku_global;


typedef struct pku_thread
{
    unsigned tid;
    struct pku_global *node;
} pku_thread;


struct pku_global* search(struct pku_global *root, unsigned x);
struct pku_global* find_minimum(struct pku_global *root);
struct pku_global* insert(struct pku_global *root, pku_global* new_node);
struct pku_global* delete(struct pku_global *root, unsigned x);
void inorder(struct pku_global *root);

void check_and_protect(unsigned tid);
void check_and_remove(unsigned tid);
void clean_all_pkeys(unsigned tid);
bool remove_pku_protect(unsigned addr, unsigned tid);
void add_pku_protect(unsigned addr, unsigned tid);
bool check_pku(unsigned addr, unsigned tid);
unsigned find_key_thread(unsigned addr, unsigned tid);