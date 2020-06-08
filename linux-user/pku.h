#include <sys/mman.h>
#include <sys/syscall.h>
static inline void
wrpkru(unsigned int pkru)
{
    unsigned int eax = pkru;
    unsigned int ecx = 0;
    unsigned int edx = 0;

    asm volatile(".byte 0x0f,0x01,0xef\n\t"
                 :
                 : "a"(eax), "c"(ecx), "d"(edx));
}

static inline int pkey_set(int pkey, unsigned long rights, unsigned long flags)
{
    unsigned int pkru = (rights << (2 * pkey));
    wrpkru(pkru);
    return 0;
}

static inline int pkey_mprotect(void *ptr, size_t size, unsigned long orig_prot, unsigned long pkey)
{
    return syscall(SYS_pkey_mprotect, ptr, size, orig_prot, pkey);
}

static inline int pkey_alloc(void)
{
    return syscall(SYS_pkey_alloc, 0, 0);
}

static inline int pkey_free(unsigned long pkey)
{
    return syscall(SYS_pkey_free, pkey);
}

#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE 0x2
#define PKEY_ACCESS_MASK (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)

typedef struct pku_global
{
    unsigned tid;
    unsigned addr;
    int protected_count;
    struct pku_global *next;
} pku_global;

typedef struct pkey_entry
{
    unsigned page_addr;
    unsigned pkey;
    struct pkey_entry *next;
} pkey_entry;

typedef struct pku_thread
{
    unsigned tid;
    struct pkey_entry *head;
} pku_thread;

void check_and_protect(unsigned tid);
void check_and_remove(unsigned tid);
void clean_all_pkeys(unsigned tid);
bool remove_pku_protect(unsigned tid, unsigned addr);
void add_pku_protect(unsigned tid, unsigned addr);
bool check_pku(unsigned tid, unsigned addr);
unsigned find_key_thread(unsigned tid, unsigned addr);