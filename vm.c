#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
struct segdesc gdt[NSEGS];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);
  
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  
  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}


// ----------------   manage pages  ----------------------

#ifndef SELECTION_NONE

int idx_page_out();

int 
get_pgidx_in_pysc(void* va)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++){
    if (proc->pysc_pgmd[i].pva == va)
      return i;
  }
  return -1;
}

int
get_pgidx_in_swap(void* va)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++){
    if (proc->swap_pgmd[i].pva == va)
      return i;
  }
  return -1;
}

int 
get_free_pgidx_swap(void)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++){
    if (proc->swap_pgmd[i].pva == (void*)-1)
      return i;
  }
  return -1;
}

int 
get_free_pgidx_pysc(void)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++)
  {
    if (proc->pysc_pgmd[i].pva == (void*)-1)
      return i;
  }
  return -1;
}

void 
init_pysc_pgmd(struct proc *p)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++)
    p->pysc_pgmd[i].pva = (void*)-1;
}

void 
init_swap_pgmd(struct proc *p)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++)
    p->swap_pgmd[i].pva = (void*)-1;
}

void 
copy_pysc_pgmd(struct proc *dstp, struct proc *srcp)
{
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++)
    dstp->pysc_pgmd[i].pva = srcp->pysc_pgmd[i].pva;
}

void 
copy_swap_pgmd(struct proc *dstp, struct proc *srcp)
{
  int i;
  for (i = 0; i < MAX_FILE_PAGES; i++)
    dstp->swap_pgmd[i].pva = srcp->swap_pgmd[i].pva;
}

// select a page from pysc memory and paged it out, if given swap_pgidx >=0 then locate the paged out page in swap_pgidx
// return the index of the freed pysc index page
int 
paged_out(int swap_pgidx)
{
  cprintf("in paged_out\n");
  char buf[PGSIZE];
  int pysc_pgidx = 4; //TODO: change it to next line in comment:
  //int pysc_pgidx = idx_page_out();

  pte_t* outpg_va = proc->pysc_pgmd[pysc_pgidx].pva;
  cprintf("the outpg_va is: %x\n", outpg_va);
  memset(buf, 0, PGSIZE); 
  memmove(buf, outpg_va, PGSIZE);
  
  pte_t* pte = walkpgdir(proc->pgdir, outpg_va, 0); //return pointer to page on pysc memory
  cprintf("the pte is: %x\n", pte);
  *pte = *pte & ~PTE_P;     // set not present
  *pte = *pte | PTE_PG;     // set swapped out
  cprintf("before kfree, in paged_out func\n");
  kfree((char*)outpg_va);   // free pysical memory
  cprintf("after kfree, in paged_out func\n");
  lcr3(v2p(proc->pgdir));   // update pgdir after page out
  proc->pysc_pgmd[pysc_pgidx].pva = (void*)-1;

  if (swap_pgidx < 0){
    // look for a free space on swap file
    if ((swap_pgidx = get_free_pgidx_swap()) == -1) {
      panic("paged_out: more then 30 pages for process");
      return -1;
    }
  }

  // write buf data to proc swap file
  uint pgoff = ((uint)swap_pgidx) * PGSIZE;
  writeToSwapFile(proc, buf, pgoff, PGSIZE);
  proc->swap_pgmd[swap_pgidx].pva = outpg_va;

  return pysc_pgidx;
}

#endif

// -----------   manage page replacemaent schemes --------

int
idx_page_out()
{
  int ret_index, next_index;
  pte_t *va_tmp;

  #if defined(SELECTION_FIFO) || defined(SELECTION_SCFIFO) 
    // find the index of of the current "oldest" page
  ret_index = get_pgidx_in_pysc(proc->pnt_page);
  next_index = ret_index;
  // find the next page in the "queue"
  for(;;)
  {
    int i=0; 
    cprintf("process->pid: %d\n", proc->pid);
    cprintf("process->pnt_page: %x     index:%d\n", proc->pnt_page, ret_index);
    while(i<15){
      cprintf("pysc_pgmd[%d] = %x\n",i, proc->pysc_pgmd[i].pva);
      i++;
    }

    next_index = (next_index + 1) % MAX_PSYC_PAGES;
    va_tmp = proc->pysc_pgmd[next_index].pva;
    if(va_tmp != (void*)-1)
    {
      #ifdef SELECTION_SCFIFO
        if(*va_tmp & PTE_A){
          *va_tmp = *va_tmp & ~PTE_A; // set off Reference bit and continue to next page
        }
        else{ // Reference bit is already off, so this is the new "oldest" page
          proc->pnt_page = va_tmp;
          break;    
        }      
      #endif

      // only FIFO scheme:
      proc->pnt_page = va_tmp;
      break;
    }
  }

  #endif 

  #if defined(SELECTION_DEAFAULT) || defined(SELECTION_NFU) 




  

  #endif

  return ret_index;
}


/*   #if defined(SELECTION_DEAFAULT) || defined(SELECTION_NFU) 
// update counter for each page if it's been referenced
  void
  update_refer_pages()
  {
    //TODO
  }
 #endif
 */


// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
// 
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP, 
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start, 
                (uint)k->phys_start, k->perm) < 0)
      return 0;
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pushcli();
  cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
  cpu->gdt[SEG_TSS].s = 0;
  cpu->ts.ss0 = SEG_KDATA << 3;
  cpu->ts.esp0 = (uint)proc->kstack + KSTACKSIZE;
  ltr(SEG_TSS << 3);
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  lcr3(v2p(p->pgdir));  // switch to new address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    int ava_pyscidx = -1;

    if (proc->pid > 2 && (ava_pyscidx = get_free_pgidx_pysc()) == -1){
      ava_pyscidx = paged_out(-1); // page out from pysc memory to swap
    }

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }

    memset(mem, 0, PGSIZE);
    mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
   
    if (ava_pyscidx >= 0){
      proc->pysc_pgmd[ava_pyscidx].pva = (void*)a;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  int pyscidx;
  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = p2v(pa);
      kfree(v);
      
      if (proc->pid > 2 && proc->pgdir == pgdir) 
      {
        if ((pyscidx = get_pgidx_in_pysc((void*)a)) == -1){
          panic("deallocate pysc: pyscidx not found");
        }
        proc->pysc_pgmd[pyscidx].pva = (void*)-1;
      }

      *pte = 0;
    }
    //not present and swapped out
    else if (((*pte & PTE_P) == 0) && ((*pte & PTE_PG) != 0))
    { 

      int swapidx;
      if ((swapidx = get_pgidx_in_swap((void*)a)) == -1){
        panic("deallocate swap: swapidx not found");
      }
      
      //char buf[PGSIZE];
      //memset(buf, 0, PGSIZE);
      //writeToSwapFile(proc, buf, swapidx*(PGSIZE), PGSIZE);
      proc->swap_pgmd[swapidx].pva = (void*)-1;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;
  
  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = p2v(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }

  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)p2v(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

#ifndef SELECTION_NONE

int
handle_pgfault(void* fadd)
{
  //index of page in swap file and in pysical memory
  int swap_pgidx;
  int pysc_pgidx;

  // the page doesn't exist in swap file or swap file doesn't allocated
  if ((swap_pgidx = get_pgidx_in_swap(fadd)) == -1 || proc->swapFile == 0)  {
    panic("handle_pgfault: not in storage");
    return -1;
  }

  // ----- the page exist in swap file ----

  uint pgoff = ((uint)swap_pgidx) * PGSIZE;
  char buf[PGSIZE];
  // kalloc to buf
  if (readFromSwapFile(proc, buf, pgoff, PGSIZE) == -1)
    panic("handle_pgfault: read from swap file");

  //return pointer to page on pysc memory
  pte_t* pte = walkpgdir(proc->pgdir, fadd, 0); 
  *pte = *pte & ~PTE_PG;  //not swaped out

  if ((pysc_pgidx = get_free_pgidx_pysc()) == -1){ 
    // pysc memory is full need to swap out a page
    pysc_pgidx = paged_out(swap_pgidx); //after that --> pysc_pgidx is free for future allocation
  }
  else {
    proc->swap_pgmd[swap_pgidx].pva = (void*)-1;
  } 

  char* mem;
  if ((mem = kalloc()) == 0) {
    panic("handle_pgfault: Kalloc - problem in allocate pysical memory"); 
    return -1;
  }
  
  memset(mem, 0, PGSIZE);   

  if (mappages(proc->pgdir, fadd, PGSIZE, v2p(mem), PTE_W|PTE_U) == -1){
    panic("handle_pgfault: mapppages - problem in mapping virtual to pysical");
    return -1;
  }
  
  // copy page content from buf to fadd 
  memmove(fadd, buf, PGSIZE);   
  //update pysc memory struct  
  proc->pysc_pgmd[pysc_pgidx].pva = fadd;
  
  return 0;
}

void
copy_swap_content(struct proc* dstp, struct proc* srcp)
{
  uint i, pgoff;
  char buf[PGSIZE];
  for (i = 0; i < MAX_FILE_PAGES; i++) {
    pgoff = i * PGSIZE;
    memset(buf, 0, PGSIZE);
    readFromSwapFile(srcp, buf, pgoff, PGSIZE);
    writeToSwapFile(dstp, buf, pgoff, PGSIZE);
  }
}


//init all proc pages initial data inculing swp file
void 
create_proc_pgmd(struct proc* p)
{
  createSwapFile(p);
  init_pysc_pgmd(p);
  init_swap_pgmd(p);
}

// clear all proc pages data
void 
free_proc_pgmd(struct proc* p, uint remove)
{
  if (remove)
    removeSwapFile(p);
  //else
  //  p->swapFile = 0; //importent to handle this case carfully so we won't lose the swap pointer (backup proc befoecalling this function)

  init_pysc_pgmd(p);
  init_swap_pgmd(p);
}

// 1. copy srcp swap file with the name of dstp   
// 2. copy all srcp pages meta data to dstp
void 
copy_proc_pgmd(struct proc* dstp, struct proc* srcp)
{
  createSwapFile(dstp);
  copy_swap_content(dstp, srcp);
  copy_pysc_pgmd(dstp, srcp);
  copy_swap_pgmd(dstp, srcp);
}

#endif
